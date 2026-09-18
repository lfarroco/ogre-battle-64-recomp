// OGRE_SAVE=<name|path> -- install a save as this run's battery (SRAM) image.
//
// The port's battery is the game's own 32 KiB SRAM image at
// `<config>/saves/<game id>.bin` (see `entry.save_type` in main.cpp). This file
// lets a run start from a save in any of the wrappings `tools/sramsave.py`
// understands, without a separate import step:
//
//   OGRE_SAVE=prologue ./build-app/ogrebattle64
//   OGRE_SAVE=~/Downloads/save.srm OGRE_SAVE_RESET=1 ./build-app/ogrebattle64
//
// Two things it has to get right, both established at instruction level in
// session 78 (docs/HANDOFF-2026-09-18-session78.md):
//
//  * A save slot's first 4 bytes are two u16 checksums over slot+0x0C..0x1850,
//    each **seeded with the slot's own device offset** (`func_8007541C` calls
//    `func_80075A84` = byte sum + seed and `func_80075B00` = set-bit count +
//    seed). The game's copy-to-pak path uses the un-seeded twins
//    (`func_80075AC4`/`func_80075B60`), so a note in a Controller Pak validates
//    with seed 0 and must be reseeded for the battery slot it lands in.
//  * The battery has only **two** save slots: slot index 2's offset 0x30B0 is
//    where the game's 19176-byte map record begins, covering the rest of the
//    image (that record is what `func_80075578` validates as slot 15).

#include "save_import.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace ogre {
namespace {

constexpr size_t kSramSize      = 0x8000;   // 32 KiB battery SRAM
constexpr size_t kSlotBase      = 0x0010;   // slot 0, after the 16-byte device header
constexpr size_t kSlotStride    = 0x1850;   // 6224 bytes per slot
constexpr size_t kSlotCount     = 2;        // slot 2's offset is the map record's
constexpr size_t kChecksumOff   = 0x000C;   // checksummed region starts here
constexpr size_t kRecord15Off   = 0x30B0;   // the game's 19176-byte map record
constexpr size_t kDexHeader     = 0x1040;   // DexDrive .N64 header size
constexpr size_t kPage          = 0x0100;
constexpr size_t kFatOff        = 0x0100;   // page 1: u16 BE next-page per page
constexpr size_t kFirstDataPage = 5;
constexpr size_t kNotePages     = 25;       // "1 note 25 pages to save"
constexpr size_t kNoteSlotOff   = 0x20;     // the battery slot inside a note
constexpr uint16_t kFatFree     = 0x0003;
constexpr uint16_t kFatEnd      = 0x0001;

constexpr char kMagic[]    = "QuestOG3";
constexpr char kDexMagic[] = "123-456-STD";

uint16_t be16(const uint8_t* p) {
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

void put_be16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v & 0xFF);
}

unsigned set_bits(uint8_t b) {
    unsigned n = 0;
    for (int i = 0; i < 8; i++) {
        n += (b >> i) & 1u;
    }
    return n;
}

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(size_t(size));
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good() || file.gcount() == size;
}

// Rewrite a slot's two u16 header checksums for the battery slot at `offset`.
void reseed_slot(uint8_t* slot, uint32_t offset) {
    uint32_t sum = 0;
    uint32_t bits = 0;
    for (size_t i = kChecksumOff; i < kSlotStride; i++) {
        sum += slot[i];
        bits += set_bits(slot[i]);
    }
    put_be16(slot + 0, uint16_t((sum + offset) & 0xFFFF));
    put_be16(slot + 2, uint16_t((bits + offset) & 0xFFFF));
}

bool is_magic(const uint8_t* p) {
    return std::memcmp(p, kMagic, sizeof(kMagic) - 1) == 0;
}

// The pak's page -> next-page table: a chain head is an allocated data page no
// other allocated page points at (pages 0..4 are the ID/inode/directory area).
std::vector<size_t> live_note_heads(const uint8_t* pak) {
    std::vector<uint16_t> next(128);
    for (size_t p = 0; p < next.size(); p++) {
        next[p] = be16(pak + kFatOff + 2 * p);
    }
    std::vector<size_t> heads;
    for (size_t p = kFirstDataPage; p < next.size(); p++) {
        if (next[p] == kFatFree) {
            continue;
        }
        bool pointed_at = false;
        for (size_t q = kFirstDataPage; q < next.size() && !pointed_at; q++) {
            if (q != p && next[q] != kFatFree && next[q] == p) {
                pointed_at = true;
            }
        }
        if (!pointed_at) {
            heads.push_back(p);
        }
    }
    return heads;
}

