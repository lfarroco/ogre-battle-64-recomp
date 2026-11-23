# Session 217 handoff — the documentation gets a reference layer, a registry and generated indexes

> **Summary.** Session 217 handoff — **the documentation gets a reference layer, a 170-row address registry and generated indexes.** A wiki was rejected because it is a separate repository outside the checkout and the commit. Nine reference pages, `make docs-index`/`docs-check`/`docs-find`/`docs-stats`, all 215 handoffs indexed (162 were), and the save globals corrected from `0x800B8xxx` to `0x800A8xxx`.

**Goal.** The repo's knowledge about the game lived in 214 handoffs, a 244 KB
`docs/README.md` table and `AGENTS.md`'s "Verified facts". Make the durable part
navigable for a human and readable by an agent without reading the record
chronologically, and make the derived indexes checkable.

**Decision.** Documentation stays in the repository. A wiki was rejected: it is a
separate git repository, so an agent working in the checkout does not get it from
`git clone`, the repo's search and history do not cover it, and a wiki edit cannot
land in the commit that changes the code it describes. The tree now has three
layers: reference (edited in place), guides (procedures) and log (append-only).
The decision is in `docs/DECISIONS.md` (session 217).

**Result.**

*Reference layer, nine pages plus `README.md`:*

| page | covers |
|---|---|
| `docs/reference/memory.md` | RDRAM, framebuffers, heap pointers, the `lui` immediate pitfall |
| `docs/reference/banks.md` | the streamed-bank table, loader pattern, record 16, `0xE80`, overlap rule |
| `docs/reference/gbi.md` | ucode hashes, S2DEX2, YUV16, the njpeg task |
| `docs/reference/scene-script.md` | the VM, the 1693-entry step table, the command mapping |
| `docs/reference/scene-dispatch.md` | the 25 types, accessor table, descriptor lifecycle, 39 transition sites |
| `docs/reference/save.md` | the 32 KiB SRAM image, slots, checksums, suspend save, the `lui` address correction |
| `docs/reference/assets.md` | asset ids, the LZ format, the njpeg container |
| `docs/reference/runtime.md` | threads, frame pump, task queue, RSP types, checkpoints, console |
| `docs/reference/glossary.md` | the project's vocabulary |

*Registry.* `docs/reference/addresses.tsv` holds 170 rows: 78 symbols, 27 scene
descriptors, 15 memory, 13 scene dispatch, 8 scene script, 7 GBI, 10 save,
5 bank, 6 asset, 1 runtime. `docs/reference/README.md` gives the schema. `docs/symbols.md`
and `docs/scenes.md` render their address tables from it, so their hand-written
tables are gone.

*Generated indexes.* `docs/README.md` now begins with generated tables for the
reference pages, the guides, the subject notes and the handoff log. The handoff
table covers all 215 files; the old hand table covered 162. The 162 curated
summaries moved verbatim into each handoff as a `> **Summary.**` line, so the
summary lives with its file and the index is rebuilt from them. `docs/README.md`
went from 244 KB and 218 lines to 60 KB and 289 lines.

*Tooling.* `tools/docindex.py` has `build`, `check`, `find`, `stats` and
`unregistered`. `make docs-index` renders the tables and indexes; `make
docs-check` verifies every relative link, every registry row's page and section,
duplicate names of one kind, every handoff filename and summary, handoff
citations, and generated-block freshness. `make docs-find Q=0x8018F1C0` prints
the registry row and every doc line that mentions it. `make docs-stats` prints
coverage and the most-mentioned gaps.

*Rules.* `AGENTS.md` §10 now requires a durable fact to get a reference page and a
registry row in the same session. The "Verified facts" section was replaced with
11 behaviour-changing facts and pointers to the pages; the removed detail is on
the pages that own it.

**Corrections found while writing the pages.**

