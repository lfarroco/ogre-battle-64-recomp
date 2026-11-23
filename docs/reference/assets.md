---
title: Assets, LZ payloads and the njpeg container
summary: How an asset id becomes a decompressed buffer, and the formats of the LZ payloads and the njpeg images.
status: verified
session: 64
order: 70
---

# Assets, LZ payloads and the njpeg container

The game reads its resources by asset id. Three functions cover the path from an
id to a buffer: `func_8009DAF4` returns the payload size, `func_8009DBB8` copies
the payload into a caller buffer, and `func_8009DD38` returns a decompressed
copy. The LZ payload carries the dialogue text, the scripted step descriptors and
the map's decoded assets. The njpeg backgrounds are a container format that the
same loader carries.

## The asset id

An asset id is an offset from ROM `0x594250`. It is not a ROM address.
`func_8009DAF4` (`asset_size`) computes `(id & 0x0FFFFFFF) + 0x594250` and reads
the `u32` there. That word is the size of the asset's payload.
`func_8009DBB8` (`asset_load`) copies that many bytes starting at `rom + 4`.
The four bytes at the offset hold the size and the payload follows them.
`docs/HANDOFF-2026-09-15-session44.md` §1 records that the loader copies `size`
bytes from `rom + 4` and that the LZ block starts there.

The base is the same for every asset. `tools/ogrelz.py` implements the
conversion in `asset_rom()` and reads the `u32` payload size at the returned
offset. Two ids from the sources:

| id | ROM | resource |
|---|---|---|
| `0x19A8804` | `0x1F3CA54` | the New Game scripted step table |
| `0x00183352` | `0x7175A2` | the New Game cathedral backdrop container |

The first row is in `docs/reference/scene-script.md`. The second is in
`docs/HANDOFF-2026-09-15-session46.md` §2.
The mask keeps the low 28 bits of the id. The sources state the mask. They do
not state what the top four bits mean.

To read an asset by hand, add `0x594250` to the masked id, read the `u32` there
as the payload size, and start at the next byte.
`python3 tools/ogrelz.py --asset assets/ogre64.z64 <id> ...` prints the asset's
ROM offset, its payload size and its decompressed size, then the text runs it
finds in the decoded bytes. It prints the first 32 bytes when the payload holds
no text (`tools/ogrelz.py`, `main`).

## The decode entry

`func_8009DD38` (`asset_decode`) is the entry the game calls for a decompressed
copy. It calls `func_8009DAF4` for the payload size and caches that size at
`0x800ABD70` (`sw v0,-17040` at `0x8009DDE0`, `docs/reference/addresses.tsv`).
It calls `func_8009DBB8` to copy the payload into a temporary buffer.
`func_8007A7E0` (`lz_size`) reads the decompressed size at the payload
start. `func_8009DD38` allocates that many bytes with `func_80070F30`
(`malloc`). It decodes with `func_8007A110` (`lz_decompress`), frees the
temporary buffer with `func_800712C4` (`free`), and returns the decompressed
buffer. The sequence is the `asset_decode` row in `docs/symbols.md`.

The return value is the decompressed buffer. The map's enter stores each return
in the screen state and decodes about 17 assets this way
(`docs/HANDOFF-2026-09-17-session64.md` §3).
The map's `state[+0x04]` is asset `0x01DD210A`. The live buffer was 21128/21128
bytes identical to an offline decode of that id (session 64 §2).
That comparison checks the entry, the loader and the decoder together.
`state` is a heap pointer read from `0x80197B18`. `docs/reference/memory.md`
covers the pointer.

## The LZ payload

The payload's first four bytes are the decompressed size, big-endian.
`func_8007A7E0` returns that word and `func_8007A110` decodes the token stream
after it. `tools/ogrelz.py` is an independent implementation of the same format,
read off the disassembly at `0x8007A110`.

