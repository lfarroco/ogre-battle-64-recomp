# Handoff — 2026-09-16, session 63: the party's two draws and the **empty texture buffer** — nothing landed

## Goal and result

**Goal (developer):** continue sessions 61/62 and make the map screen (scene
`0x05`) show the retail party: the blue **knight** (Magnus) over a **static soft
oval shadow**, plus the cursor and the date panel.

**Result: negative — nothing is landed** — but the developer's correction this
session settled which draw is which, and a live probe then found the concrete
wall: **the knight's texture buffer is zeroed at the moment the draw samples
it.**

**Read §0 first — it corrects sessions 61 and 62's identification of the two
draws.**

## 0. The two draws, and the correction (developer, this session)

Sessions 61/62 assumed the party body was the **first** builder call in the list
(`a2 = 0xB`) and the shadow the second. The developer's correction:

> "you just stretched the texture for the shadow, that is not the party. the party
> (magnus, the 'knight') is being rendered as a rectangle in the upper left of the
> shadow. it is possible to see his hair in some sprites. it is the one that
> should use 16x24"

Checked against the emitted list, that is right, and the geometry says so:

| call | ROM | `a2` live | rect (game px) | position |
|---|---|---|---|---|
| 1 | `0x81B50` | `0xB` (11) | `(a0-8, a1)` + entry | `(142,119)` — **lower** |
| 2 | `0x81BFC` | `0xA` (10) | `(a0-16, a1-24)` + entry | `(134,95)` — **upper** |

`a0`/`a1` are the party's map position. Call 2's rect is 24 px **above** call 1's,
and its origin is `-8` in x — i.e. **call 2 is the knight** (it must sit above
the shadow, feet on the shadow's top edge) and **call 1 is the shadow**. The
rows of ~18 repeated ellipses sessions 60–62 recorded were call 1 (the shadow)
stretched `144x23`; the knight (call 2) has been a small dark blob.

So the entries the two draws name today are **swapped relative to their roles**:

* **call 2 (the knight)** names slot 10 = `(16, 11)` — should be `16x24`.
* **call 1 (the shadow)** names slot 11 = `(144, 23)` — should be `16x11`.

The natural table entries are **slot 12 = `(16, 24)`** for the knight and
**slot 10 = `(16, 11)`** for the shadow (live values, confirmed at the consumer:
a probe inside the generated builder read them with the same `lhu`s the code
does). Note session 61's table printout inverts these pairs — the live table and
the ROM agree, and the emitted rect (`144x23` for slot 11) is the proof.

## 1. The wall: the knight's texture buffer is zero at draw time

With the windows derived per-entry (so rect and window agree), and slot 12 on the
knight call, the form is right — one clean `16x24` sprite with a proper soft-oval
shadow under it — but the art is a **dark block**, and a live probe shows why:

```
[probe70] call1 tex=8021AC88  w0=00000000 w1=00000000 w4=00000000
```

The builder's own texture pointer is `0x8021AC88`, and **every word at that
address is zero in the live RDRAM image** (`OGRE_DUMP_RDRAM` exit dump *and* the
in-process read agree). The shadow's buffer (`0x80264D00`, +0x10E0) is zero as
well. A zero RGBA32 texture with the map's alpha-blend combiner renders as the
flat dark block the developer sees.

The pointers themselves match the live RDRAM (`F1 … @0x8021AC88` /
`@0x80264D00` in `OGRE_DL_DECODE=all`), so the *address* is faithful; what is
missing is the **content**. Two candidates, not yet distinguished:

1. the buffer is filled by an asset/assembly pass the port does not run (the
   scene entry decodes ~20 assets through `func_8009DD38`); or
2. the port fills a *different* address and the pointer's `+0x1068` bias
   (`state 0x80197B18 → 0x801F1570`, field `+0x04`) lands elsewhere.

The state object at `0x801F1570` is a pointer table; its fields and the two
biases the draw uses (`+0x1068`, `+0x10E0`) are recorded in the previous
revision of this file's §3 — the useful part is that `+0x08` is **zero**, and
`+0x04` = `0x80219C20` is the one decoded atlas whose content *is* present.

## 2. What was tried (all reverted — nothing landed)

Generated-code A/Bs only, each rebuilt and captured:

* window derived from the entry at **call 1 only** → the ellipse row becomes one
  clean sprite, but it is the *shadow* being resized (session 62's mistake, and
  mine at the start of this session).
* window derived at **both** calls (correct form) → two clean dark blocks.
* slot 12 on **call 2** (the knight) + windows → the knight becomes a `16x24`
  dark block; the shadow stays `16x11`. This is the configuration the developer's
  correction asks for, and it is still dark because of §1.
* slot 12 on **call 1** + windows → shadow-sized ellipse block (wrong draw).
* texture pointer `+0x1068` → `+0x10E0` on the knight → white/black block.

`make bank-recomp` regenerated every probe away:
`grep -rl 'probe6[0-9]\|map_sprite_fix' Bank*Funcs/ RecompiledFuncs/ app/` is
empty, `Makefile` is untouched, and `tools/map_sprite_fix.py` is deleted.

## 3. What was run for verification

```sh
# forced scene, decode the emitted list, capture frames (12 s wall)
OGRE_SCENE=0x05 OGRE_SPEED=4 OGRE_DL_DECODE=all OGRE_CAPTURE_PRESENT=/tmp/x \
  OGRE_CAPTURE_AFTER=200 OGRE_CAPTURE_EVERY=100 OGRE_EXIT_AFTER_MS=9000 \
  ./build-app/ogrebattle64 assets/ogre64.z64
# live RDRAM at the scene, for the texture bytes:
python3 tools/rdram.py /tmp/map-settled.bin word 0x8021AC88 8     # -> all zero
```

## 4. Next leads (in order)

1. **Watch the buffer, not the pointer.** Arm `tools/watch.sh 0x8021AC88` (or a
   probe) from scene entry through the first map frame and log *who writes it and
   with what*. If nothing writes it in the port, compare against the developer's
   live snapshot `/tmp/map-settled.bin` — if that dump has zeroes too, the buffer
   is post-draw scratch and the knight's real texture is elsewhere; if it has
   art, the port is missing the write.
2. **Walk the sprite descriptor.** The two biases `+0x1068` / `+0x10E0` come from
   the state table `0x801F1570`; dump the whole table and every field the draw
   reads (`+0x34` at `0x801A2C74` for call 2 is the one that produced
   `0x80264D00`) and identify which field names a *populated* buffer. Enumerate
   the populated buffers by rendering each at the stride the autocorrelation
   gives (the 160-texel result in the earlier revision of this file is a good
   starting point) — do not assume a width.
3. **Only then** re-apply the two window patches (call 1 and call 2) plus the
   slot-12/slot-10 entry swap; they are correct in form and will land cleanly
   once the art is right. Keep them at the **call sites**, not inside the shared
   builder `func_ovlM_8019F83C` (19 callers, some with a non-zero `u`/`v`).
4. The cursor (`a2=6`) and the panel (`a2=12`) are the same rect-vs-window class.

## 5. Files changed

* `docs/HANDOFF-2026-09-16-session63.md` (this file), `PLAN.md`,
  `docs/DECISIONS.md`, `docs/README.md`, `docs/scenes.md`.
* **No code, `Makefile`, or `tools/` changes** — `tools/map_sprite_fix.py` was
  removed and `Makefile` restored.

Generated/regenerated (gitignored): `BankMFuncs/`, `RecompiledFuncs/`.
