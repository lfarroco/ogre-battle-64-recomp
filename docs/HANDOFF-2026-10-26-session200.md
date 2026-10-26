# Session 200 handoff — one command to reach a screen and capture it

## Goal

Stop re-deriving the browser scene capture every session: reach a named screen,
wait for it, and write a frame plus its log and pixel samples, in one command.

## Result

`debug/probes/scene.cjs` is that command. It owns the four steps every capture
used to re-implement: boot the wasm build (through `lib/harness.cjs`, with the
Metal Chromium flags), drive the route, wait for the `[scene]` line, then write
one PNG per requested `ogre_gfx_debug_flags` value, the sampled pixels, the page
log, the scene timeline and a meta file.

Built-in routes (data in `ROUTES` at the top of the probe):

| `--route` | Screen | Route |
|---|---|---|
| `title` | scene `0x0004` | `OGRE_SCENE=title` |
| `mission-briefing` | scene `0x0003`, Castle Burgund | `OGRE_SAVE=assets/escort_suspend.bin`, forced title, URL taps `title:start:6,0x12:a:6` |
| `tutorial-field` | scene `0x0003`, Jangurri practice field | natural boot to the title (27.3 s), `Enter`, `ArrowDown`, `Enter` (tutorial `0x0017` after 0.5 s), then one `KeyX` (A) per 1.4 s until the scene line says `0x0003` (10 presses in the test) |
| `tutorial` | scene `0x0017` | `OGRE_SCENE=tutorial` |

Each route's `steps` are `waitScene`, `sleep`, `press` and `pressUntilScene`.
`--scene`/`--query` cover a one-off screen without editing the table.

```sh
node debug/probes/scene.cjs --route tutorial-field --raw --points '12,160' --out /tmp/field
node debug/probes/scene.cjs --route mission-briefing --repeat 4 --repeat-gap 2000 --out /tmp/mission
```

Outputs, one run: `<prefix>-normal.png`, `-texel0.png`, `-texel1.png` (for
`--raw`), `<prefix>-samples.json` when `--points` is given,
`<prefix>-log.txt`, `<prefix>-meta.json` and `<prefix>-errors.txt`. The run also
prints the scene timings and the `tasks=` rate over 3 s.

The capture also produced a cleaner repro of the session-199 grass finding: at
the "Movement & Liberation" lesson, N64 `(12,160)` is `(33,33,0)` in the normal
frame and **pure white** in `TEXEL0` raw, i.e. the pixel is an untextured fill.
The earlier party-menu lesson showed the whole-field overlay's grey mottle there
instead; the tutorial's lesson advances with A without a scene change, so both
captures are of scene `0x0003`. The flat fill is common to both.

## Evidence

- `debug/probes/scene.cjs` run on all four routes. Title: scene `0x0004` after
  1.5 s, two shots and one sample, 29.93 tasks/s. Tutorial field: title after
  27.3 s, `0x0017` after 0.5 s, `0x0003` after 10 presses, three shots,
  `(12,160)` = `(33,33,0)` normal, `(255,255,255)` TEXEL0, 19.71 tasks/s.
  Mission briefing: scene `0x0003` after 40.7 s, one shot, one sample, 24.38
  tasks/s. Forced tutorial: see the run's `-log.txt`.
- The field frame `docs/proofs/web-tutorial-grass-flat-session199.png` and its
  `TEXEL0` raw pair were re-captured with the tool.

## Verification

- `node --check debug/probes/scene.cjs`.
- All four routes reached their scene and wrote a PNG, `-log.txt`, `-meta.json`
  and `-errors.txt` (one `(none)` errors file).
- `git status --short` shows only intended changes: the probe, the docs and the
  proofs.

## Files changed

| file | change |
|---|---|
| `debug/probes/scene.cjs` | new: named routes, scene wait, shots per debug flag, pixel samples, log/meta/error files |
| `debug/package.json` | `npm run scene` |
| `debug/README.md` | quick-start line and a paragraph pointing at the tool |
| `docs/guides/web-probes.md` | "Capturing a screen" section, the command list and the probe table row |
| `docs/guides/app-build.md` | a pointer from "the browser build reads the same environment knobs" to the probe |
| `AGENTS.md` | the probe added to the toolkit list (rule 4) |
| `docs/HANDOFF-2026-10-26-session199.md` | the field capture note and the two proof frames |
| `docs/HANDOFF-2026-10-26-session200.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md` | session record |

## Probes used and reverted

No game code was instrumented. The tool passes `ogre_gfx_debug_flags` values the
renderer already implements; no renderer source changed.

## Open

- `tutorial-field` stops at the scene line, which is the first tutorial lesson.
  A later lesson needs extra `press` steps in the route; add one there rather
  than a new script.
- The tool does not dump guest RDRAM. Pair it with
  `debug/probes/rdram.cjs --query 'OGRE_RAM_KEY_...'` when the question is about
  the bytes behind a pixel.
