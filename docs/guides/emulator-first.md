# Emulator-first debugging

**Why this guide exists.** Sessions 60–63 spent four sessions on the map screen's
per-unit sprites (`scene 0x05`) asking *what did the game mean by this display
list?* and answering it by reading decoded display lists and the game's sprite
tables. That is a question an emulator never asks. An emulator has no opinion
about a sprite: it runs the game's code, then runs a faithful RDP over whatever
the code wrote. Every wall those sessions hit — "a `144x23` rect over a `7x10`
window", "the texture buffer is all zero", "the builder's window is discarded" —
is a *fact about the game's own output*, not a bug you can fix by editing it.

This guide records what mature emulators do differently, which of those
mechanisms this port already has, and the short checklist that would have
answered the map screen in an hour. Companion docs: `docs/guides/rsp-microcode.md`
(LLE of one ucode), `docs/guides/app-build.md` (the diagnostics toolkit).

## 1. The three pillars, and where the port stands

An emulator that renders a game correctly stands on three things. The port has
one of them, and the gaps are where "the game's data looks wrong" theories come
from.

| Pillar | What an emulator does | What this port does |
|---|---|---|
| **GBI identification** | Hash the OSTask's ucode text+data and dispatch to the matching GBI table | **Has it.** RT64's `GBIManager::getGBIForUCode` XXH3-hashes the ucode and picks the GBI; the app calls it on every `send_dl` (`app/src/renderer.cpp`). §3 is the recipe to verify which GBI a given list got. |
| **RSP** | HLE the known ucodes (audio, JPEG, …); LLE anything else by running the real microcode | **Does not have it.** `app/src/rsp.cpp` is a **stub for everything except** the game's Nintendo-JPEG decoder. A task the stub swallows produces *no output at all*, and the game then renders whatever the missing task should have filled. Before blaming game data, check whether an RSP task went missing (§4, check 2). |
| **Ground truth** | Its own existence: mupen/GLideN64/parallel-rdp render the ROM, so "retail does X" is measurable, not inferred | **Does not have it.** The port only knows what the port does. §5 is how to borrow one — RetroArch with Mupen64Plus-Next is already installed on this machine. |

**The rule that follows:** a display list the port emitted is evidence about *the
port*. To turn it into evidence about the game you must first show the port's
code, its ucode dispatch and its RDRAM are all faithful. Until then, "the game
asks for a `7x10` window" is a statement about the port.

## 2. What does *not* count as evidence

These cost sessions 60–63 time. They are recorded because they read like
evidence and are not.

* **A decoded display list.** `OGRE_DL_DECODE` / `OGRE_DL_ANALYZE` decode every
  list as F3DEX2 (`app/src/gbi.cpp`). That is correct for this game's main ucode
  (§3) but it means the decoder *cannot* tell you the ucode is wrong — it will
  happily print plausible F3DEX2 geometry for an S2DEX2 list, which is exactly
  what session 50 §9 recorded before session 51 found the njpeg list was S2DEX2.
  Never conclude a ucode from the port's own decoder; hash it.
* **A constant read out of the ROM.** `0x1C028`, `0xB`, `0x1068` are the game's
  own data. Patching the *generated C* so the constant becomes something else
  (sessions 61/62's `tools/map_sprite_fix.py`) is fabricating game data
  (AGENTS §7) and, worse, it hides whichever mechanism actually differed.
* **An exit RDRAM dump alone.** `OGRE_DUMP_RDRAM` fires at exit. It is fine for
  "what does this buffer hold once everything has run", and useless for "what
  did it hold when the draw happened" (session 63's zeroed knight buffer was
  measured this way, and its *absence* is real — but the same dump also showed
  `*(0x80197B18) == 0` at a moment when the map had already drawn 856 lists,
  because the dump had fired at scene entry).
* **A capture from a probe build.** Session 62's best-looking frame came from a
  transient probe whose value was never reconstructed. A frame is evidence only
  if it comes from a clean `make bank-recomp && make recomp` tree.

## 3. Recipe: which GBI did this display list get?

This is the check that an emulator does implicitly and the port never logged.
It settles "is the port decoding this list with the right opcode table".

RT64 hashes the ucode bytes with **XXH3-64**, over the raw RDRAM at the task's
`ucode` and `ucode_data` addresses, at the segment lengths in `GBIManager`'s
database (`tools/RT64/src/gbi/rt64_gbi.cpp`). The port prints the task's `ucode`
on every submission with `OGRE_DL_TRACE=1`:

