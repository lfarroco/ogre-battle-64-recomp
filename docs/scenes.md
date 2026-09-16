# What each screen is supposed to show

The one thing the code cannot tell us. AGENTS §1 makes the developer the oracle
for **intent**; this file is where that intent is written down, so a future
session can tell "the port renders the wrong thing" from "the port renders the
right thing wrongly".

Rules for this file:

* Every row cites where the description came from (`dev` = the developer, with
  the session; `proof` = a capture in `docs/proofs/`; `code` = inferred from the
  ROM — mark those **unverified**).
* Keep it about *what the player sees/does*, not about how it is drawn.
* Add rows as screens are identified; do not delete a row when the port breaks
  it — the status column is the interesting part.

Status values: **renders** (matches the description), **partial**, **missing**,
**crashes**, **unknown**.

## Boot and attract

| what | scene | source | status |
|---|---|---|---|
| boot intro: soldiers, falling cube, Nintendo 64 logo | `0x09` | code + `docs/proofs/native-intro-*.png` | renders |
| publisher stills: Licensed by Nintendo / ATLUS / QUEST (640x480) | `0x0A` | proof (`native-licensed-screen.png`, `native-atlus-screen.png`, `native-quest-screen.png`) | renders |
| title: prologue text, then the logo + a menu over scrolling clouds | `0x04` | dev (session 43), proof (`native-title-menu.png`) | renders |
| attract story (world map) | `0x0B` | code | renders |
| attract unit-info book | `0x0C` | code | renders |

The title menu (no save) is **New Game / Tutorial / Stereo**, with
`Load Game` inserted as the second entry when a Controller Pak save exists
(`New Game / Load Game / Tutorial / Stereo`). `Stereo` is toggled with
left/right, it is not a screen. See session 43 for the counter mapping.

## New Game — the opening sequence (developer, session 44)

This is one flow made of several *steps*; the game plays them as repeated
`0x02` (loader) → `0x0D` (step) visits, one step per visit, with the step index
in `D_8018F1C0` (see `docs/HANDOFF-2026-09-15-session44.md` §1 for the decoded
step table).

