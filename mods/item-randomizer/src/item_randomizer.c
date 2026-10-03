// Item Randomizer - an Ogre Battle 64: Recomp code mod.
//
// Scope: items the party is given after defeating an enemy unit, and items
// found on the map. Shops, cutscenes and quest gifts are left alone.
//
// The party's owned-item list is 0x80196B20: 278 records, stride 4,
// {u16 item id at +0x00, u8 in-use at +0x02, u8 count at +0x03}. Item ids are
// 1-based into the item definition table at 0x8018C42C (stride 0x20, name
// pointer at +0x00, category byte at +0x04): 1..277 are items, 278 is "None"
// and 0 is an empty slot.
//
// There are three things to get right, and they need different treatment.
//
// 1. THE MAP-OBJECT TABLE. This is the path that makes the on-screen message and
//    the inventory agree, because both read the same table value.
//
//    * Table O at 0x801EDB38: 35 entries of 8 bytes, zero-terminated,
//      {u16 type, u16 item_a, u16 item_b, u16 item_c} (ROM 0x142278; the mission
//      bank is unit N record 7, RAM 0x801AD5C0). The game scans it for the
//      object's type (bankN 0x801ADD3C-0x801ADD64), picks a column with
//      `rand() % 3` (bankN 0x801ADD68) and reads it at bankN 0x801ADDB0.
//      func_ovlN_801AD6BC has one call site, 0x8019F4AC in the map scene's
//      image, and it writes map-object action fields, so this is the map-object
//      path.
//    * The per-map ground-item list at 0x801F1002 (a u8 count) and 0x801F1004
//      (u16 ids at `i*2`), parsed at map load by func_ovlR_80215C38 and served
//      by func_ovlN_801B9694, which posts the same word to the popup field
//      0x801F36C2 and to the grant.
//
//    One value becomes the popup id and then the item granted by
//    func_ovlN_801DC740 (bankN 0x801ADEA8 / 0x801BB930). One value, one item:
//    rewrite the table and the message names what the player actually gets.
//
//    Each item word's bit 15 selects the list the grant fills: set = the
//    278-slot player list at 0x80196B20, clear = the 40-slot list at
//    0x80193AE0. The low 15 bits are the id, and the mod preserves bit 15.
//
// 2. ANY OTHER GRANT. A battle reward, a scripted or cutscene gift, a shop
//    purchase, a tutorial grant: these call func_ovlN_801DC740 directly with no
//    table value in between, so a per-frame hook cannot change what the message
//    says. Two of them are reached anyway, because the id is in RAM before the
//    grant:
//
//    * The pending battle-reward queue at 0x801936D8, 20 u16. The mission bank
//      serves a reward from it: func_ovlN_801E49D0 reads lhu(0x801936D8) at
//      0x801E5230 with lhu(0x801936DA) as the fallback, stores it to 0x8021C760,
//      grants it at 0x801E53DC and clears the first word at 0x801E53F4. The
//      reward message is built from the same stored word.
//    * Everything else, by watching the two owned-item lists and replacing a
//      record that has just appeared. A save load replaces the whole list at
//      once, which is not an acquisition, so a wholesale change re-primes the
//      shadow instead.
//
//    The shadow carries the count column (+0x03) as well as the id, because a
//    grant that merges into a stack the party already owns changes no id. At
//    VERBOSE the mod reports that case as `count-up`, which is the only way to
//    tell "one interaction granted four items" from "one interaction merged
//    into one stack".
//
// 3. THE USED COUNT (+0x02). The item screen draws `+0x02` then `+0x03` and the
//    equip test is `sltu(+0x02, +0x03)`, while the grant writes the id and
//    `+0x03` only. A record that was empty with a non-zero `+0x02` therefore
//    reads `1/1` under the next item's name and refuses every equip. The mod
//    recomputes that byte the way the game's own rebuild func_8016B774 does,
//    from the roster; see the roster-scan section below.
//
// A session memo makes the mapping stable: an original id always becomes the
// same replacement for as long as the process lives, so re-reading a table or
// re-granting an item does not churn it.
//
// A mod cannot hook the grant routine (`func_ovlN_801DC740` is streamed bank
// code, and a hook resolves through the base ELF's own sections only), which is
// why this works by watching RAM from a base-section frame hook. The hook is
// `func_80072944`, a `.main` function that runs once per frame (measured in
// `mods/exp-overflow`: 3072 entries over a 3203-frame run).

#include "modding.h"

// The recomp mod API. `recomp_get_config_u32` serves the option values the
// client's MODS panel edits; `recomp_log` writes one line to the port's output.
RECOMP_IMPORT("*", unsigned int recomp_get_config_u32(const char* id));
RECOMP_IMPORT("*", void recomp_log(const char* message));

#define ITEM_LIST 0x80196B20u
#define ITEM_SLOTS 278u
#define ITEM_STRIDE 4u

#define ITEM_TABLE 0x8018C42Cu
#define ITEM_TABLE_STRIDE 0x20u
#define ITEM_TABLE_CATEGORY 0x04u
#define ITEM_ID_MAX 277u  // 1..277 are items; 278 is "None"

#define GOLD 0x80196A8Cu

#define DROP_TABLE 0x801EDB38u
#define DROP_ENTRY_STRIDE 8u
#define DROP_ENTRY_MAX 35u
#define DROP_ITEM_A 2u
#define DROP_TYPE_FIRST 0x0027u
#define DROP_TYPE_LAST 0x0050u
#define DROP_LAST_TYPE_ADDR (DROP_TABLE + (DROP_ENTRY_MAX - 1u) * DROP_ENTRY_STRIDE)

#define RDRAM_LO 0x80000000u
#define RDRAM_HI 0x80800000u

// More than this many records changing in one frame is a wholesale rewrite (a
// save load), not a set of acquisitions.
#define RELOAD_SLOTS 12u

// --- guest memory ----------------------------------------------------------

static unsigned int rd32(unsigned int addr) {
    return *(volatile unsigned int*)addr;
}

static unsigned short rd16(unsigned int addr) {
    return *(volatile unsigned short*)addr;
}

static void wr16(unsigned int addr, unsigned short value) {
    *(volatile unsigned short*)addr = value;
}

static void wr8(unsigned int addr, unsigned char value) {
    *(volatile unsigned char*)addr = value;
}

static unsigned char rd8(unsigned int addr) {
    return *(volatile unsigned char*)addr;
}

static unsigned int item_category(unsigned int id) {
    return rd8(ITEM_TABLE + id * ITEM_TABLE_STRIDE + ITEM_TABLE_CATEGORY);
}

// --- the two owned-item lists ----------------------------------------------

#define LIST_EQUIP 0x80196B20u
#define LIST_EQUIP_SLOTS 278u

#define LIST_CONSUMABLE 0x80193AE0u
#define LIST_CONSUMABLE_SLOTS 40u

#define LIST_STRIDE 4u
#define LIST_USE_OFFSET 2u
#define LIST_COUNT_OFFSET 3u

