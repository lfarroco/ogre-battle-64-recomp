# Proposed symbol names

`func_XXXXXXXX` / `D_XXXXXXXX` names carry an address and nothing else, and this
project's record is full of "the function at 0x8023C894". This file is the
vocabulary: address → proposed name → evidence → confidence. **No rename has
been applied** — see "Promoting a name" at the end for what that costs.

Convention (proposed):

* `<meaning>_<addr>` — keep the address in the name, so every older handoff,
  PLAN bullet, probe tag and comment stays greppable, and two units can never
  collide on a bare descriptive name.
* Bank-overlay code keeps its `ovlX_` provenance (`func_ovlC_8023C894` →
  `dl_emit_8023C894` loses which unit owns it; prefer
  `ovlC_dl_emit_8023C894`). Records overlap in RAM by design.
* Do **not** invent a name that the runtime already implements (`osXxx`,
  `alXxx`, `guXxx`, …): `config/symbols/symbol_addrs.txt` warns that a name must exist in the
  runtime or the recompiled output will not link.
* Confidence: **high** = instruction-level evidence recorded below; **medium** =
  strong but partial; **low** = a hypothesis worth a name but not a rename.

## How this file is generated

The rows live in [`reference/addresses.tsv`](reference/addresses.tsv). Every
table below is rendered from the rows whose `page` is `symbols` and whose
`section` is the block's. Add or correct a row there, then run `make docs-index`.
`make docs-check` fails when a table is stale. `docs/reference/memory.md`,
`docs/reference/gbi.md`, `docs/reference/scene-script.md` and
`docs/reference/banks.md` render their own sections from the same file.

## Globals that pay off first

These are read in dozens of places; a name here makes whole functions readable.

