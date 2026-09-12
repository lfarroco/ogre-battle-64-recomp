# Session 31 — the intro crash is fixed: the game keeps rendering (205 display lists, ~28fps)

Goal: the title intro must show the 3D block fall into the middle of the soldiers
and become the N64 logo. Session 30 populated the 13 texture records at
`base+0x6C0` but the intro still died at display list 1. This session found the
actual cause, fixed it in N64Recomp, and the game now renders continuously past
the old wall.

## 0. State at the start (session 30's wall reproduced)

```sh
OGRE_NO_AUDIO=1 OGRE_SCENE_TRACE=1 OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64
# -> [renderer] display list 1 at t=465ms, then SIGSEGV (exit 139) at ~845ms
```

`lldb` put the crash at `func_801A1FCC + 4443` (= `0x801A22F4`,
`lw $v1, 0x8($s3)`), thread "N64 Thread 4", called from `func_80177E08`
(state 9 draw). The 13 records *were* created (`OGRE_SCENE_TRACE` showed
`ptr=0x801BCE60.. flag=255`), so session 30's `func_801A1034` override is real.
The crash's `$s3` was `0x80000318`.

## 1. The real cause: two functions share one stack frame

Temporary `fprintf` instrumentation in the regenerated code (`func_801A1FCC`,
`func_801A1170`, `func_80198ECC`, `func_801989AC`) gave this, in order:

```
[1fcc] after 801A1170 sp=800C22B0 s3=800A81C0     <- fine
[8ec] ENTER sp=800C2290
[9ac] ENTER sp=800C2268
[9ac] EXIT  sp=800C21A0                            <- frame NOT unwound
[8ec] EXIT  sp=800C2258                            <- 0x38 low
[1fcc] after 801A1170 sp=800C22A0 s3=00000000      <- 0x10 leaked, $s3 = 0
[1fcc] after 801A1170 sp=800C2290 s3=00000310      <- another 0x10, $s3 corrupted
```

The frame dump at the crash showed `saved $s3` at call-entry slot `sp+0x2C`
held `0x80000310`: the high halfword of `0x800A81C0` had been overwritten with
`0x8000` by a 16-bit store aimed at the stack slot `0x10` lower — i.e. every
caller was reading its saved registers from the wrong depth.

`func_801989AC` is the leaker. `config.toml` had it as `0x520` (correct, its
real epilogue at `0x80198EC4` restores `$s3` from `0x9C($sp)` and adds `0xB8`, so
the `func_80198D28` symbol inside it is a *false split*). But the dump also
showed a **second** compiled body, `func_80198D28` (emitted from `elf.cpp`'s
symbol, reached by `jal 0x80198D28` from `func_80178920`/…), whose first
instruction is `L_80198B40` and which ends with `func_801989AC`'s epilogue
(`lw $ra, 0xB4($sp)` / `addiu $sp, 0xB8`). `func_80198D28` therefore
**computed with a frame that was never allocated**: `$ra`/`$s3` were restored
from garbage and `$sp` came back 0x38 off. That is the leak the chain inherited.

So the session-30 override *size* was right; what was missing is that
N64Recomp kept compiling (and calling) the split symbol as a standalone
function. That is the bug class fixed below.

### 1b. Correcting the `func_80197C94` override

Session 30 had extended `func_80197C94` to `0x948`, which swallowed
`func_801980A0` (a real function: own prologue at `0x801980A0`, own epilogue at
`0x801985D8`). Under that override `func_801980A0` was emitted as a ~0x10-byte
stub and its body was compiled as a label inside `func_80197C94`, so every
caller ran with an unbalanced frame. The override is now the real size,
**`0x40C`** (the gap to the next symbol; `func_801980A0` keeps its own body).