// The `+0x02` byte of a record is the number of roster entries that reference
// the record's item, not a cache: `func_8016B774` (0x8016B774, `.streamedB`,
// size 0x29C) clears it for both lists and adds one per reference, counting
// each character's four stored equipment ids (`lhu +0x2A/+0x2C/+0x2E/+0x30`)
// and four class-derived ids, plus ten unit ids for the 40-slot list. The item
// screen draws `+0x02`, a glyph and `+0x03`, and the equip test in bank S and
// bank AF is `sltu(+0x02, +0x03)`, that is "used < owned".
//
// The grant routine writes the id and `+0x03` only (`func_ovlN_801DC740`,
// 0x801DC7FC and 0x801DC808), so it inherits whatever `+0x02` the record
// carried. A record can be empty with `+0x02 != 0`: `func_8016BA6C` decrements
// `+0x02` and clears the id when `+0x03` reaches 0, and `+0x02 > +0x03` is
// reachable because `func_ovlAF_80218E68` adds a user for class equipment with
// no ownership test. The next grant into that record then reads `1/1` under
// the new item's name, and the equip test refuses every equip. Nothing rebuilds
// the column while the item screen is open (bank S and bank AB contain no call
// to `func_8016B774`), so the stale value stays on screen.
//
// The rewriter below does not write `+0x02`. A correct value for the
// replacement id is that id's roster reference count, which needs the rebuild's
// whole roster scan reimplemented in the mod; a blind clear is wrong for the
// ids that class equipment references. The acquisition line reports the byte
// instead, so a run says whether a rewritten record carried a stale value.

// The definition table each list's ids index into, and the id bound.
//
// The consumable table base is 0x8018E6EC, and the name pointer is its +0x00
// field: `func_8016F500` (0x8016F500) is
// `andi a0,0xffff; sll v0,a0,1; addu v0,v0,a0; sll v0,v0,2` (id * 0x0C) then
// `lw v0,-6420(at)` with `at = 0x80190000`, that is `lw(0x8018E6EC + id*0x0C)`.
// Entry 1 is "Heal Leaf" and entry 0 is "None". An earlier build used
// 0x8018E6F0 with the name at +0x04, which reads the next entry's +0x08 field:
// every consumable failed the name check and no consumable was ever rewritten.
#define TABLE_EQUIP 0x8018C42Cu
#define TABLE_EQUIP_STRIDE 0x20u
#define TABLE_EQUIP_NAME 0x00u
#define TABLE_EQUIP_ID_MAX 277u

#define TABLE_CONSUMABLE 0x8018E6ECu
#define TABLE_CONSUMABLE_STRIDE 0x0Cu
#define TABLE_CONSUMABLE_NAME 0x00u
#define TABLE_CONSUMABLE_ID_MAX 40u

// The consumable entry's byte at +0x04 partitions the 41 entries. Reading a
// guest word at +0x04 gives 0x0000000A for "Heal Leaf", 0x02000096 for "Quit
// Gate" and 0x0401000A for "Package for Gelda", and the byte the port sees at
// +0x04 is the word's high byte: 0x00 for Heal Leaf, 0x02 for Quit Gate, 0x04
// for Package for Gelda. The 0x00 to 0x02 entries are the usable items and the
// 0x03 and 0x04 entries are the key items, from "Medal of Vigor" to "Pedra of
// Flame". The mod's scope leaves quest items alone, so a consumable above the
// bound is kept as the game granted it. The partition is read from the shipped
// table; the game's own reader of that byte was not identified.
#define TABLE_CONSUMABLE_CLASS 0x04u
#define TABLE_CONSUMABLE_CLASS_MAX 2u

struct item_list {
    unsigned int base;
    unsigned int slots;
    unsigned int table;
    unsigned int table_stride;
    unsigned int table_name_offset;
    unsigned int id_max;
    // 0 disables the class guard for this list. When set, it is the byte offset
    // of the entry's class field and the highest value that may be rewritten.
    unsigned int class_offset;
    unsigned int class_max;
    const char* tag;
    const char* acquire_tag;
    unsigned short shadow_id[LIST_EQUIP_SLOTS];
    // The count column (+0x03) of each slot at the previous frame. An
    // acquisition that merges into a slot the party already owns leaves the id
    // unchanged, so the id shadow cannot see it; this column is what reports it.
    unsigned char shadow_count[LIST_EQUIP_SLOTS];
    // The slots this mod wrote an id into during the current frame, and the id
    // it wrote. `func_8016B774`'s count for the new id is written to `+0x02`
    // after the list walk; this array is what names the slots that need it.
    unsigned short fix_slot[LIST_EQUIP_SLOTS];
    unsigned short fix_id[LIST_EQUIP_SLOTS];
    unsigned int fix_count;
    unsigned char primed;
};

static struct item_list s_lists[2] = {
    { LIST_EQUIP, LIST_EQUIP_SLOTS, TABLE_EQUIP, TABLE_EQUIP_STRIDE,
      TABLE_EQUIP_NAME, TABLE_EQUIP_ID_MAX, 0u, 0u, "replace", "acquire",
      { 0 }, { 0 }, { 0 }, { 0 }, 0u, 0u },
    { LIST_CONSUMABLE, LIST_CONSUMABLE_SLOTS, TABLE_CONSUMABLE,
      TABLE_CONSUMABLE_STRIDE, TABLE_CONSUMABLE_NAME, TABLE_CONSUMABLE_ID_MAX,
      TABLE_CONSUMABLE_CLASS, TABLE_CONSUMABLE_CLASS_MAX, "replace-consumable",
      "acquire-consumable", { 0 }, { 0 }, { 0 }, { 0 }, 0u, 0u },
};

// A usable id: inside the list's own id range and its table entry has a
// readable name pointer. Both tables are streamedB and resident for the whole
// session, but a randomizer must never follow a pointer it has not checked.
static int slot_id_valid(const struct item_list* list, unsigned int id) {
    unsigned int name;

    if (id < 1u || id > list->id_max) {
        return 0;
    }
    name = rd32(list->table + id * list->table_stride + list->table_name_offset);
    if (name < RDRAM_LO || name >= RDRAM_HI) {
        return 0;
    }
    return rd8(name) != 0u;
}

// The item's class byte, or 0 when the list has no class field.
static unsigned int slot_class(const struct item_list* list, unsigned int id) {
    if (list->class_offset == 0u || id < 1u || id > list->id_max) {
        return 0u;
    }
    return rd8(list->table + id * list->table_stride + list->class_offset);
}

// False for a quest item. The mod replaces what an enemy drops and what the
// party finds on the map; a key item that a cutscene hands over must keep its
// identity or the chapter cannot be completed.
static int slot_rerollable(const struct item_list* list, unsigned int id) {
    if (list->class_offset == 0u) {
        return 1;
    }
    return slot_class(list, id) <= list->class_max;
}

static unsigned int config_u32(const char* id) {
    return recomp_get_config_u32(id);
}

// --- the session memo ------------------------------------------------------

#define MEMO_SLOTS (ITEM_ID_MAX + 1u)

// One memo per list: the equipment and consumable id spaces are unrelated, and
// a shared memo would map the equipment id 1 and the consumable id 1 ("Heal
// Leaf") onto each other.
static unsigned short s_memo_id[2][MEMO_SLOTS];
static unsigned char s_memo_set[2][MEMO_SLOTS];

// Ids the session mapping has produced, per list: `s_memo_target[Y]` is set when
// the mapping learns `X -> Y`. Such an id is already the replacement form of
// something, so the three writers (the two tables, the reward queue and the
// inventory list) must leave it alone. Without the flag they fight the
// involution: the list watch mapped a table-written replacement back to the
// original, verified by writing 140 into an empty slot after `drop-table 49 140`
// and getting `acquire 140 30 0 1` then `replace 140 49 30 0`.
//
// The flag is set where the pairing is recorded, so it also covers a value a
// *previous* run left in the saved reward queue when this run's mapping happens
// to reproduce it (a pinned SEED): the queue walker then leaves it, which is
// correct, because it is a replacement already.
static unsigned char s_memo_target[2][MEMO_SLOTS];
static unsigned int s_rng = 0u;

