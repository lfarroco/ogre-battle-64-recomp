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

## How the game is organised: 25 scene types, a script, and a lot of content

Worth reading before the tables, because it decides how much of the game is
"linking" and how much is data. **`tools/scenemap.py` prints all of this from the
ROM in ~3 s.**

There are exactly **25 scene types** (`0x00`..`0x18`). The scene manager
`func_80075BC0` builds `D_800AF028[0..24]` at boot from a hardcoded list of 25
accessor stubs, each returning a 5-word descriptor (`enter`, `update`, `hook`,
`leave`, bank-record `mask`). Beyond the code, the game has three layers:

1. **The registry** (code, but fully enumerable) — 25 types, their descriptors and
   their four hooks. Nothing here needs discovering by hand any more.
2. **The transitions** (code, and small) — *every* place in the game that sets the
   pending scene `D_800C4C26` is **39 sites in 30 functions**. That is the entire
   scene-to-scene graph. Most are `I am done, go to X` at the end of a type's own
   update/leave.
3. **The content** (data, and huge) — an **1693-entry scripted step table** plus a
   per-step asset. The 200+ dialogue scenes are *steps* in that table, driven by
   the same few scene types (mainly `0x0D`, the movie/dialogue engine, and `0x02`,
   its one-frame loader).

So the flow is **not** 200 hand-linked scenes. It is 25 reusable types reading
script data the game already ships; the work is decoding a *format* once, and it
then applies to every dialogue in the game — the same shape as the 75 njpeg
backgrounds (`docs/guides/njpeg-backgrounds.md`). The New Game opening is already
an example of this: `title (0x04) → 0x02 → 0x0D → 0x02 → 0x0D → …`, one step per
visit, with the step index in `D_8018F1C0`.

## Boot and attract

