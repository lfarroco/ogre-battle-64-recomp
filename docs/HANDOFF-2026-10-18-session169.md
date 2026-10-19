# Session 169 handoff — the browser mission trap is a 16-bit white fill in the game's heap block, and the mission renders when the fill lands eight bytes higher

## Goal

Fix the browser (Emscripten/WebGL2) trap that stops mission scene `0x03` from
rendering. Session 168 left it as "the mission traps after five of the six bank
loads". Depth, lighting and culling are out of scope.

Timebox: 100 minutes.

## Result

1. **The trap site is `func_80071950`, a guest message-queue node unlink, and
   the call chain is the scene-`0x03` enter.** The page's own `pageerror` stack
   (session 168 already captured it in
   `/tmp/w168/web-escort/m-f-a1-errors.txt`; this round's clean run reproduces it
   byte-for-byte) is
   ```
   RuntimeError: memory access out of bounds
     at func_80071950 (wasm-function[539]:0x9d2b9)
     at func_800712C4 (wasm-function[147])
     at func_801792DC (wasm-function[734])
     at func_80173724 (wasm-function[6268])
     at func_80075BC0 (wasm-function[3770])
     at func_80071EB0 (wasm-function[6731])
     at run_thread_function / _thread_func
   ```
   `func_80173724` is `0x8018F350 + 0x00`, the **enter** of the mission
   descriptor (`0x8018F350`: enter `0x80173724`, update `0x801737CC`, hook
   `0x801737D4`, leave `0x801737F0`, mask `0x0000038C`), and `func_80075BC0` is
   the scene dispatcher. `func_80071EB0` is the dispatch thread's entry. The
   faulting instruction in the wasm is an `i32.store` inside the
   `recomp_mem_addr` address computation, i.e. a guest `MEM_W` store, not a host
   stack access.

