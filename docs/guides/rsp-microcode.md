# RSP microcode research & recompilation

Ogre Battle 64 submits RSP tasks for graphics. Display lists are **not** run
through recompiled microcode in this port: the runtime routes gfx tasks
(`M_GFXTASK`) to the renderer's `send_dl`, and **RT64 parses the display lists
itself** with its built-in GBI interpreters (`tools/RT64/src/gbi/`). RSPRecomp
compiles the game's **njpeg** ucode (task type `M_NJPEGTASK`, 4) and its
**audio** ucode (task type `M_AUDTASK`, 2); the runtime routes both through
`recomp::rsp::get_rsp_microcode`. Any other task gets the stub.

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

## Image-decode ucode: `M_NJPEGTASK` (type 4) — recompiled and correct (session 47)

The New Game opening's full-screen backgrounds are decoded by a **type-4
(`M_NJPEGTASK`) microcode** (the SDK's `njpgdspMain`), not by the gfx ucode. Four
tasks are submitted at the second `0x02` enter (the cathedral, scene `0x0D`
step 2), one per image:

```
type=4 ucode=0x8009ED80 ucode_size=0x7C0 ucode_data=0x800AC050 ucode_data_size=0xF0
       boot=0x8009ECB0/0xD0 data_ptr=<mbs*0x300 buffer> data_size=<mbs>
       yield_data_size=<quantization scale, 0xFFFFFFFE = -2>
```

| part | RDRAM | ROM (`vram - 0x80070C60 + 0x1060`) | size |
|---|---|---|---|
| boot loader | `0x8009ECB0` | `0x2F0B0` | `0xD0` |
| main microcode | `0x8009ED80` | `0x2F180` | `0x7C0` |
| Huffman/quant tables | `0x800AC050` | `0x3C450` | `0xF0` |

The game drives the microcode directly (no `NJPEGDParam` indirection): the
`data_size` field carries the macroblock count and `yield_data_size` the
quantization scale. The CPU Huffman decode that must run first is the main
segment's `func_8008B250(payload, buffer)` (called from `func_ovlE_80199A08`),
not `func_ovlE_80197E5C` (session 46's guess). The manual is the N64 Programming
Manual's JPEG chapter
(<http://ultra64.ca/files/documentation/online-manuals/man-v5-1/ucode/jpeg/>):
`"HUFF"` + `s16 numMB` entropy-coded payload in, 16-bit YUV macroblocks out
(768 bytes each: 4x128 Y, 128 U, 128 V, 256 unused).

The config is `config/rsp-njpeg.toml`; `make rsp-recomp` regenerates
`RspFuncs/njpeg_ucode.cpp` (gitignored), which `app/CMakeLists.txt` builds into
`ogrebattle64_rsp` (the generated code is C++ — it uses librecomp's `RSP` VU
implementation). `app/src/rsp.cpp` dispatches on the ucode address and runs the
decoder by default (`OGRE_NJPEG=0` forces the stub). **Two constraints, both
load-bearing:**

* **`text_address` is a label base, not an address.** RSPRecomp labels
  instruction *i* with `(text_address + 4i) & 0x1FFF` and resolves branch targets
  the same way, while the bytes come from `text_offset`. The value must therefore
  be the RSP **IMEM DMA address** the microcode was assembled for — here
  `0x1080`, because the game's boot loader loads the text at IMEM `0x080`
  (`addi $7,$0,0x1080` / `mtc0 $7,SP_MEM_ADDR` / `jr $7` at ROM `0x2F0C0`) and
  the ucode's own `j` targets are `0x1000`-based (`j 0x84001190`). Using the
  text's RDRAM address (`0x8009ED80`, low 13 bits `0x0D80`) rotates every `j`
  target by `0x300`: the decoder then walks the wrong blocks and writes a single
  `0x300`-byte block per task (session 46's symptom). With `0x1080` it writes all
  `mbs` blocks and returns `Broke` (session 47). Apply the same rule to any other
  ucode recompiled here (a ucode loaded at IMEM `0x080` wants `0x1080`).
* `text_size = 0x7B8`, not the declared `0x7C0`: the last 8 bytes of the region
  are data (`0x0900060E` decodes as `j 0x1838`, and RSPRecomp emits a `goto` to
  a label that does not exist).

The background **renders** (sessions 50-52). The game converts the YUV
macroblocks to RGBA by drawing them (per-macroblock 16x16 **YUV16** textures, a
*second* gfx ucode `0x800A5110`), then copying the framebuffer back with the CPU.
Three fixes were needed after session 49's reverted first attempt:

- **Session 50** implemented RT64's YUV16 decode (its `case G_IM_FMT_YUV:` fell
  through to `float4(0,0,0,1)`) and bounded the render wait on the RSP worker.
- **Session 51** found why no geometry was drawn: `0x800A5110` is **S2DEX 2.08**,
  so its per-macroblock `0xDC`/`0xDA` are `G_OBJ_MOVEMEM` and
  `G_OBJ_RECTANGLE_R` (the draw), not F3DEX2 `G_MOVEMEM`/`G_MTX`; RT64 mapped
  neither. The YUV16 TMEM planes and the UYVY source word were redone too.
- **Session 52** fixed the conversion: RT64 fed `G_SETCONVERT`'s raw unsigned
  9-bit fields into the matrix, while the hardware sign-extends each field and
  scales it `2*K+1`, and the green row paired `K1` with V instead of U.

The result is `docs/proofs/native-newgame-cathedral.png`; the assembled `'B5'`
image at `0x80243E28` renders as the cathedral. Session 49's reading and the
earlier "still black" notes in `-session48.md`/`-session47.md` are superseded.
The readback's source selection is `tools/njpeg_readback.py`, and
`ogre_sync_framebuffers()` → RT64's `State::syncFramebuffers()` forces the RDP's
pixels back to RDRAM before the copy.

## Audio ucode (resolved, session 75)

The game's audio is a custom libultra-ABI driver, submitted as `M_AUDTASK`
(**type 2**, not 5) with a **standard libultra RSP boot loader**:

```
task->t.ucode_boot = 0x8009ECB0  (ROM 0x2F0B0, 0xD0 bytes)
task->t.ucode      = 0x8009E050  (ROM 0x2E450, text)
task->t.ucode_data = 0x800ABDA0  (ROM 0x3C1A0, 0x800 bytes)
task->t.data_ptr   = 0x80136110  (the command list, game heap)
```

The boot loader (disassembled at IMEM 0x1000) sets `at = 0xFC0` (the OSTask in
DMEM), DMAs `task->ucode_data` (its true size) to **DMEM 0**, DMAs a fixed
`0xF80` bytes from `task->ucode` to **IMEM 0x1080**, then `jr 0x1080`. The
recompiled task therefore needs `text_address = 0x1080`; the runtime already
loads the OSTask at DMEM 0xFC0 and `ucode_data` at DMEM 0, and RSPRecomp emits
the initial `r1 = 0xFC0` itself.

`config/rsp-audio.toml` compiles `text_offset 0x2E450`, `text_size 0xC60`,
`text_address 0x1080` into `RspFuncs/audio_ucode.cpp`. Two constraints, both
load-bearing:

* **`text_address = 0x1080` is the IMEM address the loader enters, not the text's
  RDRAM low bits** (`0x0E50`) and not `0x1000`. Session 74 used `0x1000`; every
  internal `j`/`beq` target (which is absolute in the instruction encoding) then
  resolved 0x80 bytes early, the driver never DMA'd its command list, and the
  output was silence. This is the same rule as `config/rsp-njpeg.toml`.
* **`text_size = 0xC60`, not the loader's `0xF80`.** The fixed-length text DMA
  runs past the end of the audio code into the boot loader's own ROM bytes and
  the njpeg text at IMEM 0x1CE0, and those bytes' `j` targets point below the
  text. The game never executes that tail; compiling it only emits references to
  labels that do not exist. The audio code's own last instruction is at IMEM
  0x1CD0. Same rule as `config/rsp-njpeg.toml`'s `text_size = 0x7B8`.

`extra_indirect_branch_targets` must carry the command handlers, because the
driver dispatches through a **halfword table in `ucode_data` at DMEM 0**:

```
10d4: srl  at,k0,0x17        ; opcode = top byte of the command word
10d8: andi at,at,0xfe
10dc: lh   at,0(at)          ; DMEM 0 (ucode_data[0]) + 2*opcode
10e0: jr   at
```

The table is the standard 16-entry audio ABI (`0x00` SPNOOP … `0x0F` SETLOOP);
the handler addresses it holds are data, so RSPRecomp cannot see them. The
config lists every 4-byte-aligned target the table can produce
(`(entry | 0x1000) & 0x1FFF`) plus `0x10B4`, which the command-list DMA helper
returns to through `jr $5` rather than `$ra`. The tables for opcodes 7/8 hold
0x0000 (this driver has no SEGMENT/SETBUFF; it passes counts and addresses
inline), and the game does not emit them.

**One caveat the port has to live with:** `SETLOOP` (opcode `0x0F`, handler
`0x1384`) stores its 24-bit loop address as a word at **DMEM `0x0E`** — the exact
bytes of table entries 7 and 8. So those two slots are *overwritten every time
the game sets a loop point*, and a dispatch of opcode 7 or 8 would `jr` to half
of an audio buffer address (not an instruction boundary). The store is faithful
(the RSP is byte-addressable; aligning it down to `0x0C` would clobber entry 6
= SAVEBUFF, which the game uses constantly), so `app/src/rsp.cpp` wraps the
recompiled ucode in `audio_ucode_guard`: if it ever returns non-`Broke`, log it
and report the task completed rather than letting the runtime exit the process.
See `docs/HANDOFF-2026-09-18-session75.md` §6.

**Verification recipe (no listening required):** the game's own AI descriptor is
at `*(0x800A9B90)` — `+0x00` buffer, `+0x04` length in words; a live console
`dump` while music plays then shows real PCM there:

```
printf 'c\ndump /tmp/live.bin\n' > /tmp/ogre-console75.tmp && mv … /tmp/ogre-console75.txt
OGRE_NO_AUDIO=1 OGRE_CONSOLE_AT_MS=5000 OGRE_CONSOLE_FILE=/tmp/ogre-console75.txt \
  OGRE_EXIT_AFTER_MS=7000 ./build-app/ogrebattle64 assets/ogre64.z64
```

Session 75 measured **1104/1104 non-zero stereo samples** in the AI buffer at
t=5 s (scene 0x09, the intro), values spanning roughly −9600..+5800.

`OGRE_AUDIO_UCODE=0` forces the old stub (silent, but the buffers still queue and
drain) as an A/B escape hatch; unset runs the real microcode.


