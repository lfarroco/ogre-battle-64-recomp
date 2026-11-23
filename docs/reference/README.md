# Reference

Canonical facts about the game's code and data. A page here answers one question
and is edited in place; the handoffs are the append-only record that produced it.

The tree has three layers:

| layer | where | rule |
|---|---|---|
| reference | `docs/reference/`, `docs/scenes.md`, `docs/symbols.md` | one home per fact; edit in place; cite the evidence |
| guides | `docs/guides/` | procedures that are run, not facts that are cited |
| log | `docs/HANDOFF-*.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md` | append-only; read by search, not top to bottom |

## One home per fact

A fact that a future session will look up goes on a reference page. The handoff
keeps the session narrative: what was tried, what failed, and the evidence. It
does not keep the only copy of an address or a mechanism.

When a session establishes a durable fact, add it here in the same commit. When
a page and a handoff disagree, re-read the instruction evidence, then correct the
page and the registry. `AGENTS.md` keeps the facts that change how a session
works; this tree holds the detail.

## Where to start

| question | page |
|---|---|
| What is this address? | `addresses.tsv`; `make docs-find Q=0x…` |
| What should this screen show? | [`docs/scenes.md`](../scenes.md) |
| Why is this screen black or frozen? | [`docs/guides/emulator-first.md`](../guides/emulator-first.md), [`runtime.md`](runtime.md) |
| How is a streamed bank loaded? | [`banks.md`](banks.md) |
| How does the New Game opening flow? | [`scene-script.md`](scene-script.md), [`scene-dispatch.md`](scene-dispatch.md) |
| Where does a save slot live? | [`save.md`](save.md) |
| How is a display list identified? | [`gbi.md`](gbi.md) |
| What does this word mean? | [`glossary.md`](glossary.md) |
| How do I build or run the port? | [`docs/guides/app-build.md`](../guides/app-build.md) |

## The address registry

`addresses.tsv` is the one place an address is named. The pages carry a
generated table:

```
<!-- BEGIN GENERATED: addresses:<section> -->
<!-- END GENERATED: addresses:<section> -->
```

`python3 tools/docindex.py build` rewrites the block from every registry row with
that page and section. `make docs-index` runs it, and `make docs-check` fails
when a block is stale.

Columns:

| column | meaning |
|---|---|
| `address` | `0x` guest RAM, physical RDRAM or ROM/asset offset; 6 to 8 hex digits |
| `name` | proposed symbol; keep the address in a promoted name (`meaning_<addr>`) |
| `kind` | `function`, `code`, `constant`, `global`, `table`, `struct`, `buffer`, `framebuffer`, `field`, `window`, `ucode`, `asset`, `rom`, `descriptor` |
| `page` | `symbols`, or a path under `docs/` without `.md` (`reference/memory`) |
| `section` | the generated block id on that page |
| `note` | a short qualifier such as `(half)` or `heap pointer` |
| `evidence` | the instruction, run log, tool or handoff that establishes it |
| `session` | the session that established it, `N`, `N-M`, or `-` |
| `confidence` | `high` (instruction evidence), `medium` (partial), `low` (hypothesis) |

An address can appear once per kind. The same address as a function and as an
arena base is two rows. The same address with two names for one kind fails the
check.

Do not register a heap value. Register the pointer that holds it, and read the
pointer each run.

## Adding a page

1. Add `docs/reference/<subject>.md` with front matter:

   ```
   ---
   title: <one line>
   summary: <one line, used by the index>
   status: verified | partial | open
   session: <newest session that verified it>
   order: <10, 20, 30 ...>
   ---
   ```

2. Add one generated block per section, and registry rows with
   `page: reference/<subject>`.
3. Run `make docs-index` and `make docs-check`.

`python3 tools/docindex.py unregistered <handoff>` lists guest addresses in a
handoff that the registry does not carry. It is a lead list, not a gate.

## Finding

- `make docs-find Q=0x8018F1C0` prints the registry row and every doc line that
  mentions the address. `Q=g_newgame_step` searches the registry names.
- `make docs-stats` prints how many guest addresses the docs mention, how many
  the registry covers, and the most-mentioned gaps. The gaps are leads.

## Index

<!-- BEGIN GENERATED: reference-index -->
| Page | Status | Last session | What it covers |
|---|---|---|---|
| [Working rules for agents](../../AGENTS.md) | binding | - | How a session works: ask the developer for intent, verify a handoff at instruction level, isolate one variable. |
| [Plan and open work](../../PLAN.md) | living | - | Port scope, current status, the open-work list and the browser section. |
| [Guest memory map](memory.md) | verified | 207 | RDRAM layout, framebuffers, heap state pointers and the words that select a screen. |
| [Graphics microcodes and GBI](gbi.md) | verified | 192 | How a display list's ucode is identified by hash, and the GBI commands each ucode uses. |
| [Scene script VM and the step table](scene-script.md) | verified | 59 | How the New Game sequence runs 1693 scripted steps through scenes 0x02 and 0x0D. |
| [Streamed banks and their units](banks.md) | verified | 82 | Every streamed module's ROM range, RAM base, code/data split and the bank unit that compiles it. |
| [Scene dispatch and the 25 scene types](scene-dispatch.md) | verified | 213 | The accessor table, the descriptor lifecycle, the pending-scene word and the scene-versus-step distinction. |
| [Battery save and the Controller Pak](save.md) | verified | 212 | The 32 KiB SRAM image, its slot geometry and checksums, the access functions, and the suspend-save record. |
| [Assets, LZ payloads and the njpeg container](assets.md) | verified | 64 | How an asset id becomes a decompressed buffer, and the formats of the LZ payloads and the njpeg images. |
| [Runtime, threads and diagnostics](runtime.md) | verified | 202 | The frame pump, the task queue, the RSP task types and the checkpoints and console a session uses to read live state. |
| [Glossary of project terms](glossary.md) | verified | 217 | The words this project uses for ROM, RAM, scenes, banks, assets, rendering and diagnostics. |
| [What each screen must show](../scenes.md) | living | - | Per-screen intent from the developer, the 25 scene types, the descriptor table and the New Game opening. |
| [Proposed symbol names](../symbols.md) | living | - | Address to proposed name with evidence and confidence, the naming convention, and what a rename costs. |
| [Decision log](../DECISIONS.md) | binding | - | The durable-decisions table and the session reasoning below it. |
| [Session status log](../STATUS-LOG.md) | historical | - | The per-session status that `PLAN.md` carried from session 3 to 97. |
<!-- END GENERATED: reference-index -->