static unsigned int rng_next(void) {
    unsigned int x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

// True when a slot other than `except_slot` already holds this id. The game
// grants an item by searching for the id and incrementing the record it finds
// (func_ovlN_801DC740, 0x801DC76C-0x801DC798), so a list normally holds one id
// once. A write that puts an id into a second record splits the game's own
// used-count: func_8016B6FC and the rebuild both stop at the first matching
// record, so the second record keeps +0x02 = 0 and the availability test
// (`sltu(+0x02, +0x03)`) reads the first record's numbers.
static int list_holds_other(const struct item_list* list, unsigned int id,
                            unsigned int except_slot) {
    unsigned int slot;

    for (slot = 0u; slot < list->slots; slot++) {
        if (slot == except_slot) {
            continue;
        }
        if (rd16(list->base + slot * LIST_STRIDE) == (unsigned short)id) {
            return 1;
        }
    }
    return 0;
}

// `avoid_slot` is NO_SLOT for a caller that writes a table rather than a slot.
#define NO_SLOT 0xFFFFFFFFu

// Rolls a replacement from the list's own id space. `mode` 1 keeps the
// replacement inside the original's category byte, which only the equipment
// table has. A candidate the list already holds is skipped so the rewrite
// cannot create a second record for one id; the last walk accepts one anyway,
// because leaving a valid item in place beats leaving the original.
//
// `salt` moves the sequence off the one the memo built, so a slot whose
// memoised replacement is already held can still roll a different item.
static unsigned int roll_replacement(unsigned int original, unsigned int mode,
                                     unsigned int list_index, unsigned int avoid_slot,
                                     unsigned int salt) {
    const struct item_list* list = &s_lists[list_index];
    unsigned int attempt;

    s_rng = s_rng ^ (original << 7) ^ 0x9E3779B9u ^ salt;

    for (attempt = 0; attempt < 8u; attempt++) {
        unsigned int candidate = 1u + (rng_next() % list->id_max);

        if (!slot_id_valid(list, candidate) || candidate == original) {
            continue;
        }
        if (mode == 1u) {
            // SAME KIND keeps the replacement in the original's own class: the
            // consumable table's class byte, or the equipment table's category
            // byte, which is its +0x04.
            if (list->class_offset != 0u) {
                if (slot_class(list, candidate) != slot_class(list, original)) {
                    continue;
                }
            } else if (item_category(candidate) != item_category(original)) {
                continue;
            }
        }
        if (avoid_slot != NO_SLOT && list_holds_other(list, candidate, avoid_slot)) {
            continue;
        }
        return candidate;
    }

    for (attempt = 1u; attempt <= 16u; attempt++) {
        unsigned int candidate = 1u + ((original + attempt * 37u) % list->id_max);

        if (!slot_id_valid(list, candidate) || candidate == original) {
            continue;
        }
        if (avoid_slot != NO_SLOT && list_holds_other(list, candidate, avoid_slot)) {
            continue;
        }
        return candidate;
    }

    for (attempt = 1u; attempt <= 16u; attempt++) {
        unsigned int candidate = 1u + ((original + attempt * 37u) % list->id_max);

        if (slot_id_valid(list, candidate) && candidate != original) {
            return candidate;
        }
    }
    return original;
}

// The replacement for an id, remembered for the session.
//
// The memo MUST record the self-mapping too. When a roll returns the original
// (or no candidate was valid) an unrecorded result means the next frame rolls
// the same id again -- and for a table walk that is 8 `slot_id_valid` probes,
// each a load from the item table plus a load from the name pool, for every
// entry, sixty times a second. That is what made combat crawl.
//
// The mapping is an involution: X becomes Y and Y becomes X. The game grants an
// item by searching the list for its id and merging into the record it finds,
// and the list holds no X after the rewrite, so a second grant of X maps to Y
// again. That is the one case where the memo would put one id into two records.
static unsigned int replacement_for(unsigned int original, unsigned int mode,
                                    unsigned int list_index, unsigned int avoid_slot) {
    const struct item_list* list = &s_lists[list_index];
    unsigned int forward;
    unsigned int backward;

    if (original < 1u || original > list->id_max) {
        return original;
    }
    if (s_memo_set[list_index][original]) {
        return s_memo_id[list_index][original];
    }
    forward = roll_replacement(original, mode, list_index, avoid_slot, 0u);
    if (forward >= 1u && forward <= list->id_max) {
        backward = s_memo_set[list_index][forward] ? s_memo_id[list_index][forward] : 0u;
        if (backward != 0u && backward != forward) {
            forward = backward;
        }
        if (forward != original) {
            s_memo_id[list_index][forward] = (unsigned short)original;
            s_memo_set[list_index][forward] = 1u;
            s_memo_target[list_index][forward] = 1u;
        }
    }
    else {
        forward = original;
    }
    s_memo_id[list_index][original] = (unsigned short)forward;
    s_memo_set[list_index][original] = 1u;
    return forward;
}

// The replacement for a slot, as opposed to for a table entry.
//
// `replacement_for` answers with the session mapping, which is what keeps the
// drop-table rewrite stable. A slot write must additionally not create a second
// record for an id the list already holds, and the involution produces exactly
// that when the game grants one original item twice: the first grant fills a
// record with Y, and the second grant of X maps to Y again. In that case the
// roll runs again with a salt, and the mapping is left as it was so a table
// entry still resolves to a stable value.
static unsigned int replacement_for_slot(unsigned int original, unsigned int mode,
                                        unsigned int list_index, unsigned int slot,
                                        unsigned int* duplicated) {
    const struct item_list* list = &s_lists[list_index];
    unsigned int replacement = replacement_for(original, mode, list_index, slot);

    *duplicated = 0u;
    // Already a replacement this session produced: the grant of it is the
    // outcome the table or queue write intended.
    if (s_memo_target[list_index][original]) {
        return original;
    }
    if (replacement == original || !list_holds_other(list, replacement, slot)) {
        return replacement;
    }
    *duplicated = replacement;
    return roll_replacement(original, mode, list_index, slot, 0x5BF03635u);
}

// --- logging ---------------------------------------------------------------

static char s_log_buffer[192];
static unsigned int s_log_offset;

static void log_reset_buffer(void) {
    s_log_offset = 0u;
    s_log_buffer[0] = '\0';
}

static void log_append_text(const char* text) {
    while (*text != '\0' && s_log_offset < sizeof(s_log_buffer) - 2u) {
        s_log_buffer[s_log_offset] = *text;
        s_log_offset++;
        text++;
    }
    s_log_buffer[s_log_offset] = '\0';
}

static void log_append_u32(unsigned int value) {
    char digits[12];
    unsigned int count = 0u;

    if (value == 0u) {
        if (s_log_offset < sizeof(s_log_buffer) - 2u) {
            s_log_buffer[s_log_offset++] = '0';
            s_log_buffer[s_log_offset] = '\0';
        }
        return;
    }
    while (value != 0u && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10u));
        count++;
        value /= 10u;
    }
    while (count != 0u && s_log_offset < sizeof(s_log_buffer) - 2u) {
        count--;
        s_log_buffer[s_log_offset++] = digits[count];
        s_log_buffer[s_log_offset] = '\0';
    }
}

// `what`, then the four numbers, each separated by one space. The separator is
// appended AFTER each field, which is the form that survives the recompiler.
static void log_event(const char* what, unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
    log_reset_buffer();
    log_append_text(what);
    log_append_text(" ");
    log_append_u32(a);
    log_append_text(" ");
    log_append_u32(b);
    log_append_text(" ");
    log_append_u32(c);
    log_append_text(" ");
    log_append_u32(d);
    recomp_log(s_log_buffer);
}

