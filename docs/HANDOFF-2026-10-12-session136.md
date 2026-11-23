# Handoff — 2026-10-12, session 136: the black part fix is generalised to the framebuffer's workload

> **Summary.** Session 136 handoff — **the black-part fix is generalised: a framebuffer tile copy is served only when the source framebuffer was written in the workload being processed.** Developer report: session 134's fix removed the black head in the scene it was found in, but a second cutscene reached from `pre_ogre.bin` still showed it, and the correct sprite flashed for a single frame while the black one lasted seconds. In that cutscene the shipped write-rect test never fires (`[fbcopy-outside]` = 0) while 23 `[fbcopy]` lines serve 16-32 x 1-3 row unit-sprite tiles from a framebuffer at `0x32AF40` (`bytes=153600`, 320x240 RGBA16); the fault frame's only `SETCIMG` is `0x400`, so that record is left over from an earlier scene and its stale `lastWriteRect` `0,0..319,239` covers the tiles, whose addresses hold the game's CPU-built sprite scratch. The head is the same signature as session 134's: 162 of 900 pixels in its box are exactly `0x0001`, the frame's only `G_SETENVCOLOR`, i.e. `TEXEL1` sampled as zero and the combiner painted ENV. `OGRE_FB_GPU_COPY=0` renders the head correctly, which is the A/B. `Framebuffer::lastWriteWorkload` is stamped in `State::submitFramebufferPair` from `FramebufferManager::getUsedTimestamp()` and required to equal the current `usedTimestamp` in `checkRegionsTMEM` next to the write-rect test, printing `[fbcopy-nowrite]`; the check is per load because a region stamped in an earlier workload survives in `activeRegionsTMEM`. Verified: 14 `[fbcopy-nowrite]` (`fbWorkload=9342` against `workload=9583..9660`), 0 `[fbcopy]`, developer-confirmed; the scene-14 regression run still refuses its head tile, refuses no legitimate copy, and matches the session-134 verified dump with its `y=120` scenery row unchanged. `make patch-check` PASS.

Developer request: *"we recently fixed an issue where some body part sprites would
turn black … but I found another cutscene where this happens. is the fix specific
for that scene? could we generalize the fix?"* then one save (`pre_ogre.bin`) and
the report *"the head flashes between the expected sprite and the black version …
the black version is displayed for some seconds"*, and finally *"the issue is gone
for that cutscene"*.

**Result.** Session 134's write-rect refusal is one instance of a larger
condition, and the general condition is now the guard. A tile copy is served only
when the source framebuffer was written **in the workload being processed**; a
framebuffer whose only write is from an earlier scene has no authority over the
RDRAM the game has since put at those addresses, and the load reads RDRAM the way
hardware does. In the second cutscene the shipped guard never fired at all
(`[fbcopy-outside]` = 0) while the copy path served 23 unit-sprite tiles from one
leftover framebuffer, and the frame's black head is the same flat-ENV signature
session 134 measured. With the guard in place, the same route reports 14
`[fbcopy-nowrite]` refusals, serves no copy, and the developer confirms the head
renders. The scene-14 regression check holds: its head tile is still refused, no
framebuffer in that run was refused for the new reason, and the fault frame's
scenery row and unit crop match the session-134 verified dump.

## 1. What the second cutscene showed before the change

Run `debug/head-trace.sh build-dist/ogrebattle64 <pre_ogre.bin> black-sprite-136`.
The developer drove from the save to a town cutscene where a knight's head is
black, and pressed `1` twice. Two things came out of the log:

| line | count | what |
|---|---|---|
| `[fbcopy]` | 23 | a tile served by a framebuffer copy, **every one** from `fb=0x32AF40` (`fbSiz=2 fbFmt=0`, `bytes=153600` = 320x240 RGBA16), shapes 28x1, 24x3, 32x3, 16x2 … at `fbTile=172,0..200,1`, `156,7..180,10`, `20,10..52,13`, `244,13..268,16`, `140,16..164,19`, `100,19..116,21`, `268,20..292,23`, `52,10..68,12`, `196,11..220,14`, `260,13..280,16` |
| `[fbcopy-outside]` | **0** | the session-134 write-rect refusal never fires in this cutscene |
| `[fbcopy-stale]` | 1 | the RDRAM freshness refusal fires on some frames only |

The single stale refusal is what the developer sees as a correct frame that lasts
one frame: when it fires the load reads RDRAM and the sprite is right, and on the
other frames the copy is served and the part is flat black. The deduplication on
both diagnostics is by key (`[fbcopy]` by tile and copy id, `[fbcopy-stale]` by
framebuffer address), so those counts are distinct keys and not frame counts.

## 2. The fault frame is in the dump, and the signature is session 134's

The two key-press images are the fault frame. The game's own framebuffers
(`D_800A8204 = {0x80000400, 0x80025C00, 0x8004B400}`) hold the black-headed
knight, and in the head's box (`140..170 x 95..135`) the most common value in all
three is **`0x0001`**, 162 of 900 pixels. `0x0001` is RGB555 black with the alpha
bit set, and the frame's only `G_SETENVCOLOR` in the display list at `0x802DE840`
is `0x000001`. So the head is drawn in the ENV colour, which is the same
mechanism as session 134's `0x2947` — `TEXEL1` sampled as zero and the combiner
painted ENV over the mask's silhouette. The ENV colour differs because it is a
different scene; the signature does not.