The code at `0x80197C94`'s tail (`j 0x801984BC`, `j 0x801985D8`) still jumps
into `func_801980A0`'s body — that is genuine shared code: `0x801984BC …>
0x801985D4` falls through into the shared epilogue at `0x801985D8`, which
restores exactly the `0x10`-byte frame that `0x80197C94` allocated. So the goal
is not to move the boundary but to make those jumps land in a body that
unwinds the frame — which is what §2 does.

## 2. The recompiler fixes (all in `tools/N64Recomp`, see `n64recomp-ob64.patch`)

### 2a. Calls into a size-overridden body are redirected to that body

A `jal`/`j` whose target lies inside a function whose size was set by
`config.toml`'s `function_sizes` is a continuation of that body, not a function
of its own: it has no prologue and it shares the parent's epilogue. Compiling or
calling it standalone is always wrong.

* `include/recompiler/context.h` — `Function::has_size_override`, set in
  `src/elf.cpp` when the size override is applied.
* `src/recompilation.cpp` — `find_containing_size_override()` (innermost match
  wins) + new `JalResolutionResult::SizeOverride`. The `jal` case tail-calls the
  containing function (`func_X(rdram, ctx); return;`) so its epilogue unwinds
  the parent's frame; the branch case does the same through
  `print_func_call_by_address`.
* `src/main.cpp` — discovered statics are registered in
  `context.functions_by_name` so a later pass can tell that a body already
  exists at an address.

Result at recomp time (24 sites), e.g.

```
[Info] Jal in func_80198ECC to 0x80198D28 redirected into func_801989AC
[Info] Branch in func_80198D28 to 0x80198B40 redirected into func_801989AC
[Info] Jal in func_8017B9C8 to 0x801A103C redirected into func_801A1034
```

`func_80198D28` and `static_16_80198B40` still exist as emitted bodies but are
now referenced zero times, so the broken copies never run. Session 30's
`0x948`-style "extend the parent over the split" workaround is no longer needed
anywhere.

### 2b. Bodies that fall through into the next function now tail-call it

`static_16_801984BC` ended with a plain `sw` — no `return` — because its last
instruction falls through into the discovered static `static_16_801985D8` (the
shared epilogue). N64Recomp emitted the store and closed the function, so the
0x10 frame was never unwound. `recompilation.cpp` now detects a last
instruction that is not a control transfer (`j`/`b`/`jr`/`syscall`) and:

* calls the real body at the next address when one is known, else
* registers the next address as a static (`static_funcs_out`, the same list
  `print_branch` uses) and tail-calls it by name.

That emits `static_16_801985D8(rdram, ctx); recomp_trace_return(...); return;`.
1726 sites were emitted; they are inert wherever the preceding instruction is
already a `return` (the C compiler keeps them as unreachable code), and real
where a body genuinely ran off its end.

## 3. Result

```sh
make recomp                        # 24 redirects, 1726 fall-through tail calls
cmake --build build-app -j 8
OGRE_NO_AUDIO=1 OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64   # exit 0
```

| check | session 30 | now |
|---|---|---|
| run 8s | SIGSEGV at display list 1, t≈845ms | **exit 0**, 205 display lists |
| run 15s | — | **395 display lists**, last at t=14232ms (≈28fps after the 500ms boot) |
| run 20s | — | 640 display lists, `[renderer] display list 640 at t=22682ms` |
| run 28s (`OGRE_PROFILE=1`) | — | 810 display lists |
| late crash | SIGSEGV at t≈845ms | none until **t=96.7s / display list 2770**, then a streamed-overlay stub + NULL lookup (§4b); that is the Phase-4 boundary |
| per-frame content (`OGRE_DL_ANALYZE=1`) | — | dl 120: `cmds=1262 tri2=17 vtx=17 settimg=32`; steady state `cmds=1037 tri2=12 settimg=27` |
| scene records | 13 real pointers | still 13 real pointers; `phase`/`p710` advancing, `rec[10] x=124 y=91` mid-animation |
| `build-null`, `build-wasm` | clean | clean |

The state-9 draw (`func_801A1FCC`) now runs to completion every frame and the
display lists contain real geometry (12-17 triangles, 27-32 `SETTIMG`, 17
vertices per list), so the scene is no longer stuck on the boot blanking list.

### One-off oddity

A single 55s no-input run (no `OGRE_EXIT_AFTER_MS`, no profiler) sat at 150
display lists with all threads parked — the game idling at the title. It did not
reproduce in the profiled 28s run (810 dl) or the 20s run (640 dl). Worth a look
if it turns up again; it is an idle/stall question, not a crash.

### Proof frames: the whole intro runs

Captured from a 95s run with

```sh
OGRE_NO_AUDIO=1 OGRE_CAPTURE_PRESENT=/tmp/g3.png OGRE_CAPTURE_AFTER=2500 \
  OGRE_EXIT_AFTER_MS=95000 ./build-app/ogrebattle64
# -> /tmp/g3.png.<n>.ppm; sips -s format png
```

| file | shows |
|---|---|
| `docs/proofs/native-intro-impact.png` | t≈3.3s: twelve soldiers, impact smoke, the block falling between them |
| `docs/proofs/native-intro-title.png` | t≈8.4s: the N64 logo over the "Ogre Battle 64 / Person of Lordly Caliber" title screen |

Both were confirmed by eye on this host: **soldiers appear, the block falls,
the block becomes the N64 logo, and the title screen follows.** Note
`OGRE_PRESENT_ALWAYS=1` on its own captures a **black** frame (RT64 then uploads
the RDRAM copy instead of the RDP render target, the session-27 finding), so
capture without it.

### Reported next work (from the same visual pass)

* **The "Licensed by Nintendo" / "ATLUS" / "QUEST" screens are wrong**: only
  the upper-left quarter of the image is visible and it is stretched over the
  whole screen. The N64 logo on the title screen is correct, so this is specific
  to those full-screen still images — a texture/VI source-size or
  texrect-vs-triangle issue, not the scene.
* **The game crashes after a while** — reproduced (see §4b).
* Title-scene artifacts (minor, accepted for now).

### 4b. The late crash: a streamed-overlay stub, then a NULL lookup

An unattended 96.7s run (no input) reaches **display list 2770** and then aborts
(exit 134). The end of the log:

```
[overlays] streamed function stub called @ 0x801AD5C0 (not yet loaded)
Failed to find function at 0x00000000
  #0 get_function
  #1 func_80076AE8 + 2242
  #2 func_80072398 + 1097        <- main loop
  #3 func_8008AFE0 + 502
