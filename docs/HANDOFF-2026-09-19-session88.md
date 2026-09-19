# Handoff — 2026-09-19, session 88: the boot-Start save menu (scene `0x18`) renders

**Goal (developer):** *"in the retail version of this game, if you hold start
while it is booting, it allows you to access a special menu for saves. but in
this version the screen is just black."*

**Result: fixed.** Holding Start through the boot window now enters scene `0x18`
and draws the **Controller Pak Menu** — `Save / Load / Erase / Exit` over
`Game Notes` / `Pages`, with a note box — instead of staying on a black frame.
Proof: `docs/proofs/native-boot-start-controller-pak-menu.png`.

One line of `Makefile` changed (`0x8019D67C` added to `make recomp`'s
`cross_bank.py dispatch --only` list). No game code, no config override and no
probe.

---

## 1. The bug: the boot-Start branch, and a call into the wrong bank

### 1a. The branch is a single held-Start test in the boot init

`asm/1060.s` @0x800721D0–0x800721E8:

```
    /* 25D0 800721D0 3C02800E */  lui   $v0, %hi(D_800E79B0)
    /* 25D4 800721D4 944279B0 */  lhu   $v0, %lo(D_800E79B0)($v0)
    /* 25D8 800721D8 30421000 */  andi  $v0, $v0, 0x1000
    /* 25DC 800721DC 14400002 */  bnez  $v0, .L800721E8
    /* 25E0 800721E0 24020018 */   addiu $v0, $zero, 0x18
    /* 25E4 800721E4 24020009 */  addiu $v0, $zero, 0x9
  .L800721E8:
    /* 25E8 800721E8 3C01800F */  lui   $at, %hi(D_800E8214)
    /* 25EC 800721EC 0C05E420 */  jal   func_80179080
    /* 25F0 800721F0 A4228214 */   sh   $v0, %lo(D_800E8214)($at)
```

`0x1000` is Start in libultra's pad report; `D_800E79B0` is the controller-0
word the boot's own poll fills (`func_8007297C`'s loop writes it at
`0x80072A58`). Held Start → pending scene **`0x18`**; otherwise the intro
**`0x09`**. Scene `0x18`'s descriptor is **`0x8018FDC0`** (mask `0x2`): enter
`func_8017BA60`, update `func_8017BB28`, hook `func_8017BB54` (`scenemap.py
scenes` row 24).

### 1b. The enter calls into the module it just DMA'd — and the call was bound elsewhere

`func_8017BA60` (`asm/40E80.s` @0x8017BA60) does, in order: `func_800900C0`
(module range), `func_80090010` (bss), **`func_8009DA50(ROM 0x712A0 → RAM
0x8019A7C0, 0x84B0)`** — the chunk-DMA — then `func_80073164` (gfx setup at
`0x8017BAFC`) and finally, at `0x8017BB04`:

```
    /* 51A04 8017BB04 0C06759F */  jal   func_8019D67C
```

`0x712A0` is **unit H** (the form/UI module, the same one scene `0x07` streams;
record table `app/src/bank_funcs.inc`: `{ 0x000712A0, 0x8019A7C0, 0x000084B0,
0x801A2C70, kH_bankRec07Functions }`), and `0x8019D67C` is its menu initialiser
`func_ovlH_8019D67C`. On retail the DMA makes that address unit H's code, so the
`jal` is an ordinary call into the freshly-loaded module.

In the port it was **not**:

* `config.toml` line 40 carries `{ name = "func_8019D568", size = 0xCB0 }`
  (session 30's fall-through closure: `func_8019D568` is `0x114` and falls
  through into `0x8019D67C`, `0xB9C`; `0x114 + 0xB9C = 0xCB0`). The override
  therefore **swallows `0x8019D67C`**, and N64Recomp treats it as a body
  interior.
* The generated C at `0x8017BB04` (`RecompiledFuncs/funcs_14.c`, before this
  session) was the redirect-plus-early-`return` shape:

  ```c
  // 0x8017BB04: jal         0x8019D67C
  // 0x8017BB08: nop

  func_8019D568(rdram, ctx);          // the containing body, not the target
  recomp_trace_return(rdram, 7948);
  return;                             // abandons func_8017BA60's epilogue
  ```

  So the enter never ran unit H's initialiser, never checked its return value
  (the `beqz $v0` that would hand the menu exit to scene `0x09`), and returned
  with the caller's frame abandoned.
