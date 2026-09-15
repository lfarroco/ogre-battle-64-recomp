# Handoff — 2026-09-15, session 46: the cathedral background is an **RSP-decoded image** (M_NJPEGTASK), and the port stubs the microcode

## Goal and result

**Goal:** the cathedral scene (scene `0x0D` step 2) renders with no background
(all black).

**Result: the background is a full-screen pre-rendered image that the game
decodes through an RSP microcode task of type 4 (`M_NJPEGTASK`), and the port
stubs every non-gfx RSP task.** The decoder is now recompiled
(`make rsp-recomp` → `RspFuncs/njpeg_ucode.cpp`) and dispatchable, but its
output does not yet match what the game expects, so it is **opt-in
(`OGRE_NJPEG=1`) and the stub remains the default**. The background is **still
missing**.

This is *not* a renderer bug: the display list is correct, the image source
buffer is simply zero.

**Second result (same session, after the developer asked whether we need better
tooling): the Tier-1 diagnostics toolkit.** Four recurring tasks had no tool —
finding a signal in a 20k-line log, telling a blank buffer from an image,
diffing two RDRAM dumps, and arming a watchpoint on the right host address. They
are now `tools/runlog.py`, `tools/guestmap.py` (+ `tools/n64map.py`),
`tools/rdram.py image`/`diff`, and `tools/watch.sh`, documented in
`docs/guides/app-build.md` → "Diagnostics toolkit" and referenced from
`AGENTS.md` §4. §7 below is the summary.

## 1. The background draw (what the port actually submits)

`OGRE_DL_DECODE=<n>` on a step-2 display list (`docs/guides/app-build.md`)
shows the frame's **first 48 `G_TRI2` quads** are a full-screen 320x240
`G_TEXRECT`-style blit built as 48 strips of 5 rows:

```
SETTIMG fmt=RGBA siz=16b width=320 addr=0x80243E28
LOADTILE t7 uls=0 ult=0   lrs=1276 lrt=20
VTX n=4 ... (quad x -229..91, y 235..0)
```

So the background is one 320x240 RGBA16 image at guest `0x80243E28`.
`OGRE_DUMP_RDRAM` after a step-2 run shows that buffer is **all zero** in the
null build (and a near-uniform `0x0843` in the RT64 build — mostly the same
black). The 266 quad draws in the frame are the sprites and the dialogue box;
they render.

## 2. Where the image comes from (instruction level)

Scene `0x0D` step 2's resource is asset `0x00183352` (ROM `0x7175A2`, payload
`0x5AE0`), reached from asset 4's table (`func_8009DAF4`/`func_8009DBB8`; entry
`D_80196B0D = 0x31 = 49`). Its magic is `'H','U'` with `w=0x140` (320) and
`h=0xF0` (240):

```
48 55 fe 00 01 40 00 f0   'HU' flags=0xFE w=320 h=240
48 55 46 46 01 2c f7 38   'HUFF' ... high-entropy data
```

The load chain (unit E, record 0, `bankRec0`) is:

```
func_80178920 (scene 0x02 enter, overlay C)
  -> func_ovlE_80198D28 -> func_ovlE_801988C8     (asset 49, sees 'HU')
     -> func_ovlE_8019A0A8 + func_ovlE_8019A1FC    ('HU' container walker)
        -> func_ovlE_80199D30                      (per-chunk work)
           -> func_ovlE_8019976C                   (stage machine, submits RSP)
              -> func_ovlE_80198E70                (builds the OSTask)
```

`func_ovlE_80199D30+0x136F` is a `memcpy(dst = struct->0x64 + rows,
src = struct->0x88[i] , size)` — it assembles the image from a decoded
resource. `func_ovlE_80198E70` sets up the RSP task; `func_800901C0` is just a
cache flush, and the real submission is the game's own `osSpTaskLoad`
(`0x80093A00`) + `osSpTaskStartGo` (`0x80093C0C`).

## 3. The RSP tasks the port drops (measured)

