# Handoff — 2026-09-18, session 75: **native audio works — the wall was `text_address`**

> **Read §1 for the answer.** Session 74 compiled the audio microcode at IMEM
> `0x1000` because it read `0x8009E050` as a second boot block. `0x8009E050` is
> the **text**, loaded by the **standard libultra RSP boot loader** at
> `task->t.ucode_boot = 0x8009ECB0`, which DMAs it to **IMEM `0x1080`** and
> `jr`s there. The `0x80`-byte error rotated every absolute branch target, so the
> driver entered its own command-list DMA helper mid-instruction and the AI
> buffers stayed zero. With `text_address = 0x1080` the game's own AI buffer
> carries real PCM (1104/1104 non-zero samples at t=5 s). Audio is on by default.

## Goal and result

**Goal (developer, carried from session 74):** *sound*; scoped there to "the
opening scene stops being silent", native only.

**Result: native audio plays.** The type-2 (`M_AUDTASK`) microcode is the ROM's
own code, recompiled by `make rsp-recomp`, dispatched in `app/src/rsp.cpp` and
**on by default**; `OGRE_AUDIO_UCODE=0` is the silent-stub A/B escape hatch. No
runtime/submodule change was needed, and `n64recomp-ob64.patch` is untouched
(session 74's four RSPRecomp fixes already carry what this ucode needs).

## 1. The diagnosis: one constant, `text_address`

Session 74's trace was right about the *symptom* — `$29` (`sp`) was 0 every
iteration of the command-table walk, so the dispatch always read DMEM 0 and
always computed target `0x00EC` — but the cause was the compile base.

The boot loader at `0x8009ECB0` is the **stock libultra RSP loader**, not a
game-specific one (the type-4 njpeg task uses the same address). Disassembled at
IMEM `0x1000` (ROM `0x2F0B0`, byte-swapped by RSPRecomp):

```
1000: j 0x1064
1004: addi at,zero,0xFC0     ; at = the OSTask in DMEM
1008: lw v0,0x10(at)         ; v0 = task->ucode = 0x8009E050
100c: addi v1,zero,0xF7F     ; fixed 0xF80-byte text length (task->ucode_size is 0)
1010: addi a3,zero,0x1080    ; IMEM 0x1080
1014: mtc0 a3,SP_MEM_ADDR
1018: mtc0 v0,SP_DRAM_ADDR
101c: mtc0 v1,SP_RD_LEN
1020-1028: wait SP_DMA_FULL
102c: jal 0x103c             ; (DP status helper)
1034: jr a3                  ; enter the microcode at IMEM 0x1080
...
108c: lw v0,0x18(at)         ; (the other arm) v0 = task->ucode_data
1090: lw v1,0x1c(at)         ; v1 = ucode_data_size
10a4: mtc0 zero,SP_MEM_ADDR  ; DMEM 0
10a8: mtc0 v0,SP_DRAM_ADDR
10ac: mtc0 v1,SP_RD_LEN
10c4: j 0x1008               ; then load the text and enter
```

So: `ucode_data` → **DMEM 0**, text → **IMEM 0x1080**, entry **0x1080**, and the
loader leaves `at = 0xFC0` — which is exactly the first thing the text uses
(`1080: mfc0 $5,DPC_STATUS` / `1084: lw $28,0x30($1)` = `task->data_ptr`).
RSPRecomp emits the `r1 = 0xFC0` prologue itself, and the runtime's `run_task`
already copies the OSTask to DMEM `0xFC0` and `ucode_data` to DMEM 0, so the only
mismatch was the label base.

`text_address` is a **label base**, and RSPRecomp resolves `j`/`beq` targets from
the instruction's own absolute IMEM field (`getBranchVramGeneric() & 0x1FFF`),
not relative to the base (`rsp_recomp.cpp:205-216, 1188-1197`). At base `0x1000`
an encoded target of `0x1120` matched the instruction 0x80 bytes *after* the
real one: `jal 0x1120` (the command-list DMA setup) landed in the middle of the
`0x1120` body, `sp` was never set to `0x2B0`, and the walk read DMEM 0. At base
`0x1080` everything lines up. **This is the same rule `rsp-njpeg.toml` already
documents; session 74 applied it to njpeg and not to audio.**

## 2. `rsp-audio.toml`, and why each number is what it is

```toml
text_offset  = 0x2E450   # RDRAM 0x8009E050
text_size    = 0xC60     # NOT the loader's 0xF80
text_address = 0x1080    # the boot loader's entry
```

* **`0xC60`.** The loader DMA's a fixed `0xF80` bytes, which runs past the end of
  the audio code (last instruction `j` at IMEM `0x1CCC`, delay slot at `0x1CD0`,
  then zero padding) into the boot loader's own ROM bytes at IMEM `0x1CE0` and
  the njpeg text after it. Those bytes' `j` targets point *below* the audio text,
  so compiling them emits `goto L_1064` / `goto L_1008` / `goto L_0008` to labels
  that do not exist ("use of undeclared label" — the first build failure). The
  game never executes that tail. Same rule as njpeg's `0x7B8`.
* **`extra_indirect_branch_targets`.** The command loop is

  ```
  10c8: lw   k0,0(sp)          ; sp = 0x2B0, the DMA'd command list
  10cc: lw   t9,4(sp)
  10d4: srl  at,k0,0x17        ; opcode = top byte of the command word
  10d8: andi at,at,0xfe
  10dc: lh   at,0(at)          ; halfword table at DMEM 0 (ucode_data[0])
  10e0: jr   at
  10e4: addi k1,k1,-8
  ```

  The table holds *addresses as data*, invisible to the recompiler, and the
  runtime's switch keys on `(jump_target | 0x1000) & 0x1FFF`. The config lists
  every 4-byte-aligned value that OR can produce inside the text, plus `0x10B4`
  for the `jr $5` return in the command-list DMA helper (a saved register, not
  `$ra`, so `doesLink` does not collect it). The table is the standard 16-entry
  aspMain ABI and the opcode assignments check out against mupen's
  `alist_audio.c`: `0x02` CLEARBUFF → `0x119C` clears DMEM, `0x04` LOADBUFF →
  `0x11C8` DMAs RDRAM→DMEM, `0x06` SAVEBUFF → `0x1208` DMEM→RDRAM, `0x0B`
  LOADADPCM → `0x1248` DMAs exactly `0x20` bytes (16 coefficient pairs), `0x0C`
  MIXER → `0x1C84` uses the vector unit. Opcodes 7/8 hold `0x0000` in the game's
  data (its driver has no SEGMENT/SETBUFF; counts and addresses travel inline in
  the command word) — see §6 for what happens when the game dispatches one.

## 3. What the trace shows (the mechanism, verified)

`probe75` in the generated microcode (reverted) printed the dispatch and every
DMA for the first commands of the first task:

```
DMA_R mem=02B0 dram=80136110 len=13F        ; task->data_ptr -> DMEM 0x2B0
cmd#0 k0=020004E0 t9=000002E0 sp=02B0 -> 119C   ; op 0x02 CLEARBUFF
cmd#1 k0=020007C0 t9=000002E0 sp=02B8 -> 119C
cmd#2 k0=0C00DA83 t9=07C007C0 sp=02C0 -> 1C84   ; op 0x0C INTERLEAVE/MIXER
cmd#3 k0=0C005A82 t9=093007C0 sp=02C8 -> 1C84
cmd#4 k0=061707C0 t9=0012DE70 sp=02D0 -> 1208   ; op 0x06 SAVEBUFF
DMA_W mem=0CB0 dram=0012DE70 len=16F
...
cmd#33 k0=0B000020 t9=00131D58 sp=03B8 -> 1248  ; op 0x0B LOADADPCM (0x20 B)
cmd#39 k0=0D000000 t9=00000000 sp=03E8 -> 12D4
DMA_R mem=02B0 dram=80136250 len=13F            ; next 0x140-byte chunk of the list
```

The command words are guest little-endian words (`k0=0x020004E0`), the opcode is
the top byte, and the list is consumed 8 bytes at a time with a re-DMA every
`0x140` bytes — i.e. the real driver, not a mis-based walk.

## 4. Verification (what was run)

| check | result |
|---|---|
| **live AI buffer** — console `dump` at `t=5 s` (scene `0x09`, intro) | `*(0x800A9B90)` → buf `0x8013EE70` (run-specific), len `0x8A0`; **1104/1104 non-zero** stereo samples, first `-2230,-6518,-1905,-5705`, min/max `-9579/5817`, smooth. Re-verified on the shipped build after the §6 guard: buf `0x8013FBB0`, **1104/1104** non-zero, min/max `-6525/9380` |
| stock 15 s run (audio default-on) + `tools/runlog.py --check` | **PASS**: 809 type-2 tasks, all served by `audio microcode`, 0 stubbed non-gfx tasks, no crash/stub/UNKNOWN |
| `OGRE_AUDIO_UCODE=0` A/B | silent stub as before; `--check` now **FAILs** with 227 stubbed non-gfx tasks (the new check works) |
| SDL audio-device path (no `OGRE_NO_AUDIO`) | opens, boots, 0 errors — audible output available |
| `make rsp-recomp` | `njpeg_ucode.cpp` + `audio_ucode.cpp`, 0 undeclared labels |
| `cmake --build build-app` and `build-null` | clean |
| **reproduced the rare bad dispatch** (`probe75c`, 90 s run) | `table corrupted: slots [7]=0013 [8]=41C0` right after a `SETLOOP`; 40 further 30–90 s runs did not re-dispatch op7/op8 (see §6) |
| probes reverted | `grep -rl probe75 app/ RspFuncs/` is empty; `git status` shows only the intended files + the pre-existing `tools/RT64` dirt |

The AI-descriptor address comes from the game's own `osAiSetNextBuffer` caller at
`0x800859BC`: `a0 = *(0x800A9B90)`, `a1 = *(a0+4) << 2`. That is the buffer the
session-16 runtime feeds to SDL, so the dump above is what the player hears.

## 5. Files changed

* **`rsp-audio.toml`** — `text_address 0x1080`, `text_size 0xC60`, the full
  dispatch target list, and the derivation in comments (was `0x1000`/`0x2000`).
* **`app/src/rsp.cpp`** — audio microcode is **on by default**
  (`OGRE_AUDIO_UCODE=0` stubs it); the comment now records the real load path,
  and `audio_ucode_guard` turns the rare unmappable indirect jump into a logged,
  task-scoped bail instead of a process exit (§6).
* **`tools/runlog.py`** — **bug fix**: it classified task **type 2 (`M_AUDTASK`)
  as gfx** (only type 1 `M_GFXTASK` goes to RT64), so stubbed audio was invisible
  to it. It now counts served-by-kind per type and `--check` fails on any non-gfx
  (type 2/4) task served by the stub.
* **`docs/guides/rsp-microcode.md`** — the "Audio ucode (still TBD)" section is
  replaced with the resolved layout, the two `text_size`/`text_address`
  constraints, the DMEM dispatch table, and the no-listening verification recipe.
* `PLAN.md`, `docs/DECISIONS.md`, this file.

Probes used and reverted: `probe75` in `app/src/rsp.cpp` (command-list hex dump)
and `app/src/sdl_platform.cpp` (`queue_audio_samples` non-zero counts), plus
`probe75`/`75b`/`75c`/`75d`/`75e` in the generated `RspFuncs/audio_ucode.cpp`
(dispatch + DMA trace, dispatch-target ring buffer, whole-table integrity check,
DMA-wrap check, op7/op8 detector). The generated file is restored by
`make rsp-recomp`; the two app files by reverting the markers. Rebuild both
`build-app` and `build-null` after the revert.

## 6. The one rough edge: SETLOOP aliases dispatch slots 7 and 8

Chasing a rare crash (1 in ~25 thirty-second runs) found a real property of the
game's own microcode, not a port bug:

* The dispatch table is **data at DMEM 0** (entries 0..15, read by
  `lh at,0(at)`), and the game's `ucode_data` holds **0x0000** for entries 7 and
  8 — this driver has no SEGMENT/SETBUFF, it passes counts and addresses inline.
* `SETLOOP` (opcode 0x0F, handler `0x1384`) is
  `sll/srl at,t9,8` then **`sw at, 14(zero)`** — it stores a 24-bit loop address
  as a word at **DMEM 0x0E**, i.e. exactly the bytes of table entries 7 (high
  half) and 8 (low half). `probe75c` caught it live:
  `table corrupted: slots [7]=0013 [8]=41C0` right after
  `k0=0F000000 t9=001341C0`.
* That store is **faithful**: the RSP is byte-addressable, and an unaligned word
  store must land on consecutive byte addresses 14..17. The alternative (the
  hardware aligning down to 0x0C) would clobber table entry **6 (SAVEBUFF)**,
  which the game uses 29 times per list — audio would not work at all.
* So opcodes 7/8 are unusable by construction, and the game must not emit them.
  It does anyway, very rarely: the crash was
  `Unhandled jump target 0x1226` = the low half of a loop address, dispatched as
  opcode 8. Whether that is a stale command the game's driver emits at a music
  transition or a recompile-level divergence from hardware was not settled — 45
  runs (≈13 minutes of emulated time) reproduced it once, and 40 further runs
  with a ring-buffer probe did not reproduce the op7/op8 dispatch at all.

**The port therefore has no faithful behaviour to offer for a jump to a
non-instruction address**, and `run_task`'s failure path exits the process. The
fix is a scoped guard in `app/src/rsp.cpp` (`audio_ucode_guard`): log the bail
loudly and report the task as completed so the game drops one audio buffer and
carries on. It is deliberately **audio-only** — njpeg keeps the runtime's loud
failure — and it is a workaround for an emulation limit, not a repair of game
state. `OGRE_AUDIO_UCODE=0` still gives the old silent stub.

## 7. Next leads

1. **The runtime still wakes the audio thread with a dummy.** The game blocks on
   the message queue at `0x800C49E8` waiting for the AI buffer to finish playing;
   `ultramodern/src/mesgqueue.cpp:681` posts a synthetic `msg=0` because the
   **real completion is never fired**. The game registers the event
   (`osSetEventMesg(OS_EVENT_AI, mq, msg)`) and `events.cpp` stores it in
   `events_context.ai`, but nothing posts it when a queued buffer drains.
   Audio is correct and paced with the dummy (~56 tasks/s = frame rate), so this
   is cleanup, not a wall — but it is the right fix for buffer pacing/underruns
   and it would let the dummy be deleted. `get_frames_remaining` in the platform
   callbacks is the signal to post on.
2. **The web build is still muted by default** and still doesn't run the audio
   microcode (session 22). Native now proves the ucode works; re-pointing the web
   build (and removing the default-off) is deferred web work.
3. **`ucode_data` is loaded `0xF80` bytes, not `ucode_data_size`.** The runtime's
   `run_task` (`librecomp/src/rsp.cpp:49`) loads `0xF80 - 1`, while the boot
   loader uses the real `0x800`. Today the extra `0x780` bytes are the gfx
   ucode_data and land in DMEM `0x800..0xF7F`, which the audio driver uses as
   mixing buffers and CLEARBUFFs before use — so it is harmless here, and it
   matches "DMEM beyond the seed is leftovers" in spirit. Worth aligning anyway
   (`ucode_data_size`) if another ucode ever depends on it.