| what | scene | source | status |
|---|---|---|---|
| boot intro: soldiers, falling cube, Nintendo 64 logo | `0x09` | code + `docs/proofs/native-intro-*.png` | renders |
| publisher stills: Licensed by Nintendo / ATLUS / QUEST (640x480) | `0x0A` | proof (`native-licensed-screen.png`, `native-atlus-screen.png`, `native-quest-screen.png`) | renders |
| ↳ the QUEST still itself (dev, session 77): **three overlapping 3D "q" shapes** (blue/green → yellow → red) over the `QUEST` wordmark | `0x0A` | dev + `docs/proofs/native-quest-screen.png` | renders |
| title: prologue text, then the logo + a menu over scrolling clouds | `0x04` | dev (session 43), proof (`native-title-menu.png`) | renders |
| attract story (world map) | `0x0B` | code | renders |
| attract unit-info book | `0x0C` | code | renders |
| **save-data / Controller Pak menu — hold Start through the boot window** | `0x18` | dev (session 88) + code + proof (`native-boot-start-controller-pak-menu.png`) | **renders** (session 88): `Controller Pak Menu` over `Save / Load / Erase / Exit`, the `Game Notes` / `Pages` list (`1:`..`4:`, `Left`, a scrollbar) and a note box (`Note 1` / `Not an Ogre Battle 64 note.` with a fresh pak). Entered at `t≈2.4 s` of a Start-held boot; before session 88 the scene sat on a black frame with no display lists (the enter's call into unit H was bound to the wrong bank — see the note below). |

The title menu (no save) is **New Game / Tutorial / Stereo**, with
`Load Game` inserted as the second entry when a save exists
(`New Game / Load Game / Tutorial / Stereo`). `Stereo` is toggled with
left/right, it is not a screen. See session 43 for the counter mapping.

**Correction (session 66, dev-confirmed): that save is the cartridge battery
(SRAM), not a Controller Pak.** A/B on the same `title` + Start tap schedule:
with **no** save file the menu enters **New Game** (`0x04 → 0x02 → 0x0D`); with a
converted emulator battery save whose slot 0 has data, the same taps enter
**Load Game** (`0x04 → 0x12`) and then the **map** (`0x05`) — the developer
watched the loaded game come up. So the title's `Load Game` and its default
cursor read the **battery** save, and the Controller Pak is only the
copy/backup device (session 65 §9-§10). See
`docs/HANDOFF-2026-09-17-session66.md` §3.

### The boot-Start save menu (scene `0x18`) — developer, session 88

Holding **Start while the game boots** opens the save-data menu instead of the
boot intro. The branch is a single test in the boot init, at `0x800721DC`:

```c
if (*(u16 *)0x800E79B0 & 0x1000)   /* the pad report's Start bit */
    pending_scene = 0x18;          /* the menu */
else
    pending_scene = 0x09;          /* the boot intro */
```

`D_800E79B0` is the controller-0 report the boot's own poll fills
(`func_8007297C`'s loop writes it); Start is `0x1000`, so a *held* Start is what
matters — a tap at boot is a 50/50. Scene `0x18`'s descriptor is **`0x8018FDC0`**
(mask `0x2`), enter `func_8017BA60`, update `func_8017BB28`, hook
`func_8017BB54`. **Correction to session 65 §9:** the "extra callback words"
`+0x14`/`+0x18`/`+0x1C` it found at `0x8018FDAC` are scene `0x07`'s descriptor
plus `0x14`, i.e. **scene `0x18`'s own descriptor** at `0x8018FDC0` — there are
no unread descriptor words, and `func_8017BB28` is not scene `0x07`'s callback.

The enter chunk-DMAs **unit H** (ROM `0x712A0` → RAM `0x8019A7C0`, the same
module as the name form) and calls `func_8019D67C` in it; the per-frame update
and hook call `0x8019C69C` and `0x8019C4A8`. All three go through the runtime
bank map (`make recomp`'s `cross_bank.py dispatch --only` list). The third one
(`0x8019D67C`) was **not** in the list until session 88: `config.toml` extends
`func_8019D568` to `0xCB0`, which swallows `0x8019D67C`, so N64Recomp bound the
`jal` to `func_8019D568` and emitted the redirect-plus-early-`return` shape —
the enter returned before the menu initialised and the scene stayed black. See
`docs/HANDOFF-2026-09-19-session88.md`.

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
| 10 | **the map scene** — the world map: terrain and rivers, location labels, the route of dots, unit markers and the `Flama` cursor, inside a stone frame. (Developer, session 60: *"the next scene after this prologue movie is an important one: it's the map scene. from there, it should be possible to save the game"*.) | dev (session 60) + code | **renders** (session 60): descriptor `0x8018FD70`, mask `0x2`; scene `0x05`. Proof `docs/proofs/native-newgame-map-scene.png`. The scene enters at t≈180 s on the natural route and, before this session, stayed on the prologue's black fade because **no frames were produced**: two calls in the scene update/hook (`func_8017B858` @0x8017B8A0 `jal 0x8019AF0C`, `func_8017B9C8` @0x8017BA10 `jal 0x801A103C`) were compiled as "call the containing body + early return" (a `jal` into a size-overridden body interior), so each abandoned the caller's frame; the frame-pump thread then read its `$s0`/`$s1` from the wrong stack slots, `$s1` (the message-type comparator) became 0 and the pump stopped after two frames. Both targets are in the RAM the scene module occupies and are now dispatched through the bank map (`make recomp`'s `--only` list) — `0x8019AF0C` resolves to unit **M**'s own state-1 handler, which is what the game's state machine asks for (scene `0x05`'s enter sets `0x801977E8 = 1`; scene `0x07`'s sets `3` and takes a different branch, which is why the form worked). See `docs/HANDOFF-2026-09-16-session60.md` |
| 9 | the **Controller Pak menu** (reached by **holding Start through the boot window**): `Save` / `Load` / `Erase` / `Exit` over `OgreBattle Game Notes`, with `Game Data 1` / `Game Data 2` notes, `Pages`, `No Data`, and the system messages (`Insert Controller Pak.`, `Saving data.`, `Data saved to Controller Pak.`, `1 note 25 pages to save.`, `Saving data has failed.` and its Loading/Deleting variants) | dev (session 88: *"if you hold start while it is booting, it allows you to access a special menu for saves"*); text ROM `0x790EC..0x7967C` | **renders — this is the boot-Start menu (session 88)**: scene `0x18`, descriptor `0x8018FDC0`; proof `docs/proofs/native-boot-start-controller-pak-menu.png` (`Controller Pak Menu` / `Save Load Erase Exit` / `Game Notes` `Pages` / note box). **Device implemented session 65**, menu bound to the wrong bank until session 88. Note the framing: this menu is the **copy/backup** path between the cartridge save and a pak (`Data loaded to Game Pak.` / `Data saved to Controller Pak.`); **the game's own save is a battery-backed cartridge save (SRAM/FlashRAM/EEPROM)**, not this (developer, session 65). The pak device (flat 32 KiB, `.mpk`-compatible) is implemented over `osPfs*`: 105 calls into the PFS cluster from the main segment, and the menu's strings live in **bank unit H** (`D_ovlH_801A260C`, the same UI module as the name/birthday forms). See `docs/HANDOFF-2026-09-16-session58.md` §3c and `docs/HANDOFF-2026-09-19-session88.md` |

### The map screen (scene `0x05`) — what it must show (developer, session 60)

The developer's retail screenshot is `docs/proofs/map-reference/retail-map-screen.png`
(the state right after the prologue). The screen is the framed world map plus:

* the **player's party sprite** at its location (a blue knight), and the other
  **unit markers**;
* a **cursor** (white arrow + red dot) on the selected location;
* the **date panel** in the bottom-right: a small `MONTH` / `DATE` box and the
  current date in the italic serif face, e.g. `Sombra 1` (`Sombra` is the month
  name, `1` the day).

**Status (session 64): the port is faithful here — the open lead is renderer-side,
not game data. Nothing landed.** The terrain, the location labels, the road of dots
and some unit markers draw. The developer corrected sessions 61/62 about the party:
the knight (**Magnus**) is the draw whose rect sits **above** the other — the
**second** builder call (`func_ovlM_801A2A7C`, ROM `0x81BFC`, `a2=0xA`, rect
`(a0-16, a1-24)`) — and it must be **`16x24`** (his hair is visible in some
sprites); the **first** call (ROM `0x81B50`, `a2=0xB`, rect `(a0-8, a1)`) is the
**shadow** and must be `16x11`. Live sprite-table entries are slot 12 = `(16,24)`,
slot 10 = `(16,11)`, slot 11 = `(144,23)`, so the two calls currently name each
other's entries — which is why the shadow drew as the row of ~18 repeated ellipses
(`144x23` over a `7x10`-texel window) and the knight as a small dark blob.

**Session 64 supersedes the "zeroed texture buffer" reading.** The **knight is not
an asset**: the map's enter `func_ovlM_8019A7C0` `malloc(0x18000)`s `state[+0x34]`
and **composites** an RGB555 LUT over an 8-bit index image into it — a
**32-texel-wide** RGBA32 sheet of **24 frames** (`0x1000` bytes each = 8 directions
× 3 animation frames, selected by `3*state[0x1DC] + f`) — and it **does** contain
the knight. What is genuinely zero is the **shadow's** `state[+0x04]+0x1068`
(`state[+0x04]` *is* asset `0x01DD210A`, verified byte-for-byte against an offline
decode; the only translucent-black shadow art in any map asset is at `+0x10AC` of
that same asset). **`state` is a heap pointer — read it from `*(0x80197B18)`.**
The port's decode, its GBI choice (`0x8009F540` → `F3DEX2.fifo 2.08`) and its
recompiled code all check out, so the remaining defect is stated as a **renderer**
question: the knight sheet's row stride is 128 B (`line` 16) while the draw's
render tile declares **`line=8`**, and the shadow's `line=2` + `masks=3` +
7-texel window is only self-consistent read as **16-bit** — a `G_LOADBLOCK`/
tile-line semantics question, and it needs no game-data theory. The cursor
(`a2=6`) and the date panel (`a2=12`, `MONTH`/`DATE` box and day digit still
absent) are the same rect-vs-window class. Do **not** "fix" the entry indices or
`0x1068` by editing generated C. See `docs/HANDOFF-2026-09-17-session64.md` and
`docs/guides/emulator-first.md`.

### The map's `R` menu (developer, session 60)

While on the map, pressing **R** opens a horizontal menu:

| # | entry | what it opens |
|---|---|---|
| 1 | **Organize** (the default selection) | the screen where the player manages the army |
| 2 | **Hugo Report** (press → once) | information about the game |
| 3 | **Settings** | a window with game/text speed and misc settings |
| 4 | **Save** | a window with **two slots stacked vertically**; selecting one shows the confirmation *"Any existing data will be overwritten. Proceed?"* with **Yes/No** (Yes is the default). Choosing Yes saves the game |

### The title's `Load Game` entry (developer, session 60; save device corrected session 66)

When a save exists, the title menu's cursor **starts on `Load Game`** (instead of
`New Game`), and selecting it goes to the **Load Game screen**, which lists the
two saves. **Session 66 corrects the device: it is the cartridge battery
(SRAM), not the Controller Pak** — verified A/B on the same tap schedule (no save
→ New Game; a converted emulator battery save with slot 0 populated → Load Game
`0x12` → the map `0x05`, dev-confirmed). See
`docs/HANDOFF-2026-09-17-session66.md` §3.

### The mission (scene `0x03`) — what it must show (developer, session 67)

The mission the player is sent to from the map. It is **not** the map scene: the
developer's reference (a retail screenshot, session 67) and words describe *"a
scene with lots of 3d elements, and some 2d characters over it"* — 3D terrain
with cliffs, woods and rivers; the party sprite as a **2D** character with
selection brackets; a **`NOTE` tooltip** carrying the mission's losing condition
(*"Invasion of your headquarters. Death of Magnus."*); the **location label**
(`Zemio` in the reference) with its illustration; and the `MISSION` banners along
the bottom.

The descriptor is **`0x8018F350`** (streamedB; selected by `*(0x80193700)`, which
also picks scene `0x06`'s two descriptors): enter `func_80173724`, update
`func_801737CC` (a no-op), hook `func_801737D4`, mask **`0x38C`** = records
**2, 3, 7, 8, 9**. The enter sets `D_801977E8 = 2` (scene `0x05`'s map sets `1`,
scene `0x07`'s form sets `3`) and allocates the `0xC000`-byte 320x240 buffer at
`0x8019EE70`.

| what | source | status |
|---|---|---|
| the mission map: 3D terrain, cliffs, woods, rivers, the road; the 2D party sprite with its selection brackets | dev (session 67 reference) | **renders** (session 67): `docs/proofs/native-mission-scene.png` |
| the UI: the `Stronghold` tooltip + the `R` button hint; the unit panel (`No. / FRIENDLY / STATUS`, `1. Magnus`, `STRONGHOLD / Zemio`, `START ^ FATIGUE`) | dev + proof | **renders** (session 67): `docs/proofs/native-mission-unit-panel.png` |
| the **intro sequence**: the camera pans from the party's fort to the enemy fort, the `NOTE` *winning condition* appears, the camera goes back, the `NOTE` *losing condition* appears, then `MISSION START`, then the mission is playable. The target fort is drawn as a **framed illustration + its place label** (`Theodricus Mine` in the session-68 capture) with a red pin. The camera pans on its own and **each phase waits for an `A` press** (developer, session 68) | dev (session 68) | **renders on the natural route** (session 68): `docs/proofs/native-mission-intro-natural.png`, `-natural-pan.png`. Before session 68 the fort drew garbled and `A` did nothing at all (see below) |
| the mission `MISSION` banners along the bottom | dev + proof | renders |
| the `NOTE` losing-condition tooltip, the `R` menu inside a mission | dev (session 67 reference) | unknown — not seen in the captures yet |
| **combat** — a battle between units on the mission field | dev (session 72): *"combat played beautifully"* | **renders** (session 72) — bank units W (record 9's combat bank) and X (record 10's battle bank) |
| a **neutral encounter** — a rare map event: a moving unit "encounters a wild \<monster\>" (the message quotes the unit leader and the class, e.g. `Magnus: "A wild Young Dragon? Here!?"`), a battle then starts against the wild monster, which can be weakened and **persuaded** to join (the GameFAQs guide the developer pasted; the class/terrain/level tables are `0x801ED780`/`0x801ED79E`, see `docs/HANDOFF-2026-09-18-session82.md`) | dev (session 82) + code | **renders — developer-confirmed** (session 82): the message and the monster battle are `docs/proofs/native-neutral-encounter-message.png`, `-battle.png`. Before session 82 the battle began with an **empty enemy side and won instantly**: the wild unit is created by the battle scene's setup fragment (ROM `0x23A370` → RAM `0x801D0860`), which the port had no record for, so its entry hit the runtime's streamed stub. It is now **bank unit AH** (`bankRec10s`) |
| the **settings / options** screen (`Message speed`, `Cursor speed`, `Help display`, `Icon name display`, `Game speed`, `Legion indicator`, `Destination display`, `Unit report type`, `Battle action name`, `Battle animation`, `Quick exit`, `Cancel all`, `Sound settings`, `Restore defaults`) | dev (session 72) + code (the module's string table) | **renders** (session 72) — bank unit V |
| a town **shop** (`What do you have on sale?`, `Hello, how much is this?`, …) | dev (session 72) + code (the module's string table) | **renders** (session 72) — bank unit AB |
| **the Witch's Den** — the witch's shop interior: cauldron, shelves of bottles, a counter with `WAR FUNDS 0001000 Goth`, and the **Old Witch** portrait saying *"Heh heh heh... Can I help you?"*. **Visiting it lets the player revive dead party soldiers.** | **scene `0x14`** — dev (session 73): *"on every mission, there's one city that has a 'witch den'. visiting it allows resurrecting dead party soldiers."* Proof `native-scene-14-shop.png` | **renders — developer-confirmed** (session 73): reached in normal play, the developer *"just did that — it works!"*. Reached by **walking into the Witch's Den in one city of a mission** (see the map/mission sections). It is the scene that exposed the missing **record 16** bank unit (see the arena section). Its record mask is **`0x00013C14`** — records 2, 4, 10, 11, 12, 13, **16**, 17, 18, 19, 20. For scripted testing, `OGRE_SCENE=0x14` forces it, but the poke races the boot and lands about 1 run in 5 |

**How it is reached, and the two routes are *not* the same code.** The suspend
save (`assets/save-mission-1.srm`; the game **deletes** it when the suspend slot
is resumed, so re-import with `tools/sramsave.py import`) resumes straight into
scene `0x03`, so it never plays the sequence in front of it. The natural route
is longer — measured session 68 with `OGRE_SCENE_LOG=1`:

```
0x04 title -> 0x12 Load Game -> 0x05 map --A on the mission location-->
0x02 -> 0x0D   (a scripted "story" dialogue; the step word D_8018F1C0 = 10)
0x02 -> 0x0D -> 0x16   (the closing movie the New Game opening also plays)
0x02 -> 0x0D -> 0x03   (the mission)
```

and it streams a **different bank of record 9's arena** than the suspend route
does (ROM `0x195430` → RAM `0x80214FA0` — now bank **unit R**; the suspend route
loads unit Q's bank at the same RAM). That bank is the mission's **enemy/unit
constructor**; without it the fort was built from stale bytes and the intro's
state machine waited forever. This is the general hazard: **a scene that works
from a save may still be missing code when played to**, and the run log's
`[bank] UNKNOWN module` / `streamed function stub called @ …` is the diagnosis.
See `docs/HANDOFF-2026-09-17-session68.md`.

**On the map, `A` only works with the cursor *on* a location** (the location
label, e.g. `Tenne Plains`, is shown), and a `START` press opens a **tooltip**
that blocks everything until it is dismissed.

### Every streamed arena the game can load (session 73)

**`tools/arenamap.py` prints this table from the ELFs.** Every streamed module in
OB64 is DMA'd by one generated loader block whose own instructions carry the
game's boundaries, so the map is mechanical and no address here was read by
hand:

```
lui a0,%hi(rom_start); addiu a0,a0,%lo(rom_start)
lui a1,%hi(ram_base);  addiu a1,a1,%lo(ram_base)
lui a2,%hi(rom_end);   addiu a2,a2,%lo(rom_end)
jal func_8009DA50
subu a2,a2,a0                     ; size = rom_end - rom_start (the DMA's size)
```

bracketed by `func_800900C0(ram_base, code_size)` (icache), `func_80090010(
code_end, data_size)` (dcache) and `func_80093380(data_end, bss_size)` (bss).
**A bank's size is its `subu`, never a chunk count** (session 59's rule, session
72's unit V), and the code/data split is the cache bracket's — not a guess.

| arena (RAM) | ROM start | size | code | data | BSS | unit | what |
|---|---|---|---|---|---|---|---|
| `0x800E9C20` | `0x0003F1B0` | `0x1CD0` | `0x1490` | `0x840` | shared | main (streamedA) | boot overlay (ROM-only) |
| `0x8016AF80` | `0x00040E80` | `0x25FB0` | `0x1B3B0` | `0xAC00` | shared | main (streamedB) | boot overlay: scene registry, dialogue text (ROM-only) |
| `0x8019A7C0` | `0x000712A0` | `0x84B0` | `0x7120` | `0x1390` | - | H | scene `0x07` name/birthday form |
| | `0x00079750` | `0xDAD0` | `0xC0F0` | `0x19E0` | - | M | **scene `0x05`, the map** |
| | `0x00087220` | `0x56D60` | `0x53920` | `0x3440` | - | S | **scene `0x06`, the Organize Screen** |
| `0x801D0860` | `0x00213AE0` | `0x16770` | `0x15260` | `0x1510` | - | J | record 10's arena, non-`0x16` scenes |
| | `0x0023A370` | `0xE80` | `0xE70` | `0x10` | - | AH | **the battle/neutral-encounter setup fragment** (scene `0x0E`'s enter streams it; `none` until session 82) |
| | `0x00244770` | `0x7500` | `0x6C70` | `0x890` | - | I | **scene `0x16`, the closing movie** |
| `0x801E6FD0` | `0x0022A250` | `0x10120` | `0xF920` | `0x800` | - | X | **combat / battle** |
| | `0x0023B1F0` | `0x9580` | `0x8D00` | `0x880` | - | C | record 10's main bank |
| `0x80214FA0` | `0x00165FE0` | `0xBEE0` | `0x9BD0` | `0x2310` | - | Q | mission bank (suspend route) |
| | `0x00171EC0` | `0x6030` | `0x4090` | `0x1FA0` | - | P | mission bank |
| | `0x00177EF0` | `0x7AF0` | `0x3E00` | `0x3CF0` | - | Y | not yet identified |
| | `0x0017F9E0` | `0x91A0` | `0x2930` | `0x6870` | - | W | **combat** |
| | `0x00188B80` | `0x65A0` | `0x64F0` | `0xB0` | - | Z | not yet identified |
| | `0x0018F120` | `0x6310` | `0x61F0` | `0x120` | - | AA | not yet identified |
| | `0x00195430` | `0x2380` | `0x22F0` | `0x90` | `0x20` | R | enemy/unit constructor (natural route) |
| | `0x001977B0` | `0x4F80` | `0x4810` | `0x770` | - | AB | **shop** |
| | `0x0019C730` | `0x64C0` | `0x6410` | `0xB0` | - | AC | not yet identified |
| | `0x001A2BF0` | `0x1FF0` | `0x1860` | `0x790` | `0x40` | AD | not yet identified |
| | `0x001A4BE0` | `0x4680` | `0x4320` | `0x360` | - | V | **settings** |
| | `0x001A9260` | `0x93E0` | `0x8FE0` | `0x400` | - | AE | not yet identified |
| | `0x001B2640` | `0x79E0` | `0x72B0` | `0x730` | `0x20` | AF | not yet identified |
| `0x802210E0` | `0x00275820` | `0x47D0` | - | - | - | C (bankRec13) | scene `0x14`'s arena, low half |
| | `0x00279FF0` | `0x7840` | - | - | - | **AG (bankRec16)** | **scene `0x14`'s own module — not in the segment table** (see below) |
| `0x8022ACB0` | `0x00286BA0` | `0x138F0` | `0x13180` | `0x770` | - | K | chapter animation (the "Prologue" card) |
| | `0x0029A490` | `0xE860` | `0xDE00` | `0xA60` | - | L | the same arena's other bank |
| `0x802395E0` | `0x002A8CF0` | `0x56A0` | `0x5620` | `0x80` | - | G | step ≥ 2's dialogue engine bank |
| | `0x002AE390` | `0xA7E0` | `0xA600` | `0x1E0` | - | F | the same arena's other bank |

Record 13 and record 16 are listed without a split because **neither has a loader
`subu`**: the game loads them from a descriptor table, so their sizes come from
the segment table's `ram_end` (and, for record 16, the port's own
`UNCOMPILED` log line, which printed exactly `0x7840`).

"shared" = the boot loader zeroes the boot overlays' BSS with its own
`func_80093380` calls, which span more than one record and start at the *end of
the last one's data*, so no single record owns that range. The game's own
per-record BSS (as `tools/gen_bank_funcs.py`'s `RAM_END` records it) is what the
port reproduces; the BSS column above is the size the loader states for the
record it belongs to.

**There is no uncompiled bank on a reachable path any more.** `arenamap.py`
finds **28** bank loads across the ELFs; 27 are compiled and the 28th is a
`0xE80` fragment with no loader of its own (below). The tool also proves the
negative: `--coverage` lists the ROM the scanned ELFs disassemble as code, and
its one remaining uncovered gap (`0x0DDF80..0x0E4910`, a display-list/texture
blob) contains **0** `jal 0x8009DA50` — so no *loader-pattern* bank can be
hiding. One did hide anyway: record 16's load is table-driven and its ROM
(`0x279FF0..0x281830`) was the tool's *other* uncovered gap until unit AG closed
it, and only booting the scene it belongs to revealed it (below).

#### Record 16 — the bank that hid behind a loader-less load (unit AG, session 73)

Booting into scene `0x14` produced the answer `arenamap.py` had missed:

```
[bank] UNCOMPILED streamed record 16: rom=0x279FF0 ram=0x802258B0 size=0x7840
[overlays] streamed function stub called @ 0x80226B7C (not yet loaded)   ... x3
```

**Record 16 is not a segment-table record.** Entry 13 declares
`rom 0x275820..0x279FF0 → ram 0x802210E0`, which is what the port compiled (unit
C); the game then loads a *second* module over the same arena from `0x279FF0`.
The size `0x7840` is not a loader `subu` either — it is the segment table's
`ram_end` for entry 13 (`0x8022D0F0`) minus `0x802258B0`, which is exactly the
size the port's own log printed.

It needed the session-45 treatment, and it matters far more than the three stubs
suggested: **125 distinct call targets across eight units reach into its window**
(banks B, C, F, G, K, L, N, T and the main ELF), because three *other* arenas
overlap it — record 18's tutorial module (`0x8022A860`, unit T), record 14d's
chapter animation (`0x8022ACB0`, unit K) and record 14a (unit L). Unit C's
record 13 has compiled bodies at the low half of the same RAM, so every call
from unit C's code into the high half was bound at build time to record 13's
layout — the session-45/67/72 mis-binding class. It is now **bank unit AG**
(`bankRec16b`, ROM `0x279FF0`, size `0x7840`, RAM `0x802258B0`).

With unit AG in, scene `0x14` renders with **0 stubs**, and the fix is directly
responsible: the three stubbed addresses (`0x80226B7C`, `0x8022859C`,
`0x80226CE0`) are record 16 entries, each preceded by `jr $ra`, and they are
declared in `symbol_addrs-bankAG.txt`.

#### The `0xE80` setup fragment — the one bank-shaped load with no unit

**Scene `0x0D`'s enter `func_80178568` and scene `0x0E`'s enter
`func_80177754`** (descriptor `0x8018FB2C`, mask `0x00003C00` — the **battle**)
both stream the same fragment **`rom 0x0023A370` (`0xE80`) → RAM `0x801D0860`**,
with the game's own boundaries (code `0xE70` / data `0x10`, no BSS). Scene
`0x0E`'s enter calls `0x801D0AAC` on one arm and `0x801D1508` on the other; the
neutral-encounter battle takes the `0x801D1508` arm.

Session 73 read the fragment as "a *data* copy of record 10b's code" and left it
uncompiled; **session 82 corrects that**: it is its own module (unit J's
`bankRec10a` ends at `0x23A250`, unit X's battle bank at `0x23A370`, unit C's
`bankRec10b` starts at `0x23B1F0`), and it is the code that **builds the battle's
participant table** — `func_801D1508` scans the 30 unit slots at `0x801F0CD0` and
tags a free one, `func_801D0AAC` reads the 16-byte table at `0x801D16D0`. It is
now **bank unit AH** (see the arena table above). Without it the calls hit the
runtime's streamed stub: normal battles still work (their units already exist on
the field), but a **neutral encounter**'s wild unit is created here, so the
battle started with an **empty enemy side and an instant victory** — the
developer's report, fixed in session 82
(`docs/proofs/native-neutral-encounter-message.png`, `-battle.png`,
`docs/HANDOFF-2026-09-18-session82.md`).

Scene `0x14`'s enter (`func_80178130`, descriptor `0x8018FBE8`, mask
`0x00013C14`) does **not** stream the fragment: it DMAs unit J into the same RAM
and calls `0x801D0860` (J's own code), which is why the Witch's Den renders even
before session 82.

#### The loader `arenamap.py` cannot see: a table-driven loop

Record 18's bank (unit **T**, the tutorial's practice stage) and record 16 are
both loaded without a literal loader block. `func_ovlB_80221D50` indexes a
`0x28`-byte descriptor table at RAM `0x80229DDC` by `D_80193700` and reads `ram`,
`rom_start`, `rom_end`, `bss` and `entry` out of the entry, so the addresses are
data, not immediates. The tool reports those as `jal 0x8009DA50` sites without a
cache bracket — which is the correct answer, and the reason to read that list
rather than trust a silent zero. The tutorial's other table at `0x80229E88`
selects mode 2 (unit U).

**When a screen freezes or draws nothing and the log shows no stub, check for an
`UNKNOWN module` and remember the silent case:** an unknown record is never
*evicted*, so the previous bank's body keeps running at those addresses. See
`docs/HANDOFF-2026-09-17-session72.md`.

### The mission's `R` menu, and the **Organize Screen** (scene `0x06`)

While the mission is playable, **holding `R`** opens a horizontal icon menu over
the map. It is a **hold** menu: it is drawn only while `R` is down and closes on
release (so a screenshot after a tap shows nothing). The entries read off
interactively in session 69, in cursor order:

| # | label | what it does |
|---|---|---|
| 1 | `Dispatch` (the entry the menu opens on) | |
| 5 | `Mission Objective` | |
| 7 | `End` | ends the practice / the unit's turn — the tutorial's practice stage returns to the Tutorial (`0x17`) |

The full ordered list is **not** transcribed yet (developer, please fill in);
`→` `A` on the **Organize Screen** entry enters scene `0x06`.

The **Organize Screen** is the player's army-management screen — the one the
`map → mission` route also shows before a battle (session 67 met scene `0x06`
there and called it "the briefing"; it is this screen).

| what | source | status |
|---|---|---|
| three counters across the top: `SOLDIER 030`, `CHARACTER 25`, `UNIT 03` | dev (session 69 capture) | **renders** |
| the **formation grid**: characters on a chequered floor of stepped tiles, a blue block of tiles and a lone gold tile among the cream ones | dev (session 69 capture) | **renders** |
| the selected entry's name plate, bottom-right (`Scarlet Magi` in the capture) | dev (session 69 capture) | **renders** |
| the `WAR FUNDS` / `0001000 Goth` plate, bottom-left | dev (session 69 capture) | **renders** |
| the screen's **controls** (what the cursor does; which button assigns / removes / relocates a character) | **not asked yet** | unknown |

The descriptor is **`0x8018FD84`** (mask `0x2`), or **`0x8018FD98`** (mask
`0x40002`) when `*(0x80193700)` is non-zero — both share enter `func_8017B6D0`,
update `func_8017B858`, hook `func_8017B9C8`, leave `0`. The enter DMAs **ROM
`0x87220` (`0x56D60`) → RAM `0x8019A7C0 … 0x801F1520`** (no BSS) and `jal`s the
module's entry `0x801C19B0`; with `*(0x801977E8) = 2` the generic update/hook call
`0x801C214C` and `0x801B7FC0`. Uncompiled, those three stubs made the scene return
to `0x03` **68 ms** after entering — the developer's *"the screen just reloads"*.
It is now **bank unit S**; the proof is
`docs/proofs/native-organize-screen.png`. See
`docs/HANDOFF-2026-09-17-session69.md`.


**The wall it was (session 67):** the three segment-table records it streams
(7/8/9) were uncompiled and its arena streams two more modules (units P and Q),
and the new records exposed a cross-bank mis-binding in unit A
(`rec3 -> rec6`). All four are in tree now; the evidence is
`docs/HANDOFF-2026-09-17-session67.md`.

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

## The ending and the credits

**The credits are scene `0x11`** (descriptor `0x8018FBAC`, mask `0x00008000` →
overlay C / `streamedC`, the section whose data holds the whole staff-roll text at
ROM `0x1F0117..0x1F08F4`: `Presented`, `Producer`, `Music Composer`, `Director`,
`Atlus U.S.A., Inc.`, `The END`, …). Reached at the end of a playthrough, after
the last mission and the closing movie.

| what | scene | source | status |
|---|---|---|---|
| **fade from black to `Ogre Battle 64` in the middle of the screen** | `0x11` | dev (session 86) + proof (`/tmp/cr5-3600.png`, a mid-fade frame) | **renders — developer-confirmed** (session 86) |
| **scrolling credits** with multiple backgrounds from the game | `0x11` | dev (session 86) | **renders — developer-confirmed**; some credit backgrounds still have artifacts (open) |
| the **total `Chaos Frame` points** the player finished the game with | `0x13` (descriptor `0x8018FBD4`, mask `0x8000`) | dev (session 86) | **reached and runs** (session 86): the credits exit into it at `t=328073 ms` on the natural route |
| back to the attract loop | `0x09`/`0x0A`/`0x04` | code | runs |

**The session-85 freeze is fixed (session 86).** The credits hung on their first
beat with the frame-pump thread's counter frozen (`D_800AEFA4` frozen while
`D_800C4BCC` counts). Four `config.toml` `function_sizes` overrides were leaking
t4's stack — each one a `jal` compiled as *call the containing body + early
`return`* (`func_801AB998`'s `jal 0x801AB770`, `func_801B1BBC`'s
`jal 0x801B00D4`) or a dropped `jr ra` delay slot (`func_801AB568`'s epilogue
continuation, `func_801AFC2C` itself). See
`docs/HANDOFF-2026-09-19-session86.md`. **Do not test this scene with
`OGRE_SCENE=0x11`** — a forced run plays the intro/opening (session 86 §1); use
the natural ending (a suspend save in front of the final boss works).

## Tutorial (title menu → Tutorial)

| what | scene | source | status |
|---|---|---|---|
| Deneb's dialogue box over the dark blue textured backdrop: `Welcome! / Is this your first time here?` | `0x17` | dev (session 43), proof (`native-tutorial-dialogue.png`) | renders |
| the lesson menu after Deneb's dialogue; any entry then runs the scripted sequence `0x17 → 0x02 → 0x0D →` a mission | `0x17` | dev (session 71) | renders |
| the practice stage: scene `0x03` with `D_80193700 == 1` (descriptor `0x8018F364`, records 2,3,7,8,9,**18**), the tutorial instruction box (`TUTORIAL / These are called the Field`, stronghold and `Use Item` lessons) over the field map | `0x03` | dev (sessions 69/71), proof (`native-tutorial-practice-field.png`, `-command.png`) | renders; bank unit T (session 71) |
| the lesson that selects `D_80193700 == 0` is the **normal mission** (descriptor `0x8018F350`, records 2,3,7,8,9) | `0x03` | dev (session 71) | renders (sessions 67/68) |

The same form-box look (dark blue backdrop, dialogue/form panel) is what the
name-entry and birthday screens use, so the form module that draws the Tutorial
is the one to look at when those steps are reached.

**The tutorial is not a self-contained scene.** `func_801862F0` (scene `0x17`'s
accessor) picks one of two descriptors on bit 3 of `D_80196B0C` — `0x8018FE50`
(enter `0x8019B2C0`, mask `0x00060000` = records 17 **and** 18) or `0x8018FE64`
(enter `0x8019B540`, mask `0x00020000` = record 17 only). Record 17/18 are bank
unit B (session 43). The **lessons** are scripted steps of the sequence engine:
the practice stage is scene `0x03` entered with the mode word `D_80193700 != 0`,
which `func_80173700` uses to select descriptor `0x8018F364`; the mode word is
set by `func_80173694(a0)` (callers in record 17: `0x8019B624` sets 1;
`0x8019ABDC` passes a lesson variable). Record 18's own loader
(`func_ovlB_80221D50`, RAM `0x80221D50`) streams the per-mode module into record
18's arena at RAM `0x8022A860`:

* mode 1 → ROM `0x1C32D0` (`0x5D50`), the **tutorial instruction module** — bank
  unit T (session 71); its data half holds the lesson text.
* mode 2 → ROM `0x1C9020` (`0x5020`) — bank unit U (session 71); no observed
  route loads it yet.


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
* The `0x18` menu: **answered, session 88** — it is the Controller Pak Menu
  reached by holding Start through the boot window (see "Boot and attract").
* Scene `0x14`, the **Witch's Den** (answered in part, session 73): the route is
  *mission → the one city with a witch den → walk in*, and its purpose is
  **reviving dead party soldiers**. Still open: does anything take the enter's
  **bit-3 arm** (`D_801976F8` bit 3; a `0xE80` copy of record 10b's code), or is
  that arm dead? And what should the **revive flow** look like — a list of dead
  soldiers, a price in `Goth` from `WAR FUNDS`, a confirmation? The scene's data
  half has only the Old Witch's greeting in the captures so far, so the rest is
  probably drawn from the tables record 16 builds at runtime.
* Does the opening skip the movie when Start is pressed during it? Confirmed in
  session 45: with taps gated to `OGRE_TAP_NOT_SCENE=new-game,0x0D` (no input
  inside `0x0D`) visit 1 runs its full ~28.4 s; with taps that keep firing inside
  `0x0D` it ends in ~1.4 s. So a Start press *does* skip the movie. Is that the
  retail behaviour ("skip cutscene") and is it only Start, or any button?