At the second `0x02` enter (immediately before step 2's `0x0D`, t≈39.9 s) the
game submits **four type-4 tasks**, all stubbed today
(`app/src/rsp.cpp` → `stub_ucode`):

```
[rsp] task type=4 ucode=0x8009ED80 ucode_size=0x7C0 ucode_data=0x800AC050
      ucode_data_size=0xF0 data_ptr=0x801D1030 data_size=0x12C flags=0x0
[rsp]   boot=0x8009ECB0/0xD0 stack=0x00000000/0x0 output=0x00000000/0x0
        yield=0x00000000/0xFFFFFFFE
```

Type 4 is `M_NJPEGTASK` (`ultramodern/ultra64.h`). Mapping the main segment
(`ROM = vram - 0x80070C60 + 0x1060`):

| part | RDRAM | ROM | size |
|---|---|---|---|
| boot loader | `0x8009ECB0` | `0x2F0B0` | `0xD0` |
| main microcode | `0x8009ED80` | `0x2F180` | `0x7C0` |
| Huffman/quant tables | `0x800AC050` | `0x3C450` | `0xF0` |

The main microcode is a standard libultra boot + an IDCT-style decoder
(`vmulf`/`vmacf` butterflies, DMEM tables at `0x9F0`). The four tasks'
`data_ptr`s are `0x801D1030`, `0x802644D0` (KSEG1 `0xA02644D0`), `0x8025A630`,
`0x8025C0D0`, sizes `0x12C`, `0xA5`, `0xB4`, `0x63`.

Decisive measurement with an lldb watchpoint on `rdram + 0x243E28` armed at the
second `0x02` enter: every write to the background buffer comes from the same
`func_ovlE_80199D30+0x136F` memcpy (`-> func_80080998`). The first copy carries
real data (`0x20c5290549c92905`), the next two are partly/wholly **zero** — i.e.
the decoded resource is empty, so the image is assembled as black.

## 4. What was implemented (and why it is off by default)

* `rsp-njpeg.toml` + `make rsp-recomp` → `RspFuncs/njpeg_ucode.cpp`
  (RSPRecomp; `RspFuncs/` is gitignored like `RecompiledFuncs/`). `text_size`
  is `0x7B8`, not `0x7C0`: the last 8 bytes of the declared region are data
  (`0x0900060E` decodes as `j 0x1838`), which RSPRecomp emits as a `goto` to a
  label that does not exist.
* `app/CMakeLists.txt` builds `../RspFuncs/*.cpp` into `ogrebattle64_rsp`
  (the generated code is C++ — it uses librecomp's `RSP` VU implementation).
* `app/src/rsp.cpp` dispatches on the ucode address
  (`task->t.ucode == 0x8009ED80`), behind **`OGRE_NJPEG=1`**.

Measured with `OGRE_NJPEG=1`:

* all four tasks run and return `RspExitReason::Broke`;
* an input/output pre/post dump around the decoder (`OGRE_RSP_DUMP`) shows each
  task writes **exactly one `0x300`-byte block** at `data_ptr - 0x300`
  (598/768, 768/768, 754/768, 448/768 bytes differ), not the
  `(data_size-1) * 0x300` the microcode's loop structure suggests — so the
  recompiled decoder is exiting early (or the loop's write pointer never
  advances), and the game's image assembler then copies that incomplete/zero
  resource into `0x80243E28`;
* the game **stops submitting display lists at the step-2 entry** (RT64: last
  `[renderer] display list` is 4410 at `t=37019 ms`, the transition is
  `t≈37.8 s`), and never draws the step;
* the null build dies with **SIGFPE** in the runtime's `do_recv` — the game
  calls `osRecvMesg` with a null-derived queue (`mq_ = 0x80000010`,
  `msgCount = 0`), which becomes a divide-by-zero. The RT64 build survives
  longer and then dies with SIGBUS.

### 4b. The loop probe (why one block)

A temporary probe in the generated decoder (`// probe46`, reverted by
`make rsp-recomp`) logged the loop head (`L_0DFC`, i.e. ROM offset `0x7C`):

```
[probe46] enter data_ptr=801D1030 data_size=0000012C yield_size=FFFFFFFE
[probe46] loop#0 r4=300 r5=300 r28=801D1330 r29=801D0D30
[probe46] loop#1 r4=299 r5=300 r28=801D1630 r29=801D0D30
...
[probe46] loop#11 r4=289 r5=300 r28=801D3430 r29=801D0D30
[probe46] exit broke
[probe46] enter data_ptr=A02644D0 data_size=000000A5 yield_size=FFFFFFFE
[probe46] exit broke            <- no loop iteration at all
```

So for task 0 the loop runs its `data_size` iterations and `r28` (the input
read pointer) advances `+0x300` each time, but **`r29` (the output write
pointer) never advances** — every iteration writes to the same
`data_ptr - 0x300`, so only the last block survives. The ROM does contain
`addi $29, $29, 0x300` at vram `0x8009F170` (offset `0x3F0`), but that is the
`L_1164` block (line 589 of the generated file), which the loop path we take
does not reach — it jumps to `L_1190` first. Tasks 1..3 leave before `L_0DFC`
at all. So the decoder is **stateful across the four tasks**, and per-task
input/output analysis is not enough to judge it.

A second probe (`// probe46`, reverted) read the game's decode state struct
`D_8019A680` at each submit: `+0x81` walks 1,2,3,4 (the resource index) and
`+0x68` is the task's `data_ptr`, while the stage byte `+0x7e` is `1` at every
submit in **both** the stub and the decoder runs — so that probe did not by
itself show where the game stops.

### 4c. A runtime discrepancy worth testing

`recomp::rsp::run_task` (vendored `librecomp/src/rsp.cpp`) loads the microcode
data with a **fixed `0xF80` bytes**:

```c
dma_rdram_to_dmem(rdram, 0x0000, task->t.ucode_data, 0xF80 - 1);
```

The game's own RSP boot microcode (`0x8009ECB0`) instead DMAs exactly
`task->ucode_data_size` bytes (`lw $3, 0x1C($1)` / `addi $3, $3, -1` /
`SP_RD_LEN`). For this decoder that is `0xF0`, so the fixed load also fills
DMEM `0xF0..0xF7F` with whatever follows the tables in RDRAM (`0x800AC140`, the
gfx ucode data) — and the microcode reads tables at DMEM `0x9F0`, i.e. exactly
in the clobbered range. Changing the load to `ucode_data_size - 1` was tried
this session (temporary vendored edit, then reverted): it changes the output but
does **not** make the background appear, so it is recorded here as a
hypothesis to test together with the next step, not a fix.

Because of that, enabling the decoder by default would *regress* step 2 from
"renders with a black background" to "does not draw". It stays opt-in.

## 5. What is NOT established

* That the recompiled microcode is numerically correct. It runs, but its output
  is a single `0x300`-byte block instead of the stream the game consumes, and
  the background buffer stays black. **Do not treat the microcode as the
  finished fix.**
* Which RSP task-completion protocol the game expects. The runtime's
  `run_task` runs the main ucode once and reports `Broke`; the game's boot
  microcode handles `flags & 2` ("yielded") tasks and `RspExitReason` has
  overlay/resume states, neither of which this decoder path exercises today.
* Whether the missing boot microcode matters. The runtime ignores
  `ucode_boot`; the boot's only measured work is DMA the main ucode to IMEM
  `0x080` and `ucode_data` to DMEM `0x0000`, which `run_task` already does. But
  the main ucode reads tables at DMEM `0x9F0`, which on hardware would *not* be
  the declared `0xF0`-byte `ucode_data` (the runtime's fixed `0xF80`-byte load
  puts RDRAM `0x800ACA40` there instead) — that discrepancy is unexplained
  (§4c).
* **Whether these four tasks can be the image decoder at all.** Even a perfect
  decode of them yields at most `4 * 0x300` = 3 KiB, against a 153 600-byte
  background. So either the game re-invokes the microcode many times (it
  submits only four tasks in this run, before and after the transition) or the
  tasks produce *tables/parameters* that a CPU decoder — the Huffman code
  around `func_ovlE_80197E5C` (tables `D_ovlE_8019A230`, jump table
  `jtbl_ovlE_8019A3B0`) — then uses to expand the `'HUFF'` asset. That
  distinction decides whether fixing the microcode can ever fill
  `0x80243E28`, and it is the first thing to settle next session.

## 6. What's next (session 47)

1. **Explain the write pointer.** The decoder's loop advances only its *read*
   pointer; the output pointer stays at `data_ptr - 0x300` on the path taken,
   and tasks 1..3 skip the loop head entirely. Either the recompiled control
   flow differs from the ROM's (check the `L_1164` / `L_1190` edges against the
   raw words at offsets `0x3E4`/`0x410`) or the microcode really is a stateful
   multi-task stream and the game drives it differently. `tools/N64Recomp`
   can be rebuilt with RSP debug output if needed.
2. **Test the `ucode_data` load** (§4c) together with (1) — the runtime loads a
   fixed `0xF80` bytes where the game's boot microcode loads exactly
   `ucode_data_size` (`0xF0` for this decoder), and the microcode's DMEM tables
   at `0x9F0` sit inside the clobbered range.
3. **Check what the game does with the decoder's output.** `func_ovlE_80199D30`
   is the consumer (its `memcpy` at `0x80199F70`/`+0x136F` copies
   `struct[0x88 + i*4]` into the image). Confirm the destination/source pairing
   and which buffer ends up at `0x80243E28` — with the stub the resource is
   zero, and with the (incomplete) decoder it is partly `0xFF`/`0x01`.
4. Only then re-measure `0x80243E28` (it must hold the 320x240 image) and turn
   the dispatch on by default.
5. If the microcode path is a dead end, the fallback is the CPU Huffman
   decoder around `func_ovlE_80197E5C` (tables `D_ovlE_8019A230`, jump table
   `jtbl_ovlE_8019A3B0`) — **but an lldb breakpoint on it was never hit in a
   full 70 s New Game run** (with the stub, i.e. the path the port currently
   takes), so it is *not* the active decoder today. That makes the RSP path the
   only measured route to the image, and makes point 1 the thing to settle.

## 7. Tooling (Tier 1 — built after the developer's "do we need better tooling?")

The wall above took so long partly because three of the four recurring tasks had
no tool: finding a signal in a 23k-line log, deciding whether a buffer is blank
or an image, diffing two RDRAM dumps, and arming a watchpoint on the right host
address. All four now exist, are documented in `docs/guides/app-build.md` ->
"Diagnostics toolkit", and are referenced from `AGENTS.md` §4:

* **`tools/runlog.py <run.log>`** — one screen: scene timeline, RSP tasks by
  type (and every **non-gfx task spelled out** — a non-gfx task served by
  `stub microcode` is work the port drops, which is exactly how this session's
  four `M_NJPEGTASK` tasks hid), bank loads, and a **problems** table
  (`[crash]`, `streamed function stub`, `UNKNOWN module`, `Failed to execute
  task`, bad ucode exits). `--check` exits 1 on any of them — the assertion a
  future `make smoke` should use — and `--json` gives the same data.
* **`tools/rdram.py <dump> image <addr>`** — renders a region as an N64 texture
  (`rgba16`/`rgba32`/`ia16`/`ia8`/`ia4`/`i8`/`i4`/`ci8`+`--palette`) to an RGBA
  PNG (zlib only) and prints a luminance summary with a `(near-)uniform` note.
  On this session's `0x80243E28` it says "2 unique value(s), 0.7% near-black —
  blank/fill buffer" in one command.
* **`tools/rdram.py diff A.bin B.bin`** — changed runs, merged by `--gap`,
  **grouped by owning record/overlay**, flagging RAM that several records share.
  Replaces the in-app before/after probe this session wrote.
* **`tools/guestmap.py <addr>`** — rom ↔ vram, owning segment/record/unit, the
  ELF-owner's **function-entry** check, and the other records that map the same
  RAM. `0x801B7EBC` prints as `func_801B7B9C +0x320` (not an entry, main ELF)
  vs `func_ovlC_801B7EBC` / `func_ovlG_801B7EBC` (entries, units C/G) —
  the mis-binding question of §2, answerable offline.
* **`tools/watch.sh <guest-addr>`** — arms an lldb watchpoint at
  `rdram + (guest - 0x80000000)` after a chosen guest symbol (`--ignore N`
  skips earlier hits, e.g. `--after func_80178920 --ignore 1` arms at the second
  scene-`0x02` enter) and prints a backtrace per hit. This session's manual
  watchpoint experiment reproduces in one command.
* **Frame pointers** (`-fno-omit-frame-pointer`) on the app, the recompiled
  libs, and `librecomp`/`ultramodern` (RT64 excluded): a fault in runtime code
  reached through a bridge has an empty shadow *guest* chain, and without frame
  pointers `bt` printed only `do_recv + 240`. Now a breakpoint in `do_recv`
  unwinds `do_recv -> osRecvMesg -> osRecvMesg_recomp -> func_80088F08 + 717`
  (the guest caller) and on into the thread trampoline, which is the link the
  session-46 `SIGFPE` investigation was missing. A *signal* stop can still print
  a single frame, so break on the faulting function rather than waiting for it.

Not built (Tier 2, for a later session): `make smoke` (a regression battery
asserting `runlog.py --check` on the known-good runs), a `tools/scene.sh` that
encapsulates the `OGRE_TAP_*`/`OGRE_SPEED`/capture recipes per screen, and an
offline harness that replays one RSP task's captured input through a recompiled
ucode and diffs DMEM/output.

## Verification run this session

```sh
# the bug, unchanged (stub default): step 2 entered, background buffer all zero
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1 \
  OGRE_DUMP_RDRAM=/tmp/cath.bin OGRE_EXIT_AFTER_MS=45000 ./build-null/ogrebattle64
#   -> exit 0, [scene] id=0x000D at t=39976ms, 0 [crash], RDRAM 0x243E28 all 0

# the decoder, opt-in: four M_NJPEGTASK tasks run; the scene then stops drawing
OGRE_NJPEG=1 OGRE_SPEED=6 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=60000 ./build-app/ogrebattle64

# regenerate the microcode
make rsp-recomp          # tools/N64Recomp/build/RSPRecomp rsp-njpeg.toml
```

No repo test harness exists; each check is a ROM-dependent 40-120 s game run.

## Files changed

* `rsp-njpeg.toml` (new) — RSPRecomp config for the M_NJPEGTASK microcode.
* `tools/n64map.py` (new) — offline guest↔ROM address map (config.yaml,
  config-bank*.yaml, `bank_funcs.inc`, `bank_overlays.cpp`, `build/*.elf`).
* `tools/guestmap.py` (new) — `n64map` CLI: address → segment/record/unit,
  function-entry check, shared-RAM warning.
* `tools/runlog.py` (new) — one-screen run summary + `--check` assertion.
* `tools/rdram.py` — new `image` and `diff` subcommands (and `Dump.path`).
* `tools/watch.sh` (new) — lldb watchpoint on `rdram + (guest - 0x80000000)`.
* `app/CMakeLists.txt` — `-fno-omit-frame-pointer` on the app, the recompiled
  libs and `librecomp`/`ultramodern` (not RT64).
* `RspFuncs/njpeg_ucode.cpp` (generated, gitignored) — the recompiled decoder.
* `app/CMakeLists.txt` — glob `../RspFuncs/*.cpp` into `ogrebattle64_rsp` and
  link it.
* `app/src/rsp.cpp` — dispatch on `task->t.ucode == 0x8009ED80`, behind
  `OGRE_NJPEG` (default: stub, unchanged behaviour).
* `app/src/bank_overlays.cpp` — also install the fatal-signal handler for
  `SIGFPE` (the null-build crash above arrives as a divide-by-zero and
  previously printed no guest chain).
* `Makefile` — `rsp-recomp` target.
* `.gitignore` — `RspFuncs/`.
* `docs/guides/rsp-microcode.md`, `docs/scenes.md`, `PLAN.md`,
  `docs/DECISIONS.md`, `docs/README.md`, `AGENTS.md` — record updated.
* Probes, **all reverted**: the input/output dump around the decoder
  (`// probe46` in `app/src/rsp.cpp`, replaced by `make rsp-recomp` + a clean
  rebuild), the loop/entry logging (`// probe46` in the generated
  `RspFuncs/njpeg_ucode.cpp`, dropped by `make rsp-recomp`), the game-state
  probe (`// probe46` in `app/src/rsp.cpp`), and a temporary vendored edit to
  `librecomp/src/rsp.cpp` §4c. `grep -rl "probe46\|rsp-probe" app/src/ RspFuncs/`
  is empty and `git status --short` lists only the intended files.
* No runtime/vendored change remains in the tree.
