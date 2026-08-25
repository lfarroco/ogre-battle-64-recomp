# Handoff — 2026-08-25 (session 8): post-boot spin found & fixed (yield_self for poll loops); boot now reaches the title-screen display-list build

## TL;DR

- **The session-7 "N64 threads 1+3 spin" is diagnosed and fixed.** Thread 3
  (`func_80071EB0` → `func_80075BC0`, the game's system loop) sits in a **pure
  spin at `.L80075FB8`** waiting for the 16-bit state word `D_800C4C26`
  (`0x800C4C26`) to change from `0xFFFF`/`0xFFFD`. That word only changes when
  the boot state-machine callback `func_80072398` runs, which is driven by the
  game's own **event chain** (Thread 19 `func_80088F08` → dispatcher
  `func_800891A0` → Thread 4 `func_8008AFE0`), which in turn needs **VI-retrace
  messages** from the runtime. The VI thread enqueues them, but delivery is done
  by the cooperative-scheduler **drainer** (pri 5), which can never run because
  Thread 3's pure spin never yields → **deadlock** (same class as session 6's
  `func_80089A10`).
- **Fix: N64Recomp now emits `yield_self(rdram);` for "poll loops"** — backward
  conditional branches whose loop body has no function calls, no stores, and
  reads at least one memory location at a loop-invariant/constant address. This
  is a **general** fix for every `while (flag) ;` wait in the game (verified to
  hit ~17 loops across all segments), not a one-off. See `tools/N64Recomp`
  (gitignored; the diff is in `n64recomp-ob64.patch`).
- **Result: boot now passes the spin and runs much further** — through the whole
  overlay-D data-load stream, the controller path, the RSP task pipeline, and
  into **title-screen display-list building** (`func_801A1FCC`/`func_8019FC68`,
  overlay C). It then hits the **next crash** (see below).
- **Next crash:** `func_8019FC68` (overlay C) at `0x8019FFF4` stores through
  `0x803ffa7b + entry->[0x34]` where the graphics-object entry (0x90-byte
  records in the heap buffer that `D_801B81D0` points to, base `0x803fefa0`)
  has **garbage** in `[0x34]` (varies per run: `0xED100000` vs `0`) → address
  wraps past 32 bits → SIGSEGV. The array is heap-allocated and zeroed
  (`func_801A1A2C`), so the garbage is **written later** — root cause still
  open (leads below).

## The spin (diagnosis)

Thread 3's system loop (`func_80075BC0`) builds a 24-entry function-pointer
table at `D_800AF028..`, then runs the game's main event loop. After the boot
state machine finishes loading overlay D's data blocks (the `0x213A2E4`…
`0x21C3958` stream into `0x801BD930`), the loop reaches its idle wait:

```
.L80075F94:
    func_80089990(func_80072398)   # register the boot state-machine callback
    func_80089C50()                 # D_800C4BD8 = 0x80
.L80075FB8:                         # <-- SPIN
    lhu $v0, D_800C4C26
    beq $v0, $s1(0xFFFF), .L80075FB8
    lui $v0, 0x800C
    lhu $v0, D_800C4C26
    beq $v0, $s2(0xFFFD), .L80075FB8
    ...
```

`D_800C4C26` only changes when `func_80072398` (the callback) has run 13 times
with the boot state (`D_800AEF98`) at 2 or 3 — it then writes `0xFFFC`, which
breaks the spin. The callback is invoked by **Thread 4** (`func_8008AFE0`),
which waits on queue `D_800C4C28` for events dispatched by **Thread 19**
(`func_80088F08`, the audio/event thread) through `func_800891A0`. Thread 19
wakes on `0x29A` (VI retrace) / `0x29D` (AI) messages on `D_800E8B84`. The VI
thread (native) enqueues those every retrace, but the **cooperative-scheduler
drainer** (pri 5) that delivers them can't run because Thread 3's pure spin
(branch targets the loop head, so N64Recomp's `pause_self` heuristic — which
only fires for self-branches — misses it) never yields.

## The fix (N64Recomp `yield_self` for poll loops)

`tools/N64Recomp/src/recompilation.cpp` (`print_branch`, conditional branches):

- New helper `is_yield_poll_loop(head_index, body_end, branch_target)` scans the
  loop body `[head, branch+delay)`:
  - bails on any `jal`/`jalr` (function call) or store (`sw/sh/sb/sd/…`);
  - classifies each instruction's GPR dest and whether it's a constant write
    (`lui`);
  - a load is a **poll** if its base register is loop-constant at the load site
    (cyclic last-write is a constant write, or the register is never written in
    the body → loop-invariant pointer).
- If a poll load exists, emit `yield_self(rdram);` just before the branch's
  `goto L_target`.
- `yield_self` is `wait_for_external_message` + `check_running_queue` (one
  wait, returns) — already present in the runtime (`scheduling.cpp`); only a
  declaration was missing from `recomp.h`.

New `emit_yield_self()` on `Generator`/`CGenerator`/`LiveGenerator`
(`include/recompiler/generator.h`, `src/cgenerator.cpp`,
`LiveRecomp/live_generator.cpp`). The live generator emits nothing (unused by
this project).

Verified to add yields to ~17 loops across all segments, including
`func_80075BC0` (the spin), `func_8008BA50`/`func_800997F0`/
`func_800998E0`/`func_80099A50`/`func_8009AB50` (PI status polls),
`func_80081E48`, `func_800820AC`, `func_80083874`, `func_80094FD0`,
`func_80076430`, overlay-B `func_801734F4`/`func_8017F428`/`func_8017F4B0`,
overlay-C `func_801AA4DC`. Data loops (strlen, linked-list walks, memcpy-like)
are correctly **not** flagged because their load base is written by a non-constant
op in the loop.
## Verification