## 3. The framebuffer is not part of that frame at all

A walk of the dump's display list (`debug/black-sprite-133/loads.py`, 264 loads)
gives the decisive datum. The list's only `SETCIMG` is

```
SETCIMG at 0x802DE890 fmt=0 siz=2 width=320 addr=0x00000400
```

and it never targets `0x32AF40`. The framebuffer RT64 served the sprite tiles from
is a record left over from an earlier scene; its `lastWriteRect` is the stale
`0,0..319,239` the fixed run prints in §5, which is why the write-rect test passes
on every one of those tiles.

The addresses confirm what it is sitting on. Seven loads read from inside
`0x32AF40..0x350740`, and the unit's sprite blocks are the region
`0x329098..0x32C770` — the head's colour and mask payloads are at `0x32A318`,
`0x32A5C0` and `0x32B0B0` — so the framebuffer's declared range and the game's CPU
sprite scratch overlap. The blocks carry the game's own headers: `36 34 00 02 w h`
at `0x32C1F0`, `0x32CC40`, `0x32D850`, `0x32DF80`, `0x32E350`, and `36 34 04 00 00
1a 00 2e` at `0x32B0A8` (the second header `func_8019ABC4` writes, 26x46). The
region itself is 78.9% `0xFFFE` and 11.3% `0xFFFC` — a leftover image, not the
sprite art.

## 4. The A/B, and the reason the old guard cannot cover this

`OGRE_FB_GPU_COPY=0` (run `black-sprite-137`, same save and route) removes the
copy path: `[renderer] framebuffer.copyWithGPU = 0`, zero `[fbcopy]` lines, and
the developer reports the head correct. So the fault is the framebuffer tile copy,
in the same cutscene as session 134's.

The old guard asks one question — were these texels inside the rect this
framebuffer last rendered — and it is the wrong question for a framebuffer that
has not been rendered since an earlier scene. Its rect is stale but non-null and
covering, so the answer is yes while the GPU copy holds nothing to do with the
game's sprite bytes. The freshness guard asks the right question, but its baseline
is not trustworthy: `FramebufferManager::hashTracking` ([rt64_framebuffer_manager.cpp:960](../tools/RT64/src/hle/rt64_framebuffer_manager.cpp))
sets `RAMHash = hash(RDRAM)` at the end of every `State::fullSync` for every
framebuffer with `maxHeight > 0`, whether or not that frame's GPU content was
written back to RDRAM. A framebuffer the game has overwritten in the heap is
re-hashed to the game's new bytes, so the next load reads "in sync".

## 5. The fix

A tile copy holds the framebuffer's **GPU** content, and that is the truth only
for the workload that rendered it. The guard is now:

```cpp
const bool writtenThisWorkload = (copyFb != nullptr) && (copyFb->lastWriteWorkload == usedTimestamp);
if (!insideWrite || !writtenThisWorkload) { validCopy = false; }
```

`FramebufferManager::checkRegionsTMEM` ([rt64_framebuffer_manager.cpp:744](../tools/RT64/src/hle/rt64_framebuffer_manager.cpp)), in the
same place as the write-rect test, with a new `[fbcopy-nowrite]` tag for the
second condition. `usedTimestamp` is the manager's per-workload counter, advanced
by `clearUsedTileCopies()` at the end of `State::fullSync`; the new
`Framebuffer::lastWriteWorkload` is stamped in `State::submitFramebufferPair`
([rt64_state.cpp:591](../tools/RT64/src/hle/rt64_state.cpp)) and initialised to `UINT64_MAX`
("never written").

The per-load site is the one that matters, not the copy-creation site: a
`RegionTMEM` stamped in an earlier workload stays in `activeRegionsTMEM`, and
`checkRegionsTMEM` re-reads the framebuffer's state for every load. That is how
session 134's bug worked, and it is why the same condition is evaluated here.

The rule is strictly narrower than the A/B the developer ran (that test refuses
every copy), and it keeps the copy for a framebuffer rendered in the workload
being processed, which is the only case where RDRAM is not yet up to date and a
copy is required. The draw path submits a pair before it resolves the draw's
tiles — `checkFramebufferPair()` at [rt64_rdp.cpp:1319](../tools/RT64/src/hle/rt64_rdp.cpp) and
[1501](../tools/RT64/src/hle/rt64_rdp.cpp) runs before `state->loadDrawState()` at 1338 and 1515 —
so a pair's own framebuffers are stamped before any of its tiles are decided. The
write-rect test stays; the freshness guard stays as the tagged fallback.

## 6. What was run for verification

