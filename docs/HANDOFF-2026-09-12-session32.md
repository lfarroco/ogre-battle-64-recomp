# Session 32 — the publisher screens render: a mis-named `osViSetMode`; the late crash is an overlay bank swap

Goal (from session 31 §7): the "Licensed by Nintendo" / ATLUS / QUEST stills
must render full-frame, and the ~95s crash must be understood.

Both are done: the stills are correct, and the crash is localised to a
streamed-overlay **bank swap** (not a missing overlay-C address), with the exact
ROM/RAM pieces and the calling code path.

## 0. State at the start

```sh
OGRE_NO_AUDIO=1 OGRE_EXIT_AFTER_MS=15000 ./build-app/ogrebattle64   # exit 0, ~395 display lists
OGRE_NO_AUDIO=1 OGRE_EXIT_AFTER_MS=170000 ./build-app/ogrebattle64  # exit 134 at t≈95s
```

The session-31 proofs showed the intro (soldiers, falling block, N64 logo,
title) correct, and the publisher stills drawing only their upper-left quarter.

## 1. The publisher-screen bug: the game's VI mode never reached the runtime

### 1a. The RDP output is correct — the VI scanout is not

`OGRE_CAPTURE_TARGET` dumps the framebuffer the VI renderer samples. At present
300 it is a **perfect 640x480 "Licensed by Nintendo" screen** (R16G16B16A16
format, via `python3` half-float decode). The presented frame is the top-left
320x240 of it. So the RDP work is right; the presentation reads the wrong
region — exactly what RT64's `VideoInterfacePS.hlsl` does when
`videoResolution/textureResolution` is `(0.5, 0.5)`:

```hlsl
const float2 LowerRight = gConstants.videoResolution / gConstants.textureResolution;
```

`textureResolution` is the 640x480 colour target; `videoResolution` is
`vi.fbSize()`, i.e. the VI's `width` and region. The VI trace showed
`width=320 hStart=0x006C02EC xScale=0x200` for the whole run — which is the
runtime's `dummy_vi_mode()`, not a game mode.

### 1b. Why: `0x80095820` is `__osViSwapContext`, and the real `osViSetMode` was unnamed

Session 10 already found that the function `symbol_addrs.txt` calls
`osViSetMode` at `0x80095820` is really `__osViSwapContext`: it reads
`__osViNext->modep/framep` and writes the VI MMIO registers, ignoring `$a0`.
The **real** `osViSetMode` is `func_800955C0`:

```
800955C0  addiu $sp,$sp,-0x18
800955C4  sw $s0,0x10($sp)
800955CC  jal func_800996D0        ; __osDisableInt
800955D0  addu $s0,$a0,$zero
800955D4  lui $a0,%hi(D_800ABBD4)  ; __osViNext
800955D8  lw $a0,%lo(D_800ABBD4)($a0)
800955DC  sw $s0,0x8($a0)          ; __osViNext->modep = modep
800955E0  lw $a1,0x8($a0)
800955E4  addiu $v1,$zero,0x1
800955E8  sh $v1,0x0($a0)          ; state = 1
800955EC  lw $v1,0x4($a1)
800955F0  sw $v1,0xC($a0)          ; control = modep->comRegs.ctrl
800955F4  jal func_80099740        ; __osRestoreInt
```

It is called with the game's `OSViMode` table entries:

- `func_80072738(screen_mode)`: NTSC → `D_800AB960` (320x240) / `D_800AB9B0` (640x480)
- `func_8007307C`: mode 2 → `D_800AB9B0`, mode 1 → `D_800AB960` / `D_800ABA00`
- `func_80088C50` / `func_80093CB0` (init): `D_800AA7E0 + index * 0x50`