// Logging is bounded. A per-event line goes through the port's `fprintf`, and a
// busy frame can produce one event per table slot and per list slot; at sixty
// frames a second that is thousands of lines a second and the game crawls. So a
// pass prints at most LOG_BUDGET lines and then one summary, which carries the
// counts a diagnosis needs.
#define LOG_BUDGET 20u
static unsigned int s_log_left;
static unsigned int s_log_events;

// The counters the summary line carries.
static unsigned int s_seed_value;
static unsigned int s_rerolls;
static unsigned int s_acquisitions;
static unsigned int s_reloads;
static unsigned int s_purchases_skipped;
static unsigned int s_quests_kept;
static unsigned int s_table_grants;
static unsigned int s_frames;

// Per-frame and total counters for the count-change probe.
static unsigned int s_count_ups;
static unsigned int s_count_ups_frame;
static unsigned int s_acq_frame;
static unsigned int s_rerolls_frame;

// The `+0x02` correction counters. `s_uses_stale` counts the case this fix
// exists for: a record that was empty and held a non-zero used byte, which the
// item screen printed as `1/1` under whatever name was written next.
static unsigned int s_uses_fixed;
static unsigned int s_uses_stale;
static unsigned int s_uses_frame;
static unsigned int s_uses_skipped;
static unsigned int s_roster_scans;
// `roster_populated()` costs a walk of 100 records, so it is resolved once per
// frame in `fix_list_uses` rather than per corrected record.
static unsigned char s_roster_ok;

static void log_reset(void) {
    s_log_left = LOG_BUDGET;
    s_log_events = 0u;
    s_count_ups_frame = 0u;
    s_acq_frame = 0u;
    s_rerolls_frame = 0u;
    s_uses_frame = 0u;
}

static int log_want(unsigned int verb) {
    if (verb == 0u) {
        return 0;
    }
    if (s_log_left == 0u) {
        s_log_events++;
        return 0;
    }
    s_log_left--;
    s_log_events++;
    return 1;
}

static void log_summary(unsigned int verb, unsigned int total) {
    if (verb == 0u) {
        return;
    }
    if (s_log_events > LOG_BUDGET) {
        log_event("more", s_log_events - LOG_BUDGET, total, s_rerolls, 0u);
    }
    // One line per frame that changed anything, at VERBOSE. It carries what the
    // per-event lines cannot: how many slots one object interaction filled, and
    // how many of those were a merge into an existing stack rather than a new
    // slot. A frame with `acq 4` filled four slots; a frame with `countup 1` and
    // `acq 0` merged into one.
    if (verb >= 2u && (s_acq_frame + s_count_ups_frame + s_rerolls_frame + s_uses_frame) != 0u) {
        log_event("frame acq countup reroll", total, s_acq_frame, s_count_ups_frame,
                  s_rerolls_frame);
    }
    // The +0x02 corrections are reported separately, because the four-field
    // frame line is already carrying the acquisition counts and a scan line
    // must not be mistaken for one.
    if (verb >= 2u && (s_uses_frame != 0u || s_uses_skipped != 0u)) {
        log_event("frame uses fixed stale skipped", total, s_uses_frame, s_uses_stale,
                  s_uses_skipped);
    }
    s_uses_frame = 0u;
    s_log_events = 0u;
}

// --- the source tables -----------------------------------------------------

static unsigned short s_drop_written[DROP_ENTRY_MAX * 3u];
static unsigned char s_drop_valid[DROP_ENTRY_MAX * 3u];
static unsigned char s_drop_present;


// True when record 7 (the mission bank) is the resident bank in this RAM
// window. The window is shared: unit X (ROM 0x22A250 -> RAM 0x801E6FD0) and
// record 10b (ROM 0x23B1F0) hold code at these addresses when they are
// resident, so a write without this check corrupts another module.
static int drop_table_present(void) {
    if (rd16(DROP_TABLE) != DROP_TYPE_FIRST) {
        return 0;
    }
    return rd16(DROP_LAST_TYPE_ADDR) == DROP_TYPE_LAST;
}

// The item ids of entry 0 have to look like ids as well, or the fingerprint
// matched by accident and the window is left alone.
static int drop_table_readable(void) {
    unsigned int column;

    for (column = 0u; column < 3u; column++) {
        unsigned int id = rd16(DROP_TABLE + DROP_ITEM_A + column * 2u) & 0x7FFFu;

        if (id != 0u && id > ITEM_ID_MAX) {
            return 0;
        }
    }
    return 1;
}

static void drop_slot(unsigned int index, unsigned int raw, unsigned int mode, unsigned int verb) {
    unsigned int id = raw & 0x7FFFu;
    unsigned int replacement;

    // Record the raw value as handled whatever happens next, so the walk skips
    // this slot until the game itself changes it again.
    s_drop_written[index] = (unsigned short)raw;
    s_drop_valid[index] = 1u;

    if (id < 1u || id > ITEM_ID_MAX) {
        return;
    }
    replacement = replacement_for(id, mode, 0u, NO_SLOT);
    if (replacement != id) {
        unsigned int written = (raw & 0x8000u) | replacement;

        wr16(DROP_TABLE + (index / 3u) * DROP_ENTRY_STRIDE + DROP_ITEM_A + (index % 3u) * 2u,
             (unsigned short)written);
        s_drop_written[index] = (unsigned short)written;
        if (log_want(verb)) {
            log_event("drop-table", id, replacement, index, 0u);
        }
    }
}

// --- the per-map ground-item list ------------------------------------------
//
// `func_ovlR_80215C38` (bankR record 9d, RAM 0x80214FA0) parses the map's item
// buffer at load: a u8 count to 0x801F1002, then u16 ids to 0x801F1004 + i*2.
// `func_ovlN_801B9694` (bankN record 7) serves a pickup with
// `idx = lw(0x801F0E24)`, reads `lhu(0x801F1004 + idx*2)`, writes it to the
// popup field 0x801F36C2 (0x801BB8FC) and passes the same value to the grant at
// 0x801BB930. One value feeds both, so a rewrite here makes the message and the
// inventory agree, which the list watch cannot do on its own.
//
// 0x801F1002 is in record 7's BSS and inside unit X's DMA window, so the same
// record-7 fingerprint gates the write. An earlier build wrote this array as
// 32-bit words (`0x801F1004[index]`), which an id two bytes away from the right
// one, and the developer saw a pickup with no name and garbage icon pixels.
#define PICKUP_COUNT 0x801F1002u
#define PICKUP_IDS 0x801F1004u

// The pickup save's map holds seven items. A count above this is not a map's
// list, so the walk stops rather than indexing past the shadow columns.
#define PICKUP_MAX 64u

static unsigned short s_pickup_written[PICKUP_MAX];
static unsigned char s_pickup_valid[PICKUP_MAX];

// --- the pending battle-reward queue ---------------------------------------
//
// `0x801936D8` is the first of 20 `u16` that the mission bank serves a battle
// reward from. `func_ovlN_801E49D0` takes `lhu(0x801936D8)` at `0x801E5230`
// with `lhu(0x801936DA)` as the fallback (`0x801E5254`), stores it to
// `0x8021C760`, grants it at `0x801E53DC` and clears `0x801936D8` at
// `0x801E53F4`. The reward message is built from the same stored word, so a
// rewrite here makes the message and the inventory agree.
//
// This is the only place a battle reward can be replaced. The id is in RAM
// before the grant; the drop table is not read at that moment. Verified on the
// developer's save: with the drop table's entry 10 column C rewritten 155 -> 17,
// the queue still held `0x809B` (155) and the reward granted 155, which the
// party already owned seven of, so it merged into that stack and the mod's list
// watch had nothing to rewrite.
//
// The queue is a save field (20 `u16` copied by `func_80185128`), so a rewrite
// is persisted in the player's battery save. A non-zero word keeps bit 15, which
// selects the list, and only the id changes.
#define REWARD_QUEUE 0x801936D8u
#define REWARD_SLOTS 20u

