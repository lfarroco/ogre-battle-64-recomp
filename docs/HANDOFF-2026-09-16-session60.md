# Handoff — 2026-09-16, session 60: the **map scene** (`0x05`) renders — two mis-compiled cross-bank calls were killing the frame pump

## Goal and result

**Goal (developer):** after the prologue movie the screen fades to black and
stays black; the next scene is the **map scene**, and it is important because
the game's own save (Controller Pak) becomes reachable from there. Make it
render.

**Result: the map renders** — terrain and rivers, location labels, the route of
dots, unit markers and the `Flama` cursor, inside the stone frame. Proof:
`docs/proofs/native-newgame-map-scene.png` (natural route after the prologue
fade; the forced-scene capture is the same screen).

The cause was **two main-ELF calls into RAM the scene module occupies**, each
compiled by N64Recomp as "call the containing body, then `return`" (its
tail-call emission for a `jal` into a size-overridden body interior). The early
`return` abandons the caller's frame, so the frame-pump thread reads its
callee-saved registers back from the wrong stack slots and stops dispatching
frames:

| site | call | recompiler emitted | why it matters |
|---|---|---|---|
| `func_8017B858` (scene **update**) 0x8017B8A0 | `jal 0x8019AF0C` | `func_8019ABC4(rdram, ctx); recomp_trace_return(); return;` | leaks **0x20**, and skips the update's whole state dispatch (the jump table at 0x8017B8B8) |
| `func_8017B9C8` (scene **hook**) 0x8017BA10 | `jal 0x801A103C` | `func_801A1034(rdram, ctx); recomp_trace_return(); return;` | leaks **0x18**, and skips the rest of the hook |

Both targets are in the RAM the scene module is DMA'd over (`0x8019A7C0`…), so
the call must go through the runtime bank map. **Fix:** add `0x8019AF0C` and
`0x801A103C` to `make recomp`'s `cross_bank.py dispatch --only` list. The call
becomes `LOOKUP_FUNC(addr)`, and `cross_bank.py`'s existing tail-call repair
converts the emission to **call-and-continue** (it detects the early return plus
the duplicated delay slot). Verified: forced `OGRE_SCENE=0x05` went from **2
display lists to 124**; the natural route submits **2296** for the run and the
map is drawn.

## 1. Why it presented as a frozen black screen

The frame pump is a small event thread (t4, entry `func_8008AFE0`) that:

1. `osRecvMesg(0x800C4C28)` — messages posted by the VI thread
   (`func_80088F08` → `func_800891A0`) carrying the VI event word `0x800E8B10`;
2. dispatches on `msg->type` (`1` = retrace) and calls the callback registered
   at `0x800AA090` — `func_80072398`, registered by the scene dispatcher
   (`func_80075BC0` @0x80075F94 → `func_80089990`);
3. `func_80072398` builds the frame and calls, per frame, the current
   descriptor's **update** (`*(desc+4)`) and **hook** (`*(desc+8)`).

Because the two calls above returned without restoring `$sp`, t4's *saved*
`$s0`/`$s1` were read back from the wrong slots. `$s1` is the message-type
comparator (1); once it is 0 the `msg_type == 1` test never matches again, so
**the pump stops after two frames** (and `$s0` became `0x801869E8`).

The signature that identifies this class:

* `D_800AEFA4` (the game's frame counter, written by `func_80072398` at
  0x800726C4) **freezes** at 154 while `D_800C4BCC` (VI retrace, written by the
  VI thread at 0x80088F7C) keeps counting;
* only 2 display lists are submitted, both during boot;
* `OGRE_PROFILE=1` shows every thread idle — it is not a spin;
* the thread snapshot says t4 is `BLOCKED on recv of queue 0x800C4C28`.

The scene's *logical* wall was the black fade: the fade-out to black is the
prologue's own last frame, and because no further frames were produced the
presenter kept showing it.

