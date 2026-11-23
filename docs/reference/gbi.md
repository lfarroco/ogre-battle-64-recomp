---
title: Graphics microcodes and GBI
summary: How a display list's ucode is identified by hash, and the GBI commands each ucode uses.
status: verified
session: 192
order: 20
---

# Graphics microcodes and GBI

RT64 picks a GBI by hashing the task's microcode. The hash is XXH3-64 over the
raw RDRAM bytes at the task's `ucode` and `ucode_data` addresses, at the segment
lengths in RT64's database (`tools/RT64/src/gbi/rt64_gbi.cpp`). A miss makes
`getGBIForUCode` return `nullptr` and RT64 prints `Unable to find a matching GBI
in the current database`.

The port prints the task's ucode on every submission with `OGRE_DL_TRACE=1`:

```
[renderer] display list 853 at t=8278ms entries=0 (type=1 ucode=0x8009F540 data=0x8028C120)
```

The text segment is hashed at `0x1390` for F3DEX2 and `0x18C0` for S2DEX2. The
data segment is hashed at `0x420` for F3DEX2 and `0x390` for S2DEX2. Check both lengths and both byte views
before concluding that a hash misses. `docs/guides/emulator-first.md` §3 is the
recipe.

## Microcodes

<!-- BEGIN GENERATED: addresses:ucodes -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x8009F540` | `gbi_f3dex2_208` |  | ucode | XXH3-64 over 0x1390 text bytes = 0xCF55FAE288BFE48D, RT64 F3DEX2.fifo 2.08. The map and most scenes. | 64 | high |
| `0x800A08D0` | `gbi_f3dex2_non_208` | mission 0x03 | ucode | XXH3-64 over 0x1390 = 0x4C12DAE0534D7135, RT64 F3DEX2.NoN.fifo 2.08. NoN disables only the near-depth clip. | 192 | high |
| `0x800A5110` | `gbi_s2dex2_208` | njpeg backdrop lists | ucode | Text 0x18C0 = 0x9300F34F3B438634, data 0x390 = 0x50EF0DFBD3A8CD0F. 0xDA is G_OBJ_RECTANGLE_R and 0xDC is G_OBJ_MOVEMEM. | 51 | high |
| `0x8009E050` | `ucode_audio` | type 2 | ucode | Audio microcode. A scene-0x05 run submitted 1706 type-2 tasks in 9 s. | 64 | medium |
| `0x8009ED80` | `ucode_njpeg` | type 4 | ucode | Nintendo-JPEG decoder, recompiled into RspFuncs/njpeg_ucode.cpp. data_size is the macroblock count and yield_data_size the quantization scale. | 47 | high |
| `0x8009ECB0` | `ucode_njpeg_boot` |  | ucode | Boot half of the njpeg microcode. Its IMEM DMA address is 0x1080 because it loads at IMEM 0x080. | 47 | high |
| `0x800AC050` | `g_njpeg_tables` |  | table | Huffman and quantization tables for the njpeg decoder. | 47 | high |
<!-- END GENERATED: addresses:ucodes -->

`0x800A5110` is S2DEX 2.08, not F3DEX2. In an S2DEX2 list the per-macroblock
`0xDC` command is `G_OBJ_MOVEMEM` (`gSPObjSubMatrix`; the matrix selector is
`cmd0 & 0xFFFF`, and `2` is the sub matrix) and `0xDA` is
`G_OBJ_RECTANGLE_R` (the 24-byte `uObjSprite`). They are not `G_MOVEMEM` and
`G_MTX`. `OGRE_DL_DECODE` and `OGRE_DL_ANALYZE` decode every list as F3DEX2 and
cannot answer this question.

## S2DEX2 colours

The njpeg backdrop's tiles are YUV16. The RDP stores a YUV tile in two planes:
one luma byte per texel at `offset + stride*t + s` in the upper TMEM half, and
one U/V pair per two texels in the lower half, both at `stride = line << 3`. A
YUV `LOADTILE` de-interleaves (`RDP::loadYUVTileToTMEM`), and YUV16 tiles need
raw-TMEM sampling. The decoder's source word is `[U, Y(even), V, Y(odd)]`
(mupen64plus-rsp-hle `jpeg.c` `GetUYVY`).

The conversion uses the matrix the game programs with `G_SETCONVERT`. The raw
9-bit fields are `k0=175 k1=469 k2=423 k3=222`, and each must be sign-extended
and scaled `2*K+1`, which gives `351/-85/-177/445`. The rows are
`R = Y + K0*V'`, `G = Y + K1*U' + K2*V'` and `B = Y + K3*U'`, with
`U'=U-128`, `V'=V-128`. `K1` pairs with U and `K2` with V. `K4` and `K5` are the
raw fields over 255.

## The njpeg task

The Nintendo-JPEG decoder is task type 4. The CPU Huffman decode
(`func_8008B250`) submits four `M_NJPEGTASK` tasks that decode in place to 16-bit
YUV. `data_size` is the macroblock count and `yield_data_size` is the
quantization scale. The microcode is recompiled into `RspFuncs/njpeg_ucode.cpp`
and runs by default; `OGRE_NJPEG=0` forces the stub.

## Evidence

`docs/guides/emulator-first.md` §3 and §4, `docs/guides/njpeg-backgrounds.md`,
sessions 47, 51, 52, 64 and 192. `AGENTS.md` ("Verified facts") records the
S2DEX2 and YUV corrections.