| # | what the player sees | source | status |
|---|---|---|---|
| 1 | **the intro movie**: a multi-shot sepia cutscene in a castle courtyard, ending on the subtitle *"I promise I'll make you proud."* (~28.7 s) | dev (session 43), proof (`native-newgame-cutscene.png`, `native-newgame-cutscene-2.png`) | renders |
| 2 | **cathedral**: a narrative card (`Ischka Military Academy / Graduation Ceremony`), then the player's character walks to Archbishop Odiron; a few dialogue lines. The backdrop is a **pre-rendered 320x240 image** (red carpet, columns, steps, organ; the candles animate) | dev (session 44 + retail reference, session 46), proof (`native-newgame-academy-card.png`, `native-newgame-cathedral.png`, `native-newgame-cathedral-background.png`) | **renders** (session 52): the card, the **cathedral backdrop** (red carpet, stone walls, statues, candles) and the dialogue box all draw, on the RT64 build. The backdrop is an njpeg **YUV16** image drawn by an **S2DEX2** list (session 51 implemented the missing `G_OBJ_MOVEMEM`/`G_OBJ_RECTANGLE_R` geometry); the colours were wrong (blue banding) until session 52 fixed the `G_SETCONVERT` conversion (sign-extend the 9-bit fields, scale `2*K+1`, pair `K1` with U). See `docs/HANDOFF-2026-09-15-session52.md` |
| 3 | Odiron asks the player's **name** → character-table entry form: a box with the typed name (`Magnus` in the capture) and a grid `A–Z` / `a–z` with `INS / BS / DEL / END` and a scrollbar | dev (session 44 + session 54, screenshot) | **renders** (session 55): reached as scene `0x07`; the name box, `A–Z`/`a–z` grid, `◀ ▶ INS BS DEL END`, the `Yes/No` prompt and the character cursor all draw, and the form hands the opening back to `0x02`/`0x0D` when confirmed. Proof `docs/proofs/native-newgame-name-entry.png` |
| 4 | Odiron asks the **date of birth** → form: `BIRTHDAY` banner, `Jul. 25`, and a second row `Trueno 12` | dev (session 44, screenshot) | **renders** (session 56, developer): reached as the next `0x0D` step after the name form and playable — but the cathedral backdrop behind it can show a **rectangular stale region** (the name form's grid) instead of the backdrop (session 56 §1, open) |
| 5 | **personality questions**: `"What dost thou hold within thy sword?"` with the choices `ardor / passion / vigor / talent / belief / hatred` over a live scene (characters in a hall); the answers decide the player's initial units and items | dev (session 44, screenshot) | **renders** (session 56, developer): the steps after the birthday form run and are answerable; the same intermittent backdrop artifact can appear. The sequence then reaches **scene `0x16`** (descriptor `0x8018FC00`, mask `0x400`) |
| 6 | **the closing intro movie**, in five shots: (1) `ATLUS USA` / `presents`, (2) `Developed & licensed by` / `Quest / Nintendo`, (3) `Ogre Battle Saga` / `Episode VI`, (4) `Person of Lordly Caliber` over a flower pattern (the symbol of Lodis), (5) a montage of the player and friends travelling around the kingdom (several different scenes — to be detailed later) | dev (session 58, retail screenshots `docs/proofs/intro-movie-reference/retail-shot-1..4.png`) | **renders — developer-confirmed** (session 58: *"the intro movie played perfectly, with all different animations (some of them are complex)"*). Scene `0x16` plays all five shots in order: `docs/proofs/native-newgame-movie-atlus.png`, `-quest.png`, `-episode-vi.png`, `-lodis.png` (the flower/card shot), and the montage — `-campfire.png`, `-sunset.png` (the party on a cliff at sunset), `-map.png` (the parchment world map with the red route and dagger, `Alta Región` / `Southern Coast` / `Zetegin Sea`). It chunk-DMAs ROM `0x244770` (0x7500) → RAM `0x801D0860` — record 10's arena, which unit C's `bankRec10a` also owned — and that module was uncompiled, so every call into it hit the runtime's streamed stub (session 57 §2). It is now **bank unit I** (43 functions; `bankRec10a` moved to unit **J** so the two banks of that RAM are separate units) and the scene plays and **advances out of it** (repeated `0x02 → 0x0D → 0x16` cycles, no stub calls). The sequence then **crashes** at the end — the developer's end-of-sequence crash, now reproduced: `SIGBUS` in `func_ovlC_8022C270 + 0x53A` with the step word `D_8018F1C0 = 0x0431` (step 1073, past the decoded 19-step table). See `docs/HANDOFF-2026-09-16-session58.md` §2/§3 |

The step table's commands suggest (unverified) that steps 2 and 9 (`-3`) are the
dialogue parts, the `-10` steps (3–8, 14–16) the forms, and the single `-4`
step 10 the closing movie. Confirm against the captures when those steps run.

Session 45 got as far as step 2 (which renders and then waits: its dialogue is
advanced with the button the quill prompts). **Session 56 ran the whole opening**
with a fixed tap-A schedule (developer-confirmed end to end): name form → date of
birth → personality questions → `0x16`. **Session 57 fixed the stale backdrop**
(it was the njpeg readback copying the *previous* screen — RT64's scratch word is
stale at the first pass of every assembly; `docs/HANDOFF-2026-09-16-session57.md`
§1) and found what stops the port right after `0x16`: the scene chunk-DMAs **ROM
`0x244770` → RAM `0x801D0860`**, which `config-bankC.yaml` carries as a `bin` gap
while `bankRec10a` owns that RAM, so the calls hit the runtime's streamed stub and
spin (§2). The developer's **sequence-end crash** is still not reproduced in the
port (200 s, exit 0). The tap schedule that
reaches the sequence without skipping the movie is
`OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D`; a schedule that keeps
pressing buttons inside `0x0D` advances the dialogue and the step
(the "Ischka" card capture came from such a run).

**Shortcut (session 48):** `OGRE_SCENE=new-game OGRE_STEP=<n>` seeds step `n` so
the app can reach a later step without playing the ones before it, e.g.
`OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1
./build-app/ogrebattle64` reaches the cathedral ~2 s after boot instead of after
the 28.7 s movie. **It is a seed, not a hold** (session 53), and it changes the
sequence's own state: a step entered with no step-1 visit makes the exit take its
`otherwise` arm and re-enter `0x0D` at step 0, which is the movie-mode branch and
crashes. **For the real opening use the title route** (session 54):

```sh
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,start,a,start,a,start,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=200000 ./build-app/ogrebattle64
```

Measured by the developer's account and by `OGRE_PROBE57=1`: the attract title
ignores input until a **Start** press summons the menu; the cursor starts on
`New Game`, so a second Start confirms it. The route then runs the movie (step 1)
→ cathedral (step 2) → **scene `0x07`**, the name-entry form, at `t≈19.0 s` (4×),
which **renders** (session 55) and hands off to `0x02`/`0x0D` at `t≈20.7 s` when
the name is confirmed.

## Tutorial (title menu → Tutorial)

| what | scene | source | status |
|---|---|---|---|
| Deneb's dialogue box over the dark blue textured backdrop: `Welcome! / Is this your first time here?` | `0x17` | dev (session 43), proof (`native-tutorial-dialogue.png`) | renders |

The same form-box look (dark blue backdrop, dialogue/form panel) is what the
name-entry and birthday screens use, so the form module that draws the Tutorial
is the one to look at when those steps are reached.

## New Game → scene `0x07` = the name-entry form

After the cathedral dialogue, the sequence engine's exit resets `D_8018F1C0` to
0 and sets the next scene to **`0x07`** (`func_80178CB0`, session 54). **`0x07`
is the name-entry form** — the developer supplied a retail screenshot showing the
name box (`Magnus`) and the `A–Z` / `a–z` grid with `INS BS DEL END` (session 54).

The descriptor is **`D_8018FDAC`** (streamedB ROM `0x65CAC`; the dispatcher's
accessor table maps id 7 → `func_8017B600`, session 55, correcting session 54's
`D_8018FB98`): enter `func_8017B794`, update `func_8017B858`, hook
`func_8017B9C8`, mask **`0x00000002`** (records 2 and 1). The enter chunk-DMAs a
**0x8600-byte code module, ROM `0x712A0` → RAM `0x8019A7C0`**, and calls it; the
name box, character grid, `Yes/No` prompt and the `Magnus` default are that
module's data (`ABCDEFGHIJ…` at `0x801A1B7C`, `Magnus` at `0x801A1B88`).

**Status: renders** (session 55) — the form draws over the blue textured
backdrop, accepts input, and hands the opening on to `0x02`/`0x0D` again when
confirmed. Proof: `docs/proofs/native-newgame-name-entry.png`. Session 54's
black screen was **not** a missing module: the module's RAM overlaps record 3
(unit A) and overlay C (the main ELF), so the port's build-time bindings ran
overlay C's bodies at its addresses. The module is now **bank unit H** and the
six calls from resident code into it are dispatched through the runtime bank map
(`LOOKUP_FUNC`), so the resident bank's layout is the one that runs.

## Open questions for the developer

* Scene `0x12` = Load Game (needs a save): what should it show with no save —
  hidden, greyed out, or an error? (Currently unreachable without a save.)
* The `0x18` menu: which menu is it, and what should it list?
* Does the opening skip the movie when Start is pressed during it? Confirmed in
  session 45: with taps gated to `OGRE_TAP_NOT_SCENE=new-game,0x0D` (no input
  inside `0x0D`) visit 1 runs its full ~28.4 s; with taps that keep firing inside
  `0x0D` it ends in ~1.4 s. So a Start press *does* skip the movie. Is that the
  retail behaviour ("skip cutscene") and is it only Start, or any button?
