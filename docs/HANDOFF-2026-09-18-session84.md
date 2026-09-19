# Handoff — 2026-09-18, session 84: the attract-story olive band, traced to its branch

**Developer's report:** *"in the attract scene with the lore history of the game
there's an issue, the background is olive. in the retail version it is black"*
(scene `0x0B`, the world-map "Palatinean Year 238…" attract story).

The screen was reproduced, the olive was traced to the exact instruction that
chooses it, and **the port's data at the deciding address is verifiably wrong**
— but the writer that would fix it has not been found yet. No game code, config
or generated output changed this session; the only tree difference from session
83's end state is the probe, which was reverted and rebuilt.

## 1. Reproduction and the olive's exact colour

```
OGRE_SPEED=8 OGRE_NO_AUDIO=1 OGRE_CAPTURE_PRESENT=/tmp/a2 OGRE_CAPTURE_EVERY=2 \
  OGRE_EXIT_AFTER_MS=50000 ./build-app/ogrebattle64
```

Frames `a2.504`..`a2.1090` are the attract story. Its top and bottom bands sample
**`(74,90,0)`** (`#4A5A00`) — "olive". The bands are the full 320x240 target
filled, with the map drawn over the middle; the left/right black is the
presenter's letterbox, so the whole *frame* background is that one fill.

`OGRE_FBRENDER_TRACE=1` names it:

```
[fbrender] FillRect fillColor=0x4AC14AC1 cimgSiz=2 colorTarget=yes rect=(0,0)-(1276,956)
```

`0x4AC1` as RGBA16 is `R9 G11 B0` → `(74,90,0)`. Confirmed in a live RDRAM dump
(the command is in the game's own display list, not a renderer invention):

```
0x802BA380  F7000000 FFFCFFFC   ; SETFILLCOLOR
0x802BA390  F64FC3BC 00000000   ; FILLRECT (0,0)-(319,239)
0x802BA3C0  F7000000 4AC14AC1   ; <-- olive
0x802BA3D0  F64FC3BC 00000000
```

The `SETFILLCOLOR` with `4AC1` was searched for over the whole 8 MiB image: it
exists at exactly `0x802BA3C0` and `0x802F6500` (and nowhere else).

## 2. The instruction that picks the colour

`0x4AC1` occurs **once** in the entire ROM as an immediate, at ROM `0x0EEBE0` =
guest **`0x801A1E80`**, inside **record 3 (bank A)** — so this is not a renderer
or data-read question, it is a branch:

```
801a1e50: lbu  v0,0x7AB8(0x800E)   ; player index i
801a1e5c: v1 = i*12
801a1e6c: v1 = *(0x80197C88 + v1)  ; <-- the deciding word
801a1e70: andi v0,v1,0x2000
801a1e74: bnez v0,0x801A1E84       ; -> fade path
801a1e78: andi v0,v1,0x1000
801a1e7c: beqz v0,0x801A22C8       ; -> olive
801a1e80:   (delay slot) a0 = 0x4AC1
801a1e84: fade path: a0 = 0x4F00C308
```

`func_80078C6C` (the RDP rect builder, `0x801A22D8`) stores that `a0` as the
`G_SETFILLCOLOR` operand. The fade path's `0x4F00C308` is RGBA16 **`(0,24,24)`**
— i.e. the "black" the developer sees in retail. **So retail-vs-port here is
exactly one bit of one word: `*(0x80197C88)`.**

The ROM bytes were checked three ways (raw ROM at `0x0EEBE0` = `24 04 4a c1`,
`bankA.elf` disassembly, and the port's generated C at `BankAFuncs/funcs_0.c`
line 26168) and they agree, so the port's *code* is faithful. The divergence is
in the *data* it reads.

## 3. The port reads raw record-2 code where the branch wants data

`0x80197C88` is inside record 2's RAM block (`0x80197B90..0x8019E1A0`), and the
port holds **record 2's own image** there — verified by locating the whole image
in a live dump, not just one word:

```
record-2 image head (in the runtime's 4-byte-reversed order) found at 0x80197B90
  rec2+0x00000 -> 0x80197B90    rec2+0x000F8 -> 0x80197C88
  rec2+0x000F0 -> 0x80197C80    rec2+0x001F0 -> 0x80197D80
  rec2+0x006F0 -> 0x80198280    rec2+0x016F0 -> 0x80199280
```

so `*(0x80197C88)` is `0x10400009` (`09 00 40 10` = record 2's image at `+0xF8`,
and the raw ROM image at ROM `0x0E4A08` is `10 40 00 09` — same four bytes,
4-byte-reversed as the runtime stores them). The host build is `assets/ogre64.z64`,
the same Rev A image as the developer's cartridge (verified earlier: identical to
the `… (Rev A).n64` dump after 2-byte normalisation).

