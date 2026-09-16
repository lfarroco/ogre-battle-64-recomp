# Handoff — 2026-09-16, session 58: checkpoints (save/load) + scene `0x16`'s module compiles; the sequence now plays the closing movie and crashes where the developer saw it

## Goal and result

**Goal (developer):** play the scene that shows up after the graduation ceremony
with Archbishop Odiron — the New Game opening's closing **intro movie** — once
all the questions are answered and the initial items are granted. Plus a
developer idea for faster testing: *"add a way to dump memory/state, and load it.
this will allow us to save checkpoints and make things much easier to test."*

**Result.**

1. **Checkpoints exist and are verified** (`save` / `load` in the live console):
   a file with the whole RDRAM image **plus the runtime's overlay state**, so a
   `load` rewinds the machine to the instant of the `save` and the game re-runs
   from there. This removes the ~45-60 s replay of the New Game opening from
   every experiment. §1.
2. **Scene `0x16`'s streamed module is compiled** (new bank unit **I**, ROM
   `0x244770` → RAM `0x801D0860`) and **the scene plays**: the closing movie
   loads, renders, and the sequence advances past it (repeated `0x16` visits
   instead of a stub spin). §2.
3. **The developer's end-of-sequence crash reproduces** — first time in the
   port — and it is a real `SIGBUS`: `func_ovlC_8022C270 + 0x53A` on N64 thread 4,
   faulting guest `0x7FFF43E8`, with the sequence word `D_8018F1C0 = 0x0431`
   (step **1073**, far past the decoded 19-step table). §3. The crash dump is
   `/tmp/s58-crash.bin` (regenerable with `OGRE_DUMP_RDRAM`).

## 1. Checkpoints — `save` / `load` (the developer's idea)

### What it is

`save [path]` writes

```
[8-byte magic "OGRECKPT"][u32 version][u32 rdram_size][u32 host_size][u32 flags]
[u64 fnv-1a over host+rdram][overlay-state blob][whole RDRAM image]
```

and `load [path]` validates all of it and rewrites the machine: overlay state
first, then the 8 MiB image. A bare `save` writes
`/tmp/ogre-checkpoint-NNNN.ckpt` and remembers the path for a bare `load`. A bare
`dump` is untouched (still the plain image `tools/rdram.py` reads).

### Why RDRAM alone is not enough

Two independent obstacles, both hit in this session:

1. **The function map is host state.** Which recompiled body runs at each RAM
   address is the runtime's `func_map`; restoring RDRAM alone would rewind the
   game's data but leave the map on a *later* bank, so the restored scene would
   run the wrong module's bodies at those addresses — the session-45/55
   mis-binding class. New runtime API:
   `recomp::overlays::get_overlay_state_blob()` /
   `restore_overlay_state_blob()` (`librecomp/src/overlays.cpp`) serialise the
   loaded-section and function-bank records (func pointers are process-local, so
   the blob carries extents and the map is rebuilt from the records of the
   *current* process). Magic/version/size/checksum reject a foreign build.
2. **The game runs on its own host threads** (librecomp threads are 1:1 native),
   so reading or writing 8 MiB from the console's main thread raced the game
   thread. The first attempts produced **torn images**: the checkpoint's own
   checksum did not match its bytes, its first `0x300` bytes were zeros, while
   stderr showed the game still submitting RSP tasks *during* the write. New
   runtime API: `ultramodern::checkpoint_pause_begin()` /
   `checkpoint_pause_end()` (`ultramodern/src/function_trace.cpp`) park every
   thread executing recompiled code at a function-entry boundary — the
   `recomp_trace_entry` hook N64Recomp already emits at every function start —
   for 50 ms of grace (longer than any recompiled function), then hold them until
   `end`. The console reports how many threads parked (1, in the cooperative
   scheduler).

### Verification (what was run)

* **File self-consistency**: after the pause fix, the stored checksum equals the
  checksum recomputed over the file's own bytes (before it did not —
  `/tmp/ck-a.ckpt` saved pre-fix is still on disk as the counter-example).
* **In-run rewind**: `save` at the cathedral (step 2), `load` 20 s later — the
  `c` command reports scene/step back at the saved values, and the game
  **continues from there** (a checkpoint at the personality questions replayed
  forward into `0x16`).