// The 25 pages a note at `head` occupies, or empty if it is not one note.
std::vector<size_t> note_pages(const uint8_t* pak, size_t head) {
    std::vector<uint16_t> next(128);
    for (size_t p = 0; p < next.size(); p++) {
        next[p] = be16(pak + kFatOff + 2 * p);
    }
    std::vector<size_t> pages;
    size_t cur = head;
    while (pages.size() <= 128) {
        pages.push_back(cur);
        const uint16_t n = next[cur];
        if (n == kFatEnd || n == kFatFree || n >= 128 || n < kFirstDataPage) {
            break;
        }
        cur = n;
    }
    if (pages.size() != kNotePages) {
        return {};
    }
    return pages;
}

// Copy the note's battery slot into the image's next free slot, reseeded for it.
bool place_note(const uint8_t* pak, const std::vector<size_t>& pages,
                std::vector<uint8_t>& image, size_t& placed) {
    if (placed >= kSlotCount) {
        return false;
    }
    std::vector<uint8_t> note(kNotePages * kPage);
    for (size_t i = 0; i < pages.size(); i++) {
        std::memcpy(note.data() + i * kPage, pak + pages[i] * kPage, kPage);
    }
    const uint8_t* slot = note.data() + kNoteSlotOff;
    if (!is_magic(slot + 4)) {
        return false;
    }
    const size_t off = kSlotBase + placed * kSlotStride;
    std::memcpy(image.data() + off, slot, kSlotStride);
    reseed_slot(image.data() + off, uint32_t(off));
    placed++;
    return true;
}

bool looks_like_pak(const uint8_t* pak, size_t size) {
    if (size < kSramSize) {
        return false;
    }
    if (std::memcmp(pak + kFatOff, pak + kFatOff + kPage, kPage) != 0) {
        return false;
    }
    for (size_t p = kFirstDataPage; p < 128; p++) {
        if (be16(pak + kFatOff + 2 * p) == kFatFree) {
            return true;
        }
    }
    return false;
}

void set_device_header(std::vector<uint8_t>& image) {
    std::memcpy(image.data() + 4, kMagic, sizeof(kMagic) - 1);
}

// A Controller Pak's notes -> a battery image. `all` also takes the stale
// records a deleted note leaves behind (still capped at the two real slots).
bool image_from_pak(const uint8_t* pak, bool all, std::vector<uint8_t>& image,
                    size_t& placed, std::string& how) {
    image.assign(kSramSize, 0);
    set_device_header(image);
    placed = 0;

    std::vector<size_t> starts;
    if (all) {
        for (size_t p = kFirstDataPage; p + kNotePages <= 128; p++) {
            if (std::memcmp(pak + p * kPage + kNoteSlotOff + 4, kMagic, sizeof(kMagic) - 1) == 0) {
                starts.push_back(p);
            }
        }
        how = "all note records, deleted ones included";
    }
    else {
        starts = live_note_heads(pak);
        how = "live notes, from the pak's FAT chains";
    }

    for (size_t head : starts) {
        std::vector<size_t> pages;
        if (all) {
            for (size_t i = 0; i < kNotePages; i++) {
                pages.push_back(head + i);
            }
        }
        else {
            pages = note_pages(pak, head);
            if (pages.empty()) {
                continue;
            }
        }
        place_note(pak, pages, image, placed);
    }
    return placed > 0;
}

// The port's own 32 KiB image, or an emulator wrapper of it, in either byte
// order, at any offset (parallel-n64 puts it at 0x20800, byte-swapped).
bool image_from_sram(const std::vector<uint8_t>& data, std::vector<uint8_t>& image,
                     std::string& how, std::string& order) {
    if (data.size() < kSramSize) {
        return false;
    }
    for (size_t off = 0; off + kSramSize <= data.size(); off++) {
        const uint8_t* p = data.data() + off;
        if (is_magic(p + 4)) {
            image.assign(p, p + kSramSize);
            how = "a battery image";
            order = "logical";
            return true;
        }
        // byteswapped32: every 4-byte group reversed, so the magic reads "seuQ3GOt"
        constexpr size_t kMagicLen = sizeof(kMagic) - 1;   // 8, the magic includes the '3'
        bool swapped = true;
        for (size_t i = 0; i < kMagicLen && swapped; i++) {
            const size_t group = (i / 4) * 4;
            swapped = p[4 + i] == uint8_t(kMagic[group + 3 - (i % 4)]);
        }
        if (swapped) {
            image.assign(p, p + kSramSize);
            for (size_t i = 0; i + 3 < image.size(); i += 4) {
                std::swap(image[i], image[i + 3]);
                std::swap(image[i + 1], image[i + 2]);
            }
            how = "a battery image";
            order = "byteswapped32";
            return true;
        }
    }
    return false;
}