That word is a **MIPS instruction** (`bne v0,zero,+9`) — the game is reading code
where it expects descriptor data, and bit `0x1000`/`0x2000` are both clear, so the
branch takes the olive arm. The other arm would give `(0,24,8)` (§7).

**No store instruction targets the descriptor from record 2.** A full scan of
record 2's code (`ROM 0xE4910..0xEAF20`) for stores whose immediate falls in
`0x7C80..0x7D30` finds **zero**; a whole-ROM scan for stores with immediates in
`0x7C88..0x7D24` returns only 325 coincidences inside data blobs. So the writer,
if it exists in the port's tree, computes the address (base + `i*12` + `0xF8`)
rather than naming it. The one static artefact that *does* name the window is a
pointer table at **ROM `0x71288`** whose first entries are `0x80197C8C`,
`0x80197CA4`, `0x80197CBC`, `0x80197CC4`, `0x80197D24`, `0x80197D0C` — i.e. the
game treats those addresses as *destinations*. That is the remaining lead.

### Reference states: what they did and did not settle

* **`ParaLLEl N64` state** — RZIP, 129 zlib streams, parses cleanly to
  16 790 672 B, but its RDRAM base offset is not the documented mupen64plus one
  and could not be derived reproducibly (the layout in
  `tools/RT64/src/contrib/mupen64plus-core/tools/savestate_convert.c` puts RDRAM
  at `0x1B8`, where `0x80000400` then reads zero; deriving the base from the ROM's
  own code signature gives a consistent-looking `0x709CC`, but with that base the
  main segment matches the port only 19.5% and `0x80000400` is *zeros* while the
  ROM page signature sits at a shifted offset). **The `0x00021603` figure this
  handoff originally carried is withdrawn — it came from this unreliable base.**
* **The phone's mupen `(Rev 1).state`** (attached later, 938 935 B, also RZIP,
  129 chunks → 16 793 440 B) — its main-ELF code region validates exactly
  (`0x80000400` equals ROM `0x1000` reversed; ROM page `0x8000` lands exactly
  `0x7000` later), but its streamed-record zone does **not**: the only image of
  record 3's code sits at a low address (not `0x801A1E80`), record 2's image is
  absent, and `0x80197B90..` holds small-nibble dither-ish bytes. That state is
  therefore not on the attract screen (or its streamed RAM was not captured), so
  it cannot answer the question either.
* **The developer's screenshot** is the evidence that does hold, and it is
  colour evidence, in §7.

## 4. What the runtime probe established

A temporary probe on `recomp_mem_addr` (tag `probe84`, reverted — see §5) logged
every access into the table window with a `dladdr`-resolved writer symbol:

* `func_80071EB0 +0x9A9` reads `0x197C88`/`0x197C8C`/…/`0x197C9C` (6 words) once;
* `pi_perform_dma` then writes 96 bytes there (the record-2 DMA, bytes
  `0x197C88..0x197C9F`);
* **after that, nothing in the whole 30 s attract run touches the table.**

So in the port the table is *code*, and no resident game code ever populates it.
The ROM also has **no pointer word equal to `0x80197C88`**; the only near
reference is a pointer table at ROM `0x71288` whose first entries are
`0x80197C8C, 0x80197CA4, 0x80197CBC, 0x80197CC4, 0x80197D24, 0x80197D0C` — i.e.
the table is treated as *destinations* by something, which is the best lead for
finding the writer.

`tools/stubmap.py target 0x801A1B98` confirms the draw function is a proper
entry in `bankRec3` (unit A) whose callers are `bankRec07`/`bankRec05`/
`bankRec06` body interiors — nothing there explains the data.

## 5. Verification, and the probe's revert

* 8× attract run, 1× attract run, `OGRE_FOG=0` run: the olive band is present in
  all three (so it is not the `OGRE_FOG` repair, and not a speed artifact).
* `OGRE_SCENE=story` forced entry does **not** show it (the forced entry misses
  the pre-state — expected, AGENTS §9).
