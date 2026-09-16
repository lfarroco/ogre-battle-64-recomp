# Handoff — 2026-09-16, session 59: the end-of-sequence crash is a missing **third bank** of the record-14 arena; the chapter animation plays and the sequence runs on into scene `0x05`

## Goal and result

**Goal (developer):** keep moving — from the intro movie (`0x16`, which session 58
proved renders all five shots) into the **next scene**. The concrete wall was
session 58's end-of-sequence crash: `SIGBUS` in `func_ovlC_8022C270 + 0x53A` with
`D_8018F1C0 = 0x0431` (step 1073, the "Prologue" chapter animation).

**Result.**

1. **The crash is fixed, and it was the session-45/55 mis-binding class again.**
   The New Game sequence streams a **third module over RAM `0x8022ACB0`** for the
   step whose command is `-8` — the chapter animation — and the port had no code
   for it, while `bankRec14a` (the *other* bank of that RAM) was compiled into
   unit C. So the resident interpreter's `jal 0x8022C270` / `jal 0x8022C6E4` and
   the command-`-8` callback's `jal 0x8022E3F0` ran rec14a's bodies with the
   chapter module resident. Fixed with two new units: **K** = the chapter module
   (`bankRec14d`, ROM `0x286BA0`, 0x138F0) and **L** = `bankRec14a` (moved out of
   unit C). §1.
2. **`bankRec14a` was also truncated by 0xA00 bytes** — its segment ended at ROM
   `0x2A82F0` while the game DMAs to `0x2A8CF0`. The missing tail holds its jump
   tables; `make bank-recomp` failed with "Failed to determine size of jump table
   at 0x80239270" until the end marker was corrected. §1b.
3. **The chapter animation renders.** The "Prologue / Casting their gaze on the
   ground, trudging along…" card draws (proof
   `docs/proofs/native-newgame-prologue-card.png`), the sequence advances through
   it into the story that follows (General Godeslas receiving the soldiers —
   `native-newgame-received-for-duty.png`; the Grey-Haired Old Man and Magnus —
   `native-newgame-magnus-old-man.png`), and reaches **scene `0x05`**, with **0
   stub calls** and exit 0. §2.
4. **Scene `0x05`'s module is compiled too** (unit **M**): its enter
   `func_8017B60C` DMAs ROM `0x79750` (0xDAD0) → RAM `0x8019A7C0`, the *other*
   bank of unit H's scene-`0x07` form module. Before this, scene `0x05` called
   `0x8019A7C0` and hit the runtime's streamed stub. §3. One diagnostic bug that
   hid it is fixed in the same commit (§3b).
