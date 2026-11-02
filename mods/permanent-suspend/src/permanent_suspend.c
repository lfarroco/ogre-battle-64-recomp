// Permanent Suspend Save - an Ogre Battle 64: Recomp code mod.
//
// The game's "suspend" (temporary) save is SRAM record 15: 19176 bytes at device
// offset 0x30B0, written by `func_8007485C(15)` (bank unit A's out-of-battle
// suspend action, `func_ovlA_8019F994` at 0x8019F9F8). Loading it is
// `func_800749C0(15)`, and the load menu then erases the record so it can only
// be resumed once: `func_801A9B3C` (streamedC 0x801A9B3C) checks that the
// loaded slot was 15 and calls `func_80074834(15)` at 0x801A9BCC.
//
// `func_80074834` (`.main`, 0x28 bytes) is the erase-and-commit helper:
//
//   func_80074F9C(15)   memset the record's 12-byte header to {0, 0, "QuestOG3"}
//                       in the cached 32 KiB image, then copy it back
//   func_80074BF0()     write the whole image to SRAM and free the cache
//
// Only the header is cleared; the payload stays. The erase is what makes the
// suspend save temporary, and this mod omits it for slot 15 and keeps the
// commit, so the loaded point stays loadable after the game is quit and
// relaunched. Every other slot still takes the original path, and a new suspend
// overwrites record 15 as usual.
//
// The erase is also reachable from a debug action (bank unit H's "Reset Control
// Deck", `func_ovlH_8019B340+0x10ac`, which erases slots 0, 1, 15 and 14), so
// that action leaves the suspend record in place too. Nothing else in any bank
// calls `func_80074834`, `func_80074F9C` or `func_80075360` with 15.

#include "modding.h"

// A base-game function, called through its address.
//
// A mod cannot `jal` a base function directly: the linker has no definition, and
// a 26-bit call from the mod's 0x81000000 segment to the game's 0x800xxxxx
// segment does not fit (the build fails with "relocation truncated to fit").
// Calling through a `volatile` address makes the compiler emit `lui`/`addiu`/
// `jalr`, which N64Recomp turns into a `LOOKUP_FUNC` that the runtime resolves
// against the loaded game's functions.
typedef void (*GameFunc1)(unsigned int);

static volatile unsigned int s_blank_slot_fn = 0x80074F9Cu;
static volatile unsigned int s_commit_fn = 0x80074BF0u;

// `.main` 0x80074F9C: replace one slot in the cached 32 KiB image with a blank
// one. For slot 15 it clears the 12-byte header at image + 0x30B0: both u16
// checksums go to zero and "QuestOG3" is copied to +4. The payload is untouched.
#define SAVE_BLANK_SLOT ((GameFunc1)s_blank_slot_fn)

// `.main` 0x80074BF0: write the cached image to SRAM when it is dirty, then
// free the cache. It only writes when the magic argument is 0x37081383.
#define SAVE_COMMIT_MAGIC 0x37081383u
#define SAVE_COMMIT ((GameFunc1)s_commit_fn)

// The suspend record's slot number. `func_80075578` accepts a record as empty
// on a zero first word, which is what the header erase produces.
#define SUSPEND_SLOT 15u

RECOMP_PATCH void func_80074834(unsigned int slot) {
    if (slot != SUSPEND_SLOT) {
        SAVE_BLANK_SLOT(slot);
    }
    SAVE_COMMIT(SAVE_COMMIT_MAGIC);
}