* Probe files touched: `tools/N64ModernRuntime/N64Recomp/include/recomp.h` only.
  Reverted; `grep -rl probe84 tools/N64ModernRuntime/N64Recomp/` is empty,
  `git -C tools/N64ModernRuntime diff -- N64Recomp/include/recomp.h` is empty,
  and `build-app` and `build-null` were both rebuilt after the revert.
* `git status --short` shows only session 83's pre-existing changes plus this
  handoff (no source, config or generated-C change).

## 6. Files changed

* `docs/HANDOFF-2026-09-18-session84.md` (this file)
* `PLAN.md`, `DECISIONS.md` — session-84 rows

No game code, no config, no generated C, no submodule change.

## 7. Colour evidence (corrected after the developer's reply)

**The two bands are the game's own 4:3 letterbox (the "background"), not an
emulator margin** — the developer corrected my first reading of the screenshot,
and the numbers agree: in the phone capture the picture occupies ~1056x590 of
1080x810, i.e. the game's 320x240 buffer with ~22 rows of bar above and ~37
below (a 320x180-ish picture area). The port draws the same two bands. So the
only defect is **the colour of the bands**, and the colour is fully determined:

| what | value | measured / derived |
|---|---|---|
| **port paints** | `0x4AC1` → RGBA16 `(9,11,0)` | **`(74,90,0)`** — measured, the dominant colour of both bands in every attract-story frame |
| **retail paints** (the other arm) | `0x4F00C308` → colour half `0x00C3` | **`(0,24,8)`** — visually black; this is the dark the developer sees |

The port's measured `(74,90,0)` is *exactly* `((9<<3)|(9>>2), (11<<3)|(11>>2), (0<<3)|(0>>2))`,
so the port is provably painting the `0x4AC1` arm and nothing else. The two arms
differ only by the flags word at `0x80197C88` (§2): either of bits `0x2000` or
`0x1000` set selects `(0,24,8)`; both clear selects `(74,90,0)`.

Nothing else about the colour is open. What is open is only *which data the
branch should read*, because that word sits inside record 2's **code** span and
the port therefore reads record-2 code bytes there. The measured band colour is
identical in every attract-story frame captured (frames `a2.504`..`a2.1090`),
so it is a stable state, not a one-frame glitch.

**Withdrawn:** the earlier reference figure `*(0x80197C88) = 0x00021603` came
from an RDRAM base offset I can no longer reproduce (the RetroArch `ParaLLEl N64`
RZIP layout does not match mupen64plus's documented one), so do not reuse it.
The colour table above does not depend on it.

## 8. Next step (revised)

The colour question is **closed**: the port paints `(74,90,0)` (measured, the
`0x4AC1` arm) where retail paints `(0,24,8)` (derived, the `0x4F00C308` arm) —
see §7. No renderer work is involved.

What is open is only *who is supposed to put descriptor data at `0x80197C88`*.
Concretely, in order of cheapness:

1. **Read the descriptor off a state that is actually at the attract screen.**
   Neither supplied state is (see §3). A `Mupen64Plus-Next` or phone state taken
   with the lore screen visible is the direct answer: if `*(0x80197C88)` has
   `0x1000`/`0x2000` set there, the port is reading the wrong thing at load time
   and the fix is in the bank/record that owns `0x80197B90`; if it is clear, the
   port's *value* is right and the difference is a write the port never performs.
   Note the port's `0x800E7A36` rect word is also wrong-looking
   (`0x01400600` vs the earlier reference's `0x0300 4014`), so ask for both.
2. **Chase the pointer table at ROM `0x71288`** (§3) — the only static artefact
   that names `0x80197C8C…0x80197D24` as destinations. Resolve each entry to its
   resident guest address, find the code that walks the table, and see whether the
   port ever runs it (`OGRE_COVER` already tells us which functions execute).
3. **A conditional write-watchpoint on the descriptor** armed after the
   scene-`0x0B` enter (not from boot) — `tools/watch.sh 0x80197C88` with the
   breakpoint symbol set past the record-2 DMA, to catch a writer that only runs
   on that screen. §4's probe covered the window from boot and saw none, so this
   is a long shot unless the writer is screen-specific.

Do not redo: the `0x801A1E80` ROM-byte verification, the boot-window watchpoint,
the `OGRE_FOG`/speed controls, or the two savestate parses.