std::filesystem::path resolve_source(const std::string& spec,
                                     const std::filesystem::path& rom_path) {
    const std::filesystem::path given(spec);
    std::vector<std::filesystem::path> dirs;
    if (given.has_parent_path() || given.is_absolute()) {
        dirs.push_back(given.parent_path());
    }
    else {
        if (!rom_path.empty()) {
            dirs.push_back(rom_path.parent_path() / "saves");
        }
        dirs.push_back(std::filesystem::path("assets") / "saves");
        dirs.push_back(std::filesystem::path("."));
    }
    const std::string name = given.filename().string();
    for (const std::filesystem::path& dir : dirs) {
        for (const std::string& candidate : { name, name + ".n64", name + ".bin" }) {
            std::error_code ec;
            const std::filesystem::path path = dir / candidate;
            if (std::filesystem::is_regular_file(path, ec)) {
                return path;
            }
        }
    }
    return {};
}

} // namespace

bool apply_ogre_save(const std::filesystem::path& pref_dir,
                     const std::u8string& game_id,
                     const std::filesystem::path& rom_path) {
    const char* spec = std::getenv("OGRE_SAVE");
    if (spec == nullptr || spec[0] == '\0') {
        return false;
    }

    const std::filesystem::path src = resolve_source(spec, rom_path);
    if (src.empty()) {
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: no such save (tried the path, "
                             "<rom dir>/saves/ and assets/saves/)\n", spec);
        return false;
    }

    std::filesystem::path dst = pref_dir / "saves" / (std::string(game_id.begin(), game_id.end()) + ".bin");
    const bool reset = [] {
        const char* r = std::getenv("OGRE_SAVE_RESET");
        return r != nullptr && r[0] != '\0' && std::strcmp(r, "0") != 0;
    }();

    std::error_code ec;
    if (!reset && std::filesystem::is_regular_file(dst, ec)) {
        const auto dst_time = std::filesystem::last_write_time(dst, ec);
        const auto src_time = std::filesystem::last_write_time(src, ec);
        if (!ec && dst_time >= src_time) {
            std::fprintf(stderr, "[save] OGRE_SAVE=%s: keeping the battery already at %s "
                                 "(newer than the source; OGRE_SAVE_RESET=1 re-imports)\n",
                         spec, dst.string().c_str());
            return false;
        }
    }

    std::vector<uint8_t> data;
    if (!read_file(src, data)) {
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: could not read %s\n", spec, src.string().c_str());
        return false;
    }

    const bool all = [] {
        const char* a = std::getenv("OGRE_SAVE_ALL");
        return a != nullptr && a[0] != '\0' && std::strcmp(a, "0") != 0;
    }();

    std::vector<uint8_t> image;
    std::string how;
    std::string order;
    size_t placed = 0;

    const bool dexdrive = data.size() >= kDexHeader + kSramSize &&
                          std::memcmp(data.data(), kDexMagic, sizeof(kDexMagic) - 1) == 0;
    auto note_slots = [&] {
        return placed == 1 ? std::string(" 0") : std::string("s 0..") + char('0' + int(placed) - 1);
    };

    if (dexdrive) {
        if (!image_from_pak(data.data() + kDexHeader, all, image, placed, how)) {
            std::fprintf(stderr, "[save] OGRE_SAVE=%s: %s holds no battery save slot\n",
                         spec, src.string().c_str());
            return false;
        }
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: DexDrive Controller Pak; %zu note%s -> "
                             "battery slot%s (%s)\n",
                     spec, placed, placed == 1 ? "" : "s", note_slots().c_str(), how.c_str());
    }
    else if (image_from_sram(data, image, how, order)) {
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: %s (%s)\n", spec, how.c_str(), order.c_str());
    }
    else if (data.size() == kSramSize && looks_like_pak(data.data(), data.size())) {
        if (!image_from_pak(data.data(), all, image, placed, how)) {
            std::fprintf(stderr, "[save] OGRE_SAVE=%s: %s holds no battery save slot\n",
                         spec, src.string().c_str());
            return false;
        }
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: bare Controller Pak; %zu note%s -> "
                             "battery slot%s (%s)\n",
                     spec, placed, placed == 1 ? "" : "s", note_slots().c_str(), how.c_str());
    }
    else {
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: %s is not a battery image, a DexDrive .N64 "
                             "or a Controller Pak\n", spec, src.string().c_str());
        return false;
    }

    std::filesystem::create_directories(dst.parent_path(), ec);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: could not write %s\n", spec, dst.string().c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    out.close();
    if (!out.good()) {
        std::fprintf(stderr, "[save] OGRE_SAVE=%s: write to %s failed\n", spec, dst.string().c_str());
        return false;
    }
    std::fprintf(stderr, "[save] OGRE_SAVE=%s: wrote %s (%zu bytes)\n",
                 spec, dst.string().c_str(), image.size());
    return true;
}

} // namespace ogre