```
[renderer] display list 853 at t=8278ms entries=0 (type=1 ucode=0x8009F540 data=0x8028C120)
```

Hash the dump and look the value up in the database:

```sh
tools/venv/bin/pip install xxhash        # one-off
# text segments are hashed at 0x1390 (F3DEX2) / 0x18C0 (S2DEX2);
# data segments at 0x420 / 0x390.
```

```python
import xxhash
from tools.rdram import Dump          # rdram.py's Dump applies the byte-order rules
d = Dump('/tmp/map3.bin')
# RT64 hashes the RAW bytes at the physical address (not the logical XOR-3 view)
n = 0x1390
raw = bytes(d.data[(0x8009F540 & 0x1FFFFFFF):][:n])
print(hex(xxhash.xxh3_64(raw).intdigest()))
```

Verified for scene `0x05` (session 64): `0x8009F540` at length `0x1390` gives
`0xCF55FAE288BFE48D` = **`F3DEX2.fifo 2.08`** (RT64 DB line ~213). The map's
lists really are F3DEX2, and RT64 picks the F3DEX2 GBI — so the sessions 60–63
readings of that list are *not* a ucode-dispatch artifact. (The njpeg background's
ucode `0x800A5110` at `0x18C0` gives `0x9300F34F3B438634` = `S2DEX2.fifo 2.08`,
which is session 51's result. Note the two were matched at *different* segment
lengths and byte views — check both if a hash misses.)

A hash that matches **nothing** makes `getGBIForUCode` return `nullptr` and RT64
prints `Unable to find a matching GBI in the current database`; if that line is
absent from a run log, the list was dispatched, not guessed.

## 4. The checklist, before theorising about what the game meant

Run these five, in order, and write the answers into the handoff. Each is
cheap; together they partition "the port differs from retail" into one of:
recompiler, ucode dispatch, renderer, RDRAM contents, or game state.

1. **Which GBI?** §3 above. (`OGRE_DL_TRACE=1` + hash the dump.)
2. **Did an RSP task go missing?** `tools/runlog.py <run.log>` lists tasks by
   type and flags any non-gfx task the stub swallowed. For scene `0x05` the
   answer (session 64, forced scene at 4×) is: type 1 (gfx, `ucode=0x8009F540`)
   and type 2 (audio, `ucode=0x8009E050`, 1706 tasks in 9 s), **and nothing
   else** — so no missing task explains this screen. (The audio stub is expected:
   the port has no audio yet.) On 32-bit-stub tasks the game will not notice, so
   this check is about *geometry-producing* work, not audio.
3. **Is the code the code we compiled?** `tools/rdram.py <dump> banks` says which
   module is resident at each streamed RAM window, and `make midfunc` lists the
   prologue-less tails. The sprites are drawn by CPU code in unit **M**; a wrong
   bank would make every constant in it meaningless.
4. **Are the bytes what hardware would have?** For a decoded asset: decode it
   offline and compare. `tools/ogrelz.py` is an independent LZ implementation;
   its strong self-check is that the token stream must end **exactly** on the
   asset's declared payload boundary (`consumed == payload`). Session 64 ran it
   over the map's 13 real LZ assets: 13/13 exact, sizes `0x178`..`0x2439`,
   outputs `0x2A8`..`0x7E20`. That closes "the decoder is subtly wrong" — with
   two implementations agreeing *and* the boundary invariant holding, a format
   misreading is not a live hypothesis. Do this before blaming a buffer's
   contents.
5. **Is the recompiler faithful at this call?** N64Recomp emits a `jal` as
   *delay-slot instruction, then call*, and duplicates the delay-slot block
   after the call behind a `goto after_N` where it is **dead code** (it exists so
   a *fall-through* entry at `jal+4` still works). It does **not** execute the
   delay slot twice. Session 62 asserted the opposite and built a whole theory on
   it; session 64 read the emitted C and the ROM bytes and it is false. To check
   a specific call, read the generated C at the `jal` (it is commented with both
   guest addresses) and compare with `mips-linux-gnu-objdump -d` of the same
   bank ELF *and* the raw ROM bytes at `ROM = bank_rom_start + (ram - bank_ram_start)`.

## 5. Recipe: borrow a reference emulator

