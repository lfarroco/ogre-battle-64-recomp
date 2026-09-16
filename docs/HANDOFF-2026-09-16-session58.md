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
  `OGRE_CONSOLE_AT_MS`, and the `c` command now prints the **halfword** step
  (`D_8018F1C0`) instead of a 4-byte read of it.
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

**Probes: none added.** The checkpoint code is a feature, not a probe; it is
env/console-gated and off unless `save`/`load` is used.

## 6. Next leads

1. **The step-1073 crash** (§3): find where the sequence step leaves 1..19. The
   `docs/HANDOFF-2026-09-15-session44.md` step table plus a checkpoint at
   `t≈70 s` is the cheap instrument. `D_8018F1C0`'s writer is the script VM
   `func_80170974` (opcode 0x10 at `0x80170ADC`), so the value it writes at the
   end of the script is the thing to watch (`OGRE_PROBE57=1` logs it).
2. **Scene `0x16`'s content**: the scene now runs but nobody has looked at it.
   Capture it (`OGRE_CAPTURE_PRESENT=…`) and ask the developer what the closing
   movie should show — `docs/scenes.md` row 6 is still "unknown".
3. **Does the real sequence reach `0x16` more than once?** The port loops
   `0x02 → 0x0D → 0x16` ~4 times before the crash. If retail plays `0x16` once
   and then the title/credits, the loop is itself a symptom of the step running
   off the end.
4. `docs/guides/app-build.md` -> "Checkpoints" for the new tool; a future
   session can extend the snapshot to the app's own knobs if needed.