## 2. How it was found (the technique, for the next agent)

1. `OGRE_SCENE=0x05` **forced** reproduces it in ~2 s and is a valid repro for
   this wall: the enter runs, the module loads (unit M, 55 functions), the
   callback is registered, and still only 2 display lists are submitted.
2. `OGRE_PROFILE=1` + `OGRE_PROFILE_CHAINS=1` gives per-second per-thread state
   and the frame-state words (`retrace`/`last`/`acc`/`req`/`pend`/`ready`) —
   that is where the frozen `D_800AEFA4` showed up.
3. The decisive probe is **the guest `$sp` (`ctx->r29`) around every call** in
   the frame chain: a leaking call shows as a lower `sp` after the callee
   returns; the caller (t4) then loads garbage into `$s0`/`$s1`. Probes went
   into the **generated** `RecompiledFuncs/*.c` (tagged `probe59`) and were
   reverted with `make recomp` (see §5).
   * `func_80072398` before/after its calls: the leak appeared after the scene
     **update** call (0x20) and after `func_80076AE8`'s `jalr` (0x18).
   * the `jalr` is `*(descriptor+8)` — the scene **hook**; that pointed at
     `func_8017B9C8` and its `jal 0x801A103C`.
4. The shape to grep for in generated C is a call followed by
   `recomp_trace_return(...); return;` **in a function that has more code
   after it** — the recompiler's tail-call emission for a `jal` into a
   size-overridden body. `cross_bank.py`'s dispatch repairs exactly that shape.

## 3. The two dispatches, and the wider backlog

`make recomp` was already dispatching 10 targets (sessions 37–59). Added:

* **0x8019AF0C** — the scene update's state-1 handler. It is inside overlay C's
  layout (splat split `func_8019ABC4`'s shared epilogue into its own symbol,
  which `config.toml`'s `func_8019ABC4 = 0x368` override re-swallows), but on
  the hardware the resident code at that address is the **scene module's**:
  unit M registers a real entry at 0x8019AF0C (`kM_bankRec05Functions`), which
  is what the game's state machine expects. Scene `0x05`'s enter sets
  `0x801977E8 = 1` (0x8017B6C0); scene `0x07`'s sets `3` (0x8017B848) and takes
  the `0x8019B340` branch instead — which is why the form worked and the map
  did not.
* **0x801A103C** — the scene hook's call (the hook is `func_8017B9C8` for both
  0x05 and 0x07; `0x801A1034` is the session-30 `function_sizes` override).

**Experiment recorded:** `python3 tools/cross_bank.py dispatch` with **no**
`--only` also fixes the map (89 call sites / 7 extra targets — including
0x80197B90 and 0x801989AC, which are calls *into overlay C itself* from
streamedB, 41+43 sites). That is the eventual way to clear the main unit's
"known backlog" (75 targets still have no bank entry at all; see
`make cross-bank-check`), but it changes many bindings at once, so this session
kept the minimal explicit pair and verified the whole opening.

## 4. What was run for verification

```sh
make recomp && make bank-recomp          # 12 targets dispatched, 12 tail-call
                                         # site(s) repaired; check-banks OK
cmake --build build-app -j 8 && cmake --build build-null -j 8

# forced repro: 2 -> 124 display lists, 0 stubs, 0 failed lookups
OGRE_SCENE=0x05 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 \
  OGRE_EXIT_AFTER_MS=12000 ./build-app/ogrebattle64 assets/ogre64.z64

# the natural route, 260 s at 4x: prologue -> map, 2296 display lists, 0 stubs
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,…" OGRE_SCENE_LOG=1 \
  OGRE_PRESENT_ALWAYS=1 OGRE_CAPTURE_PRESENT=/tmp/nat/c OGRE_CAPTURE_AFTER=5000 \
  OGRE_CAPTURE_EVERY=60 OGRE_EXIT_AFTER_MS=260000 ./build-app/ogrebattle64 assets/ogre64.z64

# regression on the same run's captures: the movie montage (parchment map shot)
# and the "Prologue" card still render; the null build enters 0x05 with 0 stubs
```