static unsigned short s_reward_written[REWARD_SLOTS];
static unsigned char s_reward_valid[REWARD_SLOTS];

// --- the roster scan -------------------------------------------------------
//
// `func_8016B774` (0x8016B774, `.streamedB`, size 0x29C) is the game's own
// rebuild of the `+0x02` byte of both owned-item lists from the roster. It is
// the definition of the byte the item screen prints and the equip test reads
// (`sltu(+0x02, +0x03)`), and nothing else recomputes it while a screen is
// open. A mod cannot call it: `jal` from the mod's own `0x81000000` to game
// code fails `relocation truncated to fit: R_MIPS_26`, and a mod hook on it
// crashes the save load (`func_800749C0` reaches it and the regenerated body
// fails `get_function`). The scan below is a reimplementation, so the mod can
// write a record the game just filled with the count the game itself would
// have written:
//
//   clear both columns, then one increment per roster reference:
//   1. 10 byte ids per unit/battalion record for the 40-slot list,
//   2. the 4 stored u16 equipment ids per character record for the 278-slot
//      list, and
//   3. the 4 class-derived ids those same ids resolve to.
//
// Instruction-level source, `.streamedB`, with the register-relative stores
// resolved to absolute addresses:
//
//   clear  0x8016B798-0x8016B7A8  `sb zero,0x80193AE2 + i*4`, i = 39..0
//   clear  0x8016B7B4-0x8016B7C4  `sb zero,0x80196B22 + i*4`, i = 277..0
//   units  0x8016B7CC-0x8016B870  base 0x80197210, stride 25 (0x19), 30 records
//          0x8016B7DC `lbu v0,1(a3)` / `andi v0,v0,1` is the gate
//          0x8016B7F4 `lbu a2,13(v0)` with `v0 = a3 + i` is the id byte, i = 0..9
//          0x8016B810-0x8016B830 scans 0x80193AE0 for 40 records, stride 4
//          0x8016B844-0x8016B850 `lbu`/`addiu`/`sb ...,2(v0)` increments +0x02
//   chars  0x8016B874-0x8016B9E8  base 0x80193BE0, stride 56 (0x38), 100 records
//          0x8016B890 `lbu v0,17(s1)` is the gate
//          0x8016B8DC/8E8/8F4/900 `lhu a2,42/44/46/48(s1)` are the ids
//          0x8016B918-0x8016B934 scans 0x80196B20 for 278 records, stride 4
//          0x8016B948-0x8016B954 `lbu`/`addiu`/`sb ...,2(v0)` increments +0x02
//   class  0x8016B958-0x8016B9CC  for i = 0..3
//          0x8016B964 `lw v0,28660(at)`, `at = 0x80180000 + i*4` is the table at
//          0x80186FF4; `jalr v0` is called with `a0 = lbu 17(s1)` (the class
//          byte) and `a1 = lbu 18(s1)`. Each of the four accessors returns
//          `lhu(table + (a1*0x48 + 0x62 + i*2))` when the class table's byte
//          at `+0x79` equals `a0`, and the `a0` entry of the same table
//          otherwise (`func_8016DFA8` 0x8016DFA8, `DFFC`, `E050`, `E0A4`).
//          A non-zero result is searched in 0x80196B20 as above.
//
// The scan is the whole roster, so it is made once per frame at most, and only
// on a frame that is about to write a record. `s_ref_ready` says the arrays
// below describe the current frame.
#define ROSTER_UNITS 0x80197210u
#define ROSTER_UNIT_STRIDE 0x19u
#define ROSTER_UNIT_COUNT 30u
#define ROSTER_UNIT_GATE 0x01u
#define ROSTER_UNIT_IDS 0x0Du
#define ROSTER_UNIT_ID_COUNT 10u

#define ROSTER_CHARS 0x80193BE0u
#define ROSTER_CHAR_STRIDE 0x38u
#define ROSTER_CHAR_COUNT 100u
#define ROSTER_CHAR_GATE 0x11u
#define ROSTER_CHAR_CLASS 0x12u
#define ROSTER_CHAR_IDS 0x2Au

// The first character record's first equipment id. `lhu` at `+0x2A` equals the
// id byte at `+0x2B` because the high byte is 0, which is why both readings
// reproduce the game's column.
#define ROSTER_CHARS_FIRST_ID 0x2Au

// The class table and the class byte it is compared against: stride 0x48, the
// four ids at +0x62, the class byte at +0x79.
#define ROSTER_CLASS_TABLE 0x80187C62u
#define ROSTER_CLASS_STRIDE 0x48u
#define ROSTER_CLASS_IDS 0x00u
#define ROSTER_CLASS_KEY 0x17u

static unsigned short s_ref[2][LIST_EQUIP_SLOTS];
static unsigned char s_ref_ready;

// One entry of the class table at 0x80187C62: entry `a0` when its key byte
// equals `a0`, entry `a1` otherwise. `slot` is 0..3 and picks the halfword.
static unsigned int class_equipment_id(unsigned int class_index,
                                      unsigned int cls, unsigned int slot) {
    unsigned int table = ROSTER_CLASS_TABLE + class_index * ROSTER_CLASS_STRIDE;
    unsigned int entry = table;

    if (rd8(table + ROSTER_CLASS_KEY) != (cls & 0xFFu)) {
        entry = ROSTER_CLASS_TABLE + cls * ROSTER_CLASS_STRIDE;
    }
    return rd16(entry + ROSTER_CLASS_IDS + slot * 2u) & 0xFFFFu;
}

// One increment into a list's reference column. The id is a record index in
// that list, and the bounds are the game's own scan bounds (`slti v1,40` and
// `slti v1,278`), so the 511 the game returns for "absent" counts nothing.
// Zero is the game's empty id and is skipped by the scan.
static void ref_add(unsigned int list_index, unsigned int id) {
    if (list_index == 0u) {
        if (id != 0u && id < ITEM_SLOTS) {
            s_ref[0][id]++;
        }
    } else if (id != 0u && id < LIST_CONSUMABLE_SLOTS) {
        s_ref[1][id]++;
    }
}

// The 40-slot list: 10 byte ids per unit/battalion record.
static void roster_scan_units(void) {
    unsigned int record;

    for (record = 0u; record < ROSTER_UNIT_COUNT; record++) {
        unsigned int base = ROSTER_UNITS + record * ROSTER_UNIT_STRIDE;
        unsigned int i;

        if ((rd8(base + ROSTER_UNIT_GATE) & 0x01u) == 0u) {
            continue;
        }
        for (i = 0u; i < ROSTER_UNIT_ID_COUNT; i++) {
            ref_add(1u, rd8(base + ROSTER_UNIT_IDS + i));
        }
    }
}

// The 278-slot list: the four stored equipment ids of every character record,
// then the four class-derived ids for the same record.
static void roster_scan(void) {
    unsigned int record;
    unsigned int list_index;
    unsigned int slot;

    for (list_index = 0u; list_index < 2u; list_index++) {
        for (slot = 0u; slot < LIST_EQUIP_SLOTS; slot++) {
            s_ref[list_index][slot] = 0u;
        }
    }

    roster_scan_units();

    for (record = 0u; record < ROSTER_CHAR_COUNT; record++) {
        unsigned int base = ROSTER_CHARS + record * ROSTER_CHAR_STRIDE;
        unsigned int cls;
        unsigned int i;

        if (rd8(base + ROSTER_CHAR_GATE) == 0u) {
            continue;
        }
        cls = rd8(base + ROSTER_CHAR_CLASS);
        for (i = 0u; i < 4u; i++) {
            ref_add(0u, rd16(base + ROSTER_CHAR_IDS + i * 2u) & 0xFFFFu);
        }
        for (i = 0u; i < 4u; i++) {
            ref_add(0u, class_equipment_id(i, cls, i));
        }
    }
    s_roster_scans++;
    s_ref_ready = 1u;
}