`~/Documents/RetroArch/system/Mupen64plus/mupen64plus.ini` is mupen64plus's game
database, and RetroArch is installed with `mupen64plus_next_libretro.dylib` and
`parallel_n64_libretro.dylib`. Ground truth for "what is in RDRAM at the moment
this screen draws" means a reference emulator's memory image, which is what the
port cannot produce.

The cheapest routes, in order:

1. **Reference savestate → RDRAM.** Run the ROM in RetroArch/Mupen64Plus-Next to
   the screen, save a state, and parse RDRAM out of it with mupen64plus's own
   `savestate.c` (vendored at
   `tools/RT64/src/contrib/mupen64plus-core/src/device/...`; the format and the
   RDRAM block are in the core). Then diff the two images at the same guest
   address — for the map that is `state[+0x04]+0x1068` and `state[+0x34]`.
2. **Screenshot A/B.** Weaker but immediate: the same screen from a reference
   emulator next to `docs/proofs/native-newgame-map-scene.png`. This settles
   "does retail show the knight here at all" (AGENTS §1 oracle question) without
   any memory plumbing.
3. **mupen64plus-rsp-hle as the HLE reference.** Its task detection is in
   `src/hle.c` (`task_detection`). It knows exactly one OB64-specific thing —
   the JPEG ucode (`sum 0x130de` / `0x278b0` → `jpeg_decode_OB`, "found in Ogre
   Battle, Bottom of the 9th"), which this port already implements — and its
   default for an unrecognised non-audio task is to pass the display list to the
   gfx plugin. Its ucode list has **no** gfx entries: graphics HLE lives in the
   video plugin, which is why GLideN64 (§6) is the right reference for sprites.
4. **GLideN64 as the reference implementation.** See §6.

## 6. The reference implementation already has per-game OB64 handling

GLideN64 — the video plugin most OB64 emulator users run — carries two
game-specific workarounds for this ROM. Both are worth knowing because they say
*other people also found OB64's 2D path unusual*, and because one of them is a
`2D sprite` fix:

`GLideN64/ini/GLideN64.custom.ini`:

```ini
[OGREBATTLE64]
Good_Name=Ogre Battle 64 - Person of Lordly Caliber (U)
graphics2D\enableTexCoordBounds=1
```

`src/mupenplus/Config_mupenplus.cpp` describes the knob: *"Bound texture
rectangle texture coordinates to the values they take in native resolutions. It
prevents garbage due to fetching out of texture bounds, but can result in hard
edges."* GLideN64 implements it in `src/uCodes/S2DEX.cpp` (`S2DEX_Obj_Sprite`),
setting `gDP.m_texCoordBounds` from the sprite's `uls/lrs/ult/lrt`.

And `src/RSP.cpp` (romname `OgreBattle64`) sets `hack_Ogre64`, whose only use is
`src/uCodes/S2DEX.cpp` `gSPObjRectangleR` → `_drawYUVImageToFrameBuffer`, *"Ogre
Battle needs to copy YUV texture to frame buffer"* — the njpeg background
machinery this port already has (sessions 51–52).

**Read this as a lead, not a conclusion.** Both hooks are in GLideN64's **S2DEX**
handler, and the map's party draw is F3DEX2 `G_TEXRECT` (§3), so neither hook is
obviously the map's bug. What it does establish is that OB64's 2D drawing leans
on RDP behaviour (texcoord clamping against a tile window) that a naive
implementation gets wrong — which is exactly the shape of the map's symptom.
**Before writing any code, find out whether RT64 has an equivalent of
`enableTexCoordBounds`; if it does not, that is a renderer-side difference worth
an A/B, and it needs *no* game-data theory at all.**

## 7. The pattern to stop

Three sessions in a row ended with "the game's data says X, therefore the
descriptor was never filled / the constant is stale". The emulator-first
replacement is a two-question split:

1. **Is the port faithful here?** (§4). If any check fails, fix *that*.
2. **If all five pass, the port is doing what hardware would do for this state.**
   Then the difference is in the *state* the game reached — which is a question
   about the path into the screen (forced scene vs. the natural New Game route,
   save pre-state, frame counter), not about the display list. Compare against a
   reference emulator's RDRAM at the same screen, in the same state, before
   touching anything.

Do not patch generated C to make a picture look right. If the port is faithful
and the picture is still wrong, the finding is *"the game reaches this screen in
a state retail does not"* — write that down (AGENTS §7) instead of inventing the
missing bytes.
