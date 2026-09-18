# Handoff — 2026-09-18, session 74: **the audio microcode is located, recompiled and running — it does not yet make PCM**

> **Read §1 for the answer to "why is the game silent".** The port's native audio
> is not screeching, it is *silent*: the game hands the AI three rotating buffers
> and every byte is zero, because the type-2 (`M_AUDTASK`) microcode is still
> stubbed. The screech the developer disabled was the **web build's unmuted
> AudioWorklet** (confirmed this session), so native sound is Phase-6 work from a
> clean slate. §2 is the microcode's identity and layout, §3 what was built, §4
> the exact wall reached, §5 verification, §6 files, §7 next leads.

## Goal and result

**Goal (developer):** *"rendering now seems to be mostly completed. the next goal:
sound!"* Scoped this session to **"just the opening scene stops being
silent/screeching"**, native only (the developer explicitly deferred the web
build).

**Result: the microcode is identified, recompiled and executes — but produces no
audio yet, so the shipped default is unchanged (stub) and nothing regresses.**

* The game's audio *driver* is alive: a 30 s run submits **1694 type-2 tasks**,
  always with the same `ucode=0x8009E050`, `ucode_data=0x800ABDA0` (0x800 bytes),
  `data_ptr=0x80136110` and a varying `data_size` (0x3D8..0x9A0, 103 distinct
  values) — so the game is building real command lists, not empty ones.
* Every AI buffer the game queues is **all zero** (`OGRE_AI_DUMP` probe): three
  rotating buffers 0x450 bytes apart (1104 samples ≈ 23 ms at 48 kHz). The
  screech was therefore never native; the developer confirmed it was the web
  build (`?audio`, session 22).
* The type-2 microcode is now **recompiled** (`rsp-audio.toml` →
  `RspFuncs/audio_ucode.cpp`), dispatched in `app/src/rsp.cpp`, and it **runs**:
  no crash, no unhandled instruction, no unhandled jump. It is **gated off by
  default** (`OGRE_AUDIO_UCODE=1` enables it) because it currently spins without
  emitting PCM (§4).
* Four RSPRecomp bugs were found and fixed on the way (they affect any ucode
  with a >4 KB image, `cfc2`/`ctc2`, `bgezal`, or a `DPC_*` read); the vendored
  tool patch `n64recomp-ob64.patch` was regenerated to carry them.

## 1. The silence is the stub, not the game

Two runs with the stock build (`OGRE_NO_AUDIO=1`, no audio device needed):

```
[rsp] task type=2 ucode=0x8009E050 ucode_size=0x0 ucode_data=0x800ABDA0
      ucode_data_size=0x800 data_ptr=0x80136110 data_size=0x3D8 ... 0x9A0
```

A probe on `queue_audio_samples` (`app/src/sdl_platform.cpp`, temporary, now
reverted) dumped everything the game sends the AI: **3 800 704 bytes, 0 non-zero
samples**, in the three-buffer rotation `+0x000/+0x450/+0x8A0`. So:

* the game's audio path is healthy end-to-end (driver, buffers, AI handoff,
  `0x800C49E8` bookkeeping);
* the buffers are silent because the **RSP never synthesizes** — the stub in
  `app/src/rsp.cpp` returns `RspExitReason::Broke` without doing work;
* the "wall of screeching" of session 22/15 was the *web* page playing those
  same buffers through the AudioWorklet before they were zeroed — i.e. it was
  stale RDRAM, and it does not reproduce natively.

**The developer confirmed the screech was the web build**, so no native
regression hunt was needed.

## 2. The microcode: where it is and what it is

| part | RDRAM | ROM (`vram − 0x80070C60 + 0x1060`) |
|---|---|---|
| boot block + segment table | `0x8009E050` | `0x2E450` |
| microcode text (entry IMEM `0x1120`) | `0x8009E0F0` | `0x2E4F0` |
| `ucode_data` (DMEM seed, game-owned) | `0x800ABDA0` | `0x3C1A0` |
| command list (task `data_ptr`) | `0x80136110` | — (heap) |

* `0x8009E050` is the microcode's **own boot block**, not the text: it reads the
  OSTask at DMEM `0xFC0` (confirmed live: `+0x00`=2 type, `+0x08`=boot ucode,
  `+0x10`=`0x8009E050`, `+0x30`=`0x80136110` = `data_ptr`, `+0x34`=`0x520` =
  `data_size` — the standard libultra OSTask layout), DMAs segments into IMEM and
  calls the text entry at IMEM `0x1120`.