<!-- BEGIN GENERATED: addresses:globals -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x8018FC39` | `g_step_engine_mode` |  | global | byte; only writer `func_80227700` (`sb a0`, `0x80227708`); `0` = movie engine, `2` = the cutscene/DL engine every step ≥ 2 selects; read by `func_ovlC_8022D1CC` (`0x8022D208`), `func_ovlC_8022ACB0`, `func_ovlC_8023A9AC`, … | - | high |
| `0x8018F1C0` | `g_newgame_step` | (half) | global | scene-script VM opcode `0x10` stores `var[0]` at `0x80170ADC`; read as the step index (`F1C0 & 0xFFF`) by `func_80226FA8` (`0x80226FFC`) | - | high |
| `0x8018F1C2` | `g_newgame_next_scene` | (half) | global | stored `0x8002` by the same VM opcode (`0x80170ACC`); copied into `D_800C4C26` by the title transition `func_80177A58` | - | high |
| `0x8022A978` | `g_step_descriptor` | (ptr) | global | `func_80227FF8` decompresses the step asset into it (`0x80228110`); the interpreter `s3` | - | high |
| `0x8022A970` | `g_step_desc_pc` | (word index) | global | interpreter `s1`; reset to 0 by `func_80227FF8` (`0x80228184`) | - | high |
| `0x8022A994` | `g_step_objects` | (0x1CB8 malloc) | global | allocated + bzeroed by `func_ovlC_8022D1CC`; object pointer arrays at `+0x18` (28 entries, read by the render tail) and `+0x1C54` (20 entries, written by `func_ovlC_8023A9AC`) | - | high |
| `0x8022A9B4` | `g_step_index` | (half) | global | `func_8022683C` stores its second argument (`0x8022685C`); read by `func_80227FF8` and by the object loop | - | high |
| `0x8022A9A0` | `g_step_finished_gate` | (byte) | global | compared with `0xD7` by `func_ovlC_802261B0`/`func_ovlC_802262E4`; set to 0 by the command handlers | - | medium |
| `0x80196F80` | `g_step_ptr_reloc` | (u16 table) | global | `func_80227FF8` rewrites `0x088800NN` descriptor words with `u16` entries from here (`0x80228138`-`0x8022817C`) | - | high |
| `0x800E9BA0` | `g_dl_cursor` |  | global | the RDP emitters advance it (`sw v0, D_800E9BA0`); loaded into `a3` by the proper emitters | - | high |
| `0x800E82C8` | `g_task_queue[6]` | (stride `0xA8`, `+0x10` = callback) | global | filled by `func_80076F5C`, consumed by `func_800765D8` | - | high |
| `0x800E7A30` | `g_current_task` | (0xA8 copy) | global | `func_800765D8` copies the slot here before the `jalr` (`0x80076664`) | - | high |
| `0x800E7A44` | `g_scene_hooks[0]` |  | global | the selector callbacks store the scene's update/draw/aux pointers (`func_ovlC_80225A3C`, `func_ovlC_80226110`) | - | medium |
| `0x800E7A48` | `g_scene_hooks[1]` |  | global | the selector callbacks store the scene's update/draw/aux pointers (`func_ovlC_80225A3C`, `func_ovlC_80226110`) | - | medium |
| `0x800E7A4C` | `g_scene_hooks[2]` |  | global | the selector callbacks store the scene's update/draw/aux pointers (`func_ovlC_80225A3C`, `func_ovlC_80226110`) | - | medium |
| `0x800C4C26` | `g_pending_scene` |  | global | the scene manager reads it; `0x8000\|id`, plus `0xFFFC`/`0xFFFE` control words | - | high |
| `0x800E810E` | `g_active_scene_id` |  | global | what the dispatcher is running; the app's `[scene]` log and `OGRE_TAP_SCENE` read it | - | high |
| `0x800E8294` | `g_scene_descriptor` |  | global | scene manager stores the accessor's return here; words are enter/update/hook/leave/mask | - | high |
| `0x8022A9A4` | `g_render_matrix_0` |  | global | written by the `0x80239874` render tail and consumed by its callers | - | medium |
| `0x8022A9A8` | `g_render_matrix_1` |  | global | written by the `0x80239874` render tail and consumed by its callers | - | medium |
| `0x8022A9AC` | `g_render_matrix_2` |  | global | written by the `0x80239874` render tail and consumed by its callers | - | medium |
| `0x802395E0` | `g_rec14_arena` | (RAM range `0x802395E0..0x80243DC0`) | global | record 14's arena: the game streams **two banks** here, `bankRec14b` (ROM `0x2AE390`, 0xA7E0 — scene `0x0D` visit 1 / the movie) and `bankRec14c` (ROM `0x2A8CF0`, 0x56A0 — steps ≥ 2); unit F / unit G, and the reason the arena's call sites must be `LOOKUP_FUNC` | - | high |
| `0x80239C24` | `func_ovlG_80239C24` | (rec14c) | global | descriptor-interpreter opcode 42 calls it (`jal` at `0x80229004`) with `(6, 0x20, 0x20)`; a real function in rec14c (`addiu sp,sp,-0x60`), a body interior of `func_ovlC_80239874` in rec14b — session 45's wall | - | high |
| `0x80197B18` | `g_screen_state` | (ptr) | global | the map's module entry stores `malloc(0x470)` here (`0x8019A82C`) and every map draw loads `state = *(0x80197B18)` from it (`func_ovlM_801A2A7C` `0x801A2ADC`). **It is a heap pointer that changes per run** — read it from this global, never hardcode an observed value (`0x801F1570` in sessions 60/63 is run-specific) | - | high |
| `0x801A6FE0` | `g_sprite_table` | (stride 8) | global | entries `{u16 W, u16 H, s16 u, s16 v}`, static module data (unit M's data half, byte-identical to ROM); the consumer is `sprite_rect_emit` (`0x8019F87C`: `(a2 & 0xFFFF)*8 + 0x801A6FE0`, `lhu`/`lh` the four fields). Live: slot 10 `(16,11)`, 11 `(144,23)`, 12-15 `(16,24)` | - | high |
| `0x801936C9` | `g_chaos_frame` | (u8) | global | the player's Chaos Frame, 0..100: it picks the good and the bad ending, and the scene-`0x13` screen after the credits is the only place the game prints it. Instruction evidence: the game-state initialiser `func_8016C900` bzeroes the flags block at `0x80193698` for `0x3A` bytes and stores `50` here (`0x8016C998`) — the documented starting value; the scene-`0x13` update `func_801B5128` reads this byte and masks it `0x7F` (`0x801B5360`); the scene-script VM opcode `0xFF` stores a script operand here (`0x80171480`, jump table `0x80190BC8`). The community figure for a Project64 symbol is `0x801936A9`; that byte has no read or write in this ROM, and reads 0 at the title while this one reads 50. The app's DEBUG tab reads it live (`app/src/game.hpp` `CHAOS_FRAME_ADDRESS`) | - | high |
<!-- END GENERATED: addresses:globals -->

## Functions worth naming

<!-- BEGIN GENERATED: addresses:functions -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x80075BC0` | `scene_manager` |  | function | per-frame: `D_800AF028[id]()` → descriptor → enter/leave | - | high |
| `0x80072398` | `frame_update` |  | function | calls the descriptor's `+0x04` update (`0x8007265C`) then `func_800765D8` (`0x8007266C`) | - | high |
| `0x800765D8` | `task_queue_process` |  | function | loops the 6 slots, copies to `D_800E7A30`, `jalr` the callback with `a0` = slot | - | high |
| `0x80076F5C` | `task_queue_push` |  | function | finds a free `0x8000`-clear slot, stores `{id, callback, args}` | - | high |
| `0x80178568` | `scene_0d_enter` |  | function | 0x0D descriptor word 0; picks movie vs command mode on `D_8018F1C0` | - | high |
| `0x80178954` | `scene_0d_update` |  | function | descriptor word 1 | - | high |
| `0x80226FA8` | `step_command_run` |  | function | `func_8022683C(-5, F1C0 & 0xFFF)`; bumps the step word (`0x80227010`) | - | high |
| `0x8022683C` | `step_command_dispatch` |  | function | `jtbl_ovlC_8022ABA0[cmd + 10]`; DMAs 14a/14b, sets `D_8018FC39` | - | high |
| `0x80227E64` | `step_command_of` |  | function | reads the step asset, returns `jtbl_ovlC_8022ABE0[last_byte - 1]` | - | high |
| `0x80227FF8` | `step_descriptor_load` |  | function | loads + LZ-decompresses the step asset, applies `0x088800NN` relocations | - | high |
| `0x802282D8` | `step_descriptor_interp` |  | function | walks `g_step_descriptor[g_step_desc_pc]`; opcodes `0x8000000x` | - | high |
| `0x80227030` | `step_enter_run` |  | function | resets on mode 0, then `func_802282D8` | - | high |
| `0x802329D0` | `step_alloc_0x960` |  | function | malloc + bzero, stored at `D_802395D0` | - | high |
| `0x8009DAF4` | `asset_size` |  | function | reads the u32 at `(id & 0x0FFFFFFF) + 0x594250` | - | high |
| `0x8009DBB8` | `asset_load` |  | function | copies the payload (that u32 bytes) from `rom + 4` | - | high |
| `0x8007A7E0` | `lz_size` |  | function | returns the u32 at the payload start (decompressed size) | - | high |
| `0x8007A110` | `lz_decompress` |  | function | `(dst, src)`, tokens per `tools/ogrelz.py` | - | high |
| `0x80093380` | `bzero` |  | function | all-zero stores | - | high |
| `0x80070F30` | `malloc` |  | function | the allocator family traced by the runtime | - | high |
| `0x800712C4` | `free` |  | function | the allocator family traced by the runtime | - | high |
| `0x8009AAA0` | `tlb_map_0xC0000000` |  | function | writes TLB entry 31, EntryHi `0xC0000000` (`0x8009AAB0`) | - | medium |
| `0x8009AB00` | `tlb_clear` |  | function | writes entries 30..0 with EntryHi/Lo 0 | - | medium |
| `0x800988A0` | `pack_f32x16_to_s16` |  | function | reads 16 floats from `a0`, writes 8+8 words packed to `a1` | - | medium |
| `0x80098AA0` | `pack_f32x3_to_s16` |  | function | same shape, inputs in `a1`/`a2`/`a3` | - | low |
| `0x80092C18` | `build_fixed_matrix` |  | function | builds the packed matrix then `func_800988A0` | - | medium |
| `0x8009DD38` | `asset_decode` |  | function | `asset_size` (`0x8009DAF4`) → cache the payload size at `0x800ABD70` → `asset_load` (`0x8009DBB8`) into a temp → `lz_size` (`0x8007A7E0`) → `malloc` (`0x80070F30`) → `lz_decompress` (`0x8007A110`) → `free` the temp; **returns the decompressed buffer** (verified: the map's `state[+0x04]` equals an offline LZ decode of the id passed in, byte for byte). The map's module entry calls it ~17 times | - | high |
| `0x8019A7C0` | `scene_05_enter` | (map) | function | the module the scene-`0x05` enter `jal`s after its DMA; mallocs `g_screen_state` (`0x8019A7E4`-ish), decodes ~17 assets into its fields with **software-pipelined `asset_decode` calls** (each `jal`'s delay slot stores the *previous* return), then `malloc(0x18000)`s `state[+0x34]` and composites an RGB555 LUT (`+0x18`/`+0x20`) over an 8-bit index image (`+0x1C`/`+0x24`) into it as RGBA32 | - | high |
| `0x8019F83C` | `sprite_rect_emit` |  | function | `(a0 = x, a1 = y, a2 = sprite-table index)`; emits `G_RDPPIPESYNC`, the `G_TEXRECT` (**w0 = lower-right `(x+W, y+H)`, w1 = upper-left `(x, y)`**, both in quarter-px), `G_RDPHALF_1` carrying **s,t** (`u<<21 \| v<<5`), `G_RDPHALF_2` with `dsdx = dtdy = 1.0`, `G_RDPPIPESYNC`. It emits **no `G_SETTILESIZE`** — the caller supplies that | - | high |
| `0x801A2A7C` | `map_party_draw` |  | function | `(a0 = x, a1 = y)`; two `sprite_rect_emit` calls — **call 1 = the shadow** (ROM `0x81B50`, `a2=0xB`, origin `(x-8, y)`, texture `state[+0x04]+0x1068`, static) and **call 2 = the knight** (ROM `0x81BFC`, `a2=0xA`, origin `(x-16, y-24)`, texture `state[+0x34] + ((3*state[0x1DC] + f) << 12)`, i.e. 24 frames of `0x1000` bytes). Each call writes its own `SETTIMG`/`SETTILE`/`LOADBLOCK`/`SETTILESIZE` block into `g_dl_cursor` first | - | high |
<!-- END GENERATED: addresses:functions -->

## Save (battery SRAM) — the whole access path

OB64 saves to 32 KiB of battery SRAM; the Controller Pak is only the game's
copy/backup device. Read at instruction level in `build/ogrebattle64.elf`
(session 78):

<!-- BEGIN GENERATED: addresses:save -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x80074CF0` | `save_image_read(dst, dev_off, size)` |  | function | if `*(0x800A83B8)` is null: `malloc(0x8000)` then read the whole image in 256-byte `save_dma(dev, off, 256, dir=0)` calls (`0x80074D24`-`0x80074D60`); then `memcpy(dst, image + dev_off, size)` (`0x80074D68`) | - | high |
| `0x80074C58` | `save_image_write(image)` |  | function | allocates a 16-byte handle holding `save_dma`, then writes `0x8000` bytes in 256-byte chunks with direction `1` (`0x80074CA8`-`0x80074CC4`); skipped while `*(0x800C4800)` is set | - | high |
| `0x80074BF0` | `save_commit(magic)` |  | function | if `*(0x800A83BC)` (dirty) **and** `magic == 0x37081383`: `save_image_write(*(0x800A83B8))`; then clears the flag and frees the cached image | - | high |
| `0x80074AD4` | `save_slot_summary_load(slot)` |  | function | `malloc(6224)`; `save_slot_validate(slot, buf)`; `save_image_read(buf, 0x10 + slot*0x1850, 6224)`; returns a 26-byte `memcpy` of `buf + (*(0x800A8258) + 12)` when `*(buf+0x0C) != 0`, else NULL. `slot == 15` takes the other arm: `malloc(19176)`, `save_record15_validate`, `save_image_read(buf, 0x30B0, 19176)` | - | high |
| `0x8007541C` | `save_slot_validate(slot, buf)` |  | function | reads 6224 bytes from `*(0x800A83B8) + 0x10 + slot*0x1850`; `memcmp(0x800A8240, buf+4, 8)` — the 8-byte `QuestOG3` signature; `u16[0x00] == checksum_sum_seeded(buf+0x0C, 6212, 0x10+slot*0x1850)`; `u16[0x02] == checksum_bits_seeded(same)` | - | high |
| `0x80075578` | `save_record15_validate(buf)` |  | function | the same magic check for the 19176-byte record at device `0x30B0`; **a zero first word is accepted as it stands** (`beqz` at `0x80075620`), otherwise the two checksums over `buf+0x0C`, length `0x4ADC` = 19164, seed `0x30B0` | - | high |
| `0x80075A84` | `checksum_sum_seeded(data, len, seed)` |  | function | loop of `lbu`/`addu`; returns `(sum + seed) & 0xFFFF` | - | high |
| `0x80075B00` | `checksum_bits_seeded(data, len, seed)` |  | function | per byte, 8 `srl`/`andi` steps counting set bits; returns `(count + seed) & 0xFFFF` | - | high |
| `0x80075AC4` | `checksum_sum` |  | function | the same two loops **without** the seed; the un-seeded pair is what the copy-to-pak path uses, so every note slot inside a Controller Pak validates with seed 0 | - | high |
| `0x80075B60` | `checksum_bits` |  | function | the same two loops **without** the seed; the un-seeded pair is what the copy-to-pak path uses, so every note slot inside a Controller Pak validates with seed 0 | - | high |
| `0x80093060` | `memcpy` |  | function | the two `jal` helpers this cluster is built from | - | high |
| `0x80092F50` | `memcmp` |  | function | the two `jal` helpers this cluster is built from | - | high |
| `0x800A83B8` | `g_save_image` | (ptr) | global | the cached 32 KiB image, allocated by `save_image_read` and freed by `save_commit` | - | high |
| `0x800A83BC` | `g_save_dirty` | (byte) | global | set by the writers, tested by `save_commit` | - | high |
| `0x800A8240` | `g_save_signature` | (8 bytes) | global | the `memcmp` target in `save_slot_validate`; BSS, filled at boot, and every valid slot carries `QuestOG3` at `+0x04` | - | high |
| `0x800A824C` | `g_save_field_table` | 13 entries, stride 0x1C, ends at 0x800A83B8 | table | func_800749C0 walks 13 entries; calls entry+0x04 (`lw -32176` at 0x80074A70) with slot+0x0C+*(entry+0x0C) (`lw -32168` at 0x80074A84). Entry 0 offset1 is at 0x800A8258. | - | high |
| `0x800A8258` | `g_save_field0_offset` | entry 0, +0x0C | field | Entry 0 offset1 holds 4, so the summary is at slot+0x10 (`lw -32168` at 0x80074A84). | - | high |
<!-- END GENERATED: addresses:save -->

**Slot geometry**: slot `n` is the 6224-byte (`0x1850`) record at device
`0x10 + n*0x1850`. Slot index **15 is special** — the 19176-byte (`0x4AE8`)
record at device `0x30B0`; the game writes it at the map, which is why even a
"blank" battery image has a `QuestOG3` at `0x30B4` (with a zero first word, which
`save_record15_validate` accepts without checking its checksums). A slot's first
4 bytes are two `u16` checksums over `slot+0x0C .. slot+0x1850`, each **seeded
with the slot's device offset**, so the same 6224 bytes validate only in the slot
they were checksummed for (`reseed_slot` in `tools/sramsave.py`).

## Shared tails (`jal` targets with no prologue)

`make midfunc` lists all 67; the ones that have bitten this project:

<!-- BEGIN GENERATED: addresses:tails -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x802399AC` | `tail_of_0x80239874` | cutscene render | code | frame `sp+0x1EC` (the `a2` output buffer writes 16 words through `func_800988A0`), `s0`, `fs0`/`fs1`/`fs3` | - | high |
| `0x80239AA4` | `tail_of_0x802399AC` |  | code | `s2` (the tail chain continues past the label splat chose) | - | high |
| `0x8023C824` | `tail_of_0x8023C804` |  | code | `a3` = `g_dl_cursor`, `v0` = the new cursor, `t0`/`t1` = the RDP words | - | high |
| `0x8023C894` | `tail_of_0x8023C824` |  | code | `a3`, `t0`, `v1` (same emitter contract) | - | high |
| `0x801AFC2C` | `tail_of_0x801AFAF4` |  | code | `s1`, `s3`, `s2`, `s7`, `s6` and `0x2C/0x46/0x4E($sp)` — the session-41 mis-binding | - | high |
| `0x80198D28` | `tail_of_0x801989AC` |  | code | `s7` and `0x74..0x88($sp)` — the scene-0x02 loader | - | high |
<!-- END GENERATED: addresses:tails -->

## Promoting a name

A rename is not free — check the blast radius first:

1. **Size overrides are keyed by name** (`config/config.toml`: 18 entries;
   `config/banks/config-bankC.toml`: 4). Renaming without updating them reintroduces the
   session-41 mis-binding bug through `find_containing_size_override`.
2. **`asm/` is committed.** A `splat split` after a rename rewrites every
   reference, which is a large but mechanical diff.
3. **Cross-bank seeds**: `python3 tools/cross_bank.py write-seeds` writes
   `build/bank<U>/symbol_addrs.txt` using the unit's name format; renaming a
   bank symbol changes those seeds too.
4. **No runtime-name collisions** (see the convention above).
5. Regenerate and re-verify: `tools/venv/bin/splat split config/config.yaml` → `make` →
   `make recomp && make bank-recomp` → rebuild both app variants, then re-run
   the battery in the newest handoff.

The cheap middle path (and what this file is for): keep the machine names, use
the proposed name in prose and new comments, and promote a symbol only when it
is high-confidence *and* used in three or more places.
