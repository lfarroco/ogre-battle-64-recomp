// Skip Boot Logos - an example mod for Ogre Battle 64: Recomp.
//
// The boot runs three scenes before anything else:
//
//   scene 0x09  the boot intro (soldiers, a falling cube, the N64 logo), ~9.4 s
//   scene 0x0A  the publisher stills (Licensed by Nintendo / ATLUS / QUEST),
//               ~16.3 s
//   scene 0x04  the title, with its prologue text over the scrolling clouds
//
// The game's own save is read before the first scene (the log's `[save] ...`
// line), so the title is reached with the battery already loaded.
//
// Each scene's per-frame update writes the next scene into the pending-scene
// word `D_800C4C26` as `0x8000 | id` when that scene is finished, and the scene
// dispatcher switches on the next frame. This mod places an entry hook on scene
// 0x09's update (`func_80177DCC`) and writes the title's id itself, so the boot
// goes straight to the title.
//
// **Scene 0x0A must not be entered.** Setting the word from scene 0x0A's update
// instead (a first attempt) cut the stills to one frame, and the title then
// produced no display lists at all and presented a stale frame: 0x0A's enter
// and leave set up and tear down globals in overlay C that its *completed*
// state machine is what leaves consistent, and a half-run scene leaves the
// title's state invalid. Jumping from 0x09 to 0x04 never runs 0x0A, and the
// title renders correctly (verified by capture).
//
// Descriptors and addresses are in `docs/scenes.md`; `tools/scenemap.py scenes`
// reprints the descriptor table from the ROM.

#include "modding.h"

// The scene dispatcher's pending-scene word (`docs/symbols.md`, `D_800C4C26`).
#define PENDING_SCENE (*(volatile unsigned short*)0x800C4C26)

// `0x8000 | scene id`: scene 0x04 is the title, descriptor `0x8018FB40`.
#define SCENE_TITLE 0x8004

RECOMP_HOOK("func_80177DCC")
void skip_to_title(void) {
    PENDING_SCENE = SCENE_TITLE;
}
