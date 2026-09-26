// EXP overflow - an Ogre Battle 64: Recomp code mod.
//
// A character levels up when their EXP reaches 100. The game's battle-result
// code adds the gained EXP to the battle participant, and when the total is
// 100 or more it zeroes the participant's EXP and levels the character up. The
// remainder is lost: a character at 90 EXP who gains 30 ends the fight at
// level +1 with 0 EXP instead of 20.
//
// This mod keeps the remainder. The battle-result code itself cannot be hooked:
// it lives in streamed bank unit J (record 10's arena, `func_ovlJ_801D61C0` at
// 0x801D61C0), and the port's mod system only covers the base sections
// (`.entry`, `.main`, `.streamedA/B/C`) - a hook in a `app/src/bank_overlays.cpp`
// unit does not resolve (`docs/guides/app-build.md` -> "What a hook can reach").
//
// Neither can the level-up routine `func_8016EBC4` be hooked, even though it is
// in `.streamedB`: regenerating it makes the port look up a garbage function
// pointer during boot ("Failed to find function at 0x46220003", then a SIGSEGV
// in the game's main loop). Its neighbours (`func_8016E1FC`, `func_8016DE3C`)
// and the example mod's `func_80177DCC` hook fine, so that target is the
// problem, not streamed sections.
//
// What is left is a frame hook plus a deferred fix-up:
//
//   * `func_80072944` runs once per frame (measured: 3072 entries over a 3203
//     frame run) and regenerates cleanly. It is in `.main`, size 0x38, and is
//     one of the small per-frame functions the port's main loop calls.
//   * On each tick the battle-result state is scanned for participants whose
//     working EXP has reached 100, and `exp - 100` plus the participant's level
//     is remembered against the roster index.
//   * The result loop calls `func_ovlJ_801D8E90` (participant -> record), then
//     levels the record up, then `func_ovlJ_801D8DC4` (record -> participant).
//     Once the record shows `level + 1`, this mod writes the remembered
//     remainder into the record *and* into the still-live participant, because
//     a record-only write is undone by the next participant -> record sync. It
//     keeps both copies for a short window so a late sync cannot put the zero
//     back.
//
// Verified in a real battle (attached suspend save, Dio's unit): four
// front-row soldiers at 89 EXP gained enough to cross 100, reached level +1 and
// ended the result at 20 EXP, and 20 survived the next battle. The same run
// without the mod ends them at 0.
//
// Hook targets that were tried and rejected: `func_80075BC0` (the one-shot
// scene-table setup, not per frame; its regeneration also fails with "Failed to
// find function at 0x8007DA14"), `func_80089BC0` / `func_80072900` /
// `func_800728BC` (boot-only, ~13 entries in 25 s), `func_80072398` (per frame
// but the hook faults with SIGBUS), and `func_8016EBC4` itself (see above).
//
// Addresses, all read at instruction level:
//
//   character record n:  0x80193BE0 + n*0x38   (n = 1 is the first roster
//                         character; its name starts at 0x80193C18)
//     +0x13  level (byte)
//     +0x35  EXP   (byte)  <- the value the level-up check reads
//
//   battle result state: *(0x801CE8DC)          (bank unit J's data;
//                         `lui a1,0x801d; lw a1,-5924(a1)` in func_ovlC_801C9008)
//     +0x6048  result state (word)
//     participant n: + n*248 + 452, in use when the word at + n*248 + 524 is
//                    nonzero
//       +0x31  level      (byte)
//       +0x32  EXP        (byte)  <- 100 or more triggers a level up
//       +0xf6  roster index (byte)

#include "modding.h"

#define CHAR_TABLE 0x80193BE0u
#define CHAR_STRIDE 0x38u
#define CHAR_COUNT 100u
#define REC_LEVEL 0x13u
#define REC_EXP 0x35u

#define BATTLE_PTR 0x801CE8DCu
#define PART_STRIDE 248u
#define PART_OFF 452u
#define PART_VALID 524u
#define PART_LEVEL 0x31u
#define PART_EXP 0x32u
#define PART_CHARIDX 0xF6u

#define RDRAM_LO 0x80000000u
#define RDRAM_HI 0x80800000u
#define TICK_WINDOW 120u

static unsigned char s_overflow[CHAR_COUNT];
static unsigned char s_valid[CHAR_COUNT];
static unsigned char s_level[CHAR_COUNT];
static unsigned int s_frame[CHAR_COUNT];
static unsigned int s_tick;

static unsigned char rd8(unsigned int addr) {
    return *(volatile unsigned char *)addr;
}

static void wr8(unsigned int addr, unsigned char value) {
    *(volatile unsigned char *)addr = value;
}

static unsigned int rd32(unsigned int addr) {
    return *(volatile unsigned int *)addr;
}

static unsigned int record_of(unsigned int index) {
    return CHAR_TABLE + index * CHAR_STRIDE;
}

// The result loop zeroes the participant's EXP, syncs it into the record and
// only then levels up, so a record-only write is undone the next time the game
// syncs participant -> record. The participant struct is still live at that
// point (the result state is still present), so its copy is corrected too.
static void fix_participant(unsigned int index, unsigned char overflow) {
    unsigned int base;
    unsigned int i;

    base = rd32(BATTLE_PTR);
    if (base < RDRAM_LO || base >= RDRAM_HI) {
        return;
    }
    for (i = 0; i < 20u; i++) {
        unsigned int part = base + i * PART_STRIDE + PART_OFF;

        if (rd32(part - PART_OFF + PART_VALID) == 0u) {
            continue;
        }
        if (rd8(part + PART_CHARIDX) != index) {
            continue;
        }
        if (rd8(part + PART_EXP) != overflow) {
            wr8(part + PART_EXP, overflow);
        }
        return;
    }
}

// Runs once per frame. First remembers the EXP of any battle participant that
// has reached the level-up threshold, then fixes up a level up that the game has
// just performed.
RECOMP_HOOK("func_80072944")
void exp_overflow_tick(void) {
    unsigned int base;
    unsigned int i;

    s_tick++;

    base = rd32(BATTLE_PTR);
    if (base >= RDRAM_LO && base < RDRAM_HI) {
        for (i = 0; i < 20u; i++) {
            unsigned int slot = base + i * PART_STRIDE;
            unsigned int part;
            unsigned char exp;
            unsigned char index;

            if (rd32(slot + PART_VALID) == 0u) {
                continue;
            }
            part = slot + PART_OFF;
            exp = rd8(part + PART_EXP);
            if (exp < 100u) {
                continue;
            }
            index = rd8(part + PART_CHARIDX);
            if (index >= CHAR_COUNT) {
                continue;
            }
            s_overflow[index] = (unsigned char)(exp - 100u);
            s_level[index] = rd8(part + PART_LEVEL);
            s_frame[index] = s_tick;
            s_valid[index] = 1u;
        }
    }

    // The level-up sync has already zeroed both copies. Restore the remainder
    // as soon as the record shows the level it was levelled to, and hold it for
    // a short window so a later participant -> record sync cannot put the zero
    // back.
    for (i = 0; i < CHAR_COUNT; i++) {
        unsigned int record;

        if (!s_valid[i]) {
            continue;
        }
        if (s_tick - s_frame[i] > TICK_WINDOW) {
            s_valid[i] = 0u;
            continue;
        }
        record = record_of(i);
        if (rd8(record + REC_LEVEL) != (unsigned char)(s_level[i] + 1u)) {
            continue;
        }
        if (rd8(record + REC_EXP) != s_overflow[i]) {
            wr8(record + REC_EXP, s_overflow[i]);
        }
        fix_participant(i, s_overflow[i]);
    }
}