* **Cross-process**: `load` of a checkpoint written by an *earlier process*
  rewinds that process too (`ck-y.ckpt`: `scene 0x0100 step=0 -> scene 0x0000
  step=32770`, i.e. back to the New Game step).
* **Null build**: loads, resumes, `0` streamed-stub calls.

### New knob

`OGRE_CONSOLE_AT_MS=<n>` delays every watched-file read until `n` ms of wall
clock have elapsed, so a scripted run can leave the command file in place at
launch instead of racing it from a background writer.

## 2. Scene `0x16`'s module (the closing movie)

### What the game does (instruction/trace level)

Scene `0x16` (descriptor `0x8018FC00`, record mask `0x400`) chunk-DMAs
**ROM `0x244770` (0x7500 bytes) → RAM `0x801D0860`** in 59 × 0x200 chunks,
verified with `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1`:

```
rom=0x244770 ram=0x801D0860 ... rom=0x24BB70 ram=0x801D7C60   (59 chunks, 0x200 each)
```

The module's RAM is record 10's arena (`0x801D0860`), which **unit C's
`bankRec10a`** also owned — the session-45/55 shape. The port had no code for
this ROM range (`config-bankC.yaml` carried it as a `bin` gap), so the three
calls resident code makes into it (`0x801D40D0`, `0x801D410C`, `0x801D62A0`,
already `LOOKUP_FUNC` from `cross_bank.py`) missed the function map and hit the
runtime's streamed stub (session 57 §2).

### The fix

* **Unit I** (`config-bankI.yaml`/`.toml`) holds `bankRec16`: code
  ROM `0x244770..0x24B3E0` → RAM `0x801D0860..0x801D74D0`, data
  `0x24B3E0..0x24BC70` → `0x801D74D0..0x801D7D60` (43 functions). The code/data
  boundary is splat's own — the last function is `func_ovlI_801D7484` and its
  first `dlabel` is `0x801D74D0`; the data half holds the module's string
  fragments and the jump table.
* **Unit J** (`config-bankJ.yaml`/`.toml`) holds `bankRec10a` (147 functions),
  moved out of unit C. It is the *other bank* of RAM `0x801D0860`, so it cannot
  share a unit with `bankRec16`; it also cannot stay in unit C, because unit C's
  own code calls into that RAM (the session-45 rule).
* `BANK_UNITS := … I J`, and three build-hygiene fixes that the move exposed:
  * `build/bank%.ld` now clears `build/bank$*/asm` and `assets` before
    `splat split` — splat does not delete a removed segment's output, and the
    ELF rule globs its inputs, so unit C linked a stale `213AE0.o`
    ("multiple definition", session 58).
  * `recomp` and `bank-recomp` clear `RecompiledFuncs/` and `Bank*Funcs/` before
    regenerating. N64Recomp never deletes a previous run's files, so a symbol
    that moved (`static_17_8021F470` became a data label) left its old
    definition behind and the build died in a file the new run no longer emits
    ("conflicting types").
  * `config-bankC.yaml` needed an explicit `bin` gap at ROM `0x213AE0`: splat
    extends a `data` subsegment to the *next* segment, so without it record 10's
    data ran to `bankRec11`'s start and re-emitted `bankRec10b`/`bankRec11` as
    data.
  * `tools/gen_bank_syms.py` now also sees spimdisasm's local-label references
    (`.LovlI_801D7D70`), not just `D_ovlI_…`; the scene-`0x16` module's tail
    pointers are emitted that way and the unit did not link without it.

### Result

The sequence plays `0x16` and **advances out of it**: one 120 s run shows the
`0x02 → 0x0D → 0x16` cycle at t=58.8 s, 66.0 s, 68.6 s, 71.4 s (and
`0x0D`/`0x02` between), with **no `not yet loaded` stub lines**. Before the fix
the same point was a 17 439-iteration stub spin. Bank table now reports
`20 streamed-overlay record(s), 1975 function(s) armed`.

### The movie itself — confirmed against the developer's shot list

The developer supplied the shot list (session 58) and retail screenshots
(`docs/proofs/intro-movie-reference/retail-shot-1..4.png`):

1. `ATLUS USA` / `presents`
2. `Developed & licensed by` / `Quest / Nintendo`
3. `Ogre Battle Saga` / `Episode VI`
4. `Person of Lordly Caliber` over the flower (the symbol of Lodis)
5. a montage of the player and friends travelling the kingdom (several scenes)