* **Byte order:** the ROM stores the RSP words byte-reversed relative to the
  RDRAM image. Verified against a live dump: the guest words at `0x8009E0F0`
  (`struct.unpack_from('<I', dump, 0x9E0F0)`) equal the ROM's big-endian words at
  `0x2E4F0` for the whole region. `RSPRecomp` byteswaps each ROM word it reads,
  so `text_offset` points straight at the ROM bytes.
* `tools/rdram.py`, `tools/guestmap.py` and the ELF all agree on the mapping
  above (`0x8009E050 → 0x2E450`).

`rsp-audio.toml` (text_offset `0x2E450`, size `0x2000`, text_address `0x1000`)
compiles the **whole 4 KB instruction window** including the boot block, so the
image is compiled in place and the entry is the array's first instruction. Notes
that cost time:

* **RSPRecomp starts executing at the first instruction of the array** — so the
  first instruction must be the real entry. Compiling only the text from
  `0x2E500` with `text_address 0x1120` looks right in the disassembly and then
  returns `RspExitReason::UnhandledJumpTarget` (it ran the entry registers
  uninitialised); the boot-block-based config runs.
* `extra_indirect_branch_targets = [0x10EC]`: the boot code dispatches through a
  halfword table, and the generated switch matches
  `(jump_target | 0x1000) & 0x1FFF`, so extra targets must be given in the
  `0x1xxx` convention (`0x00EC` in the register is `L_10EC`).

## 3. What was built

