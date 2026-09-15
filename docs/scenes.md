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
| 2 | **cathedral**: the player's character walks to Archbishop Odiron; a few dialogue lines | dev (session 44) | crashes before drawing (RT64); the null build now enters and runs the step |
| 3 | Odiron asks the player's **name** → character-table entry form: a box with the typed name (`Magnus` in the capture) and a grid `A–Z` / `a–z` with `INS / BS / DEL / END` and a scrollbar | dev (session 44, screenshot) | unknown |
| 4 | Odiron asks the **date of birth** → form: `BIRTHDAY` banner, `Jul. 25`, and a second row `Trueno 12` | dev (session 44, screenshot) | unknown |
| 5 | **personality questions**: `"What dost thou hold within thy sword?"` with the choices `ardor / passion / vigor / talent / belief / hatred` over a live scene (characters in a hall); the answers decide the player's initial units and items | dev (session 44, screenshot) | unknown |
| 6 | an **intro movie** closes the sequence | dev (session 44) | unknown |

The step table's commands suggest (unverified) that steps 2 and 9 (`-3`) are the
dialogue parts, the `-10` steps (3–8, 14–16) the forms, and the single `-4`
step 10 the closing movie. Confirm against the captures when those steps run.

## Tutorial (title menu → Tutorial)

| what | scene | source | status |
|---|---|---|---|
| Deneb's dialogue box over the dark blue textured backdrop: `Welcome! / Is this your first time here?` | `0x17` | dev (session 43), proof (`native-tutorial-dialogue.png`) | renders |

The same form-box look (dark blue backdrop, dialogue/form panel) is what the
name-entry and birthday screens use, so the form module that draws the Tutorial
is the one to look at when those steps are reached.

## Open questions for the developer

* Scene `0x12` = Load Game (needs a save): what should it show with no save —
  hidden, greyed out, or an error? (Currently unreachable without a save.)
* The `0x18` menu: which menu is it, and what should it list?
* Does the opening skip the movie when Start is pressed during it? The port
  appears to (a held Start at New Game confirmation ends visit 1 in ~1.5 s
  instead of 28.7 s), and that is useful for iteration — but it may be a
  side effect of one long press rather than a real "skip" feature.
