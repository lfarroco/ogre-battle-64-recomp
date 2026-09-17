# Handoff — 2026-09-16, session 62: the map's sprite tile window — **the window bug is *diagnosed*, not fixed** (the fix was reverted)

## Goal and result

**Goal (developer):** continue from session 61 and make the map screen (scene
`0x05`) look like the retail reference they supplied: the framed world map with
the **party sprite** (a blue knight) and its **shadow**, the **cursor** (white
arrow + red dot) and the bottom-right **date panel** (`MONTH`/`DATE` box, the
month name and the day digit).

**Result: negative. The *mechanism* is identified and proven at instruction
level, but the fix I derived from it was **reverted at the developer's
instruction** — it regressed a better mid-session frame and did not reproduce
the reference. `git status` on the build side is clean: `Makefile` is untouched
and no post-recomp patch is in the pipeline. What follows is the diagnosis, the
reproducible leads, and the developer's oracle data. Read §0 first.**

### §0 — the honest status (developer feedback, end of session)

The developer reports that the landed build is *worse* than what they saw
mid-session: at 22:28 a build showed the knight with shadows below him
(`/tmp/w4.2000.ppm`), and the landed fix no longer does. The developer is right.
What happened:

* The 22:28 frame came from a **transient probe build**, not from a
  configuration I then reproduced. The probe that made it
  (`probe99b`, writing the shadow's window after its call) used a value I have
  not been able to reconstruct exactly, and I never captured the generated C.
  **Lesson: a capture from a probe build is not evidence for a landed fix.**
  Before claiming a fix, re-run it from a clean `make bank-recomp && make
  recomp` tree and compare against the probe frame.
* The **window mechanism** (§1) is solid and instruction-level verified; that
  part stands.
* The configuration I landed and then reverted (window from the entry's
  `(W,H)`, body entry `0xC`) removed the tiling — the row of ~18 ellipses is
  gone and the party rect becomes `16x24` — but it sampled a sparse region of
  the atlas and left the shadow a hard dark block, so it did **not** match the
  reference. **Nothing is landed today.**
* One concrete, reproducible improvement found late: giving the shadow a
  window of `(W, H+16)` turns it into the **soft oval** the reference shows
  (`/tmp/W09cc_z.jpg`). It was not landed. Treat it as a lead, not a solution:
  the same run also changed the body's window, so the two were not isolated.
* **Developer's oracle data (asked at the end of the session, and the most
  useful thing in this file):** the party is **two sprites** — the knight
  (`16x24`) and a **static soft oval shadow below it**, i.e. the shadow does
  *not* animate with the knight and is not a second copy of the knight art.
  Any candidate fix must produce exactly one soft oval under one knight.

The window question session 61 left open ("the writer is still unidentified") is
answered: the `G_SETTILESIZE` window that reaches the RDP is the **caller's
`jal` delay-slot store**, and N64Recomp emits that store *before* the call as
well as after it, so the callee's own window is discarded.

Also carried out this session, per the developer: **the dark ellipses are the
knight's shadow** (confirmed, and it is the 144x23 rect) — so the shadow, not the
body, is the wide draw, and the "row of ~18 ellipses" session 60/61 recorded is
that one draw wrapping a 28x40-texel window across 144x23 pixels.

What is **not** done: the shadow still renders as a solid dark block rather than
the reference's soft oval (entry 10, `16x23`, is the best candidate but its
sampled region is not yet right), and the date panel's `MONTH`/`DATE` box and day
digit are still missing. Neither is a bank/code-binding problem — both are the
same "which atlas region does this entry name" question.

## 1. The mechanism (instruction-level, corrects session 61 §4)

The map's party draw is `func_ovlM_801A2A7C` (`bankRec05` = unit **M**, ROM
`0x79750` module at RAM `0x8019A7C0`). It draws the party with two calls into the
generic sprite builder `func_ovlM_8019F83C`, and each call's **delay slot** is a
store into the display list:

| ROM | instruction | meaning |
|---|---|---|
| `0x81B44` | `sw $t5, 0x44($v0)` | `$t5 = 0x0001C028` -> `F2 0001C028` = a **28x40-texel** window |
| `0x81B48` | `jal 0x8019F83C` | the builder call (`$a2 = 0xB`, entry 11) |
| `0x81BFC` | `addiu $a2, $zero, 0xA` | second call's entry (10) |
| `0x81C18` | `sw $a2, 0x4($v0)` | second call's delay slot |

