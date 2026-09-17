# Handoff — 2026-09-16, session 61: the map's sprite *sizes* — the party marker and the cursor draw the wrong sprite-table entry

## Goal and result

**Goal (developer):** fix the map screen's sprites — the party marker (Magnus), the
cursor and the date panel are wrong. Session 60 had measured the party rect as
`144x23 px` sampling a `7x10`-texel window and called it "a sprite descriptor the
map never finished filling".

**Result: the mechanism is identified and one bug is proven; the fix is not
landed.** Two independent things are wrong on the map's per-unit sprites, and
they are *separate*:

1. **The destination rect comes from the sprite-table entry, and the party's
   entry is wrong.** The builder always emits `rect = (a0,a1)-(a0+W, a1+H)` for
   the entry it reads, so the rect's size names the slot the caller asked for.
   The party draw (`func_ovlM_801A2A7C`'s first builder call, ROM `0x81B50`)
   passes `a2=0xB` = slot 11 = **`(144,23)`**. The developer's retail reference
   (screenshots supplied twice this session) shows the knight is about **16x24
   px** — 10% of 240 px tall, 5% of 320 px wide — which is the `(16,24)` entries
   at slots **12-15**. Changing that one immediate to `0xC` makes the rect
   exactly `ulx=568 uly=476 lrx=632 lry=572` = **16x24**, verified in the
   emitted list.
2. **The texture window is written by something else and does not follow the
   entry.** With the index fixed, the rect is correct but the `G_SETTILESIZE`
   window stays `F2000000 0001C028` (`lrs=28 lrt=40` = **7x10 texels**), i.e. a
   ~2.3x magnification of the wrong region — so the marker is still not the
   knight. The builder's own arithmetic (ELF `0x8019F8E4..0x8019F930`) does not
   produce `28/40` from the live inputs, so **the window has a second writer
   that is still unidentified** (§4).

Also corrected this session, from the developer's second screenshot: the dark
oval at `(142,119)` is the knight's **shadow**, a separate sprite drawn with the
knight's feet at the shadow's centre (`origin 0.5,1`). It is the shadow whose
rect is `144x23`, not the body — the body was not located in the port's list at
all.

What is **not** yet done: (a) the window writer; (b) the knight body draw was
never found in the emitted list, so the pair (body, shadow) is not yet
identified in the port; (c) the cursor has the same wrong-rect signature; (d) no
fix has been committed.

## 1. What is now proven (and corrects session 60 §7)

All of this is measured on the developer's own live snapshots
`/tmp/map-early.bin` / `/tmp/map-settled.bin` plus the matching
`OGRE_DL_DECODE=all` log `/tmp/map-live.log` from the same run:

* **The `7x10` window is in the game's own display list**, immediately before
  the party rect — not an RT64/app artifact. Emission order is exactly
  `SETTIMG ... @0x8021AC88` → `F2000000 0001C028` (window `lrs=28 lrt=40`)
  → `E4478238 002381DC` (`TEXRECT ulx=568 uly=476 lrx=1144 lry=568`).
  Same at `0x8027CFC0/0x8027CFD0` and the second buffer `0x8028CFA0/...`.
* **The sprite table is static.** `0x801A6FE0..0x801A7100` is byte-identical in
  the early and settled dumps and identical to unit M's data half (ROM `0x85F68`),
  so nothing patches descriptors at runtime.
* **The builder is faithful.** `func_ovlM_8019F83C` (`bankRec05`, ROM `0x79750`
  module) reads entry `0x801A6FE0 + (a2 & 0xFFFF)*8` — `lhu 0(a2)` = W,
  `lhu 2(a2)` = H, `lh 4(a2)`/`lh 6(a2)` = u/v — and emits
  `rect = (a0,a1)-(a0+W, a1+H)` and a UV window derived from `(u,v)`+`(W,H)`.
  Verified live: `a2=11` reads `0x801A7038` = **W=144 H=23** and emits exactly a
  `144x23` rect; `a2=6` reads `0x801A7010` = **24x23** and emits `24x23`. The
  rect *always* equals the entry's `(W,H)` — so the width you see is the slot
  the caller asked for.
* **Session 60 §7's "the sampled region is genuinely transparent" is a
  consequence, not the cause**: a `144x23` rect sampling a `7x10` window must
  magnify, and the atlas bytes it lands on (asset `0x01DD210A` at `0x8021AC88`,
  a row of small dagger/flag icons) are simply the wrong region for the sprite.

### The table (live, byte-exact, base `0x801A6FE0`)

```
slot  0 ( 18,22)  1 ( 64,13)  2 ( 25, 8)  3 ( 35, 8)
slot  4 (  6, 6)  5 ( 16,16)  6 ( 24,23)  7 ( 24,23) uv(32,0)
slot  8 (  1,23)  9 ( 32,32) 10 ( 16,11) 11 (144,23)   <- party reads THIS
slot 12 ( 16,24) 13 ( 16,24) uv(16,0) 14 ( 16,24) 15 ( 16,24) uv(16,0)  <- knight frames
slot 16 ( 23, 8) 17 ( 22, 8)
```

The four `(16,24)` slots are the knight; `(16,24)` at 10% of 240 px and 5% of
320 px is exactly the developer's measurement.

## 2. The party call site

`func_ovlM_801A2A7C` (bankRec05) makes the party draw as its **first** builder
call, with a hardcoded index:

```
/* 81B50 801A2BC0 2406000B */  addiu  $a2, $zero, 0xB     <- slot 11 = (144,23)
/* 81BFC 801A2C6C 2406000A */  addiu  $a2, $zero, 0xA     <- second call
```

ROM address of that immediate: **ROM `0x81B50`** (`ram - 0x8019A7C0 + 0x79750`).
This is the line to change (to `0xC`) if the knight really is the `(16,24)`
entry — but see §4 before doing it.

The cursor's call site is `func_ovlM_801A1C50` with `a2=6` (slot 6 = `24x23`),
gated on `state+0x50`; its coordinates come from `state+0x52`/`+0x54`
(`(188,166)` in the dump).

## 3. The A/Bs (all in generated code, all reverted)

* **Global `base+8`** (`ctx->r2 = ADD32(ctx->r2, 0X6FE8)` at `0x8019F874`): the
  party rect became `ulx=568 uly=476 lrx=632 lry=572` = **16x24 px**. But it
  shifts *every* sprite, so the terrain/label sprites also move a slot (the map
  visibly changes). **Not the fix**, but it proved the rect tracks the entry.
* **Targeted `0xB -> 0xC`** at ROM `0x81B50`, run twice with
  `OGRE_DL_DECODE=all`. Both runs agree:
  * rect `ulx=568 uly=476 lrx=632 lry=572` = **16x24 px** (correct size), while
  * the window **stays** `F2000000 0001C028` (`lrs=28 lrt=40`, 7x10 texels).
  Captured frame `/tmp/idx_fixed.png` shows a small dark oval — the knight's
  **shadow** (developer), still ~2.3x magnified because its window is wrong. The
  body was not found in the list.

## 4. Open items

**(a) The window writer.** With the index fixed the rect is right but the
`G_SETTILESIZE` window is unchanged, so the window is written independently of
the entry. The builder's tail (`0x8019F8E4..0x8019F930` in the ELF) computes
`(a0+W)`/`(a1+H)` masked to `0xFFC` quarter-texels; with the live inputs
(`a0=142 a1=476 W=144 H=23`) that is **not** `28/40`. So either a second writer
sets the tile size after the builder, or the builder is fed different values than
the ones it reads for the rect. A watchpoint on the window word
(`guest 0x8027CFC4`) fired in `func_ovlM_8019F83C + 196`'s frame **but the word
already held `1c028`**, i.e. its writer ran earlier. **The writer is still
unidentified** — the next experiment is a watchpoint armed on a *cold* buffer
(the two display-list buffers alternate: `0x8027CFxx` and `0x8028CFxx`).

**(b) The knight body was never located.** The map's per-unit draws observed in
the live log are, with their emitted window:

| texture | pos | size | window (quarter-texels) |
|---|---|---|---|
| `0x8021AC88` | (568,476) | **144x23** | (0,0,28,40) = 7x10 texels |
| `0x80264D00` | (536,380) | 16x11 | (0,0,124,124) = 31x31 |
| `0x8021ADE8` | (600,812) | 16x24 | (0,0,572,92) = 143x23 |
| `0x8021A110/190/210/290` | (752,664) | 24x23 | (0,0,60,60) = 15x15 |

Every one pairs a window and a rect of comparable scale **except** the `144x23`
draw, which magnifies 7x10 texels ~20x. The developer's reference shows the body
sitting above the shadow, feet at the shadow's centre (`origin 0.5,1`), which
means the body and shadow are two draws at different y. The body may be the
`0x8021ADE8` `16x24` draw (its window `143x23` texels and `16x24` px rect are
consistent) — **verify against the reference before changing anything.**

**(c) The cursor** (`func_ovlM_801A1C50`, `a2=6`, coords from `state+0x52/0x54`)
has the same wrong-size signature.

## 5. Snapshots produced this session

* `/tmp/map-early.bin`, `/tmp/map-settled.bin` — developer, live, via
  `OGRE_KEY_1`/`OGRE_KEY_2 = dump ...`; `/tmp/map-live.log` is the same run's
  `OGRE_DL_DECODE=all` log. Scene `0x05`, `desc=0x8018FD70`, step `0xFFFE` at
  both presses; table and state unchanged between them (the pin/dots animation
  is not visible in this static scene's state).
* `/tmp/fixed_frame.png` — the global `base+8` A/B (party rect 16x24).
* `/tmp/targeted.png`, `/tmp/idx_fixed.png` — the targeted `0xB->0xC` A/Bs
  (rect 16x24, window unchanged).
* The developer's two reference screenshots this session: the map with the
  knight above its shadow and the crossed-swords pin, and a close-up of the
  knight + shadow + pin + cursor. These are the oracle for size and layout.

## 6. Files changed and probes

* **No source changes landed.** `git status --short` shows only the pre-existing
  `tools/RT64` submodule modification plus this session's docs
  (`PLAN.md`, `docs/DECISIONS.md`, `docs/README.md`, this file).
* Probes used and **all reverted** by `make recomp && make bank-recomp` +
  rebuild of `build-app` and `build-null`; `grep -rl 'probe9' RecompiledFuncs/
  Bank*Funcs/ app/src/` is empty. Probe tags used: `probe90` (global base+8),
  `probe91` (builder entry inputs), `probe92`/`probe94` (targeted `0xB->0xC`).
* Generated/regenerated (gitignored): `RecompiledFuncs/`, `Bank*Funcs/`.

## 7. Next leads

1. **Arm the window watchpoint on the cold buffer.** The two display-list
   buffers alternate; arm on `guest 0x8028CFA4` (the other half of the pair at
   `0x8027CFC4`) *before* the map's first frame so the first write of the window
   is caught, not an already-set word. That names the writer of the `7x10`
   window — the last unknown for bug (2).
2. **Locate the knight body draw.** It is not among the four draws tabled in
   §4(a). Log every `TEXRECT` in the scene (not just the atlas pages above) and
   match against the developer's close-up: the body sits above the shadow, feet
   at its centre. The `0x8021ADE8` `16x24` draw is the best candidate.
3. The cursor has the same wrong-rect signature; its call site
   (`func_ovlM_801A1C50`, `a2=6`) and coordinates (`state+0x52/0x54`) are known.
4. The date panel's `MONTH`/`DATE` box and day digit are still absent; the panel
   rect is `(150,203)` `a2=12` (slot 13 = `(16,24)`), so the panel may have the
   same index-shift problem in its own renderer.
