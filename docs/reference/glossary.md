---
title: Glossary of project terms
summary: The words this project uses for ROM, RAM, scenes, banks, assets, rendering and diagnostics.
status: verified
session: 217
order: 90
---

# Glossary of project terms

These are the words the handoffs and the code use. Each entry says what the term
means in this project, which is sometimes narrower than the general N64 meaning.

## ROM, code and RAM

**ROM** — the big-endian 40 MiB image at `assets/ogre64.z64`. The project does
not commit it.

**RDRAM** — the console's 8 MiB of main memory. A guest address in KSEG0 is
`0x80000000 + physical offset`.

**Recompilation** — N64Recomp translates the ROM's MIPS instructions into C. This
is a port, not a matching decompilation; the generated C is not evidence of the
game's original source.

**splat** — the splitter that writes `asm/`, `assets/*.bin` and
`ogrebattle64.ld` from `config/config.yaml`.

**record** — one entry in the game's segment table: a ROM range, a RAM range and
a size. Records overlap in RAM by design, so the resident record decides which
code runs at an address.

**streamed bank** — one module the game DMAs into a RAM window at runtime. A
bank's size is its loader's `subu`. `docs/reference/banks.md` has the table.

**arena** — the RAM window that a record owns. Two or more banks can be loaded
into one arena at different times, so a fixed-address call can land in whichever
bank is resident.

**unit** — one compiled translation unit of bank code
(`Bank{A..Z,AA..AH}Funcs/`, 34 units) and its entry in the runtime's bank map.
The unit is the port's side of the mapping; the bank is the game's side.

**overlay** — a streamed code segment. `streamedC` is overlay C, the main game
overlay; a streamed module that is not in the segment table still owns a
swappable RAM range.

**bank map** — the runtime table that maps a RAM range to the recompiled unit
that is resident there. A call through the map compiles as `LOOKUP_FUNC(addr)`;
a direct call binds at build time to one unit's body.

**HLE / LLE** — high-level emulation runs an RSP task in C; low-level emulation
runs the game's own microcode. `app/src/rsp.cpp` runs the recompiled njpeg and
audio microcodes (LLE) and stubs every other task.

## Scenes and script

**scene type** — one of 25 ids (`0x00..0x18`) in the dispatcher's accessor table
`D_800AF028`. A scene type is code, not content.

**accessor** — a 1-3 instruction stub that returns a scene descriptor. The
dispatcher stores 25 accessor pointers at boot.

**descriptor** — the five words an accessor returns: `+0x00` enter, `+0x04` and
`+0x08` per-frame hooks, `+0x0C` leave, `+0x10` bank-record mask.

**pending scene** — `D_800C4C26`, the word a leave hook or the script writes to
request the next scene (`0x8000 | id`, plus the `0xFFFC`/`0xFFFE` control words).

**step** — one entry in the 1693-entry scripted sequence at ROM `0x1F3CA54`. A
step is content: dialogue, a movie phase, a form or a chapter card. The New Game
opening visits scene `0x02` then `0x0D` once per step.

**step word** — `D_8018F1C0`, the current step index and mode. The next scene the
script requests is `D_8018F1C2`.

**forced scene** — a run with `OGRE_SCENE=<name|hex>`. It stores the game's own
request at a scene change, so it arrives at the first transition and enters
without the pre-state a natural path builds.

## Assets

**asset id** — the `u32` the game passes to the asset loader. It is not a ROM
address. `func_8009DAF4` reads the size at `(id & 0x0FFFFFFF) + 0x594250`.

**payload** — the bytes after an asset's `u32` size word. Most payloads are LZ
compressed and start with their decompressed size.

**LZ payload** — a stream encoded by the game's compressor and decoded by
`func_8007A110`; `tools/ogrelz.py` is the independent implementation. A valid
stream ends on its declared payload boundary.

**njpeg** — the game's JPEG-like image container, magic `HU` + `HUFF`. The CPU
Huffman decode feeds four type-4 RSP tasks and the result is drawn by an S2DEX2
list.

## Rendering

**display list** — the command stream the game builds for the RSP and the RDP.
`g_dl_cursor` is the write cursor the emitters advance.

**GBI** — the display-list command set. This game uses F3DEX2, F3DEX2.NoN and
S2DEX2, selected by hashing the task's ucode. `docs/reference/gbi.md`.

**ucode** — the RSP microcode text and data for a task. The hash is XXH3-64 over
the raw RDRAM bytes at the segment lengths in RT64's database.

**RSP / RDP** — the console's signal processor and rasterizer. The RSP runs
microcode; the RDP consumes the display list.

**TMEM** — the RDP's 4 KiB texture memory. A YUV tile uses two planes and is
de-interleaved on load.

**YUV16** — the 16-bit YUV texture format the njpeg backdrop uses. It needs
raw-TMEM sampling and the game's `G_SETCONVERT` matrix.

**framebuffer** — a colour image the RDP renders into. The VI scans it out; a
16-bit writeback stores each value at `off ^ 2` and `off ^ 2 + 1`.

**VI** — the video interface. Its `origin` is `0x280` for the standard NTSC
modes, so the scanout address is `framebuffer + 0x280`, not the RDP's colour
address.

**writeback / present** — writing rendered pixels back to RDRAM versus scanning a
framebuffer out to the screen. The browser renderer defers writebacks; RT64 does
them per workload.

**RT64** — the native renderer. The browser build has its own WebGL2 renderer,
described in `docs/guides/web-renderer.md`.

## Diagnostics and process

**probe** — temporary instrumentation added to generated code or the app. Tag it
(`// probe42`), revert it with `make recomp && make bank-recomp`, and prove the
revert. A diagnostic that stays in the toolkit is not a probe.

**A/B** — two runs that differ in exactly one variable. Anything else cannot be
interpreted.

**run log** — the app's stdout, read with `tools/runlog.py`: scene timeline, RSP
tasks by type, bank loads and problems.

**live console** — guest-RAM reads and writes against a running game. Off unless
a trigger names it (`OGRE_LIVE_CONSOLE=1`, `OGRE_CONSOLE_FILE`,
`OGRE_CONSOLE_AT_MS`, `OGRE_KEY_<n>`).

**checkpoint** — `save`/`load` in the live console. It writes RDRAM plus the
runtime's overlay state, and is valid only for the process and binary that wrote
it.

**null renderer** — the `build-null` build. It runs the game without RT64, for
scene, timing and memory checks that do not need pixels.

**handoff** — `docs/HANDOFF-YYYY-MM-DD-sessionN.md`, the per-session record. The
newest number wins. A handoff usually starts with a `> **Summary.**` line.

**durable decision** — the table at the top of `docs/DECISIONS.md`: the decisions
that still bind the port. The entries below it are session reasoning.

**reference page** — a page in `docs/reference/`, edited in place and cited by
the handoffs. `docs/reference/addresses.tsv` is its registry.

## Evidence

`AGENTS.md` (rules 6, 8, 9, 12, 13, 14), `docs/reference/memory.md`,
`docs/reference/banks.md`, `docs/reference/gbi.md`,
`docs/reference/scene-script.md`, `docs/guides/app-build.md` and
`docs/guides/emulator-first.md`.