// Recomputes the columns for this frame if a writer needs them.
static void roster_require(void) {
    if (!s_ref_ready) {
        roster_scan();
    }
}

// The scan's count for a record. The array index is the record index in the
// list; the loop bound matches the game's (`slti v1,278` / `slti v1,40`), so
// the sentinel the game uses for "absent" (511) is out of range and counts
// nothing.
static unsigned int ref_count(unsigned int list_index, unsigned int slot) {
    if (slot >= LIST_EQUIP_SLOTS) {
        return 0u;
    }
    return s_ref[list_index][slot];
}

// True only once the roster the scan reads is in RAM. The two structures are
// filled at different times by the save loader, not together: measured, the
// equipment list is already seated while the character records are still
// empty, and a scan then returns `ref 0` where the game has `2`. A single set
// gate byte is not enough either -- one stray byte made an earlier version of
// this return true on an otherwise empty roster, and the sweep then reported 15
// mismatches out of 25.
//
// A live character and a live unit record are both required, which is the
// weakest condition that distinguishes "loaded" from "one stray byte". The
// correction uses this one; the sweep wants a bigger margin before it asserts,
// which is `ROSTER_SWEEP_CHARS` below.
static unsigned int roster_chars(void) {
    unsigned int record;
    unsigned int chars = 0u;

    for (record = 0u; record < ROSTER_CHAR_COUNT; record++) {
        if (rd8(ROSTER_CHARS + record * ROSTER_CHAR_STRIDE + ROSTER_CHAR_GATE) != 0u) {
            chars++;
        }
    }
    return chars;
}

static unsigned int roster_units(void) {
    unsigned int record;
    unsigned int units = 0u;

    for (record = 0u; record < ROSTER_UNIT_COUNT; record++) {
        if (rd8(ROSTER_UNITS + record * ROSTER_UNIT_STRIDE + ROSTER_UNIT_GATE) != 0u) {
            units++;
        }
    }
    return units;
}

static int roster_populated(void) {
    if (roster_chars() == 0u) {
        return 0;
    }
    return roster_units() != 0u;
}


// Writes the correct `+0x02` for a record this mod has just changed the id of,
// and for a record that was empty and has just been filled. The byte is what
// the item screen prints first and what the equip test compares, so a stale
// value makes the new item read `1/1` and refuse every equip.
//
// The record is only touched while it still holds the id the caller wrote, so
// a record the game has since changed is left alone. `+0x02` is recomputed
// over the whole roster, so it is also correct when the replacement is an item
// a unit genuinely references, which is the case that forbids a blind clear.
static void fix_record_use(struct item_list* list, unsigned int list_index,
                           unsigned int slot, unsigned int id) {
    unsigned int base = list->base + slot * LIST_STRIDE;
    unsigned int use;
    unsigned int want;

    if (slot >= list->slots || id == 0u) {
        return;
    }
    // A roster that is not in RAM yet counts nothing, so a correction now would
    // write a zero that is not the game's answer. Leave the byte alone and say
    // so: the next acquisition corrects it.
    if (!s_roster_ok) {
        s_uses_skipped++;
        return;
    }

    use = rd8(base + LIST_USE_OFFSET);
    want = ref_count(list_index, slot);
    if (use == want) {
        return;
    }

    // The stale byte the grant routine inherited from an emptied record: a
    // record that was empty had no reference, so the rebuild would clear it.
    if (use != 0u && want == 0u) {
        s_uses_stale++;
    }
    s_uses_fixed++;
    s_uses_frame++;
    wr8(base + LIST_USE_OFFSET, (unsigned char)want);
    if (log_want(2u)) {
        log_event("use", id, slot, use, want);
    }
}

// Applies the correction to every record this frame's walk wrote an id into.
// The scan is built once per frame, here, and not per record: it is the whole
// roster (30 unit records, 100 character records, two class lookups each).
static void fix_list_uses(unsigned int list_index) {
    struct item_list* list = &s_lists[list_index];
    unsigned int i;

    if (list->fix_count == 0u) {
        return;
    }
    s_roster_ok = (unsigned char)roster_populated();
    roster_require();
    for (i = 0u; i < list->fix_count; i++) {
        unsigned int slot = list->fix_slot[i];

        if (rd16(list->base + slot * LIST_STRIDE) == list->fix_id[i]) {
            fix_record_use(list, list_index, slot, list->fix_id[i]);
        }
    }
    list->fix_count = 0u;
}

// Walks the array every frame, the same way as the drop table: a value that
// still equals what the mod wrote costs two loads, and a map reload restores
// the ROM values, which are derived again through the memo.
static void randomize_pickup_table(unsigned int mode, unsigned int verb) {
    unsigned int count = rd8(PICKUP_COUNT);
    unsigned int i;

    if (count > PICKUP_MAX) {
        // Not this array: a count that large means another bank owns the window.
        return;
    }
    for (i = 0u; i < count; i++) {
        unsigned int raw = rd16(PICKUP_IDS + i * 2u);
        unsigned int id = raw & 0x7FFFu;
        unsigned int list_index = (raw & 0x8000u) ? 0u : 1u;
        unsigned int replacement;

        if (s_pickup_valid[i] && raw == (unsigned int)s_pickup_written[i]) {
            continue;
        }
        s_pickup_valid[i] = 1u;
        s_pickup_written[i] = (unsigned short)raw;
        if (id < 1u || !slot_id_valid(&s_lists[list_index], id) ||
            !slot_rerollable(&s_lists[list_index], id)) {
            continue;
        }
        replacement = replacement_for(id, mode, list_index, NO_SLOT);
        if (replacement != id) {
            unsigned int written = (raw & 0x8000u) | replacement;

            wr16(PICKUP_IDS + i * 2u, (unsigned short)written);
            s_pickup_written[i] = (unsigned short)written;
                if (log_want(verb)) {
                log_event("pickup-table", id, replacement, i, 0u);
            }
        }
    }
}

// Walks table O every frame. An entry whose raw value still equals what the mod
// wrote costs two loads and is skipped. After a map reload the game restores
// the ROM values, so those entries are derived again through the memo and land
// on the same replacements.
static void randomize_drop_table(unsigned int mode, unsigned int verb) {
    unsigned int entry;
    unsigned int column;

    if (!drop_table_present()) {
        // Not record 7's window: another mission bank is resident, so there is
        // no table to rewrite. Say so once per arrival, or a run in the
        // tutorial (scene 0x17, records 18/20/21) looks like a broken mod
        // rather than an out-of-scope map.
        if (s_drop_present) {
            s_drop_present = 0u;
            if (verb != 0u) {
                log_event("drop-table absent", rd16(DROP_TABLE), rd16(DROP_LAST_TYPE_ADDR), 0u, 0u);
            }
        }
        return;
    }
    if (!s_drop_present && !drop_table_readable()) {
        return;
    }
    s_drop_present = 1u;

    for (entry = 0u; entry < DROP_ENTRY_MAX; entry++) {
        if (rd16(DROP_TABLE + entry * DROP_ENTRY_STRIDE) == 0u) {
            break;
        }
        for (column = 0u; column < 3u; column++) {
            unsigned int index = entry * 3u + column;
            unsigned int raw = rd16(DROP_TABLE + entry * DROP_ENTRY_STRIDE + DROP_ITEM_A + column * 2u);

            if (s_drop_valid[index] && raw == (unsigned int)s_drop_written[index]) {
                continue;
            }
            drop_slot(index, raw, mode, verb);
        }
    }
}

