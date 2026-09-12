# Session 30 — the falling-block/N64-logo scene: why it was missing, and the wall behind it

Goal: the title intro must show the 3D block fall into the middle of the soldiers
and become the N64 logo. This session found (and fixed) the reason the whole
scene was absent, and isolated the next wall.

## 0. The port did not boot on this host (fix: `OGRE_NO_AUDIO=1`)

Every run wedged before a single recompiled function executed: RDRAM stayed all
zero, `[profile] 0 (tid,function) pairs`, no display lists. Instrumenting
`ultramodern::preinit` showed the game start thread blocked in
`init_audio()` → `SDL_OpenAudioDevice()`, which hangs indefinitely on this macOS
host (no usable output device). `app/src/sdl_platform.cpp` now accepts
`OGRE_NO_AUDIO=1` to skip opening a device and run silently:

```sh
OGRE_NO_AUDIO=1 OGRE_DL_TRACE=1 OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64
```

Without it nothing in this document is observable. This is a real robustness
bug (a blocking audio open on the game start thread kills the whole boot), not
just a test convenience.

## 1. Root cause of the missing scene: `func_801A1034` was a 2-instruction stub

Overlay C's state-9 init (`func_80177DA0` → `func_801A1A2C`) calls
`jal 0x801A1034`. Splat had emitted the *prologue* of `func_801A103C`
(`addiu $sp,-0x30` / `sw $s1,0x14($sp)`) as its own symbol, so N64Recomp
compiled `func_801A1034` as a 2-instruction function that falls off the end and
returns immediately. The 13 texture records at `scene_base+0x6C0` — the 11
soldier/object records plus record 10 (the object the intro script animates
downward = the fall) — were therefore never created (`cfg`/`func_80070F30`
buffer stayed zero, every `+0x4` draw flag 0).

`config.toml` now carries `{ name = "func_801A1034", size = 0x13C }`. With it,
`OGRE_SCENE_TRACE=1` shows all 13 records created with real object pointers and
`flag=255`, e.g. record 10 (`base+0x760`) at `x=124 y=-144` moving per the
script.

## 2. A whole class of splat splits in overlay C (19 more overrides)

The same failure mode affects every symbol splat cut out of the middle of a
function: the compiled function returns without unwinding its frame, so callers
get a clobbered `$sp` / s-register. `config.toml`'s `function_sizes` list now
covers every such symbol in overlay C. The sizes were computed from
`asm/1CE040.s` by fixpoint over two rules:

* **fall-through**: the last instruction is not a control transfer → absorb the
  whole next symbol (and repeat);
* **delay-slot / forward branch**: the last instruction is a branch whose delay
  slot, or whose forward target (e.g. a shared epilogue), lies in the next
  symbol → extend over it.

Both closures were computed and the **larger** size used (a superset only adds
unreachable code; the executed path is unchanged). Sizes: `func_801989AC 0x520`,
`func_8019A7C0 0x404`, `func_8019AF5C 0x118`, `func_8019B300 0x3F8`,
`func_8019C188 0x44C`, `func_8019D568 0xCB0`, `func_801AB76C 0x22C`,
`func_801AFAF4 0x5E0`, `func_801B00D0 0x544`, `func_801B7F14 0x154`,
`func_80197C94 0x948`, `func_8019C5D4 0x5D8`, `func_801AB568 0x1E0`,
`func_801AB740 0x30`, `func_801AFC2C 0x4A8`, `func_801B8068 0x2C`, plus the two
pre-existing ones. `make recomp` stays deterministic (14 `yield_self` sites).

## 3. The remaining wall: fall-through into a discovered shared epilogue