A capture run (`OGRE_CAPTURE_PRESENT=/tmp/s16-caps/f OGRE_CAPTURE_EVERY=10
OGRE_CAPTURE_AFTER=500`, the maintained 1500 ms tap route, `OGRE_SPEED=4`) was
scanned for the movie (the shots are the only near-black frames): **all five
shots draw, in order**, and match the retail screenshots:

| shot | port frame | proof |
|---|---|---|
| 1 ATLUS USA | f.4350 | `docs/proofs/native-newgame-movie-atlus.png` |
| 2 Quest / Nintendo | f.4400 | `docs/proofs/native-newgame-movie-quest.png` |
| 3 Ogre Battle Saga / Episode VI | f.4500 | `docs/proofs/native-newgame-movie-episode-vi.png` |
| 4 Person of Lordly Caliber + flower | f.4600 | `docs/proofs/native-newgame-movie-lodis.png` |
| 5 montage: campfire | f.5100 | `docs/proofs/native-newgame-movie-campfire.png` |
| 5 montage: party on a cliff at sunset | f.5200 | `docs/proofs/native-newgame-movie-sunset.png` |
| 5 montage: parchment world map (red route, dagger, `Alta Región` / `Southern Coast` / `Zetegin Sea`) | f.5400 | `docs/proofs/native-newgame-movie-map.png` |

Frames are the RT64 swap-chain readback (`OGRE_CAPTURE_PRESENT`), converted
PPM→PNG with `sips -s format png … && sips -Z 640 …` (no PIL on this host).
**Unverified observation, for a later session:** several of these frames carry a
1-pixel vertical line and, in the transition frames, grey horizontal bands
(e.g. the f.4900 castle shot); the campfire/sunset/map shots themselves are
clean. Not investigated — it may be the game's own letterbox/border rather than
a renderer defect, so measure before "fixing" it.


## 3. The crash (reproduced, deterministic)

The developer's "crash at the end of the sequence" now reproduces, twice, at the
same instruction:

```
[crash] host pc func_ovlC_8022C270 + 0x53A
[crash] signal 10 on N64 thread 4 fault=0x7FFF43E8 (guest)
```

* The instruction is `lw v0,0(v1)` at `0x8022C7A4`
  (`func_ovlC_8022C270+0x534`, inside `func_ovlC_8022C6E4`'s loop — the
  disassembler names the enclosing symbol, the report names the entry); `$v1` is
  `0x7FFF43E8`, i.e. a truncated/wild pointer, not an RDRAM address.
* The caller is `func_ovlC_80228EC0` (the sequence-descriptor interpreter, in
  `BankCFuncs/funcs_4.c`), which loads `$a0..$a3` from a descriptor at `$v1` and
  calls `func_ovlC_8022C270`.
* The crash dump (`OGRE_DUMP_RDRAM=/tmp/s58-crash.bin`) shows
  **`D_8018F1C0 = 0x0431`** (step **1073**, `next = 1`) — far past the decoded
  19-step table (`docs/HANDOFF-2026-09-15-session44.md` §1). The sequence has
  run off its script, which is the most likely source of the wild pointer.
* It fires right after a `0x02 → 0x0D` visit at `t≈78.1 s` (4x), not inside the
  `0x16` module (the module is no longer at those addresses when the fault
  happens; the enclosing symbol is `bankRec14a`'s).

## 3b. Corrected and extended (same session, after the developer's answers)

The developer supplied what comes after the movie — a **"Prologue" chapter
animation** (characters revealed over *"Casting their gaze on the ground,
trudging along..."*), then a **second movie** (the player received for duty with
other soldiers) — and confirmed the movie is **one scene in five phases**
(*"the game reuses the same scene and displays content in 5 different phases"*),
so the port's repeated `0x16` visits are correct.

That changes the reading of the crash:

* **Step 1073 is a legitimate step.** The step table at ROM `0x1F3CA54` (asset
  `0x19A8804`) is a raw `u32` array whose header word `0x1A74` gives **1693
  entries**; entry 1073 = asset `0x01A1625A`, a valid LZ block (declared size
  `0x12C` == decoded size). The step is **not** out of range — the "past the
  19-step table" phrasing above is wrong: session 44 decoded only the first 19
  steps as a sample, the table is much longer.
* The movie's five phases are steps **970..974** (each descriptor starts with
  opcode `80000006`; their last-word commands are 5,1,1,5,1).
