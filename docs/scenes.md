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
| 6 | **the closing intro movie**, in five shots: (1) `ATLUS USA` / `presents`, (2) `Developed & licensed by` / `Quest / Nintendo`, (3) `Ogre Battle Saga` / `Episode VI`, (4) `Person of Lordly Caliber` over a flower pattern (the symbol of Lodis), (5) a montage of the player and friends travelling around the kingdom (several different scenes — to be detailed later) | dev (session 58, retail screenshots `docs/proofs/intro-movie-reference/retail-shot-1..4.png`) | **renders — developer-confirmed** (session 58: *"the intro movie played perfectly, with all different animations (some of them are complex)"*). Scene `0x16` plays all five shots in order: `docs/proofs/native-newgame-movie-atlus.png`, `-quest.png`, `-episode-vi.png`, `-lodis.png` (the flower/card shot), and the montage — `-campfire.png`, `-sunset.png` (the party on a cliff at sunset), `-map.png` (the parchment world map with the red route and dagger, `Alta Región` / `Southern Coast` / `Zetegin Sea`). It chunk-DMAs ROM `0x244770` (0x7500) → RAM `0x801D0860` — record 10's arena, which unit C's `bankRec10a` also owned — and that module was uncompiled, so every call into it hit the runtime's streamed stub (session 57 §2). It is now **bank unit I** (43 functions; `bankRec10a` moved to unit **J** so the two banks of that RAM are separate units) and the scene plays and **advances out of it** (repeated `0x02 → 0x0D → 0x16` cycles, no stub calls). The sequence then **crashes** at the end — the developer's end-of-sequence crash, now reproduced: `SIGBUS` in `func_ovlC_8022C270 + 0x53A` with the step word `D_8018F1C0 = 0x0431` (step 1073, past the decoded 19-step table). See `docs/HANDOFF-2026-09-16-session58.md` §2/§3. **Session 59 fixed the crash** (row 7) and the sequence now runs on past it. |
| 7 | a **chapter animation**: characters are revealed with an animation over the text `Prologue` / `Casting their gaze on the ground, trudging along...` | dev (session 58) | **renders** (session 59): reached at step **1073** (asset `0x01A1625A`, **command `-8`** — not "command 6", see `docs/HANDOFF-2026-09-16-session59.md` §4c). The step's `-8` arm (`func_ovlC_8022683C` @0x802269DC) streams a **third bank** of the record-14 arena (ROM `0x286BA0`, 0x138F0 → RAM `0x8022ACB0`) that the port had no code for, and `bankRec14a` — the other bank of that RAM, which *was* compiled into unit C — was bound at build time to the interpreter's `jal 0x8022C270`/`0x8022C6E4` and the callback's `jal 0x8022E3F0`; with the chapter module resident the interpreter therefore ran rec14a's layout and crashed. The two banks are now **units K and L**, the calls compile as `LOOKUP_FUNC`, and the card draws: `docs/proofs/native-newgame-prologue-card.png`. Steps **1073..1077** are its five phases. |
| 8 | another **movie**: the player being received for duty with other soldiers | dev (session 58, screenshots offered) | **renders** (session 59, unconfirmed by the developer): after the chapter animation, scene `0x0D` plays the hall scene with `General Godeslas` — *"It looks like some of you are gr…"* — over rows of soldiers (`docs/proofs/native-newgame-received-for-duty.png`), then the `Grey-Haired Old Man` / *"So you're Magnus… Hmm… I see."* dialogue with Magnus (`docs/proofs/native-newgame-magnus-old-man.png`). The sequence then reaches **scene `0x05`** (below). |
| 10 | **scene `0x05`** — unknown; the sequence's next scene after the post-movie story (descriptor `0x8018FD70`, mask `0x2`) | code session 59 | **partial: black.** The scene enters and its module loads (unit **M**, ROM `0x79750` (0xDAD0) → RAM `0x8019A7C0` — the *other* bank of the same RAM as scene `0x07`'s form module, unit H) with **no stub calls**, and it presents static **black** frames (`OGRE_PRESENT_ALWAYS=1`; without it RT64 never re-presents the unchanged frame, so the run looks stalled). Its update/hook are scene `0x07`'s (`0x8017B858`/`0x8017B9C8`) and its enter `0x8017B60C` is a near-twin of `0x07`'s `0x8017B794`, so it looks like another UI/form screen — **what should it show?** |
| 9 | the **Controller Pak menu** (reached from a later scene): `Save` / `Load` / `Erase` / `Exit` over `OgreBattle Game Notes`, with `Game Data 1` / `Game Data 2` notes, `Pages`, `No Data`, and the system messages (`Insert Controller Pak.`, `Saving data.`, `Data saved to Controller Pak.`, `1 note 25 pages to save.`, `Saving data has failed.` and its Loading/Deleting variants) | dev (session 58: *"the game has a save system, which we should arrive in one of the next scenes"*); text ROM `0x790EC..0x7967C` | **missing** — the device is unimplemented. The save system is the **Controller Pak** (`osPfs*`): 105 calls into the PFS cluster from the main segment, and the menu's strings live in **bank unit H** (`D_ovlH_801A260C`, the same UI module as the name/birthday forms), so drawing should already work. The runtime's `librecomp/src/pak.cpp` is an upstream stub returning `PFS_ERR_NOPACK` for every entry point, and `recomp::SaveType` has no Controller Pak, so the game currently sees "no pak inserted". See `docs/HANDOFF-2026-09-16-session58.md` §3c |