// Walks the queue every frame, the same way as the two tables: a value that
// still equals what the mod wrote costs two loads, and a value the game restores
// is derived again through the memo.
static void randomize_reward_queue(unsigned int mode, unsigned int verb) {
    unsigned int i;

    for (i = 0u; i < REWARD_SLOTS; i++) {
        unsigned int addr = REWARD_QUEUE + i * 2u;
        unsigned int raw = rd16(addr);
        unsigned int id = raw & 0x7FFFu;
        unsigned int list_index = (raw & 0x8000u) ? 0u : 1u;
        unsigned int replacement;

        if (raw == 0u) {
            s_reward_valid[i] = 0u;
            s_reward_written[i] = 0u;
            continue;
        }
        if (s_reward_valid[i] && raw == (unsigned int)s_reward_written[i]) {
            continue;
        }
        s_reward_valid[i] = 1u;
        s_reward_written[i] = (unsigned short)raw;
        // A value this mod produced is already the intended reward. Rerolling it
        // would map it back to the original, the same fight the list watch has.
        if (s_memo_target[list_index][id]) {
            continue;
        }
        if (id < 1u || !slot_id_valid(&s_lists[list_index], id) ||
            !slot_rerollable(&s_lists[list_index], id)) {
            continue;
        }
        replacement = replacement_for(id, mode, list_index, NO_SLOT);
        if (replacement != id) {
            unsigned int written = (raw & 0x8000u) | replacement;

            wr16(addr, (unsigned short)written);
            s_reward_written[i] = (unsigned short)written;
            // The grant of this id is the intended outcome, so the list watch
            // leaves the inventory record it lands in alone.
                if (log_want(verb)) {
                log_event("reward-queue", id, replacement, i, 0u);
            }
        }
    }
}

// --- the list fallback -----------------------------------------------------
//
// Two owned-item lists exist, and the grant routine chooses between them with
// bit 15 of the item word (func_ovlN_801DC740): set -> the 278-slot equipment
// list at 0x80196B20, clear -> the 40-slot list at 0x80193AE0. They have
// separate definition tables, separate id spaces and separate counts, and the
// 40-slot table is where the consumables live: its id 1 is "Heal Leaf"
// (RAM 0x8018E6EC, name pointer at +0x00, stride 0x0C). A healing item therefore
// can never appear in the equipment table, which is why watching only
// 0x80196B20 missed grants like the tutorial's Heal Leaf.

// Set when the item list has changed as a whole rather than by a grant, so the
// next tick re-primes both shadows and rerolls nothing.
//
// `func_800749C0` is the save-field reader: it walks the table at 0x800A824C and
// unpacks each field, and entry 1 is the packed blob that carries both item
// lists (`func_8016CEC4`, which calls the used-count rebuild at 0x8016CF30). It
// is `.main` code, so a hook can bind to it. The used-count rebuild
// `func_8016B774` would be the more precise signal but a hook on it breaks the
// save load (`get_function` fails; see `mods/exp-overflow/src/exp_overflow.c`).
//
// A count of the records that appeared in one frame is the fallback for a signal
// this misses. It is not enough on its own: the 40-slot list holds four records
// in total in the pickup save, so a load looked like four grants.
static unsigned char s_force_prime = 1u;
static unsigned char s_save_read;

RECOMP_HOOK("func_800749C0")
void item_randomizer_after_save_read(void) {
    s_save_read = 1u;
    // Logged from the hook, because the next tick's re-prime line cannot say
    // whether it was this signal or the record count that fired.
    recomp_log("save-read");
}