* Step **1073's descriptor starts with opcode `0000001B`** (27), and the
  interpreter's compare tree sends every opcode **< 31** to one shared case
  (`0x80228E84`: `v1 = s3 + s1*4`, then nine operands, then `jal 0x8022C270`).
  So the chapter card is a *low-opcode* descriptor whose handler is
  `func_ovlC_8022C270` — the function that faults.
* `func_ovlC_8022C270` walks a table based at **`*(0x8023A994)`** and does
  `lw a0, 0x248(v1)` / `free` / `malloc(0xC)` on entries. `0x8023A994` is a
  global **written by `func_ovlC_8022D1CC`** (`0x8022D1E8`, in unit C's
  `bankRec14a`) as the result of `malloc(0x1CB8)`; its callers are the two
  scene-setup callbacks `func_ovlC_80225A3C` (`0x80225AC8`) and
  `func_ovlC_80226110` (`0x8022619C`) — the `sel 0` / `sel 2` setup paths session
  42 identified.
* **Measured (live console, checkpoint `/tmp/ck-movie.ckpt` at the movie):
  `0x8023A994` holds `0x52513AFF`, and `0x8023A960..0x8023A9A0` is a byte table
  of `xx xx xx FF` rows (a colour/palette table), i.e. the address is inside a
  *module's data*, not a live engine pointer.** The earlier `OGRE_PROBE57` run
  shows the same word taking values like `0x58933768`/`0x46000086`/`0x3C01801D`
  (code-like words) at other moments. So the handler dereferences a global that
  is only a pointer while the engine structure it belongs to has been
  initialised *and* its owning bank is resident.

**Leading hypothesis (not yet proven):** the chapter-card step takes a
scene-setup path that should (re-)run the engine init `func_ovlC_8022D1CC` — or
the bank that owns `0x8023A994` is swapped after the init — and in the port the
global is stale module data when the handler reads it, so `0x248(v1)` walks off
the mapping. The cheapest discriminating experiment is a probe on `0x8022D1E8`
(does the init run on the step-1073 visit?) plus reading `0x8023A994` at the
exact crash instant from a checkpoint.

**Repro that makes this cheap:** `/tmp/ck-movie.ckpt` is a checkpoint taken at
`t=75.5 s` (scene `0x02` heading to step 974, ~2.5 s before the crash). Loading
it re-runs the end of the sequence; a live-console driver then reads whatever it
needs (`load /tmp/ck-movie.ckpt`, then drop `r`/`d`/`c` commands into the watched
file). Note the checkpoint was written by the *current* build — a rebuild now
invalidates it by design (see §1's build id).

## 3b-bis. Tool bug fixed: halfword reads were byte-swapped (the `step`/`next` mix-up)

Chasing the crash exposed a defect in the port's own instruments. The runtime
stores guest bytes reversed **inside each word**, so the logical halfword at an
even `a` is the little-endian halfword at `(a & 0x1FFFFFFE) ^ 2` — the `^ 2`
matters. The live console's `console_half` (and `tools/rdram.py`'s `half`) read
it *without* the XOR, i.e. returned the **neighbouring** halfword. The game
stores the sequence step with an `sh` at `0x8018F1C0`, so every `c` line and
every `rh` read printed the `next` word under the name `step` and vice versa
(e.g. the console said `step=32770` = `0x8002`, which is really `D_8018F1C2`'s
value, while the true step `974` appeared under `next`). Both accessors are
fixed — the console now reports `scene=0x000D step=542 next=0x8002` at a point
where the `[scene]` log says `id=0x000D`, and `OGRE_PROBE57`'s (correct
`MEM_HU`) readings agree. **Any earlier quoted console `step`/`next`/`spin`
value in this or an older handoff is swapped.** The checkpoint save/load
messages used the same accessor and so printed the `next` word as the step.

## 3d. The crash mechanism, pinned down (`probe58`, reverted)

Two measurements closed this out.

**1. The engine init runs on *every* sequence visit except the one that crashes.**
`probe58` (a tagged `fprintf` at `func_ovlC_8022D1CC`'s store, guest `0x8022D1E8`,
in `BankCFuncs/funcs_10.c`; reverted — `grep -rl probe58` is empty) ran the
maintained title route. Nine hits, each `*(0x8023A994) = 0x8019F490` (a real
`malloc(0x1CB8)` result), and they line up one-per-visit:

