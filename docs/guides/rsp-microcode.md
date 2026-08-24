# RSP microcode research & recompilation

Ogre Battle 64 submits RSP tasks for both graphics and audio. The RSP runs the
game's microcode, which must be statically recompiled with `RSPRecomp` (part of
the `N64Recomp` toolchain) and provided to the runtime through the
`recomp::rsp::callbacks_t::get_rsp_microcode` callback.

## Microcodes present in the ROM

Scan of `assets/ogre64.z64` for known ucode signature strings (big-endian dump):

| Name | ROM offsets (decimal → hex) | Notes |
|---|---|---|
| `F3DEX` | 0x3C686, 0x3CA56, 0x3D046 | fast 3D/extended 3D (2D sprite games often use it for rects) |
| `L3DEX` | 0x3D6A6 | line 3D |
| `S2DEX` | 0x3DA96, 0x3DE97 | sprite 2D (likely used for HUD / sprites) |
| audio | TBD | no `n_audio`/`gsp` signature found yet; likely custom |

All gfx ucode strings sit inside the main segment's data range
(ROM `0x2E570..0x3F1B0`), i.e. they are already part of the ELF and get loaded
into RDRAM with the main segment. The exact text/data boundaries and the RDRAM
addresses the game stores in `OSTask` fields must be confirmed before writing
`RSPRecomp` configs.

## Work items

1. Locate ucode text/data boundaries. Best source of truth: the `OSTask`
   (`t.ucode`, `t.ucode_data`) the game submits at runtime, or the constants in
   the main segment referenced by the task-loading code.
2. Identify the audio ucode (task type 0x5) and gfx ucodes (task type 0x1/0x2).
3. Write one `RSPRecomp` config per ucode
   (`text_offset`, `text_size`, `text_address`, `rom_file_path`,
   `output_file_path`, `output_function_name`).
4. Compile the generated RSP funcs into the app and implement
   `get_rsp_microcode(const OSTask*)`.