The stream is a sequence of tokens and each token begins with a flag byte `b`.
The decoder tests the bits in this order (`tools/ogrelz.py`):

* `b & 0x80`: a back-reference. The length is `((b >> 3) & 0xF) + 3` and the
  offset is `((b & 7) << 8) | next_byte`.
* `b & 0x40`: a literal run of `(b & 0x3F) + 1` bytes.
* `b & 0x20`: a zero run of `(b & 0x1F) + 2` bytes.
* `b & 0x10`: a long back-reference with two bytes after the flag. The length is
  `4 + (b & 0xF) + ((hi >> 2) & 0x30)` and the offset is
  `((hi & 0x3F) << 8) | lo`.
* `b == 0`: a far back-reference with three bytes after the flag. The length is
  `next_byte + 5` and the offset is `(hi << 8) | lo`.
* `b == 1`: a run of `0xFF` of `next_byte + 3` bytes.
* `b == 2`: a zero run of `next_byte + 3` bytes.

A back-reference copies from `dst - offset - 1`. Overlapping copies are legal and
must be done one byte at a time (`tools/ogrelz.py` docstring).

The compressed stream must end exactly on the declared payload boundary, which
session 64 writes as `consumed == payload`.
Session 64 ran that check over the map's 13 real assets: 13/13 exact, payloads
`0x178`..`0x2439`, outputs `0x2A8`..`0x7E20`.
A misread token desynchronises the stream, so the end position stops landing on
the boundary. The boundary is a format check that needs no game run.
`docs/guides/emulator-first.md` §4 lists it as the "are the bytes what hardware
would have" check.

Dialogue text uses the same compressor.
`docs/DECISIONS.md` ("Dialogue text is LZ-compressed") records that
`Archbishop`, `learned` and `promise` appear nowhere in the ROM as ASCII, and
that cutscene and dialogue text lives in these LZ blocks.
The name table at ROM `0x64810` and the attract-story text at ROM `0x100710` are
stored without compression.
The step descriptors are LZ payloads too. `docs/reference/scene-script.md`
covers the table and the descriptor loader.

## The njpeg container

An njpeg payload is a container. It begins with the magic `'HU'`, a two-byte
scale field, a big-endian `u16` width, a big-endian `u16` height, then `'HUFF'`
and a big-endian `u16` macroblock count.
For the cathedral's 320x240 image the bytes are
`48 55 fe 00 01 40 00 f0 48 55 46 46 01 2c`: `'HU'`, scale `0xFE`, width 320,
height 240, `'HUFF'`, 300 macroblocks (`docs/HANDOFF-2026-09-15-session46.md`
§2). `docs/guides/njpeg-backgrounds.md` records the same fields for the width,
the height, `'HUFF'` and the macroblock count.

Asset `0x00183352` maps to ROM `0x7175A2` and its payload is `0x5AE0` bytes
(session 46 §2).
The macroblock count must equal `(width / 16) * (height / 16)`.
All 75 njpeg assets satisfy that equation, and 42 of the 75 are 320x240
(`docs/guides/njpeg-backgrounds.md`).

The CPU Huffman decode is `func_8008B250`.
`func_ovlE_80199A08` allocates the `mbs * 768` buffer at `state[0x68]` and calls
`func_8008B250(payload, state[0x68])`
(`docs/HANDOFF-2026-09-15-session47.md` §3).
The buffer feeds one `M_NJPEGTASK` task, RSP task type 4.
The task carries `ucode = 0x8009ED80`, `data_size` equal to the macroblock count
and `yield_data_size` equal to the scale (session 47 §3).
The cathedral step submits four of these tasks, with macroblock counts 300, 165,
180 and 99 (session 47 §2).

The microcode and its recompilation are on
[`docs/reference/gbi.md`](gbi.md). The `text_address` label base is on
[`docs/reference/runtime.md`](runtime.md). The display list that draws the
macroblocks and the tile geometry are on
[`docs/guides/njpeg-backgrounds.md`](../guides/njpeg-backgrounds.md).