Rebuilt everything (`make` → `make recomp` → app). Boot now logs ~1433
`[pi] func_8008BC40` DMA calls (was: stuck after the last `0x21C3958` load),
the controller request/response path, the `osSpTaskStartGo type=2` pipeline,
and finally DMAs ROM `0x9528F6` → RDRAM `0x803FFCC0` (0xAB8 + 0x138 bytes —
looks like audio/graphics data) before crashing in `func_8019FC68`.

## Next crash (the new wall): `func_8019FC68` at 0x8019FFF4

Backtrace: `func_80072398` → `func_80177E08` (overlay B) → `func_801A1FCC`
(title-screen DL builder, overlay C) → `func_801A0E44` → `func_8019FC68`.

`func_801A0E44` iterates the graphics-object array that `D_801B81D0` points to
(entries 0x90 bytes; base in a heap allocation, observed `0x803fefa0`) and calls
`func_8019FC68(entry, 1, 0, 0)` for entries with `entry[0x4C] != 0`.

Crash site (recompiled C):
```
// 0x8019FFEC: lw $v1, 0x34($s1)        ; r3 = entry->[0x34]
// 0x8019FFF0: addu $v1, $s5, $v1       ; r3 = 0x803ffa7b + entry->[0x34]
// 0x8019FFF4: sw $v0, 0x0($v1)         ; MEM_W(0, r3) = func_8019ABC4() result
```
With a valid small `entry[0x34]` the address stays in the `0x803fxxxx` DL
buffer; with garbage (`0xED100000` in one run, `0` in another) it wraps to a
KUSEG address and the store lands in unmapped RDRAM → SIGSEGV. The
nondeterminism means the entry is **populated by an earlier step with a bad
value, or left uninitialized despite `func_801A1A2C` zeroing the buffer**.

Leads for the next session:
1. Find the writer of `entry[0x34]` (offset 0x34 in the 0x90-byte records).
   `func_801A1A2C` allocates (0x834 bytes via `func_80070F30`) + zeroes it
   (`func_80093380`); `func_801A18A4`/`func_801A1170`/`func_8019F25C` are
   called around it in `func_801A1FCC` and likely populate the records.
2. The data DMA'd just before the crash (ROM `0x9528F6` → `0x803FFCC0`,
   `0xAB8` bytes starting `0x00000AB8 0x4B555402 0x05001400 0x22555400…`)
   looks like **audio** data, not graphics — confirm whether the title-screen
   path is audio-init and the graphics array is filled from a table.
3. Re-examine whether splat's `nonmatching func_8019FC68, 0xF58` boundary is
   right: the ELF symbol spans `0x2C8C` bytes (`0x8019FC68..0x801A28F4`), and
   `func_801A0E44`/`func_801A1FCC` are inside that range (N64Recomp created
   them as tail-call "static function" splits). A wrong boundary could mis-split
   the DL builder and corrupt the record writes.
4. The KUSEG mapping hole: the runtime's `TO_PTR`/`MEM_W` assume KSEG0
   addresses (sign-extended `0xffffffff80xxxxxx`); a wrapped address like
   `0x6dc0fa7b` maps to `rdram + 0xEDC0FA7B` and faults. Even with correct KUSEG
   mapping this would still be a garbage address, so it's a symptom, not the
   cause.

## Known issue (unchanged): flaky renderer-init segfault

Native runs still occasionally segfault right after
`[renderer] RT64 renderer initialized (api=2)` (before any game code runs).
Under gdb (slower timing) it does not reproduce and boot proceeds. Re-run or
run under gdb to debug the boot path.

## Changes (repo, committed)

- `n64recomp-ob64.patch` — regenerated from the vendored `tools/N64Recomp`
  working tree (now includes the `yield_self` additions; apply with
  `git -C tools/N64Recomp apply ../../n64recomp-ob64.patch`).

## Changes (vendored, gitignored — re-apply on re-clone)

N64Recomp (`tools/N64Recomp`):
- `include/recompiler/generator.h` — `emit_yield_self()` on `Generator` +
  `CGenerator`.
- `src/cgenerator.cpp` — `emit_yield_self()` → `yield_self(rdram);`.
- `LiveRecomp/live_generator.cpp` + `include/recompiler/live_recompiler.h` —
  stub `emit_yield_self()`.
- `src/recompilation.cpp` — `is_yield_poll_loop` + yield emission in
  `print_branch` (see above).
- Rebuilt `tools/N64Recomp/build/N64Recomp`; re-ran `make recomp`.

N64ModernRuntime (`tools/N64ModernRuntime`):
- `N64Recomp/include/recomp.h` — declared `void yield_self(uint8_t *rdram);`
  (the implementation already existed in `ultramodern/src/scheduling.cpp`).

## How to reproduce

```sh
git -C tools/N64Recomp apply ../../n64recomp-ob64.patch   # if re-cloned
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4
make            # rebuild ELF
make recomp     # regenerate RecompiledFuncs with yield_self
cmake --build build-app -j2
timeout 60 ./build-app/ogrebattle64 assets/ogre64.z64 > /tmp/boot.log 2>&1
# or under gdb (avoid the flaky renderer-init crash):
gdb -batch -ex run -ex bt ./build-app/ogrebattle64
```

Expected: boot through the overlay loads + controller + RSP pipeline, then the
`func_8019FC68` SIGSEGV (host backtrace: `func_80072398` →
`func_80177E08` → `func_801A1FCC` → `func_801A0E44` → `func_8019FC68`).