5. **Corrections to session 58** (§4): the engine/interpreter globals are at
   `0x8022A970`/`0x8022A978`/`0x8022A994` (**not** `0x8023A9xx` — the `lui
   0x8023` + negative-immediate addressing wraps), the crash dump has all three
   at **0**, `func_80227700` only stores the selector, and step 1073's *command*
   is `-8` (its last word's low byte 6 is the raw byte, not the command).

## 1. The crash: a missing third bank of the record-14 arena

### 1a. How the bank is chosen (instruction level)

The sequence step's *command* comes from the step descriptor's **last word**:

* `func_ovlC_80227E64` (0x80227E64) decompresses the step's asset and takes its
  last word; if the high byte is `0xFF`, the low byte (1..8) indexes the table at
  **`0x8022ABE0`** (`lw v0,-21536(at)` with `lui at,0x8023` — the immediate wraps
  to `0x8022ABE0`, §4a). The table maps 1..8 to `-3 -6 -5 -4 -7 -8 -9 -10`.
  Step 1073's last word is `0xFF000006` → byte 6 → **command `-8`**.
* `func_ovlC_8022683C` (0x8022683C) then indexes the table at **`0x8022ABA0`**
  with `command + 10` (`sltiu v0,a0,9` at 0x80226898). `-8 + 10 = 2` →
  **arm `0x802269DC`**.

That arm (verified in `build/bankC.elf`) loads the chapter module:

```
0x802269E4  a0 = 0x8022ACB0, a1 = 0x8023DE30   -> icache invalidate (code)
0x802269FC  a0 = 0x8023DE30, a1 = 0x8023E5A0   -> dcache invalidate (data)
0x80226A0C  a0 = 0x00286BA0, a1 = 0x8022ACB0
0x80226A28  a2 = 0x29A490 - 0x286BA0 = 0x138F0 -> func_8009DA50: DMA
0x80226A2C  bzero 0x8023E5A0 .. 0x8023E630 (bss)
0x80226A4C  func_ovlC_80227700(2)               -> D_8018FC39 = 2
0x80226A6C  register callback 0x80225C28 (step 0) / 0x80225F60 (step != 0)
```

The chapter module's own entry (`0x8022E3F0`, unit K) is the chapter-animation
setup: it `malloc(0x1CB8)`s the engine struct at `0x8022A994`, reads
`D_8018F1C0 & 0xFFF`, calls the **shared** step loader `func_80227FF8(step, 0)`
and the **shared** interpreter `func_802282D8`, then switches on
`(step - 1073) & 0xFFFF < 5` — steps **1073..1077 are the chapter animation's
five phases** (like the movie's 970..974).

### 1b. Why the port crashed, and the fix

The interpreter (`func_ovlC_802282D8`, in `bankRec14`, unit C) calls
`0x8022C270` (0x80228EC0) and `0x8022C6E4` (0x8022A190); the command-`-8`
callback `func_ovlC_80225F60` calls `0x8022E3F0` (0x80225FE4). While
`bankRec14a` was *defined* in unit C, N64Recomp bound those calls at build time
to rec14a's bodies. When the game DMA'd the chapter module over that RAM the
shared interpreter ran rec14a's layout there — a different program — leaving the
engine state at `0x8022A978` (base) and `0x8022A970` (pc) **0**, so the
interpreter entered with base/pc = 0, read guest `0` as its descriptor stream,
and was off in the weeds by the time it reached the opcode-`0x8000000A` handler
that passed the wild `$a1`.

Fix (the session-45/55 rule, applied to the remaining two banks):

* **`config-bankK.yaml`/`.toml` (new)** — `bankRec14d`: ROM `0x286BA0`
  (0x138F0) → RAM `0x8022ACB0`; code to ROM `0x299D20` (RAM `0x8023DE30`), data
  to the end, no bss (the enter's bzero is skipped). 136 functions.
* **`config-bankL.yaml`/`.toml` (new)** — `bankRec14a`: ROM `0x29A490`
  (**0xE860**, see below) → RAM `0x8022ACB0`. 151 functions. The session-39
  `function_sizes` override for `func_ovlC_8022EA08` moved here as
  `func_ovlL_8022EA08`.
* **`config-bankC.yaml`/`.toml`** — `bankRec14a` removed; the ROM gap is one
  `bin` again. Unit C's calls into 0x8022ACB0+ now compile as `LOOKUP_FUNC`
  (verified: `BankCFuncs/funcs_8.c` has `LOOKUP_FUNC(0x8022C270)`/`(0x8022C6E4)`,
  `funcs_5.c` `(0x8022E3F0)`, `funcs_6.c` `(0x8022E758)`).
* **`Makefile`** — `BANK_UNITS := A … M`; **`tools/gen_bank_funcs.py`** — the
  chapter bank's `RAM_END` (`0x286BA0` → `0x8023E630`).

**The 0xA00-byte truncation.** `config-bankC.yaml` ended `bankRec14a` at ROM
`0x2A82F0` (size `0xDE60`) while the game's `subu` at 0x80226A28 shows the
module is `0x2A8CF0 - 0x29A490 = 0xE860`. The last `0xA00` bytes (RAM
`0x80238B10..0x80239510`) hold the module's jump tables — splat named
`jtbl_ovlL_80239270` but could not emit it, and N64Recomp aborted with
`Failed to determine size of jump table at 0x80239270 for instruction at
0x8022EB3C`. Unit L's end marker is now `0x2A8CF0`.

**Verification of the diagnosis.** At the crash
(`/tmp/s58-crash.bin`, `OGRE_DUMP_RDRAM`) RAM `0x8022ACB0` is **byte-identical
to ROM `0x286BA0`** (100% over 8 KiB) and RAM `0x802258B0` is record 14 (100%),
i.e. the chapter module was resident under the shared interpreter. After the
fix the same point runs to scene `0x05`.

## 2. What now plays (the actual goal)

One 240 s title-route run (`OGRE_SPEED=4`, the maintained 26-tap schedule),
`OGRE_SCENE_LOG=1`:

```
0x16 (movie)  t=58.9/67.1/70.0/73.2s      five shots, unchanged
0x0D -> 0x02 -> 0x0D
  chapter bank rom=0x286BA0 ram=0x8022ACB0 (136 functions)  loaded at t=81.1s
0x0D (chapter card)  t=81.1s -> 0x02 t=86.5s
  the "Prologue" card renders (proof below)
0x0D (chapter animation live) t=86.6s -> 0x02 t=129.6s
  the hall with soldiers; "General Godeslas: 'It looks like some of you are gr…'"
0x0D t=129.7s -> 0x05 t=171.4s
  "Grey-Haired Old Man: 'So you're Magnus… Hmm… I see.'" / "Magnus: '...?!'"
0x05 (descriptor 0x8018FD70, mask 0x2) at t=171.4s
  unit M (rom 0x079750 ram 0x8019A7C0, 55 functions) loads; no stub call
```

**0 streamed-stub calls** in the whole run, exit 0. Proofs (swap-chain captures,
`OGRE_CAPTURE_PRESENT`, PPM→PNG with `sips`):

| what | proof |
|---|---|
| the chapter animation's **Prologue** card (`Prologue` / `Casting their gaze on the ground, trudging along…`) | `docs/proofs/native-newgame-prologue-card.png` |
| the story after it: **General Godeslas** receiving the soldiers (the developer's row 8 "received for duty") | `docs/proofs/native-newgame-received-for-duty.png` |
| the dialogue after that: **Grey-Haired Old Man** / **Magnus** | `docs/proofs/native-newgame-magnus-old-man.png` |

Present-index ↔ scene map (from the run's log): the Prologue card is at present
≈8250–8460, the hall/Godeslas ≈8700–9600, the Magnus dialogue ≈13500. Matching
the developer's scene list, **row 7 (chapter animation) is now "renders"**, and
the "received for duty with other soldiers" content is reachable.

**Open / for the developer:** scene `0x05` enters and loads its module (unit M,
55 functions, 0 stub calls), but it renders **black**. With the default tap
schedule the port presents no frames after the scene line (RT64 does not
re-present an unchanged frame); a second run with `OGRE_PRESENT_ALWAYS=1` and
taps for the whole 4 minutes reaches `0x05` at t=201 s and presents **static
black** frames for the rest of the run (57 captures, all identical). The scene
shares the update/hook (`0x8017B858`/`0x8017B9C8`) with scene `0x07` (the
name-entry form) and its enter is a near-twin of `0x07`'s (`0x8017B60C` vs
`0x8017B794`, same DMA target, same `0x8019A7C0` call), so it looks like another
**UI/form screen** rather than the row-8 movie. **What does scene `0x05` show?**
That is the next wall (a graphics/DL question, not a bank one).

## 3. Scene `0x05`'s module (unit M)

`func_8017B60C` (`.streamedB`) at instruction level:

```
0x8017B614  invalidate 0x8019A7C0..0x801B68B0
0x8017B644  a0 = 0x00079750, a1 = 0x8019A7C0, a2 = 0x00087220-0x00079750 = 0xDAD0
            (subu at 0x8017B660)   -> DMA
0x8017B664  bss 0x801A8290..0x801A8290 (the beql skips the bzero; no BSS)
0x8017B6B0  jal 0x8019A7C0        -> the module's entry
```

So RAM `0x8019A7C0` has **two banks**: unit H (`bankRec07`, ROM `0x712A0`,
scene `0x07`) and this one (ROM `0x79750`). Scene `0x07`'s enter's `subu` at
0x8017B7E4 gives `a2 = 0x79750 - 0x712A0 = 0x84B0` — so unit H's segment, which
carried `0x798A0` (session 55's 67×0x200 rounded figure, 0x8600), is now sized
`0x84B0` and no longer overlaps unit M's ROM.

The module's code/data boundary is its own last function: the last `jr ra` is at
RAM `0x801A68A0` (ROM `0x85830`; delay slot at `0x85834`), and ROM `0x85838` is
a byte table (`0203FFFF FF030204 …`). Unit M is code `0x79750..0x85838` + data
`0x85838..0x87220` (55 functions).

### 3b. The diagnostic that hid it

`app/src/bank_overlays.cpp`'s `is_known_module` only asked "does some record's
ROM range contain this DMA's ROM offset and share its RAM base". Unit H's record
(`rom 0x712A0 size 0x8600`) therefore covered scene 0x05's DMA source
`0x79750`, so the port believed it had a module for it and never printed
`[bank] UNKNOWN module` — the report that normally names the next wall. It now
also requires the chunk's **rom→ram delta** to match (`same_stream_delta`). The
missing module is what that would have reported.

## 4. Corrections to session 58 (instruction-level)

1. **The engine/interpreter globals are at `0x8022A9xx`, not `0x8023A9xx`.** The
   generated code addresses them as `lui at,0x8023` + a *negative* immediate, and
   the addition wraps:
   * `func_ovlC_802282D8` 0x802282E4 `lw s3,-22152(s3)` → base
     `0x80230000 + 0xFFFFA978 = 0x8022A978`;
   * 0x80228300 `lw s1,-22160(s1)` → pc `0x8022A970`;
   * `func_ovlC_8022D1CC` 0x8022D1E8 `sw a0,-22124(at)` → engine struct
     `0x8022A994` (the `malloc(0x1CB8)` result);
   * `func_ovlC_80227FF8` stores the descriptor base at 0x80228110
     (`sw a0,-22152(at)`) and pc = 0 at 0x80228184 (`sw zero,-22160(at)`);
   * the "current step" halfword is **0x8022A9B4**: `func_ovlC_8022683C`
     stores its `a1` there at 0x8022685C (`sh a1,-22092(at)`), and
     `func_ovlC_8022D1CC` 0x8022D1F8 (`lh a0,-22092(a0)`) reads it as the step
     to load.
   In `/tmp/s58-crash.bin`: `0x8022A970 = 0x8022A978 = 0x8022A994 = 0` and
   `0x8022A9B4 = 0x0431`. Session 58's "`0x8023A994` holds `0x52513AFF`, a
   colour table / module data" read `0x8023A9xx`, which at that instant is the
   **DMA'd arena image**, not the state. Consequence: the crash is a *downstream*
   symptom (interpreter entered with base 0), not "the handler dereferences a
   stale global".
2. **`func_ovlC_80227700` does not select a setup path.** It is two
   instructions: `jr ra` / `sb a0,-967(at)` (0x80227700/0x80227704) — it stores
   the selector byte `D_8018FC39`. The setup paths are the arms of
   `func_ovlC_8022683C`'s command jump table (§1a).
3. **Step 1073's command is `-8`,** not 6: `func_ovlC_80227E64` maps the
   descriptor's last-word low byte through the table at `0x8022ABE0`; byte 6 →
   `-8` (the handoff's "command 6" is the raw byte). The `-8` arm is what loads
   the chapter module — the movie's last words (5,1,1,5,1 bytes) map to
   `-7 -3 -3 -7 -3`, i.e. the rec14a/rec14b/c arms, which is why the movie was
   unaffected.
4. `bankRec14a`'s size (§1b).

## 5. What was run for verification

```sh
make bank && make bank-recomp            # check-banks OK, 2419 direct calls into
                                         # swappable RAM / 0 outside their record
cmake --build build-app -j 8 && cmake --build build-null -j 8

# the opening, 240 s at 4x, ending in scene 0x05 with 0 stub calls (exit 0)
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,…,a" OGRE_SCENE_LOG=1 \
  OGRE_EXIT_AFTER_MS=240000 ./build-app/ogrebattle64 assets/ogre64.z64

# the same on the null renderer: scene 0x05 at t=159 s, 0 stubs, 0 crashes
… ./build-null/ogrebattle64 assets/ogre64.z64

# a long-tap + PRESENT_ALWAYS run: scene 0x05 at t=201 s, 57 post-scene captures,
# all the same static black frame
OGRE_TAP_BUTTON="start,a,start,a,a,…(135)" OGRE_PRESENT_ALWAYS=1 …

# the proofs (PPM -> PNG, 640 wide)
OGRE_CAPTURE_PRESENT=/tmp/caps/c OGRE_CAPTURE_AFTER=5500 OGRE_CAPTURE_EVERY=30 …

# offline reads of the session-58 crash dump at the *correct* addresses
tools/rdram.py /tmp/s58-crash.bin word 0x8022A960 24       # pc/base/engine = 0
tools/rdram.py /tmp/s58-crash.bin word 0x8022ABA0 9        # command jump table
tools/rdram.py /tmp/s58-crash.bin word 0x8022ABE0 8        # byte -> command table
tools/rdram.py /tmp/s58-crash.bin banks                    # what is live where
```

No repo test harness; every check is a ROM-dependent run. stdout must be
redirected on long runs (the periodic `[snap]` dump stalls boot otherwise).

## 6. Files changed

* `config-bankK.yaml` / `config-bankK.toml` (new) — `bankRec14d`, the
  chapter-animation module (ROM `0x286BA0`).
* `config-bankL.yaml` / `config-bankL.toml` (new) — `bankRec14a`, moved out of
  unit C (ROM `0x29A490`, `0xE860`).
* `config-bankM.yaml` / `config-bankM.toml` (new) — `bankRec05`, scene `0x05`'s
  module (ROM `0x79750`, `0xDAD0`).
* `config-bankC.yaml` / `config-bankC.toml` — `bankRec14a` removed (calls into
  the arena now compile as `LOOKUP_FUNC`); the moved `function_sizes` override
  dropped.
* `config-bankH.yaml` — `bankRec07`'s extent corrected to ROM `0x79750`
  (`0x84B0`), no longer overlapping unit M.
* `Makefile` — `BANK_UNITS := A … M`.
* `tools/gen_bank_funcs.py` — `RAM_END[0x286BA0] = 0x8023E630`.
* `app/src/bank_overlays.cpp` — `is_known_module` also requires the rom→ram
  delta (`same_stream_delta`), so a chunk-DMA of a *different* module into a
  known base is reported as unknown.
* `docs/proofs/native-newgame-prologue-card.png`,
  `docs/proofs/native-newgame-received-for-duty.png`,
  `docs/proofs/native-newgame-magnus-old-man.png` (new) — the chapter card and
  the story after it, captured from the port.
* `PLAN.md`, `docs/scenes.md`, `docs/DECISIONS.md`, `docs/README.md`,
  `AGENTS.md`, this file.

Generated/regenerated (gitignored): `RecompiledFuncs/`, `Bank*Funcs/` (now
A..M), `app/src/bank_funcs.inc`, `build/bank*.elf`.

**Probes:** none. No generated code or app probe was added this session; the
`is_known_module` change is a diagnostic fix, not a probe, and nothing needed
reverting.

## 7. Next leads

1. **Scene `0x05`** (§2) — it enters and loads unit M but renders **black**
   (static; needs `OGRE_PRESENT_ALWAYS=1` to see the frames at all). Decide with
   the developer whether it is a form (it shares scene `0x07`'s update/hook and
   its enter is a near-twin) and what it should draw; then treat it as a
   display-list question (`OGRE_DL_ANALYZE`/`OGRE_DL_DECODE`, the scene's
   `[scene]` descriptor `0x8018FD70`), not a bank one.
2. **The rest of the post-movie sequence**: the "received for duty" and Magnus
   dialogue render, but the steps after scene `0x05` are unexplored; a run with
   taps for the full 4 minutes would show how far the opening now goes.
3. **The `save`/`load` checkpoint** did not visibly resume in two runs this
   session: after loading a checkpoint saved at step 974 the scene log showed no
   further scene change (the snapshot thread kept running). Session 58 verified
   a rewind that replays, so the difference may be the save point (the docs warn
   against saving at a transition) or the load happening before the natural path
   caught up. Worth one A/B in a later session; it is a tooling issue, not a wall.
4. Still open from session 58: the **Controller Pak** save system
   (`librecomp/src/pak.cpp` is upstream's `PFS_ERR_NOPACK` stub) and the movie
   branch's 1-pixel vertical line / transition bands (uninvestigated).