- **The save globals are `0x800A8xxx`, not `0x800B8xxx`.** The documented form is
  the `lui` immediate plus the low half. `lui 0x800b` + `lw -31816` at `0x80074C1C`
  resolves to `0x800A83B8`; `lbu -31812` at `0x80074BF4` to `0x800A83BC`;
  `addiu -32192` at `0x800754D4` to `0x800A8240`. The 13-entry field table runs
  `0x800A824C..0x800A83B8`, and entry 0's summary offset is at `0x800A8258`.
  `mips-linux-gnu-nm build/ogrebattle64.elf` has `D_800A83B8` and no
  `D_800B83B8`. `docs/notes-save-items.md` §0 had the correction; the registry,
  `docs/reference/save.md`, `docs/reference/memory.md` and
  `docs/guides/app-build.md` now use it.
- **The asset cache word is `0x800ABD70`.** `asset_decode` stores it with
  `sw v0,-17040` at `0x8009DDE0`. The `0x800BBD70` in `docs/symbols.md` is the
  same immediate error.
- **The scene descriptor table is complete for the 20 static ids**, plus the six
  alternate descriptors the game selects at runtime. `0x8018FB98` is scene
  `0x10`, not scene `0x07` (session 54's reading). The table is in
  `docs/scenes.md` → "Scene descriptors".

**Evidence.** Before the change, `docs/**/*.md` mentioned 2043 distinct
`0x8xxxxxxx` addresses; `docs/symbols.md` carried 114 and 1407 were absent from
`symbols.md`, `scenes.md` and `DECISIONS.md`. `python3 tools/docindex.py
unregistered` on the session-216 handoff found two unknown addresses
(`0x8018FB40`, `0x8018FB70`), both scene descriptors; they are now rows with the
session-216 `[scene]` line as evidence. The save address correction is the
disassembly above plus the ELF symbol table. The scene descriptor rows come from
`tools/scenemap.py scenes`.

**Verification.**

- `make docs-index` writes nine pages, `docs/README.md` and `docs/symbols.md` /
  `docs/scenes.md`, and reports `generated blocks are current` on a second run.
- `make docs-check` reports `docindex: ok - 9 reference page(s), 170 address
  row(s), links resolve`. It checks every markdown file under `docs/` plus the
  three root documents.
- A fake registry row was appended and `docs-check` failed with
  `generated block is stale`; the file was restored byte-identical
  (`sha256 d82683e5…`), and the check passed again.
- The handoff index has 215 rows for 215 files, and all 162 moved summaries were
  compared verbatim against the old `docs/README.md` table.
- Two false positives in the link checker were C++ lambdas in handoff code
  fences; the checker skips fenced code and whitespace-bearing targets.

**Files changed.** `AGENTS.md`, `Makefile`, `PLAN.md`, `docs/DECISIONS.md`,
`docs/README.md`, `docs/scenes.md`, `docs/symbols.md`, `docs/guides/app-build.md`,
161 existing `docs/HANDOFF-*.md` files (a `> **Summary.**` line each),
`docs/HANDOFF-2026-11-23-session217.md`, the ten files in `docs/reference/`, and
`tools/docindex.py`.

**Probes.** None. No game code, generated code or app source was touched. The
game was not run. `tools/RT64` already had local modifications before this
session; it was not entered or changed.

**Notes.**

- The registry covers 152 of the 2078 guest addresses the docs mention. The rest
  are leads, not facts. `make docs-stats` lists the most-mentioned gaps.
- The historical handoffs still contain the `0x800B8xxx` save form and other
  superseded readings. They are the record and were not rewritten; the reference
  pages carry the corrections.
- `AGENTS.md`'s three "easy to repeat" corrections are in the trimmed section.
  The full history stays in `docs/scenes.md` and the handoffs.
- Next pass: harvest the most-mentioned unregistered addresses by topic, and
  consider splitting `docs/guides/app-build.md` (2953 lines) into build, knobs
  and diagnostics.