RECOMP_HOOK("func_80072944")
void item_randomizer_tick(void) {
    unsigned int mode;
    unsigned int chance;
    unsigned int allow_shops;
    unsigned int verb;
    unsigned int slot;
    unsigned int gold_now = rd32(GOLD);
    static unsigned int gold_prev;
    static unsigned int gold_primed;
    int purchase_frame = 0;

    if (s_rng == 0u) {
        unsigned int seed = config_u32("seed");
        if (seed == 0u) {
            seed = 0x2545F491u ^ (rd32(0x800AEFA4u) * 2654435761u);
        }
        s_seed_value = seed;
        s_rng = seed | 1u;
    }

    mode = config_u32("mode");
    chance = config_u32("chance");
    allow_shops = config_u32("shops");
    verb = config_u32("log");

    s_frames++;
    log_reset();
    // The roster scan describes one frame. Its result is dropped here and
    // rebuilt on demand, because a roster entry can change between frames
    // (equip, unequip, class change) and the count must not be stale when the
    // mod writes it.
    s_ref_ready = 0u;
    // A frame can abort the list walk (a re-prime) with slots recorded from an
    // earlier pass. The records are re-derived on the next acquisition, so the
    // stale list is dropped rather than applied.
    s_lists[0].fix_count = 0u;
    s_lists[1].fix_count = 0u;
    // Unconditional once: a run must always say the mod is alive and which
    // settings it resolved, or a silent log is ambiguous between "the hook
    // never ran", "the option read as 0" and "nothing happened".
    {
        static unsigned int announced;

        if (!announced) {
            announced = 1u;
            log_event("start verb mode shops", verb, mode, allow_shops, 0u);
            log_event("start chance seed", chance, s_seed_value, 0u, 0u);
            log_event("start droptable first last", rd16(DROP_TABLE), rd16(DROP_LAST_TYPE_ADDR), 0u, 0u);
            // The inputs of the `+0x02` scan: how many character and unit
            // records are loaded at boot, which is 0 until the save is read.
            log_event("start roster chars units", roster_chars(), roster_units(), 0u, 0u);
        }
    }

    // The A/B of the scan against the game's own column, once per session and
    // at NORMAL as well as VERBOSE. It runs before the table walks because
    // those can emit enough lines to push it past the per-frame budget, and its
    // result is what says the reimplementation reproduces `func_8016B774`.
    if (gold_primed) {
        // Gold falls on a purchase. Primed on the first frame, because a save
        // load moves gold wholesale.
        if (gold_now + 1u < gold_prev) {
            purchase_frame = 1;
        }
    } else {
        gold_primed = 1u;
    }
    gold_prev = gold_now;

    // The source tables first: the popup and the grant both read them, so a
    // replacement here is what makes the message and the inventory agree.
    randomize_drop_table(mode, verb);
    // The per-map ground-item list is a second window in record 7's image, so it
    // is gated on the same fingerprint.
    if (drop_table_present()) {
        randomize_pickup_table(mode, verb);
    }

    // A probe for the one question a title-screen run cannot answer: does the
    // drop table ever look like record 7? It watches the two fingerprint words
    // and the map count, and reports every change (and once every 600 frames
    // while a mission is up), bounded by the same log budget.
    {
        static unsigned short last_first;
        static unsigned short last_last;
        static unsigned int probe_frame;
        unsigned short first = rd16(DROP_TABLE);
        unsigned short last = rd16(DROP_LAST_TYPE_ADDR);

        if (first != last_first || last != last_last || (s_frames - probe_frame) >= 600u) {
            last_first = first;
            last_last = last;
            probe_frame = s_frames;
            if (log_want(verb)) {
                // 39 80 (0x27, 0x50) means record 7 is resident and the drop
                // table is at DROP_TABLE. Anything else means another bank owns
                // that window, and the rewriter is deliberately doing nothing.
                log_event("probe first last present", first, last,
                          drop_table_present(), 0u);
            }
        }
    }

    // The list fallback runs always, and it is what guarantees "one find, one
    // item". Only a slot that was EMPTY and now holds an item counts, so a game
    // that re-sorts the list cannot cause a reroll, and an acquisition the drop
    // table already changed is rerolled at most once more -- through the same
    // per-list memo, so it stays the same replacement rather than drifting.
    // The reward queue and the list walk share a second log budget. The two
    // table walks run first and can print an event per entry on a frame where
    // the game has just restored the ROM values, which used to hide every
    // acquisition, reward-queue and re-prime line behind them. Total output per
    // frame stays bounded at two budgets.
    s_log_left = LOG_BUDGET;
    s_log_events = 0u;

    // The reward queue is plain RAM, not a bank window, so it needs no
    // fingerprint. It is the path a battle reward takes.
    randomize_reward_queue(mode, verb);

    {
        unsigned int list_index;
        unsigned int equipment_reload = 0u;
        unsigned int forced = s_force_prime;

        s_force_prime = 0u;
        if (s_save_read) {
            s_save_read = 0u;
            forced = 1u;
        }

        for (list_index = 0u; list_index < 2u; list_index++) {
            struct item_list* list = &s_lists[list_index];
            unsigned int changed = 0u;

            // The slots this walk writes an id into. They are corrected after
            // the walk, because the correction needs the whole-roster scan.
            list->fix_count = 0u;

            // Pass 1: how many records were filled that were empty.
            if (list->primed) {
                for (slot = 0u; slot < list->slots; slot++) {
                    unsigned int id = rd16(list->base + slot * LIST_STRIDE);

                    if (id != 0u && list->shadow_id[slot] == 0u) {
                        changed++;
                    }
                }
            }

            // A wholesale replacement (a save load) is not a set of
            // acquisitions: re-prime instead of rerolling the lot. Two signals
            // say so: the game's own used-count rebuild has run since the last
            // tick, or more records were filled in one frame than a grant can
            // fill. The count alone is not enough for the 40-slot list, whose
            // whole contents are four records in an early save.
            // The equipment list is the load signal for both: one save blob
            // carries both lists, and its 278 records make a load stand out
            // where the 40-slot list's four records do not.
            if (forced || !list->primed || changed > RELOAD_SLOTS ||
                (list_index == 1u && equipment_reload)) {
                for (slot = 0u; slot < list->slots; slot++) {
                    unsigned int base = list->base + slot * LIST_STRIDE;

                    list->shadow_id[slot] = rd16(base);
                    list->shadow_count[slot] = rd8(base + LIST_COUNT_OFFSET);
                }
                if (list->primed) {
                    s_reloads++;
                    if (log_want(verb)) {
                        log_event("re-prime", changed, s_reloads, list_index, forced);
                    }
                }
                list->primed = 1u;
                list->fix_count = 0u;
                if (list_index == 0u && changed > RELOAD_SLOTS) {
                    equipment_reload = 1u;
                }
                continue;
            }

            // Pass 2: a slot that was empty and now holds an item is a new
            // acquisition. A slot that merely changed its id is the game
            // re-sorting the list, and is left alone. A slot whose count grew
            // was a merge into a stack the party already owned, which the id
            // column cannot see.
            for (slot = 0u; slot < list->slots; slot++) {
                unsigned int base = list->base + slot * LIST_STRIDE;
                unsigned int id = rd16(base);
                unsigned int use = rd8(base + LIST_USE_OFFSET);
                unsigned int count = rd8(base + LIST_COUNT_OFFSET);
                int acquired = 0;

                if (id != 0u && list->shadow_id[slot] == 0u) {
                    acquired = 1;
                }

                if (id != 0u && !acquired && count > (unsigned int)list->shadow_count[slot]) {
                    s_count_ups++;
                    s_count_ups_frame++;
                    if (verb >= 2u && log_want(verb)) {
                        log_event("count-up", id, slot, list->shadow_count[slot], count);
                    }
                }

                // The scan's count for this record beside the byte the game
                // has there. The correction writes only when they differ, so
                // this line is what makes a false mismatch visible: `ref`
                // against `game` disagreeing here is the scan reading a roster
                // that is not in RAM, and it needs no gate to be observed.
                if (acquired && list_index == 0u && verb >= 2u && log_want(verb)) {
                    unsigned int rc;

                    roster_require();
                    rc = ref_count(0u, slot);
                    if (rc == use) {
                        log_event("acquire ref game", id, rc, use, roster_chars());
                    } else {
                        log_event("acquire ref game DIFF", id, rc, use, roster_chars());
                    }
                }

                if (acquired) {
                    s_acquisitions++;
                    s_acq_frame++;
                    if (verb >= 2u && log_want(verb)) {
                        // The third field is the record's +0x02, the used count
                        // the game displays and tests. A non-zero value on a
                        // record that was empty is the stale byte the grant
                        // routine leaves behind; see the func_8016B774 note at
                        // the top of this file.
                        log_event(list->acquire_tag, id, slot, use, count);
                    }
                }

                if (acquired && slot_id_valid(list, id)) {
                    if (s_memo_target[list_index][id]) {
                        // The id is one this mod put into a source table, so the
                        // game granting it is the intended outcome. Rerolling it
                        // here would map it back to the original and make the
                        // inventory disagree with the popup.
                        s_table_grants++;
                        if (verb >= 2u && log_want(verb)) {
                            log_event("already-replaced", id, slot, list_index, count);
                        }
                    } else if (!slot_rerollable(list, id)) {
                        s_quests_kept++;
                        if (verb >= 2u && log_want(verb)) {
                            log_event("keep-quest", id, slot, slot_class(list, id), count);
                        }
                    } else if (purchase_frame && !allow_shops) {
                        s_purchases_skipped++;
                    } else if (chance >= 100u || (rng_next() % 100u) < chance) {
                        unsigned int duplicated = 0u;
                        unsigned int replacement =
                            replacement_for_slot(id, mode, list_index, slot, &duplicated);

                        if (replacement != id) {
                            // Reported when the session mapping would have put
                            // this id into a record the list already holds, which
                            // splits the game's used count across two records.
                            if (duplicated != 0u && log_want(verb)) {
                                log_event("dup-fix", id, duplicated, replacement, list_index);
                            }
                            wr16(base, (unsigned short)replacement);
                            if (log_want(verb)) {
                                log_event(list->tag, id, replacement, slot, list_index);
                            }
                            id = replacement;
                            s_rerolls++;
                            s_rerolls_frame++;
                        }
                    }
                }

                // A slot that was empty and now holds an item is the case the
                // stale byte is visible in: `func_ovlN_801DC740` wrote the id
                // and `+0x03` and left `+0x02` as the emptied record carried
                // it, so the item screen prints `1/1` under the new name and
                // the equip test refuses. Record the slot and correct it after
                // the walk, with the count `func_8016B774` would have written.
                //
                // This is recorded for EVERY acquisition and not only for one
                // the mod rerolled: the game leaves the stale byte on the
                // vanilla item too, and the correction of the mod's own write
                // is what the `acquire` line's third field is checked against.
                if (acquired && list->fix_count < list->slots) {
                    list->fix_slot[list->fix_count] = (unsigned short)slot;
                    list->fix_id[list->fix_count] = (unsigned short)id;
                    list->fix_count++;
                }

                list->shadow_id[slot] = (unsigned short)id;
                list->shadow_count[slot] = (unsigned char)count;
            }

            fix_list_uses(list_index);
        }
    }

    log_summary(verb, s_frames);
}