`D_800AB9B0` is the hi-res mode: `ctrl=0x324E` (16-bit, interlaced),
`width=640`, `xScale=0x400`, `yScale=0x800`, `fldRegs[0].origin=0x500`. Because
`func_800955C0` was never named, it was compiled verbatim and the runtime never
saw the mode — it stayed on `dummy_mode` (which coincidentally matches the
320x240 mode's `width`/`hRegion`/`xScale`, hiding the bug on the intro).

### 1c. Fix

| file | change |
|---|---|
| `symbol_addrs.txt` | `func_800955C0` → `osViSetMode`; `0x80095820` → `__osViSwapContext` |
| `tools/N64Recomp/src/symbol_lists.cpp` | add `__osViSwapContext` to `reimplemented_funcs` so a declaration is emitted |
| `tools/N64ModernRuntime/librecomp/src/vi.cpp` | `__osViSwapContext_recomp` = no-op (the runtime VI thread's `update_vi` does the register swap; the game-side `__osViNext->modep` is never populated because `osViSetMode` is bridged, so running the game body would deref NULL) |
| `n64recomp-ob64.patch`, `n64modernruntime-ob64.patch` | regenerated (`git -C tools/… diff HEAD`) |

`make recomp` now emits `osViSetMode_recomp` at the 4 real call sites and
`__osViSwapContext_recomp` at the 2 swap-context sites. Re-split is required
because the symbol names come from the ELF:

```sh
tools/venv/bin/splat split config.yaml
make && make recomp && cmake --build build-app -j 8
```

`OGRE_DEBUG_VI=1 OGRE_DEBUG_TRACES=1` now shows the game's modes arriving and
the geometry changing:

```
[vi-debug] osViSetMode(0x800AA880) ctrl=0x0000311E width=320 hStart=0x006C02EC xScale=0x00000200 ...
[vi-debug] osViSetMode(0x800AB960) ctrl=0x0000311E width=320 ...
[vi-debug] osViSetMode(0x800AB9B0) ctrl=0x0000324E width=640 hStart=0x006C02EC xScale=0x00000400 ...
[vi-debug] VI change width=640 hStart=0x006C02EC xScale=0x00000400 vStart=0x002301FD yScale=0x02000800 origin=0x00026100 ctrl=0x0000324E
```

(The `OGRE_TRACE` macro additionally requires `OGRE_DEBUG_TRACES=1`; `OGRE_DEBUG_VI`
alone only opens the `debug_trace_vi()` gate.)

## 2. Result

| check | session 31 | now |
|---|---|---|
| "Licensed by Nintendo" | top-left quarter, stretched | full 640x480 still, 4:3 pillarboxed — `docs/proofs/native-licensed-screen.png` |
| ATLUS | quarter / noisy | beige full-frame with centred logo — `docs/proofs/native-atlus-screen.png` |
| QUEST | quarter | white full-frame with the QUEST logo — `docs/proofs/native-quest-screen.png` |
| intro / title | correct | unchanged |
| 15s run | 395 dl at 14232ms | exit 0, **400 dl at 14266ms** (~28fps) |
| 8s / 20s runs | exit 0 | exit 0 (200 dl / 540 dl) |
| `make recomp` | 24 `redirected into`, 1726 `Fall-through` | 24 `redirected into`, **1723** `Fall-through` (3 fewer: `__osViSwapContext`'s body is no longer compiled) |
| `build-null`, `build-wasm` | clean | clean |

Capture recipe (unchanged; do **not** use `OGRE_PRESENT_ALWAYS`, it captures
black per session 27):

```sh
OGRE_NO_AUDIO=1 OGRE_CAPTURE_PRESENT=/tmp/p OGRE_CAPTURE_AFTER=270 \
  OGRE_EXIT_AFTER_MS=45000 ./build-app/ogrebattle64
# presents ~292 licensed, ~320 ATLUS, ~404 QUEST; sips -s format png /tmp/p.<n>.ppm
```

## 3. The late crash: confirmed to be an overlay **bank swap**

Session 31's lead asked whether `0x801AD5C0` is a different bank. It is.

### 3a. The caller

Instrumenting `get_function` with `ultramodern::debug_dump_call_chain` (new
diagnostic in `librecomp/src/overlays.cpp`) gives the exact path for both
lookups:

```
[snap] callchain t4 streamed-stub (3 deep): func_8008AFE0 → func_80072398 → func_800765D8
[snap] callchain t4 null-lookup  (3 deep): func_8008AFE0 → func_80072398 → func_80076AE8
```

`func_800765D8` does `jalr $a1` at `0x800766B4` with
`$a1 = *(0x800E7A40)` (`lui/lw` at `0x80076698`), an object-template callback
field; `func_80076AE8` then calls a NULL pointer. The `func_80076AE8 + 2242`
frame in the raw `backtrace_symbols` output is the host (x86) offset, not a
MIPS address — the lldb `bt` and the call-chain dump agree on `func_800765D8`.

### 3b. The DMA evidence

`OGRE_DEBUG_TRACES=1` logs every inline PI DMA (`[pi] inline DMA dram=… dev=…
size=…`). Immediately before the abort (t≈95s) the game loads a **new overlay
over overlay C** in exactly three pieces:

| RAM | ROM | size |
|---|---|---|
| `0x80197B90..0x8019EE50` | `0xE4910..0xEBBD0` | `0x72C0` |
| `0x8019EE70..0x801AD2B0` | `0xEBBD0..0xFA010` | `0xE440` |
| `0x801AD5C0..0x801B4CC0` | `0xFA600..0x101D00` | `0x7700` |

The third piece lands exactly on the stubbed address, and ROM `0xFA600` begins
with a normal prologue (`27BDFFD0 AFB10024 00808821` = `addiu $sp,$sp,-0x30` /
`sw $s1,0x24($sp)` / `addu $s1,$a0,$zero`), so `0x801AD5C0` is that overlay's
entry point. In the linked overlay C the same address is `D_801AD5C0`, a
mid-function label inside `func_801AD558` — i.e. the port is answering from
overlay C's `func_map` while the game has swapped banks.

Overlay C is `0x80197B90..0x801B8090` (ROM `0x1CE040`, size `0x229C0`, loaded 3
times earlier in the run); the new overlay reaches `0x801B4CC0` at least. The
two ranges overlap, so they cannot both be linked at their fixed RAM addresses
in one ELF.

### 3c. Consequence

This is the Phase-4 overlay boundary, not a regression: the stub returns
without initialising the object whose callback is `0x801AD5C0`, so the caller's
next function-pointer call is NULL and the process aborts. The fix is a
relocatable/overlay-section scheme with runtime load (and unload), plus
recompiling the overlays in the ROM gap `0x66E30..0x1CE040` (this one starts at
`0xE4910`); another `register_streamed_overlays()` entry cannot express it.

Unattended-run numbers after the VI fix: exit 134 at **t≈94.8s, display list
2770** (same wall as session 31's 96.7s), reached from
`func_8008AFE0 → func_80072398` (main loop).

## 4. Diagnostics added this session

| knob | effect |
|---|---|
| `OGRE_DL_DECODE=<n>\|all` | dumps the decoded F3DEX2 command stream of display list `n` (`gbi::decode_dl`: `SETTIMG`/`SETTILE`/`SETTILESIZE`/`LOAD*`/`TEXRECT`+`RDPHALF` halves/`FILLRECT`/`SCISSOR`/combiner/othermode). This is how the 640x480 destination and the 640x480 `SETCIMG` were confirmed. |
| `OGRE_DEBUG_VI=1` + `OGRE_DEBUG_TRACES=1` | logs every `osViSetMode` (mode pointer + geometry) and every VI geometry change |
| always-on | `get_function`'s stub/null paths now dump the calling thread's recompiled call chain (`debug_dump_call_chain`) |

## 5. Files changed (this session)

* `symbol_addrs.txt` — `osViSetMode = 0x800955C0`, `__osViSwapContext = 0x80095820`.
* `tools/N64Recomp/src/symbol_lists.cpp` — `__osViSwapContext` reimplemented.
* `tools/N64ModernRuntime/librecomp/src/vi.cpp` — `__osViSwapContext_recomp` no-op.
* `tools/N64ModernRuntime/librecomp/src/overlays.cpp` — stub/null call-chain dump.
* `tools/N64ModernRuntime/ultramodern/src/events.cpp` — `OGRE_DEBUG_VI` mode logging.
* `app/src/gbi.cpp`, `app/src/gbi.hpp` — `decode_dl`.
* `app/src/renderer.cpp` — `OGRE_DL_DECODE`.
* `asm/*.s` (re-split symbol renames only), `n64recomp-ob64.patch`,
  `n64modernruntime-ob64.patch`, `PLAN.md`, `docs/DECISIONS.md`, this file.
* `docs/proofs/native-{licensed,atlus,quest}-screen.png`.

## 6. Next

1. **Phase 4 — overlay sections.** Recompile the streamed overlays in the ROM
   gap (`0x66E30..0x1CE040`; the first swap starts at ROM `0xE4910`), link them
   as relocatable sections, and teach `app/src/overlays.cpp` (or the runtime) to
   swap a bank on the game's DMA: the load is observable as the inline-DMA hook
   in `librecomp/src/pi.cpp` (`set_pi_request_queue`), so a bank switch can be
   driven from the DMA (dest, size) rather than from the game's (unlinked)
   loader code. Overlay C and the next overlay overlap in RAM, so this cannot be
   done with fixed-address segments in one ELF.
2. Then re-run the unattended 170s run and find the next wall.
3. Open from sessions 29–31: the runtime must not block the game start thread on
   `SDL_OpenAudioDevice` (`OGRE_NO_AUDIO=1` is the workaround), and the
   idle-stall oddity in session 31 §3 (not reproduced in any run this session).