libc++abi: terminating
```

`0x801AD5C0` is one of the app's log-and-return stubs for a function outside the
linked ELF (`app/src/overlays.cpp`), i.e. overlay C's upper reach —
`func_801AD368`/`func_801AD558` are in the same region and *are* compiled
(they appear in the recompiler's fall-through log), so this looks like the next
addresses in that region still being stubbed rather than a real bank switch. The
stub returns without doing the work, and the caller then calls through a
function pointer it never filled (the `0x00000000` lookup), which aborts. This is
the Phase-4 boundary, not a regression from the session-31 fixes: the title and
intro run indefinitely, and the abort needs a deliberate menu action or a long
idle to reach.

**Lead:** register the remaining `0x801AD...`/overlay-C addresses (or confirm
whether `0x801AD5C0` is genuinely a different bank) in `app/src/overlays.cpp`,
then re-run to find the next wall.

## 4. Diagnostics used this session (all temporary, removed)

* `fprintf` at the entry/prologue/every call-return/epilogue of
  `func_801A1FCC`, `func_801A1170`, `func_80198ECC`, `func_801989AC` printing
  `ctx->r29`/`ctx->r19`, plus a dump of 24 stack words at the failing
  dereference. This is what showed the frame depth defect.
* `N64RECOMP_DEBUG_OVERRIDE` / `N64RECOMP_DEBUG_FALLTHROUGH` in the recompiler
  (printed the applied override ranges and each fall-through's last
  instruction). Both removed; `RecompiledFuncs/` is regenerated clean (`make
  recomp`, no `DBG` markers).

The lldb recipe from session 30 still works:

```sh
OGRE_NO_AUDIO=1 lldb -b -k "bt 8" -k "register read rsi rbx r14" -o run -- ./build-app/ogrebattle64
```

## 5. Verified

| check | result |
|---|---|
| `make recomp` | clean, deterministic; 24 `redirected into`, 1726 `Fall-through` |
| `cmake --build build-app -j 8` | clean |
| `cmake --build build-null -j 8` | clean |
| `cmake --build build-wasm -j 8` | clean |
| 8s / 9s / 15s / 20s / 28s / 55s runs | no SIGSEGV, always exit 0 when `OGRE_EXIT_AFTER_MS` set |
| 15s rate | 395 display lists, last at 14232ms |
| display-list content | 12-17 triangles, 27-32 textures, 17 vertices per list |
| redirects de-duplicated | `func_80198D28` and `static_16_80198B40` have zero references |

## 6. Files changed (this session)

* `tools/N64Recomp/include/recompiler/context.h` — `Function::has_size_override`.
* `tools/N64Recomp/src/elf.cpp` — set it when a size override is applied.
* `tools/N64Recomp/src/recompilation.cpp` — `find_containing_size_override`,
  `JalResolutionResult::SizeOverride` for `jal`/branch, and the fall-through
  tail call.
* `tools/N64Recomp/src/main.cpp` — register discovered statics by name.
* `n64recomp-ob64.patch` — regenerated.
* `config.toml` — `func_80197C94` back to its real size `0x40C` (session 30's
  `0x948` swallowed `func_801980A0`); comment recording why.
* `PLAN.md`, `docs/DECISIONS.md`, this file.

## 7. Next

1. **The "Licensed by Nintendo" / "ATLUS" / "QUEST" screens render only the
   upper-left quarter, stretched over the whole screen** (reported from a visual
   pass; the N64 logo and the title screen are correct). Start from the
   display-list analyzer (`OGRE_DL_ANALYZE=1`) and the texture/VI source-size
   path: those screens are a single full-screen image, so the likely causes are
   a wrong `SETTIMG`/`SETTILE` source size, a texrect whose destination is
   quartered, or the VI `width`/`hRegion` being taken as half the real size.
2. **The late crash is reproduced and localised** (§4b): 96.7s reaches display
   list 2770, then `streamed function stub called @ 0x801AD5C0 (not yet loaded)`
   and `Failed to find function at 0x00000000` aborts the process. Register the
   remaining overlay-C addresses in `app/src/overlays.cpp` (or determine whether
   `0x801AD5C0` is a different bank) and re-run.
3. The idle oddity in §3 (55s run parked at 150 dl) if it recurs —
   possibly the same cause as the crash.
4. Still open from session 29/30: the runtime must not block the game start
   thread on `SDL_OpenAudioDevice` (`OGRE_NO_AUDIO=1` is a workaround).