2. **The flush reads one global and the browser's value there is white pixel
   data, not a queue.** `func_801792DC` is four instructions: load the global
   `0x8018FC88`, call `func_800712C4` with it, store the result back. That global
   is the queue-pool pointer; `func_8017909C` (called through `func_80179080`
   from every scene enter that uses the queue) allocates `0x100 * 0x80` bytes
   with the game allocator `func_80071288` and stores the block at `0x8018FC88`.
   With `OGRE_P169`-tagged probes in `func_801792DC` (dump of 16 words around the
   pointer) and in `func_8017909C` (the allocation), the browser and the native
   build hold the **same pointer at the same four flush points** of one run:
   ```
   [probe169] flush enter G=0x80243E10      (title)
   [probe169] queue init  n=0x100 buf=0x80243E10
   [probe169] flush enter G=0x80243E10
   [probe169] queue init  n=0x100 buf=0x801BA770
   [probe169] flush enter G=0x801BA770
   [probe169] queue init  n=0x100 buf=0x80243E10
   [probe169] flush enter G=0x80243E10      (the mission enter: traps here in the browser)
   ```
   The pointer is identical in both builds and on every visit, so the pointer is
   not the difference. The words **at** the pointer are:

   | word | native, mission enter | browser, mission enter |
   |---|---|---|
   | `0x80243DF0` (G-0x20) | `0x80243DD0` | `0xFFFDFFFD` |
   | `0x80243DF4` (G-0x1C) | `0x8024DA20` | `0xFFFDFFFD` |
   | `0x80243DF8` (G-0x18) | `0x800AEDB8` | `0xFFFDFFFD` |
   | `0x80243E00` | `0x00000000` | `0xFFFDFFFD` |
   | `0x80243E04` | `0x00009C10` | `0x00009C10` |
   | `0x80243E08` (G-8) | `0x00000000` | `0xFFFDFFFD` |
   | `0x80243E10` (G) | `0x00000000` | `0x00000000` |
   | `0x80243E14` (G+4) | `0x80243E90` | `0x80243E90` |

   `0xFFFDFFFD` is two 16-bit `0xFFFD` halfwords: RGBA5551 `(31,31,31,1)`,
   opaque white. So a run of **16-bit pixel data** was written into the game's
   heap block. The words the queue init itself writes (`G+0`, `G+4`, and each
   entry's `+0x78`) still hold the init's values, so the fill predates that init.

3. **How the fill becomes the trap.** `func_800712C4` (queue send) reads
   `v0 = *(a0 - 8)`; `v0 >= 33` is the "a message is pending" test. Native has
   `0` there, so the send returns immediately. The browser reads `0xFFFDFFFD`, so
   it unlinks the node at `a0 - 0x20`, whose `+0x08`/`+0xC`/`+0x10` are the white
   pattern, and `func_80071950` stores through one of them
   (`sw v1,0(v0)` / `sw v0,8(v1)`). A guest address of `0xFFFDFFFD` makes
   `recomp_mem_addr` return `0x7FFDFFFD`, i.e. `rdram + 2 GiB`, past the wasm
   module's linear memory: `memory access out of bounds`.

4. **The bank link and the missing sixth record are not the cause.** Three
   checks, all negative:
   - `build-wasm/CMakeFiles/ogrebattle64.dir/linkLibs.rsp` lists
     `libBankPFuncs.a`, and `grep -ao "func_ovlP_[0-9A-Fa-f]*"
     build-wasm/ogrebattle64.wasm | sort -u | wc -l` is **22**, the record's whole
     function table (`kP_bankRec9bFunctions` has 22 entries, entry 20 is
     `func_ovlP_80214FA0`). `kP_bankRec9bFunctions` itself is a file-local static
     array, so it has no symbol to find; its entries do.
   - The sixth record is not on the enter path. In the native census log the
     mission enter is at `[scene] t=10336ms id=0x0003` and the
     `rom=0x171EC0 ram=0x80214FA0` load appears 62 lines later, after
     `[SDL] automation scene tap 1 ... scene=0x0003 buttons=a`. The browser traps
     before that press.
   - Everything on the faulting chain is main-ELF or streamedB code
     (`func_80071950`, `func_800712C4`, `func_801792DC` in streamedB,
     `func_80173724` in streamedB, `func_80075BC0` in the main ELF). No bank
     function runs on it.

5. **The trap moves with timing and is not deterministic in its exact
   instruction, which is the signature of a clobber and not of a bad pointer.**
   Three browser runs of the same route:
   - session 168 (`m-f`): trap at wasm offset `0x9d2b9`;
   - this round's clean build (`m-clean`): same chain, offset `0x9d2b9`, and the
     scene-`0x0003` canvas is black (`nonBlack=0`, `tasks=18`);
   - this round with the word-dump probe enabled (`m-q`): **no trap**, and the
     canvas is the mission (`nonBlack=303148`, `colorful=287168`). The dump
     prints 16 words per flush and slowed the guest thread enough that the fill
     started at `0x80243E08` instead of `0x80243DF0`, which leaves `G-0x20`
     holding a valid pointer.

   So the fill's extent at the instant of the flush decides the trap. That also
   means a probe that changes timing can hide it; the clean build traps.

6. **The browser renders the mission when the flush survives.**
   `docs/proofs/web-mission-first-frame.png` is the browser canvas at
   `scene 0x0003` (left) beside the native reference `/tmp/w168/nat2/t.800.bin`
   (right), both 2x. The browser frame has the `NOTE / Winning condition: Taking
   over enemy headquarters.` box, the `Castle Burgund` fort illustration and the
   two `MISSION` banners over the terrain, i.e. the same screen as the native
   capture. `tools/web_frame_cmp.py` over the 319x239 drawing buffer and the
   320x240 native target gives `meanAbs R=23.54 G=14.96 B=7.86`,
   `meanSigned R=-6.64 G=12.56 B=-4.26`, `exact=0.3094`. Session 168's baseline
   pair (frozen Load Game frame vs the same native frame) was
   `81.14/73.78/54.39` and `0.0406`, so the browser frame is now the same screen
   rather than a different one. Names of the remaining differences: the browser's
   terrain is unshaded (no `G_LIGHTING`, 916 448 lit draws in the census) and
   brighter than native; with no depth test (516 463 `ZBUFFER` draws) and no
   backface culling (926 539 `CULL_BACK` draws), far geometry can overpaint near
   geometry and backfaces are drawn. Those three are not implemented yet.

7. **What wrote the white fill was not named.** The native
   `tools/watch.sh 0x80243E10 --after func_80179080 --ignore 1 --hits 4` reports
   only `func_8017909C` (the pool init) writing that block; native never holds
   the white pattern. Every port path that writes 16-bit guest pixels was checked
   and none targets this address: `web_renderer.cpp`'s framebuffer writeback
   (`readback_framebuffer`) writes only the framebuffers it created, and the run
   created only `0x000400`, `0x025C00`, `0x04B400` (`[GFX-FB]` lines);
   `hd_backgrounds.cpp`'s `debug_fill`/pack path writes red/green/blue/yellow and
   no pack was loaded (`[hdbg]` present in the native log, absent in the
   browser's); `fog_put32` writes display-list words only. The leading remaining
   candidate is the game's own framebuffer-to-heap copy reading a framebuffer the
   web renderer wrote back as white where RT64's RDRAM still holds zeros - the
   browser log has `[GFX-FBREAD] fb 0x04B400 ... meanRGB=255` (a fully white
   320x240 image) around the Load Game scene. That is the next experiment, and it
   is named in Open item 1.

## Evidence

- Trap stack: `/tmp/w168/web-escort/m-f-a1-errors.txt` (session 168) and
  `/tmp/w169/m-clean-a1-errors.txt` (this round, clean build), both
  `func_80071950` ... `func_80071EB0`.
- The faulting instruction: `llvm-objdump -d
  /usr/local/Cellar/emscripten/6.0.8/libexec/llvm/bin/llvm-objdump
  --start-address=0x9d240 --stop-address=0x9d2e0
  build-wasm/ogrebattle64.wasm` resolves `0x9d2b9` inside `<func_80071950>` to
  `i32.store 0` on the `recomp_mem_addr` path.
- Descriptor: `0x8018F350` in streamedB (ROM `0x40E80` +
  `(0x8018F350 - 0x8016AF80)`) = `80173724 801737CC 801737D4 801737F0 0000038C`.
- Probe dumps: `/tmp/w169/m-p-a1-page.txt` (trapping run),
  `/tmp/w169/m-q-a1-page.txt` (surviving, mission frame), native
  `/tmp/w169/nat-dump.log`.
- Link: `build-wasm/CMakeFiles/ogrebattle64.dir/linkLibs.rsp`,
  `app/src/bank_funcs.inc:6447` (`kP_bankRec9bFunctions`), `:7195` (the record).
- Native ordering: `/tmp/census167/escort-dec-s4.log` lines 985118-985192.
- Proof: `docs/proofs/web-mission-first-frame.png`, composed by
  `/tmp/w169/compose169.py` (calls `tools/web_frame_cmp.py`'s `canvas_pixels`,
  `compare`, `write_png`).

## What changed

The tree is unchanged apart from the docs and the proof image. No source file was
modified: the round produced a diagnosis and a proof, not a fix.

Probes were added and removed. Files that carried `probe169`:
`RecompiledFuncs/funcs_0.c` (`func_800712C4`: `a0` and the node words when called
from the flush), `RecompiledFuncs/funcs_9.c` (`func_8017909C`: the allocation and
its caller), `RecompiledFuncs/funcs_14.c` (`func_801792DC`: the queue pointer and
16 words around it). They were reverted with `make recomp`, and
`grep -rn "probe169\|OGRE_P169" RecompiledFuncs/ Bank*Funcs/ app/ debug/` is
empty. The browser wasm was rebuilt after the revert and contains no
`probe169` string.

## Files changed

| file | change |
|---|---|
| `docs/proofs/web-mission-first-frame.png` | browser mission frame (left) and native `t.800` (right), 2x each |
| `docs/HANDOFF-2026-10-18-session169.md` | this file |
| `PLAN.md` | item 8(s): the trap site, the fill, the link refutation, the mission frame |
| `docs/STATUS-LOG.md` | session 169 entry |
| `docs/README.md` | session 169 row |
| `docs/DECISIONS.md` | decision 169 |

No commit was made.

## What was run for verification

- `make recomp` after the probes (revert) and
  `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean
  (`[100%] Built target ogrebattle64`), wasm has no `probe169`.
- `cmake --build build-dist --target ogrebattle64` — clean.
- `node debug/probes/boot.cjs --attempts 3 --secs 40` — `RENDERED` on attempt 1,
  `nonBlack=24972`, `colorful=16468`.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1`.
- The browser route (`/tmp/w168/drive.cjs`, URL
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`)
  three times: clean (`/tmp/w169/m-clean*`, traps, black canvas), word-dump
  (`/tmp/w169/m-q*`, renders), and the earlier probe run (`/tmp/w169/m-p*`,
  traps).
- The native route with the same probes
  (`OGRE_SAVE=assets/escort_suspend.bin OGRE_SAVE_RESET=1
  OGRE_PREF_DIR=/tmp/w169/natpref OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title
  OGRE_TAP_MS=1000 OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6"`) twice, once
  to 22 s and once to 30 s.
- `tools/watch.sh 0x80243E10 --after func_80179080 --ignore 1 --hits 4`
  (`/tmp/w169/watch1.log`).

There is no repo test harness; each check is a ROM-dependent run.

## Open

1. **Name the writer of the white 16-bit fill at `0x80243E10`.** The cheapest
   discriminating experiment is an A/B on the presentation path: log the guest
   word at `0x0004B400` and at `0x80243E10` in both builds at the mission enter
   (a four-line probe in `func_801792DC`, which already has the exact instant),
   and run the browser with the framebuffer writeback suppressed for one run. If
   the browser's `0x0004B400` is `0xFFFD`-white while native's is `0` at that
   instant, the fill is the game copying a framebuffer whose RDRAM content the
   web renderer has already written back and RT64 has not, and the fix belongs in
   the writeback's timing or content. If the browser's `0x0004B400` is not white,
   the writer is something else and the next step is a `MEM_H` store trace on the
   range.
2. **No fix was landed, so the clean build still traps** on this route at
   `func_80071950`. A run must not carry timing-changing probes to be compared
   with native, and a clean run does not reach a mission frame today.
3. **Depth, lighting and culling remain unimplemented**, so the proof pair
   differs in exactly those three ways: the browser terrain is unshaded and
   brighter, near geometry can be overpainted, and backfaces are drawn.
4. **The save import and scene-tap knobs from session 168 are unchanged** and
   were exercised by every run above.