The step table's commands suggest (unverified) that steps 2 and 9 (`-3`) are the
dialogue parts, the `-10` steps (3–8, 14–16) the forms, and the single `-4`
step 10 the closing movie. Confirm against the captures when those steps run.

**The step's command comes from the descriptor's last word.** `func_ovlC_80227E64`
takes the last word of the decompressed step descriptor; when its high byte is
`0xFF`, the low byte (1..8) indexes the table at `0x8022ABE0`, which maps it to
one of `-3 -6 -5 -4 -7 -8 -9 -10`; `func_ovlC_8022683C` then indexes the table at
`0x8022ABA0` with `command + 10` to pick the setup arm (and the bank DMA that arm
performs). The movie's last words (bytes 5,1,1,5,1) map to commands
`-7 -3 -3 -7 -3`, and the chapter animation's step 1073 (last word `0xFF000006`,
byte 6) to **`-8`** — the arm that streams the chapter module. Session 58's
"command 6" was the raw byte. See `docs/HANDOFF-2026-09-16-session59.md` §1a.

**The movie is one scene in five phases** (developer, session 58): *"from the
player's perspective, it's a single scene, there's no time interval or fade to
black transitions. it seems that the game reuses the same scene and displays
content in 5 different phases"* — so the port's repeated `0x02 → 0x0D → 0x16`
visits are the correct shape, and the five phases are steps **970..974** (the
first-word opcodes `80000006`; the phase count matches the shot count exactly).
The **chapter animation** is the same shape with steps **1073..1077** (its module
switches on `(step - 1073) & 0xFFFF < 5`).


Session 45 got as far as step 2 (which renders and then waits: its dialogue is
advanced with the button the quill prompts). **Session 56 ran the whole opening**
with a fixed tap-A schedule (developer-confirmed end to end): name form → date of
birth → personality questions → `0x16`. **Session 57 fixed the stale backdrop**
(it was the njpeg readback copying the *previous* screen — RT64's scratch word is
stale at the first pass of every assembly; `docs/HANDOFF-2026-09-16-session57.md`
§1) and found what stops the port right after `0x16`: the scene chunk-DMAs **ROM
`0x244770` → RAM `0x801D0860`**, which `config-bankC.yaml` carries as a `bin` gap
while `bankRec10a` owns that RAM, so the calls hit the runtime's streamed stub and
spin (§2). **Session 59 fixed the sequence-end crash** — the chapter animation's
step streams a *third bank* of the record-14 arena (ROM `0x286BA0` → RAM
`0x8022ACB0`) that the port had no code for, so the interpreter ran rec14a's
layout (§1) — and the opening now plays **past** the movie and the chapter
animation into the post-movie story and scene `0x05`. The tap schedule that
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