## Checking an asset offline

`tools/ogrelz.py` has two entry points. The `--asset <rom> <id>...` form decodes
assets by id. The positional form `<rom> <start_hex> [size_hex]` scans a ROM
range for LZ text blocks (`tools/ogrelz.py`, `main`).

The njpeg assets are not LZ payloads. The njpeg guide's "Asset format and the
full inventory" section carries the script that enumerates them from the ROM and
checks the macroblock count against the dimensions. Its "Checking another
background" section carries the run recipe that reads the readback log of a live
background.

For an LZ asset the check is the boundary condition. Decode the payload with
`tools/ogrelz.py` and require the token stream to end on the declared payload
boundary. Session 64 did that for the map's 13 assets.

## Addresses

<!-- BEGIN GENERATED: addresses:addresses -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x800ABD70` | `g_asset_payload_size` | last payload size | global | `func_8009DD38` stores it (`sw v0,-17040` at 0x8009DDE0) and reads it at 0x8009DD74. `lui 0x800b` + negative offset resolves here, not to 0x800ABD70. | 47 | high |
| `0x594250` | `g_asset_size_table` | asset id base | table | `func_8009DAF4` reads a u32 at (id & 0x0FFFFFFF)+this; the address is formed by `lui 0x59`/`addiu 0x4250` at 0x8009DB50. | 44 | high |
| `0x183352` | `asset_njpeg_cathedral` | cathedral container | asset | The njpeg container at ROM 0x7175A2; `HU` + `HUFF`, 320x240, 300 macroblocks (session 46 §2). | 46 | high |
| `0x7175A2` | `rom_njpeg_cathedral` | asset 0x183352 | rom | ROM offset of the cathedral njpeg payload; the LZ/container data starts at +4 after the size word. | 46 | high |
| `0x8008B250` | `njpeg_huff_decode` | CPU Huffman decode | function | `func_ovlE_80199A08` calls `func_8008B250(payload, state[0x68])` before the type-4 task (session 47 §3). | 47 | high |
| `0x8009DDE0` | `asset_cache_store` | sw -17040 | code | Stores the payload size; resolves to 0x800ABD70. | 217 | high |
<!-- END GENERATED: addresses:addresses -->

## Evidence

- `docs/symbols.md` — the `asset_size`, `asset_load`, `asset_decode`, `lz_size`
  and `lz_decompress` rows and the order of the decode entry.
- `docs/reference/addresses.tsv` — the `0x800ABD70` payload-size row, with the
  `sw v0,-17040` store at `0x8009DDE0`.
- `tools/ogrelz.py` — the independent LZ implementation, the token table, the
  `asset_rom()` conversion and the `--asset` entry point.
- `docs/HANDOFF-2026-09-15-session44.md` §1 — the id-to-ROM mapping, the payload
  size word and the LZ block at `rom + 4`.
- `docs/HANDOFF-2026-09-17-session64.md` §2 — the 13/13 payload boundary check
  and the byte-identical map buffer for asset `0x01DD210A`.
- `docs/HANDOFF-2026-09-15-session46.md` §2 — asset `0x00183352` at ROM
  `0x7175A2`, its `0x5AE0` payload and the `'HU'` header bytes.
- `docs/HANDOFF-2026-09-15-session47.md` §2 and §3 — the four type-4 tasks, the
  macroblock counts, `func_8008B250` and the task fields.
- `docs/guides/njpeg-backgrounds.md` — the container header order, the 75-asset
  inventory, the macroblock check and the live readback recipe.
- `docs/guides/emulator-first.md` §4 — the boundary check as the decoded-bytes
  faithfulness test.
- `docs/DECISIONS.md` ("Dialogue text is LZ-compressed") — the dialogue and
  cutscene text blocks and the uncompressed exceptions.
