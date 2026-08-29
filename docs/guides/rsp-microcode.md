# RSP microcode research & recompilation

Ogre Battle 64 submits RSP tasks for graphics. Display lists are **not** run
through recompiled microcode in this port: the runtime routes gfx tasks
(`M_GFXTASK`) to the renderer's `send_dl`, and **RT64 parses the display lists
itself** with its built-in GBI interpreters (`tools/RT64/src/gbi/`). RSPRecomp
is only needed for the **audio** ucode (task type `M_AUDTASK`, 5), which the
runtime routes through `recomp::rsp::get_rsp_microcode`.

## The game's gfx ucode (identified, session 5)

The first gfx task's OSTask (dumped at runtime):

```
type=1 ucode=0x8009F540 ucode_size=0x1000 ucode_data=0x800AC140 ucode_data_size=0x800 data_ptr=0x800C6500 data_size=0x18
```

Mapping RDRAM → ROM (main segment: `ROM = vram − 0x80070C60 + 0x1060`):

| part | RDRAM | ROM | size |
|---|---|---|---|
| ucode text | 0x8009F540 | 0x2F940 | 0x1000 |
| ucode data | 0x800AC140 | 0x3C540 | 0x800 |

The data block contains `"RSP Gfx ucode F3DEX fifo 2.08 ... Yoshitaka Yasumoto
1999 Nintendo"` (and an `F3DEX.NoN` variant) — so the game's gfx ucode is
**F3DEX 2.08**, using the **short-format opcode set**:

- `G_SETOTHERMODE_H = 0xDE`, `G_SETOTHERMODE_L = 0xDF`, `G_RDPFULLSYNC = 0xE9`,
  `G_RDPPIPESYNC = 0xE7`, `G_ENDDL = 0xD0`/`0xD1`, `G_MTX = 0xB1`,
  `G_VTX = 0xB4`, `G_DL = 0xB6`, `G_TRI1/2 = 0xC7/0xC8`, ...

## GBI selection in RT64 (RESOLVED 2026-08-29 — auto-detection is correct)

**Update (session 10): this section's earlier conclusion was wrong.** Dumping the
game's actual display lists showed they are parsed correctly by RT64's
auto-detected GBI (`GBIUCode::F3DEX2`):

- First gfx task DL: `DE000000 / 000A9EF0 / E9000000 / 0 / DF000000 / 0` =
  `G_DL(0xA9EF0); G_RDPFULLSYNC; G_ENDDL` — `0xDE`=G_DL, `0xDF`=ENDDL under
  RT64's F3DEX2 map.
- Branch target `0xA9EF0` is standard RDP color/rect setup; later boot DLs
  branch to real KSEG0 targets (`0x801869E8`).

So `getGBIForUCode` already selects the right interpreter and **no override is
needed** — the earlier "force F3DEX" experiment (session 9 binary) misparsed the
DLs (plain F3DEX does not map 0xDE/0xDF) and was removed. The note below is kept
for history.

<details><summary>Original (superseded) analysis</summary>

RT64's `GBIManager::getGBIForUCode` identifies the GBI by XXH3-hashing the
ucode text/data against a database. For OB64's F3DEX 2.08 it was believed to
match `GBIUCode::F3DEX2` (value 7), whose opcode semantics were thought to
differ from the game's display lists. That turned out to be wrong — the game's
lists use the RT64 F3DEX2 opcode values (`0xDE`=G_DL, `0xDF`=G_ENDDL). Options
that were considered and are NOT needed:

1. ~~Add an RT64 GBI map for the F3DEX-2.08 short-format opcode set~~ — not needed.
2. ~~In `app/src/renderer.cpp::send_dl`, override the GBI~~ — tried, wrong; reverted.
3. ~~Check RT64's ucode hash DB for a "F3DEX 2.08" entry~~ — the existing F3DEX2
   match is correct.

</details>

## Audio ucode (still TBD)

No audio tasks (type 5) are submitted yet during boot, so the stub microcode in
`app/src/rsp.cpp` is still fine. When the game reaches audio:

1. Capture the audio OSTask via `log_task` in `app/src/rsp.cpp` (non-gfx tasks
   log their full OSTask) to get the ucode text/data RDRAM addresses + sizes.
2. Map them to ROM offsets and write an `RSPRecomp` config
   (`text_offset`, `text_size`, `text_address`, `rom_file_path`,
   `output_file_path`, `output_function_name`).
3. Build `RSPRecomp` (`cmake --build tools/N64Recomp/build --target RSPRecomp`),
   compile the generated C into the app, and implement
   `get_rsp_microcode` dispatch in `app/src/rsp.cpp`.