The intro still SIGSEGVs at display list 1, but *later* than before: inside
`func_801A1FCC` (state 9's draw) at the tail

```c
// 0x801A22F4: lw $v1, 0x8($s3)          ctx->r3 = MEM_W(ctx->r19, 0X8);
```

with `$s3` (`ctx->r19`) = `0x310` instead of the valid `0x800A81C0` it was
entered with. Chain and evidence (temporary canaries, since removed):

```
func_801A1FCC  s3 = a0 = 0x800A81C0
  -> func_801A1170  after 0x801A2184: s3 0xFFFFFFFF800A81C0 -> 0x0
       -> func_80198ECC
            -> func_801989AC   save: sp=0xC21B0  restore: sp=0xC21A0  slot=0x0
                 -> func_80197C94  leaks exactly 0x10 of stack
```

`func_80197C94` does `addiu $sp,$sp,-0x10` and never restores it: its shared
epilogue is at `.L801985D8` (rom `0x1CEA88`, `lw $s3,0xC($sp)` … `jr $ra` /
`addiu $sp,$sp,0x10`). That label is jumped to from eight places, so N64Recomp
registers it as a *discovered* function (`static_16_801985D8`). Consequently:

* extending `func_80197C94` over the label does not help — N64Recomp still ends
  its body at the discovered function boundary;
* the path actually taken goes `.L801981F0 → .L801984BC`, and
  `static_16_801984BC` **falls through** into `static_16_801985D8`. N64Recomp
  emits `static_16_801984BC` ending with a plain `sw` and no `return`
  (`RecompiledFuncs/funcs_14.c`), so the trailing epilogue never runs and the
  0x10 frame leaks — which then makes `func_801989AC` restore its saved `$s3`
  from the wrong stack slot, and `func_801A1FCC` eventually dereferences a wild
  pointer.

### Proposed fix (recompiler, next session)

In `tools/N64Recomp/src/recompilation.cpp`, when a function's body ends without
a control transfer (falls through) and the next address is another known
function (declared *or* discovered static), emit a tail call to it plus
`return` instead of just ending the body. That is the mirror of the existing
`print_branch` tail-call handling a few lines above (`recompilation.cpp`
~line 368), and it fixes this whole class globally rather than one symbol at a
time. `n64recomp-ob64.patch` must be regenerated afterwards.

## Diagnostics added this session (kept)

| env | effect |
|---|---|
| `OGRE_NO_AUDIO=1` | skip `SDL_OpenAudioDevice` (otherwise the boot wedges) |
| `OGRE_SCENE_TRACE=1` | once per 200 ms, dumps `D_800E8214`/`D_800C4C26`, the scene base (`0x801B81D0`), all 13 `base+0x6C0` records (ptr/flag/x/y), `base+0x790/794/810/82C/830`, `D_801BA70C/710/700/701/72C`, `D_801977E8`, the boot scene mask `0x80190F30`, and the state-10 object's `0x1114/0x1116/0x1118`. All plain RDRAM reads (word-swapped layout per `MEM_W`/`MEM_HU` in `recomp.h`), so no recompiled code needs patching. |

## Repro

```sh
make recomp                       # config.toml function_sizes -> RecompiledFuncs
cmake --build build-app -j 8
OGRE_NO_AUDIO=1 OGRE_SCENE_TRACE=1 OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64
# -> records now created (session-30 fix), then SIGSEGV at dl 1 in func_801A1FCC
```

Crash localisation recipe that worked (repeat for any wild register):

```sh
OGRE_NO_AUDIO=1 lldb -b -k "register read rsi rbx r14" -k "bt 6" -o run -- ./build-app/ogrebattle64
```

## Verified

| check | result |
|---|---|
| `make recomp` | clean, 14 `yield_self` sites (unchanged) |
| `cmake --build build-app -j 8` | clean |
| RDRAM after boot (with `OGRE_NO_AUDIO=1`) | game state present, scene base set, 13 records filled |
| records `flag`/`ptr` | 11 × `flag=255` + record 10 `flag=255` (fall), records 11/12 `flag=0` |
| display lists | 1 then SIGSEGV in `func_801A1FCC` (wild `$s3`), wall isolated to §3 |

## Files changed (this session)

* `config.toml` — `func_801A1034` + 19 split-function sizes (see §1/§2).
* `app/src/sdl_platform.cpp` — `OGRE_NO_AUDIO` gate; also reset
  `audio_device = 0` after closing.
* `app/src/renderer.cpp` — `OGRE_SCENE_TRACE` dump.
* this file.