| run | evidence |
|---|---|
| `black-sprite-136`, pre-change, `pre_ogre.bin` | 23 `[fbcopy]` all from `fb=0x32AF40`, 0 `[fbcopy-outside]`, 1 `[fbcopy-stale]`; two whole-RDRAM dumps; the head box is 162/900 pixels of `0x0001` = the frame's `G_SETENVCOLOR`; the DL's only `SETCIMG` is `0x400`; `[spr]` and `[gpu-tile]` had already hit their caps (40000 / 5000) at call 201, so the late frames have no per-tile line — `[fbcopy]`/`[dec]` are deduplicated and still print |
| `black-sprite-138`, fixed build, `pre_ogre.bin` | discarded: the run stayed on the title and the attract loop (`[scene]` `0x04`/`0x0B`/`0x0C`, 1470 s of game time) and produced no copy line |
| `black-sprite-139`, fixed build, `pre_ogre.bin` | **14 `[fbcopy-nowrite]`, all `fb=0x32AF40`, `writeRect=0,0..319,239`, `fbWorkload=9342` against `workload=9583..9660`; `[fbcopy]` = 0**; `[fbcopy-outside]` 1 and `[fbcopy-stale]` 1 still present; **developer-confirmed: "the issue is gone for that cutscene"** |
| `black-sprite-137`, `OGRE_FB_GPU_COPY=0`, `pre_ogre.bin` | the A/B: `framebuffer.copyWithGPU = 0`, 0 `[fbcopy]`, head correct |
| `black-sprite-141`, fixed build, `scene_14.bin` | the regression check: `[fbcopy-outside] fb=0x2E6840 fmt=5 tile=468,65..484,67 writeRect=null fbWorkload=773 workload=2037` (session 134's head tile, still refused by the rect test), **`[fbcopy-nowrite]` = 0** (the new condition refuses no legitimate copy here), `[fbcopy]` = 0; the dumped frame differs from the session-134 verified dump (`~/Desktop/ogre-battle-64-recomp/head-trace/ram-1-full.bin`) by 949/153600 bytes at `0x400`, 1024 at `0x25C00`, 241 at `0x4B400`, the unit crop's colour histogram is identical (`0x0001` n=290, `0x72CD` n=143, `0xDEEB` n=136), and `y=120`, `x=110..119` is `5a49 624b 51c7 5a09 5a09 628b 6a8b 628b 72cd 7b0f` in the new dump, the session-134 dump **and** the pre-fix fault dump, i.e. the one-row scenery strip is unchanged |
| `make patch-check` | PASS; `patches/rt64-ob64.patch` regenerated with `tools/patchcheck.py --fix` (2487 lines, 26 files) |
| `grep -rl probe RecompiledFuncs/ Bank*Funcs/ app/` | empty: no generated code and no probe was touched |

## 7. What is left open

* The writer of the framebuffer record at `0x32AF40` is still not identified — no
  `SETCIMG` in the fault frame targets it, so it comes from an earlier scene.
  `OGRE_RDP_TRACE=2`'s colour-image histogram and a print of each
  `FramebufferManager::get` would name it. This is session 134's open item for
  `0x2E6840` unchanged.
* `[spr]` (40000) and `[gpu-tile]` (5000) are fixed caps and are consumed early by
  a long run. A run that needs per-tile lines at a late scene needs the caps
  raised or the trace armed at a key, which is not in the tree.
* The freshness guard's baseline is still the end-of-frame re-hash, so
  `[fbcopy-stale]` remains a fallback and not a completeness argument. Removing
  the re-hash for framebuffers that were not written back is the other general
  fix; it was not needed here and was not attempted.
* Not re-checked by hand: scenes that use a framebuffer as a texture on purpose
  (render-to-texture within one workload). The rule keeps those copies, which is
  what the draw-path order in §5 establishes, and no run has yet shown one
  refused.

## 8. Files changed

| file | what |
|---|---|
| `tools/RT64/src/hle/rt64_framebuffer.h` | `Framebuffer::lastWriteWorkload` |
| `tools/RT64/src/hle/rt64_framebuffer.cpp` | initialise it to `UINT64_MAX` |
| `tools/RT64/src/hle/rt64_state.cpp` | stamp it for the colour and depth framebuffer in `submitFramebufferPair` |
| `tools/RT64/src/hle/rt64_framebuffer_manager.cpp` | the workload condition in `checkRegionsTMEM`, and the `[fbcopy-nowrite]`/`[fbcopy-outside]` tags from one print |
| `patches/rt64-ob64.patch` | regenerated, `make patch-check` PASS |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/guides/app-build.md`, `docs/notes-black-sprite.md` | the record |
| `debug/black-sprite-136/fault-frame-dump-crop.png` | the knight with the black head, rendered out of the key-press image (`debug/head-crop.py`-style crop `120,85..200,165` of `black-sprite-136/ram-1-full.bin`) — the frame the dump captured is the fault frame |
| `debug/black-sprite-136/regression-scene14-unit.png` | the same crop of `black-sprite-141/ram-1-full.bin`, the scene-14 regression run, for comparison with `debug/black-sprite-134/fixed-head-unit.png` |
| `docs/HANDOFF-2026-10-12-session136.md` | this file |

No probe was used, so nothing was reverted. `git status --short` shows the RT64
change (`patches/rt64-ob64.patch` and the `tools/RT64` submodule), the six docs
and this handoff, and nothing else.