```
t=56113 0x0D -> init      t=66492 0x0D -> init
t=60796 0x0D -> init      t=69467 0x0D -> init
t=63918 0x0D -> init      t=75235 0x0D -> (no init)  <- this visit faults
```

**2. The arena DMA really does cover the engine word, every visit.** From the
full PI-DMA trace, `bankRec14c`'s load is 44 contiguous chunks
ROM `0x2A8CF0..0x2AE2F0` → RAM `0x802395E0..0x8023EBE0` (span `0x5800`), and
`bankRec14b`'s is 84 chunks → RAM `0x802395E0..0x80243BE0` (span `0xA800`).
The engine pointer sits at `0x8023A994` = base `+0x13B4`, i.e. **inside both
spans**, so each visit's DMA overwrites it and the game's own `func_ovlC_8022D1CC`
call is what re-establishes it. Measured directly at step 974 (checkpoint
replay): the word holds `0x58933768`; at other moments `0x46000086` /
`0x3C01801D` — and those two are exactly the module files' own data words at
`+0x13B4` (`bankRec14c` ROM `0x2AA0A4` = `0x46000086`, `bankRec14b` ROM
`0x2AF744` = `0x3C01801D`). So the value is the module image, not a pointer.

**Conclusion.** The fault is not a missing module, not an out-of-range step, and
not a stale checkpoint: **the visit that plays the chapter card takes a scene
setup path that does not re-run the engine init, while its DMA has just wiped the
engine pointer.** Retail must run the init on that visit too (the DMA is the
game's own code), so the next thing to look at is the step **command → setup
selector** dispatch for step 1073. Its command is **6** (the descriptor's last
word is `0xFF000006`), whereas the movie's five phases are commands 5,1,1,5,1
and the earlier cathedral/form steps are -3/-10/-4; session 42 decoded that
`func_80227700(sel)` selects the setup path (`sel 0` = `func_ovlC_80225A3C`,
`sel 2` = `func_ovlC_80226110`, both of which call `func_ovlC_8022D1CC` at
`0x80225AC8` / `0x8022619C`). So: log/derive the selector for command 6 and
check whether the port takes the `sel 0`/`sel 2` path (a mis-dispatch there is
the session-42-class bug this project has hit repeatedly).

**Also learned (route determinism).** The step *after* 974 differs between runs —
sometimes 975 (command 2, no crash), sometimes 1073 (the chapter card, crash) —
because the opening's continuation depends on the answers given at the
personality questions. A checkpoint taken with `OGRE_CONSOLE_ON_SCENE`/`_STEP`
carries that state, so it reproduces *its own* branch; to reproduce the crashing
branch, checkpoint a run that is about to crash. That trigger is also the fix
for the wall-clock route flakiness: `OGRE_CONSOLE_ON_SCENE=0x0D
OGRE_CONSOLE_ON_STEP=974 OGRE_CONSOLE_ON_CMD='save /tmp/ck.ckpt'` fired exactly
when the game reached step 974, in a run whose taps started before the title was
even up (`[scene] console trigger: scene 0x000D step 974 -> save …`).

## 3c. Recon for the next milestone: the game's own save system is the Controller Pak

The developer (session 58): *"the game has a save system, which we should arrive
in one of the next scenes. after that, it will be possible to save the progress
and load the state using the game's own system."* Static recon so that session
does not start cold:

* **The save device is the Controller Pak (`osPfs*`), not cartridge
  SRAM/EEPROM/FlashRAM.** The main segment alone has **105 `jal`s into the
  PFS/Controller-Pak cluster** (`0x8009616C`..`0x80097DC0`; the largest targets
  are `func_80097BD0` ×23, `func_800970D0` ×18, `func_80097DC0`/`func_8009788C`
  ×14 each), and the ROM carries the whole menu as text at ROM
  `0x790EC..0x7967C`: `Controller Pak Menu`, `Save`/`Load`/`Erase`/`Exit`,
  `OgreBattle Game Notes`, `OGREBATTLE64 %d`, `Pages`, `No Data`,
  `Insert Controller Pak.`, `Saving data.`/`Loading data.`/`Deleting data.`,
  `Do not remove Controller Pak.`, `Data saved to Controller Pak.`,
  `Data loaded to Game Pak.`, `Saving data has failed.` (and its Load/Delete
  variants), `1 note 25 pages to save.`, `Insufficient pages to copy game.`,
  `Different Controller Pak has been inserted.`, `{C0}Not an Ogre Battle 64
  note.`, `Overwrite data?`, `Delete data?`, `Game Data 1`/`Game Data 2`.
* **Those strings are in bank unit H** (`guestmap.py 0x790EC` →
  `D_ovlH_801A260C`, the scene-`0x07` module, RAM `0x801A260C`), i.e. **the
  Controller Pak menu is drawn by the same UI module as the name-entry/birthday
  forms that already render**. So the *menu's* drawing is very likely already
  working; what is missing is the device underneath it.
* **The runtime cannot answer it today.** `librecomp/src/pak.cpp` is upstream's
  51-line stub: every `osPfs*` entry (`osPfsInitPak`, `osPfsAllocateFile`,
  `osPfsFindFile`, `osPfsReadWriteFile`, `osPfsDeleteFile`, `osPfsFileState`,
  `osPfsFreeBlocks`, `osPfsNumFiles`, `osPfsChecker`, `osPfsRepairId`) returns
  `1` = `PFS_ERR_NOPACK`. `recomp::SaveType` (`game.hpp`) has `None`, `Eep4k`,
  `Eep16k`, `Sram`, `Flashram`, `AllowAll` — **no Controller Pak**, and the
  app sets `entry.save_type = recomp::SaveType::None` with a TODO
  (`app/src/main.cpp:92`). So today the game sees "no pak inserted" and the
  menu path can only report `Insert Controller Pak.`
* **What the milestone needs**, in order:
  1. A **32 KiB pak image** with the real PFS layout (ID sector at page 1,
     directory, file headers, 256-byte pages) so existing emulator/hardware pak
     dumps are usable and the game's own saves are interchangeable.
  2. A real `osPfs*` implementation over that image (the ten entry points above)
     plus the raw SI access the PFS layer uses (`__osContRamRead`/
     `__osContRamWrite` and the pak-presence bits `osContInit`'s query returns).
  3. A **save file path** for it. The existing cartridge-save plumbing is
     `pi.cpp`'s `save_context` (`save_buffer`, `update_save_file()` →
     `<config_path>/<save_folder>/<name>.bin` with `.temp`+`.bak` via
     `files.cpp`), keyed on `get_save_size(SaveType)` — a Controller Pak is a
     *separate device*, so it wants its own image file rather than that buffer.
  4. Only then: the scene that hosts the menu (reachable after the chapter card
     / second movie) can be exercised, and the title's `Load Game` entry
     (`docs/scenes.md`, needs a save) becomes testable.



**Next lead:** why the step runs past 19. The step table and the script VM
(`func_80170974`, 16 opcodes per visit) are decoded; the question is whether the
retail loop ends the sequence at step 19 (title/credits) and the port's missing
piece makes it fall through, or whether the answers route to a different
continuation. A checkpoint at `t=70 s` plus `load` makes this cheap to iterate.

## 4. What was run for verification

```sh
# checkpoint: file self-consistency, in-run rewind, cross-process load, resume
printf 'c\nsave /tmp/ck-a.ckpt\nc\n' > /tmp/ogre-console.txt
OGRE_CONSOLE_AT_MS=21000 OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=45000 ./build-app/ogrebattle64 assets/ogre64.z64
printf 'c\nload /tmp/ck-a.ckpt\nc\n' > /tmp/ogre-console.txt   # -> rewind + resume

# the module: a 120 s opening run, scene 0x16 plays and the sequence advances
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=120000 ./build-app/ogrebattle64 assets/ogre64.z64

# the crash, with a dump for offline reads
OGRE_DUMP_RDRAM=/tmp/s58-crash.bin ... OGRE_EXIT_AFTER_MS=100000 …   # exit 138 (SIGBUS)

# bank invariant + builds
make bank && make bank-recomp        # check-banks: OK
cmake --build build-null -j 8 && cmake --build build-app -j 8
```

No repo test harness; every check is a ROM-dependent run. stdout must be
redirected on long runs (the periodic `[snap]` dump stalls boot otherwise).

## 5. Files changed

* `app/src/sdl_platform.cpp` — the `save`/`load` console commands, the
  checkpoint file format/verification (`write_checkpoint`/`read_checkpoint`),
  the `OGRE_CONSOLE_AT_MS` gate, the build-id guard, and the **halfword
  accessor fix** (`console_half` now applies the runtime's `^ 2`, §3b-bis).
* `tools/rdram.py` — the same halfword fix in the offline `half` accessor.
* `app/src/bank_overlays.cpp` + `app/src/sdl_platform.{hpp,cpp}` — the
  **`OGRE_CONSOLE_ON_SCENE` / `OGRE_CONSOLE_ON_STEP` / `OGRE_CONSOLE_ON_CMD`**
  trigger (run one console command when a chosen scene/step goes live) and the
  `ogre::console::exec` entry point it calls. This is what made the step-974
  checkpoint land deterministically; it fires once per process.
* `tools/N64ModernRuntime/librecomp/{include/librecomp/overlays.hpp,src/overlays.cpp}`
  — `get_overlay_state_blob()` / `restore_overlay_state_blob()`.
* `tools/N64ModernRuntime/ultramodern/{include/ultramodern/ultramodern.hpp,src/function_trace.cpp}`
  — `checkpoint_pause_begin/end`, the park in `recomp_trace_entry`.
* `n64modernruntime-ob64.patch` — regenerated (includes the two changes above).
* `config-bankI.yaml`/`.toml` (new) — `bankRec16`, scene `0x16`'s module.
* `config-bankJ.yaml`/`.toml` (new) — `bankRec10a`, the other bank of the same
  RAM.
* `config-bankC.yaml` — `bankRec10a` removed, explicit `bin` gap at `0x213AE0`.
* `Makefile` — `BANK_UNITS += I J`; `bank%.ld` clears the unit's asm/assets
  before splitting; `recomp`/`bank-recomp` clear their generated trees first.
* `tools/gen_bank_syms.py` — also match `.Lovl<U>_<addr>` local-label references.
* `docs/guides/app-build.md` — "Checkpoints" + `OGRE_CONSOLE_AT_MS` +
  `save`/`load` in the command table.
* `docs/proofs/native-newgame-movie-{atlus,quest,episode-vi,lodis,campfire,sunset,map}.png`
  (new) — the movie's five shots, captured from the port and matched to the
  developer's shot list; `docs/proofs/intro-movie-reference/retail-shot-1..4.png`
  are the developer's retail screenshots for shot 4 and the montage.
* `PLAN.md`, `docs/scenes.md`, `docs/README.md`, `docs/DECISIONS.md`,
  `AGENTS.md`, this file.

Generated/regenerated (gitignored): `RecompiledFuncs/`, `Bank*Funcs/` (now A..J),
`app/src/bank_funcs.inc`, `build/bank*.elf`, `BankEFuncs/funcs_0.c` (the njpeg
readback patch re-applied by `make bank-recomp`).

**Probes:** one, `probe58`, in the **generated** `BankCFuncs/funcs_10.c` (a
tagged `fprintf` at the engine init's store, `0x8022D1E8`). It was reverted with
`make recomp && make bank-recomp`, and
`grep -rl probe58 RecompiledFuncs/ Bank*Funcs/ app/` is empty; both builds were
rebuilt after the revert. The checkpoint code and the console trigger are
features, not probes (env/console-gated, inert unless used).

## 6. Next leads

1. **The chapter-card crash** (§3b) — the concrete next wall. Steps:
   * probe `0x8022D1E8` (the `malloc(0x1CB8)` store in `func_ovlC_8022D1CC`) to
     see whether the engine init runs on the step-1073 visit at all;
   * read `0x8023A994` (and `0x8023A970`/`0x8023A978`, the interpreter's index
     and descriptor base) at the crash instant from a checkpoint — the faulting
     handler walks `*(0x8023A994) + s1*4 + 0x248`;
   * decide whether the value is stale because the *owning bank* was swapped
     after the init (the session-45 class, but for a global instead of code) or
     because the init path was not taken.
   `/tmp/ck-movie.ckpt` (t=75.5 s, ~2.5 s before the crash) exists but is
   invalidated by any rebuild — recreate it in ~80 s with the maintained route.
2. **The chapter animation and second movie** (`docs/scenes.md` rows 7/8): the
   developer described the chapter card's text and offered screenshots of the
   second movie (the player received for duty). Ask for them when the port
   reaches that far, so the render can be validated the way the five movie
   shots were.
3. `docs/guides/app-build.md` -> "Checkpoints" for the new tool; a future
   session can extend the snapshot to the app's own knobs if needed.