`$t5` is built at `0x801A2B14` (`lui $t5,0x1` / `ori $t5,$t5,0xC028`), i.e. a
hardcoded constant, not derived from the sprite table.

The builder writes **its own** window to a different slot: it computes
`((W+u)&0xFFC)<<12 | ((H+v)&0xFFC)` at `0x8019F990` and stores it at `$t2+4`
where `$t2 = $t1+0x10` and `$t1` is the display-list cursor. The caller stores at
`$v0+0x40`..`0x44`. On hardware the delay slot runs **after** the callee, so both
stores land and the RDP ends up sampling with the caller's `28x40` window — for a
`16x24` marker that is a wrap, i.e. the row of ellipses.

With N64Recomp's output the order is worse: the generated C emits the delay-slot
store, then the call, then a `goto after_N` that re-emits the delay-slot store
(the tail-call "repair" shape `cross_bank.py` looks for). Confirmed live with a
temporary probe in the generated `BankMFuncs/funcs_0.c`: at the builder's own
window write the computed word was `0x00090017` (`144x23`, entry 12) in one build
and `0` in another, yet the emitted list always carried `0001C028` at
`0x8027CFC0` — i.e. the caller's constant.

**Session 61 §4's "second writer" is therefore the call site itself.** Do not
look for another game function that writes a sprite's tile size.

### Why the builder's own window is useless for these entries

The table the map uses lives at `0x801A6FE0` (unit M's data half, ROM `0x85F68`;
static, byte-identical to ROM — session 61 verified this). Entries are
`{u16 W, u16 H, s16 u, s16 v}` (little-endian halves in RDRAM; read them with
`tools/rdram.py half`). The entries the map draws all have **`u=v=0`**, so the
builder's `(u+W)`/`(v+H)` arithmetic degenerates and cannot name a sane window.
The caller's constant is what was authored, and it is wrong for every map marker.

## 2. The fix that was landed and then reverted (do not re-apply as-is)

For the record, the reverted change rewrote the two window stores in the
generated `BankMFuncs/funcs_0.c` so they compute the window from the sprite
entry the builder is about to read:

```c
unsigned _e = 0x801A6FE0 + ((unsigned)ctx->r6 & 0xFFFF) * 8;
MEM_W(0X44, ctx->r2) = ((MEM_HU(_e, 0) & 0xFFC) << 12)
                       | (MEM_HU(_e, 2) & 0xFFC);
```

`ctx->r6` is `$a2` at the call, i.e. the entry index the builder will use — the
same `W`/`H` the builder turns into the rectangle, so rect and window can no
longer disagree.

The same script raises the party body's entry from `0xB` to `0xC`
(`ctx->r6 = 12` immediately before the first builder call). This one is a
**correction of the call site, not a recompiler artifact**, and the script says
so: entry 11 is `16x11`, entry 12 is `16x24` (the knight; entries 12-15 are its
four frames with `u = 0/16`).

The helper script (`tools/map_sprite_fix.py`) and its `Makefile` hook were
**both removed**; `make bank-recomp` regenerates the generated C without it, so
the tree is back to the session-61 state.

## 3. What was run for verification

```sh
# forced scene, per-capture decode, 4x
OGRE_SCENE=0x05 OGRE_SPEED=4 OGRE_PRESENT_ALWAYS=1 OGRE_DL_DECODE=all \
  OGRE_CAPTURE_PRESENT=/tmp/... OGRE_CAPTURE_AFTER=1800 OGRE_CAPTURE_EVERY=400 \
  OGRE_EXIT_AFTER_MS=11000 ./build-app/ogrebattle64 assets/ogre64.z64

# live RDRAM at the moment the scene is live (console trigger)
OGRE_SCENE=0x05 OGRE_CONSOLE_ON_SCENE=0x05 OGRE_CONSOLE_ON_CMD='dump /tmp/map.bin' ...

# the natural route (title -> New Game -> ... -> scene 0x05), 4x, ~250 s
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,…" OGRE_SCENE_LOG=1 \
  OGRE_PRESENT_ALWAYS=1 OGRE_CONSOLE_ON_SCENE=0x05 \
  OGRE_CONSOLE_ON_CMD='dump /tmp/nat-map.bin' OGRE_CAPTURE_PRESENT=/tmp/nat \
  OGRE_CAPTURE_AFTER=40000 OGRE_CAPTURE_EVERY=3000 OGRE_EXIT_AFTER_MS=250000
```