* **`rsp-audio.toml`** (new) — the audio microcode's RSPRecomp config; `make
  rsp-recomp` now runs it too, and `app/CMakeLists.txt` already globs
  `RspFuncs/*.cpp`, so no CMake change was needed.
* **`app/src/rsp.cpp`** — dispatch by ucode address (`0x8009E050 → audio_ucode`),
  mirroring the njpeg pattern. **Default is the stub**; `OGRE_AUDIO_UCODE=1` runs
  the recompiled microcode.
* **RSPRecomp fixes** (vendored `tools/N64Recomp`, carried in
  `n64recomp-ob64.patch` — all previous hunks byte-identical, one file added):
  1. **Instruction addresses are masked to the 4 KB window**
     (`vram & rsp_mem_mask`). Branch targets were always collected and emitted
     masked, and labels were not, so any text longer than
     `0x2000 − text_address` emitted `L_2FEC:`-style labels that no
     `goto L_0FEC` could match ("undeclared label", 153 of them here).
  2. **`mfc0`/`mtc0` of the `DPC_*` registers** are handled (read as idle,
     written as no-ops) — the audio ucode reads `DPC_END` and writes
     `DPC_STATUS`; no recompiled gfx ucode exists, so the RDP is inert.
  3. **`cfc2`/`ctc2`** emit `rsp.CFC2/CTC2` (the runtime already implements
     them), and **`bgezal`/`bltzal`** emit the branch plus `$ra = pc+8`
     (`jal`'s convention; the audio ucode uses them for its sample helpers).
  4. **The unhandled-jump diagnostic goes to `stderr`**: it was a `stdout`
     `printf`, and the runtime exits through `_Exit`, so the one message that
     names the failing target was always discarded.
  5. **`mfc0 $x, SP_STATUS` now reads `1`** (`SP_STATUS_HALTED`), matching the
     runtime's own `osSpGetStatus_recomp` (rule: the RSP is halted whenever
     microcode reads its own status). The audio boot block branches on that bit
     to tell a fresh task from a resumed one; with `0` it took the resume path
     and re-entered its entry forever. **njpeg does not read `SP_STATUS`**, so
     this cannot regress it (checked: its only `mfc0`s are `SP_DMA_BUSY`,
     `SP_DMA_FULL`, `SP_SEMAPHORE`).

## 4. The wall: the command walk reads DMEM 0 and never DMAs

With `OGRE_AUDIO_UCODE=1` the microcode is entered once and spins (a 4 s run:
1 call, no crash, no further tasks). Instrumenting the generated ucode (probe,
reverted by regenerating) shows:

```
[probe74] ucode CALL 1
[probe74] DMEM 0x000=10EC139C 0x040=00010001 0x200=FEE82D2C 0x2B0=00000010 ...
[probe74] TASK 0xFC0=00000002 +04=00000000 +08=8009ECB0 +10=8009E050 +28=00000000 +2C=00000000 +30=80136110 +34=00000520
[probe74] READ  mem=0x0000 dram=0x00000000 len=0x0
[probe74] jr -> 0x00EC (r26=00EC139C r28=80136118 r29=00000000 r1=000000EC)
[probe74] READ  mem=0x0000 dram=0x00000000 len=0x0
[probe74] jr -> 0x00EC (r26=00EC139C r28=80136120 r29=00000000 r1=000000EC)
... (repeats; r28 advances 8 per iteration, r29 stays 0)
```

so:

* the OSTask **is** where the ucode expects it (DMEM `0xFC0`, standard layout),
  and `ucode_data` **is** at DMEM 0 (`0x10EC139C` matches the ROM's first word);
* the boot block's loop at IMEM `0x1048` is `lw $26,0($29)` / `lw $25,4($29)` /
  `addi $28,$28,8` / `srl $1,$26,23` / `andi $1,$1,0xFE` / `lh $1,0($1)` /
  `jr $1` — a table walk whose **`$29` is 0 for every iteration**, so it reads
  the same DMEM word, always computes target `0x00EC`, and never issues a DMA
  with a non-zero address (every `DO_DMA_READ` seen has `dram=0 len=0`). The
  game's command list at `data_ptr` is therefore **never fetched**;
* `r28` advancing by 8 per iteration shows `$28` is the walk's pointer (it was
  loaded from the task's `data_ptr`) while `$29` — the *table* base — is never
  initialised on this path.

**Both hypotheses, and the cheapest experiment for each:**

* **(A) `$29` should have been set up by the boot path we no longer take.**
  The boot block's first test is `mfc0 $5, DPC_STATUS` (tests on bits 0 and 8)
  before it `jal 0x1120` at `L_102C`; with `DPC_STATUS` reading 0 it takes the
  `beq` arm, the other arm polls bit 8. **DISPROVEN this session:** forcing the
  read to `0x1`, `0x100`, `0x101` and `0x1FF` in the generated code and watching
  for a DMA with a non-zero DRAM address changed nothing — still 0 such DMAs and
  still `$29` = 0, so the status value is not the gate.
* **(B) the audio list must be DMA'd to a DMEM base the ucode computes, and the
  runtime's DMEM layout only covers `ucode_data` at 0 and the task at `0xFC0`.**
  mupen64plus-rsp-hle's `alist_audio.c` uses **`DMEM_BASE 0x5C0`** for every
  audio-list address, and its `memory.h` puts the task at `0xFC0` exactly as this
  runtime does — so the reference for the expected DMEM contents is
  `mupen64plus-rsp-hle/src/` (`alist_audio.c`, `alist.c`, `audio.c`; checkout
  under `/private/tmp/emu-research/`). The subroutine the boot block calls
  (`jal 0x1120`, with `r2` = `data_ptr` and `r24` = `0xFA0`) starts
  `addi $1,$zero,0x2B0` — i.e. it expects the command list at DMEM **`0x2B0`** —
  and it is the one that must issue the list DMA. *Experiment:* log the first
  `SP_MEM_ADDR`/`SP_DRAM_ADDR`/`SP_RD_LEN` writes inside `L_1120` and compare
  with the alist layout.
* **A reference emulator is the decisive oracle here** and is already on the
  machine: OB64 audio in mupen is **LLE** (its HLE `try_audio_task_detection`
  keys on `*ucode_data == 1` and OB64's data block starts `0x10EC139C`, so the
  HLE does **not** claim this ucode), so a savestate at the title gives the
  correct DMEM/IMEM image to diff against. See
  `docs/guides/emulator-first.md` §5 for the savestate → RDRAM recipe.

## 5. Verification (what was run)

| check | result |
|---|---|
| `OGRE_AI_DUMP` probe on `queue_audio_samples` (stock build) | 3 800 704 bytes captured, **0 non-zero samples**; three rotating buffers 0x450 apart, 1104 samples |
| audio task census (30 s stock run) | **1694 type-2 tasks**, 1 ucode, 1 `ucode_data`, 1 `data_ptr`, **103 distinct `data_size`** (0x3D8..0x9A0) |
| ROM↔RDRAM byte-order check | guest words at `0x8009E0F0` == ROM big-endian words at `0x2E4F0` across the region |
| `RSPRecomp` on `rsp-audio.toml` | compiles; **0 duplicate labels, 0 missing labels** |
| `make rsp-recomp` | `njpeg_ucode.cpp` + `audio_ucode.cpp`, both regenerate |
| `cmake --build build-app` | clean |
| `OGRE_AUDIO_UCODE=1` run (6 s) | exit 0, microcode entered, **0 unhandled jumps, 0 crashes** (it spins, §4) |
| `OGRE_NO_AUDIO=1` stock run (6 s) + `tools/runlog.py --check` | **PASS**: no crash, no stub call, no unknown module, no bad RSP exit; 288 type-2 tasks on the stub |
| `make rsp-recomp` + probes reverted | `grep -rl probe74 RspFuncs/ app/` is empty |
| vendored-tool patch | `n64recomp-ob64.patch` regenerated: all 10 previous file hunks **byte-identical**, plus `RSPRecomp/src/rsp_recomp.cpp` |

## 6. Files changed

* **`rsp-audio.toml`** — new (the microcode config, with the layout and byte-order
  derivation in comments).
* **`app/src/rsp.cpp`** — audio dispatch (`0x8009E050`), off by default,
  `OGRE_AUDIO_UCODE=1` to enable.
* **`Makefile`** — `make rsp-recomp` also runs `rsp-audio.toml`.
* **`n64recomp-ob64.patch`** — regenerated with the five RSPRecomp fixes
  (`tools/N64Recomp` is gitignored, so the patch is the record).
* `PLAN.md`, `docs/DECISIONS.md`, this file.

Probes: `app/src/sdl_platform.cpp` (`queue_audio_samples` dump, tag `probe74`) and
`RspFuncs/audio_ucode.cpp` (DMA/register trace, tag `probe74`) — **both reverted**
(the SDL file restored from a copy, the ucode regenerated by `make rsp-recomp`);
`grep -rl probe74 app/ RspFuncs/` is empty. `git status --short` shows only
`Makefile`, `app/src/rsp.cpp`, the new `rsp-audio.toml`, and the pre-existing
`tools/RT64` dirt.

## 7. Next leads

1. **Diff DMEM/IMEM against a reference emulator at the first audio task**
   (§4, hypothesis B). mupen uses LLE for this game, so a savestate's RSP
   DMEM/IMEM is the ground truth for what the boot block expects; the
   `alist_audio.c` `DMEM_BASE 0x5C0` layout is the likely answer.
2. **Resolve `DPC_STATUS` at the boot test** (§4, hypothesis A): if the bit-8
   path is the real one, the boot block's table base (`$29`) gets initialised
   there and the walk starts working. Making the register value a config knob is
   a 5-line change in RSPRecomp (it already takes per-ucode config).
3. Once PCM appears: the runtime already feeds `queue_audio_samples` from
   `osAiSetNextBuffer`, and the game's `0x800C49E8` wait is auto-satisfied in
   `mesgqueue.cpp` — that auto-response should be revisited once the real task
   completes on its own (`OGRE_AI_DUMP` is the instrument; the probe is in this
   session's history).
4. **`mfc0 $x, DPC_*`** now reads 0 in RSPRecomp; if a future ucode needs the
   real RDP status, that is where to plumb it.

## 8. Reference material used (developer's tip)

The developer pointed at `mupen64plus-rsp-hle` (checked out at
`/private/tmp/emu-research/mupen64plus-rsp-hle`, alongside GLideN64,
angrylion-rdp-plus and parallel-rdp) instead of more address archaeology. What it
settled immediately:

* **the OSTask DMEM layout** (`src/memory.h`: `TASK_TYPE = 0xfc0`,
  `TASK_DATA_PTR = 0xff0`, …) — identical to this runtime's `memcpy(&dmem[0xFC0],
  task, …)`, which confirmed the boot block is reading the right place;
* **the audio-list DMEM base** (`src/alist_audio.c: enum { DMEM_BASE = 0x5c0 }`)
  — the strongest lead for §4;
* **that OB64's audio is not HLE-covered**: `try_audio_task_detection` requires
  `*ucode_data == 1` (ABI1/2/3 families) and OB64's block starts `0x10EC139C`, so
  emulators fall through to LLE — meaning there is no HLE shortcut to port, and a
  reference *emulator run* is the oracle rather than a reference *implementation*.