`docs/proofs/native-newgame-map-scene.png` is a capture from the natural run
(present 44640, after scene `0x05` went live at t=180.4 s).

## 5. Files changed and probes

* `Makefile` — `cross_bank.py dispatch --only …` gains **0x8019AF0C** and
  **0x801A103C**.
* `tools/cross_bank.py` — the rewrite summary now lists each dispatched target
  (`cross_bank:   dispatched 0x…`, N site(s)); useful for the next wall.
* `docs/proofs/native-newgame-map-scene.png` (new) — the map, captured from the
  port.
* `docs/proofs/map-reference/retail-map-screen.png` (new) — the developer's
  retail screenshot of the map screen (the reference for §7).
* `PLAN.md`, `docs/scenes.md`, `DECISIONS.md`, `docs/README.md`, `AGENTS.md`,
  this file.

Generated/regenerated (gitignored): `RecompiledFuncs/` (the final `make recomp`
dispatches 15 call sites across **12 distinct targets** and repairs 12 tail-call
sites), `Bank*Funcs/`, `app/src/bank_funcs.inc`, `build/bank*.elf`.

**Probes:** `probe59`, in the generated `RecompiledFuncs/funcs_2.c`,
`funcs_4.c` and `funcs_9.c` (guest-`$sp` logging around the frame chain), and
`probe60` in `RecompiledFuncs/funcs_0.c` (the asset loader
`func_8009DD38`'s `(id, arg)` per call, §7). All reverted by `make recomp`;
`grep -rl 'probe59\|probe60' RecompiledFuncs/ Bank*Funcs/ app/` is empty and
both builds were rebuilt afterwards. No app or runtime probe.

## 6. Next leads

1. **The map's sprites are missing/garbled — the concrete next wall** (§7).
2. **The game's own save system (Controller Pak)** — the developer's stated next
   milestone, reachable from the map. Recon is in
   `docs/HANDOFF-2026-09-16-session58.md` §3c: a 32 KiB PFS image, a real
   `osPfs*` implementation over it (`librecomp/src/pak.cpp` is upstream's
   `PFS_ERR_NOPACK` stub), the raw SI pak access, and a save file for it.
3. **The remaining cross-bank backlog** — `python3 tools/cross_bank.py report`:
   75 main-unit targets have no bank entry (kept as direct calls) and the
   overlay-C-base calls (0x80197B90/0x801989AC) are dispatched only by the
   no-`--only` full pass. Any of these is the same wall one scene further on.

## 7. The map screen's content: what it must show, and what is missing

### The developer's spec (session 60) — recorded in `docs/scenes.md`

Retail reference: `docs/proofs/map-reference/retail-map-screen.png` (the state
right after the prologue). The screen is the framed world map plus the **party
sprite** (a blue knight) at its location, the other **unit markers**, a
**cursor** (white arrow + red dot), and the **date panel** in the bottom-right
(a `MONTH`/`DATE` box and the date, e.g. `Sombra 1`).

The map's **`R` menu** is a horizontal menu: 1 **Organize** (default), 2 **Hugo
Report**, 3 **Settings**, 4 **Save**. Save opens a window with two vertically
stacked slots; picking one prompts *"Any existing data will be overwritten.
Proceed?"* with Yes (default) / No. With a save present the title's cursor
starts on **Load Game**, which opens the Load Game screen listing the two saves.
All of that is written up in `docs/scenes.md`.

### The port's state: terrain ✓, sprites ✗

The port draws the terrain, the stone frame, the road/label mask (the 23
full-width **I8** rows) and the panel's month name (`Flama` in the capture), but:

* the **party sprite** is a row of ~18 repeated dark ellipses where retail has
  the knight and plain terrain;
* the **cursor** and the **panel box / `MONTH DATE` label / day digit** are
  likewise wrong or missing;
* the same run's capture is `docs/proofs/native-newgame-map-scene.png`.

### What the display list actually asks for (measured)

`OGRE_DL_DECODE=all` on the forced scene gives the map's per-frame list: 1773
commands, **97 `G_TEXRECT`s**, 8 nested `G_DL`s, 0 unknown opcodes — a pure
2D F3DEX2 list. The pieces: 25 CI16 terrain tiles, 38 CI16 border rows, 23
**I8** full-width overlay rows (the roads/labels/markers), 5 CI16 glyph rects
(the date text) and five small sprite rects. The suspicious one is the party:

```
SETTIMG fmt=RGBA siz=32b addr=0x8021AC88
SETTILE t7 fmt=0 siz=3 line=0 tmem=0     + LOADBLOCK   (load the tile)
SETTILE t0 fmt=0 siz=3 line=2 tmem=0                       (render tile)
SETTILESIZE t0 uls=0 ult=0 lrs=28 lrt=40                   -> a 7x10-TEXEL window
TEXRECT ulx=568 uly=476 lrx=1144 lry=568 dsdx=1024 dtdy=1024  -> a 144x23-PIXEL rect
```

A 7x10-texel window over a 144x23-pixel rect at dsdx=1.0 (RDP S5.10) repeats the
tile ~18 times — exactly the row of ellipses — and retail's screenshot has no
such pattern, so the **port's descriptor values differ from retail's** (the
renderer is doing what the list says: the same list in RT64 gives the same
stripes, and `OGRE_RECT_STATE=119-142` confirms the tile window, `fmt=0 siz=3
line=2`, and an alpha-blend combiner `RGB/A = TEXEL0`, `P=CC A=CC_A M=FB B=1-A`).

### Where the data comes from (measured)

* The sprite buffers are part of a decoded **sprite atlas**: the loader
  `func_8009DD38(asset_id)` (ROM `(id & 0x0FFFFFFF) + 0x594250`, 4-byte size
  header, `func_8007A110` LZ) is called from unit **M**'s entry
  (`func_ovlM_8019A7C0`) with ~20 assets; the cursor/party/panel sprites come
  from **asset `0x01DD210A` (ROM `0x2356DBC`, 0x12D0 payload → 0x5288 bytes)**,
  which lands at `0x80219C20` in the port. The cursor is at atlas `+0x4F0` and
  the party rect samples atlas `+0x1068`.
* **The port's decode is correct**: `tools/ogrelz.py`'s offline decode of that
  asset matches the port's RDRAM byte for byte at those offsets, and the bytes
  at `+0x1068` really are mostly zero/transparent 32-bit RGBA.
* A write watchpoint (`tools/watch.sh 0x8021AC88`) confirms `func_8007A110`
  writes there, from `func_8009DD38` ← `func_ovlM_8019A7C0` ← the scene-`0x05`
  enter `func_8017B60C` — i.e. the load is the map's own setup, and it writes
  the same bytes the asset holds.

**So the wall is not the decode and not the renderer**: the game's map is asking
for a sprite whose tile window and rect do not match any sane sprite, and whose
sampled region of the atlas is empty. That reads as **a sprite descriptor that
the map never finished filling** — the next thing to find is what fills those
descriptors (the map's object/sprite table) and which step of scene `0x05`'s
setup was skipped. Unit M's entry, the 20 asset ids and their buffer addresses
are all in this section; `OGRE_RECT_STATE` and `tools/watch.sh` are the tools
that got this far. Note the same investigation should ask the developer whether
the party appears *immediately* when the map opens or only after its intro
animation — the port's frame is completely static (700+ identical display
lists), so if retail animates the party/panel in, "stuck at frame 0" is the
wall rather than a wrong descriptor.