A/Bs run this session (all in generated code, all reverted except the landed
fix):

* **window only** (`tools/map_sprite_fix.py` window half, body entry untouched):
  the party bar shrank from ~18 ellipses to a compact marker but was still the
  wrong sprite — entry 11 is `16x11`.
* **window + entry 12**: the party rect becomes `16x24` and the ellipse row is
  gone; whether the sampled art is the knight's *correct* frame is **not
  verified** — the capture shows a sparse sliver, and the developer reports the
  landed build looks worse than the 22:28 probe frame. This is the landed
  configuration; `docs/proofs/native-newgame-map-scene.png` is a capture from
  it.
* **party atlas pointer `+0x1068` -> `+0x10D0`** (the only region whose alpha
  channel holds `0x65..0xAA` translucent-black texels): draws two black bars —
  rejected, the sprite is not sampled from there by absolute address alone.
* **builder table base `0x801A6FE0` -> `0x801A6FD8`** (a global shift so every
  entry index means what session 61 believed): the pin and cursor sprites broke
  (`0x801A7010` = `(23,24)` is a real entry, not the `(24,23)` session 61 read),
  so the base is **not** off by one. Rejected.
* **shadow entry** 7, 8, 9: every one renders either `16x24`/`144x23` blocks or
  stripes — still open (§4).

## 4. Open items (the next session)

1. **The shadow.** The 144x23 draw is the knight's shadow (developer). Its entry
   is 10 in the table the builder reads, and 10 is `(16,23)` there — not
   `(144,23)`, which is entry 12 of that same base but the *body's* index on the
   original code. The shadow's rect under the landed fix is `16x23` and it draws
   a solid dark block; the reference's shadow is a soft oval roughly a third the
   knight's width. The atlas region the entry names (bytes at
   `0x80219C20 + 0x1068`) begins with a few dark texels then zeros, so the entry's
   `u=v=0` still does not appear to name the blob. **Next experiment:** read the
   shadow's atlas pointer out of the live display list (`TIMG` word at the draw,
   `OGRE_DL_DECODE=all`) and render that region offline with
   `tools/rdram.py image` / a small raw-RGBA dump, instead of inferring it from
   the table; then decide whether the entry index or the window is wrong.
2. **The date panel.** The `MONTH`/`DATE` box and the day digit are still
   missing; the month name draws (the I8 font rows). One `16x24` draw at
   `(150,203)` exists (`a2=12`) and lands inside the panel area but draws a
   striped/black block. Same question as (1).
3. **The cursor** (`func_ovlM_801A103C` @0x801A1C50, `a2=6`, coords from
   `state+0x52/0x54`) now gets a correct per-entry window from the fix; verify
   it against the reference's white arrow + red dot.
4. **The knight's animation**: the developer reports only one frame renders
   correctly and the others are "a blob of pixels, like a cloud". The four
   `(16,24)` entries (12-15) with `u = 0/16/0/16` and the `s0 = (3*timer+frame)
   << 12` byte offset at `0x801A2C2C` are the frame mechanism; the landed fix
   fixes the window per entry but not the frame's *atlas address*. This is the
   most valuable next lead — likely the `u` field must also offset the `TIMG`
   pointer or the `TEXRECT` `s`/`t` (the builder currently emits `s=0 t=0`,
   which is why every frame samples the same place).

## 5. Files changed

* `tools/map_sprite_fix.py` (**new**) — the post-recomp patch, with the ROM
  evidence and the deviation rationale in its docstring.
* `Makefile` — `make recomp` runs `python3 tools/map_sprite_fix.py apply` after
  `cross_bank.py dispatch`.
* `PLAN.md`, `docs/DECISIONS.md`, this file.

Generated/regenerated (gitignored): `BankMFuncs/`, `RecompiledFuncs/`.

**Probes:** all in generated code (`BankMFuncs/funcs_0.c`), tagged `probe92`
through `probe102` (builder args/table entry/window word, buffer dumps, the
F2 write addresses, the party entry inputs, the table-base and slot A/Bs). All
reverted by `make bank-recomp`; the only surviving generated-code edits are the
two tagged `map_sprite_fix` blocks. `grep -c probe BankMFuncs/funcs_0.c` is
**0**.

## 6. Tree state

`git status --short` shows `Makefile` (modified), `tools/map_sprite_fix.py`
(new), the docs above, and the pre-existing `tools/RT64` submodule
modification. No ROM dumps or extracted assets are committed.