* The bodies really are different modules (raw ROM, 32 bytes):
  unit H `0x7415C` = `27bdffe8 afbf0014 0c05ef78 …` (a real prologue,
  `addiu sp,sp,-0x18`); the main ELF's `.streamedC` `0x1D3B2C` =
  `ac510000 ac530004 …` (prologue-less: `sw $s1,0($v0)` — a continuation body
  that needs the caller's registers). Session 17's "menu 0x18 crashes storing
  through `$v0=2`" was this same mis-binding seen from the other side.

### 1c. Reproduced

Before the fix, with Start held continuously (`OGRE_TAP_MS=150` — a 150 ms
period with a 150 ms hold is a press on every poll, i.e. a hold):

```
[scene] t=8938ms id=0x0018 descriptor=0x8018FDC0 mask=0x00000002
[bank] loading overlay record rom=0x0712A0 ram=0x8019A7C0 size=0x84B0 (45 functions)
[dma-trace] rom=0x0712A0 ram=0x8019A7C0 first=594 last=594 chunks=1
```

then no further scene change and **every captured frame fully black**
(49/49 `.ppm` files: mean 0, max 0). The DMA and the scene entry are the
game's own and are correct; only the call is wrong.

## 2. The fix

`Makefile`, `recomp`'s dispatch line: add `0x8019D67C` to the `--only` list
(`cross_bank.py` rewrites the `jal` comment's site to
`LOOKUP_FUNC(0x8019D67C)(rdram, ctx)` and repairs the tail to call-and-continue
— both shown below). This is the sessions-55/60 mechanism: the address is
registered by bank unit H (`app/src/bank_funcs.inc`:
`{ 0x8019D67Cu, func_ovlH_8019D67C }`), so the runtime bank map resolves it to
whichever module is resident, and here that is unit H, which the enter itself
just DMA'd in.

The `func_8019D568` override is deliberately **left in place**: `0x8019D67C` is
a genuine continuation of that body (no `jr $ra` between them), and the *only*
`jal` to `0x8019D67C` in the whole ROM is the site dispatched here. A future
session that trims the override (the session-39/86 pattern) must keep the
dispatch, or the call will bind to `.streamedC`'s prologue-less body instead.

Generated code after (`RecompiledFuncs/funcs_14.c`):

```c
// 0x8017BB04: jal         0x8019D67C
// 0x8017BB08: nop

LOOKUP_FUNC(0x8019D67C)(rdram, ctx);
    goto after_33;
// 0x8017BB08: nop

after_33:
// 0x8017BB0C: beq         $v0, $zero, L_8017BB1C
```

`make recomp` reported `dispatched 0x8019D67C (1 site(s))` and
`14 tail-call site(s) repaired to call-and-continue`.

There are three call sites from resident code into unit H on this path; the
other two (`func_8017BB28` → `0x8019C69C`, `func_8017BB54` → `0x8019C4A8`) were
already in the `--only` list from session 55 and were already `LOOKUP_FUNC`.

## 3. Verification

Rebuilt with the missing **Metal toolchain** component
(`xcodebuild -downloadComponent MetalToolchain`; the shader sources are newer
than the cached `build-app` shader outputs, so `cmake --build` needs it), then
ran the RT64 build with Start held:

```sh
OGRE_TAP_MS=150 OGRE_SPEED=4 OGRE_EXIT_AFTER_MS=60000 OGRE_SCENE_LOG=1 \
  OGRE_PRESENT_ALWAYS=1 OGRE_CAPTURE_PRESENT=/tmp/menufix OGRE_CAPTURE_AFTER=40 \
  OGRE_CAPTURE_EVERY=100 OGRE_COVER=/tmp/cover-fix.txt \
  ./build-app/ogrebattle64 assets/ogre64.z64
```

Observed (A/B against the pre-fix run above):

| measurement | before | after |
|---|---|---|
| scene `0x18` entered | yes (`t=8938 ms`) | yes (`t=2435 ms`) |
| unit H DMA (`rom=0x712A0`) | yes | yes |
| `0x8019D67C` covered | never | **once** (`func_ovlH_8019D67C`) |
| display lists submitted | (enter returned early) 0 from this scene | **436** |
| captured frames non-black | 0 / 49 | **93 / 93** (mean 83, max 255) |

The rendered screen is the **Controller Pak Menu**: title plate
`Controller Pak Menu`, tabs `Save / Load / Erase / Exit`, headers
`Game Notes` / `Pages`, a four-row note list (`1:`..`4:`) with `Left` and a
scrollbar, and a note box (`Note 1` / `Not an Ogre Battle 64 note.` — the pak is
one the port formatted fresh in `build-app/saves/ogrebattle64-us-rev1.mpk`, so
its note 1 is not an OB64 note). `docs/proofs/native-boot-start-controller-pak-menu.png`
is frame 4700 of that run.

Also run: the null build (`build-null`, freshly linked against the same
recompiled code) with the same schedule — entered `0x18` at `t=1369 ms`, update
hook `0x8017BB28` / `0x8019C4A8` / `0x8019C69C` each 4396 times, 30 display
lists, no stubs. `make bank-recomp` was not needed: the fix touches neither the
ELF nor the bank units.

**Not re-verified this session** (unchanged by the fix, pre-existing on this
tree): `make cross-bank-check` was not re-run to completion — the tool takes
minutes here and the change only adds one `--only` target; the developer's usual
`make recomp && make bank-recomp && build-app + build-null` battery applies.

## 4. What this also answers

* **The session-65 §9 "puzzle" is resolved.** It read `0x8018FDAC` (scene
  `0x07`'s descriptor) plus `0x14`/`0x18`/`0x1C` and found "extra callback words
  nothing reads"; `0x8018FDAC + 0x14 = 0x8018FDC0`, which is **scene `0x18`'s own
  descriptor**. There are no unread descriptor words, and `func_8017BB28` is
  scene `0x18`'s update, not scene `0x07`'s `+0x18`.
* **`docs/scenes.md`'s open question "The `0x18` menu: which menu is it?"** is
  answered: it is the Controller Pak (save-data) menu, reached by holding Start
  through boot; the row-9 entry ("reached from a later scene") is corrected.
* **The `0x18` black screen and session 17's `func_8019D67C` "OOB store through
  `$v0=2`" are the same mis-binding.** Session 17 concluded the branch "must be
  unreachable on HW" — that inference is withdrawn; the branch is the developer's
  boot-Start menu and the crash was the port running `.streamedC`'s
  continuation body as a callable entry.

## 5. Open leads

* `cross_bank.py` has **no detector for this shape**: a `config.toml` size
  override that swallows an address which is (a) a registered bank function
  entry and (b) the target of a `jal` from main-unit code. `make recomp` prints
  `dispatched …`/`already dispatched` for the `--only` targets but nothing flags
  the un-listed ones. Session 86 built the analogous static detector for
  frame-leaking `jal`s; the same pass could list these (there will be a
  handful — the `0xCB0`-swallowed range contains several other unit H entries
  such as `0x8019D650`, `0x8019D8B4`, `0x8019DA00`).
* The fresh pak shows `Note 1` / `Not an Ogre Battle 64 note.` The note the
  game reads should probably be empty (`No Data`); whether the port's fresh
  `format_mempak` layout or the game's enumeration is at fault is unexamined.
  **Deferred on purpose (developer, end of session):** the screen is the
  *copy/backup* menu — *"that screen allows copying saves from the battery into
  the controller pak, and the inverse. this is a pc port, so it's low priority.
  will not be fixed now."* So this lead and the `Save`/`Load`/`Erase` flow are
  feature requests, not defects.

## 6. Files changed

* `Makefile` — `0x8019D67C` added to `recomp`'s `cross_bank.py dispatch --only`
  list, plus the dispatch-notes comment block (what the target is, why the call
  was mis-bound, why the override stays).
* `docs/scenes.md` — boot table row for the hold-Start menu; the analysis
  paragraph (the `0x800721DC` branch and the unit-H call); row 9's status;
  the open question answered.
* `PLAN.md` — status entry.
* `docs/DECISIONS.md` — durable decision entry.
* `docs/proofs/native-boot-start-controller-pak-menu.png` — new proof.
* `docs/HANDOFF-2026-09-19-session88.md` — this file.

No probes were added, so none had to be reverted; `grep -rl probe88
RecompiledFuncs/ Bank*Funcs/ app/` is empty by construction. `config.toml` is
**unchanged**.
