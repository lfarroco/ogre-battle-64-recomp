# Technical Decisions

This is the running log of technical decisions for the Ogre Battle 64 PC port.
Each entry records what was decided, why, and when. New entries go on top.

---

## Durable decisions (read this first)

This log is append-only and older entries are long; each one is a *session's*
reasoning. The decisions that still bind the port are these — the entry or
handoff holds the evidence.

| date (session) | decision | evidence |
|---|---|---|
| 2026-09-17 (67) | **The mission scene (`0x03`) renders, and the reason it did not was the same swappable-RAM class as sessions 45/55/59 — in two new shapes.** (1) The three records it streams (segment table 7/8/9, `0x101D00`/`0x145230`/`0x14EC00` → RAM `0x801AD5C0`/`0x801F4050`/`0x801FDA90`) were uncompiled: **unit N**. (2) Unit A's record 3 called `0x801AD6BC` — record 6's RAM — and both records being in unit A, N64Recomp bound it directly to unit A's record-6 body; scene `0x03`'s mask `0x38C` loads record 3 **without** record 6 (record 7 is resident), so the wrong bank ran and the process died on wild pointers that moved between runs. Record 6 moved to **unit O**, and the call compiles as `LOOKUP_FUNC`. (3) Record 9's arena streams **two more banks** (units **P** `0x171EC0` and **Q** `0x165FE0`, both → RAM `0x80214FA0`). **A `LOOKUP_FUNC` onto a non-entry is a silent no-op** (`get_function` has no interior fallback), and a cross-*record* `jal` leaves no trace while the target record is disassembled, so the entries are forced with `symbol_addrs_path` files (`symbol_addrs-bank{N,O,P,Q}.txt`) — a subsegment split at the same address would insert the assembler's 16-byte `.text` padding and break the ELF-vs-ROM identity | `config-bankN/O/P/Q.yaml`, `symbol_addrs-bank*.txt`; `docs/proofs/native-mission-scene.png`; `docs/HANDOFF-2026-09-17-session67.md` |
| 2026-09-17 (67b) | **`tools/cross_bank.py check-banks` was a silent no-op, and its rule needs the scene masks.** `parse_yaml_segments` matched `- name: bankRecX` with its item-line branch and dropped the name, so every record was `"?"` and `record_of(target) == record_of(caller)` was vacuously true — the session-45 invariant had been unchecked for as long as the configs have used the one-line form. Fixed, and refined: a same-unit cross-record call is only a hazard when **some scene's descriptor mask loads the caller's record without the target's** (1728 such calls, 23 genuine), and the genuine pre-existing unit-C backlog is an explicit allowlist (`tools/cross_bank_known_hazards.txt`) that still hard-fails on anything new | `tools/cross_bank.py`; `tools/cross_bank_known_hazards.txt`; `docs/HANDOFF-2026-09-17-session67.md` §6 |
| 2026-09-17 (66) | **OB64's save is 32 KiB of battery-backed SRAM (not a Controller Pak, not EEPROM/FlashRAM), and the *device* of a game-issued PI DMA is chosen by the `OSPiHandle` the `OSIoMesg` carries, never by the address.** `func_8008A040` builds the save handle with `baseAddress 0xA8000000` (physical `0x08000000`); the boot accessors `func_80074CF0..` read the whole image in 256-byte DMAs at offsets `0..0x7F00` and `func_80074BF0` → `func_80074C58` writes it back the same way (direction 1 = `OSIoMesg` type `0x10`; `0xF` = read). `entry.save_type = recomp::SaveType::Sram`, and `librecomp/src/pi.cpp`'s inline handler (`pi_perform_dma`) routes the window; mupen64plus's database agrees (`SaveType=SRAM`, with `Mempak=Yes` for the unrelated copy/backup device). Emulator files wrap the same logical bytes differently, so `tools/sramsave.py` imports them by magic at any offset/byte order (a parallel-n64 dump has the SRAM at `0x20800`, 32-bit byteswapped) | `app/src/main.cpp`; `librecomp/src/pi.cpp`; `tools/sramsave.py`; `docs/guides/app-build.md` → "Saves"; `docs/HANDOFF-2026-09-17-session66.md` |
| 2026-09-17 (64) | **A display list, or a decoded asset, that the port produced is evidence about the *port*, never about the game's intent — establish the port's faithfulness first, and treat a difference that survives as a *state* question.** The five checks (which GBI; was an RSP task swallowed; is the code the code we compiled; are the bytes what hardware would have; is the recompiler faithful at this `jal`) partition "the port differs" into recompiler / ucode dispatch / renderer / RDRAM contents / game state. Two session-61/62 mechanisms are **withdrawn**: N64Recomp does **not** execute a `jal` delay slot twice (the duplicate after `goto after_N` is dead code — the delay slot runs once, *before* the callee), and the sprite builder `func_ovlM_8019F83C` emits **no `G_SETTILESIZE` at all** (the word at `0x8019F990` is the TEXRECT's **s,t**), so there was never a "second window writer". The map's artist data is likewise **not** the defect: the LZ decoder is exact (13/13 assets end on their declared payload boundary), the port's `state[+0x04]` is byte-identical to an offline decode of asset `0x01DD210A`, and scene `0x05`'s ucode `0x8009F540` hashes to RT64's `F3DEX2.fifo 2.08` entry. **Do not patch generated C to make a picture look right** (rule 7) | `docs/guides/emulator-first.md`; `docs/HANDOFF-2026-09-17-session64.md` §1-§2 |
| 2026-09-16 (60) | **Every `jal` from the main ELF into RAM a scene module occupies must be dispatched (`LOOKUP_FUNC`), not left bound to overlay C's body — and a `jal` into a size-overridden body interior is emitted as "call the containing body + early `return`", which abandons the caller's frame.** That emission is not cosmetic: the early return dereferences the *caller's* epilogue, so each call leaks its frame and the caller reads its callee-saved registers from the wrong stack slots. The map scene was black because of two such sites in the scene update/hook; the frame-pump thread's `$s1` (the message-type comparator) was corrupted and the pump stopped, which presents as a frozen frame counter (`D_800AEFA4`) with the VI retrace (`D_800C4BCC`) still counting. `cross_bank.py dispatch` repairs the shape to call-and-continue; targets `0x8019AF0C` and `0x801A103C` are now in `make recomp`'s `--only` list | `docs/HANDOFF-2026-09-16-session60.md` §1-§3; `Makefile`; `tools/cross_bank.py` |
| 2026-09-16 (59) | **Every bank of a swappable RAM gets its own unit, and the unit whose code calls into that RAM defines none of them — the record-14 arena has a *third* bank (ROM `0x286BA0` → RAM `0x8022ACB0`, the chapter animation's module), now unit K, with `bankRec14a` moved to unit L; scene `0x05`'s module (ROM `0x79750` → RAM `0x8019A7C0`, the other bank of unit H's RAM) is unit M.** A bank's extent is its DMA's own size, not a previous session's chunk-rounded figure (`bankRec14a` `0xE860` not `0xDE60`; `bankRec07` `0x84B0` not `0x8600`), and the "is this DMA a module we know?" test must compare the rom→ram delta, not just the ROM range — the range test hid unit M behind unit H's over-claimed size | `docs/HANDOFF-2026-09-16-session59.md`; `config-bankK/L/M.yaml`; `app/src/bank_overlays.cpp` |
| 2026-09-16 (58) | **A save state is RDRAM *plus* the overlay state, and the two must be captured with the game threads parked.** `save`/`load` in the live console write/restore the whole 8 MiB image and the runtime's function map/bank records (`recomp::overlays::get_overlay_state_blob` / `restore_overlay_state_blob`). RDRAM alone is not a machine rewind: which recompiled body runs at each RAM address is host state, and a restore that leaves the map on a later bank runs the wrong module's bodies (the session-45/55 mis-binding class). The file I/O is wrapped in `ultramodern::checkpoint_pause_begin/end`, which parks every thread executing recompiled code at a function-entry boundary — the recompiled N64 threads are 1:1 native threads, and without the park the image **tears** (session 58: the checkpoint's own checksum did not match its bytes and its first `0x300` bytes were zeros while the game kept submitting RSP tasks during the write). A checkpoint is only valid in the process/binary that wrote it | `docs/guides/app-build.md` → "Checkpoints"; `docs/HANDOFF-2026-09-16-session58.md` §1 |
| 2026-09-16 (58c) | *(**Corrected in session 65**: the save is a battery-backed cartridge save; the Controller Pak is the copy/backup device — see the session-65 entry.)* **OB64's save hardware is the Controller Pak (`osPfs*`), and the port has no Controller Pak support at all — the runtime's `pak.cpp` is an upstream stub returning `PFS_ERR_NOPACK` for every entry point.** Evidence: 105 `jal`s from the main segment into the PFS cluster (`0x8009616C`..`0x80097DC0`) and the whole save menu as ROM text (`0x790EC..0x7967C`: `Controller Pak Menu`, `Save`/`Load`/`Erase`, `Insert Controller Pak.`, `1 note 25 pages to save.`, `Data saved to Controller Pak.`). The menu's strings are in **bank unit H** (`D_ovlH_801A260C`), the same UI module that draws the name/birthday forms, so the menu's *drawing* is likely already working and the missing piece is the device. `recomp::SaveType` (cartridge EEPROM/SRAM/FlashRAM) does not cover it, and `app/src/main.cpp` sets `SaveType::None` with a TODO. The game's own save system therefore needs a 32 KiB Controller Pak image (real PFS layout) + a real `osPfs*` implementation + the raw SI pak access, before the save scene is usable | `docs/HANDOFF-2026-09-16-session58.md` §3c; `tools/N64ModernRuntime/librecomp/src/pak.cpp`; `docs/scenes.md` row 9 |
| 2026-09-16 (58b) | **A streamed module that shares RAM with another record gets its own unit and the calls into it are `LOOKUP_FUNC` — scene `0x16`'s closing movie is bank unit I (ROM `0x244770` → RAM `0x801D0860`), and `bankRec10a` moved out of unit C to unit J because it is the *other bank* of that exact RAM.** `bankRec10a` could not stay in unit C (unit C's own code calls into that range — session 45's rule) and could not share unit I with `bankRec16` (overlapping records). Splat/N64Recomp hygiene this exposed: a unit's asm/assets must be cleared before `splat split` (splat never deletes a removed segment's output, and the ELF rule globs its inputs), `RecompiledFuncs/`/`Bank*Funcs/` must be cleared before regenerating (N64Recomp never deletes a previous run's file, so a symbol that changed section leaves a conflicting definition behind), a `data` subsegment needs an explicit following `bin` gap (splat extends it to the next segment otherwise), and `gen_bank_syms.py` must also see spimdisasm's `.Lovl<U>_<addr>` local-label references | `docs/HANDOFF-2026-09-16-session58.md` §2; `config-bankI.yaml`, `config-bankJ.yaml` |
| 2026-09-16 (57) | **The njpeg readback's source is the game's own framebuffer choice (`state[0x64]`), not RT64's scratch word.** RT64's `OGRE_NJPEG_SCRATCH` `+8` is only *re*set by a YUV-texture-image-then-colour-image pair and is **never cleared**, so at the first pass of an assembly it still names the previous step's framebuffer — which by then holds the previous screen (the name/date-of-birth form). That is the intermittent stale-backdrop rectangle, not a render-vs-readback race. `tools/njpeg_readback.py` now keeps `state[0x64]` whenever `D_800C4BB8` matches an entry of the framebuffer table `0x800A8204`, and uses the scratch word only as the session-48 fallback. Measured over 10 runs / 120 stage-3 passes: old rule wrong **13** (always pass 0), new rule wrong **0** | `docs/HANDOFF-2026-09-16-session57.md` §1; `tools/njpeg_readback.py` |
| 2026-09-16 (57b) | **A game-thread framebuffer readback is already ordered by the game's DP-completion wait, so no extra handshake is needed.** `sp_complete()` does run before `send_dl()` (`ultramodern/src/events.cpp:422`/`:429`), but the game waits on the **DP** event (`:432`, its registered queue is `0x800E8BF4`); delaying the display list 400 ms inside `send_dl` never let the readback run inside the delay, and an in-flight-gfx-task handshake implemented for the window never blocked once (reverted). `ogre_sync_framebuffers()` is what forces the RDP's pixels back to RDRAM before the copy | `docs/HANDOFF-2026-09-16-session57.md` §1, "Why the render-vs-readback race hypothesis is wrong" |
| 2026-09-16 (56) | **Guest RAM is queried from a *running* game, not only from a bounded run's exit dump.** A live console in the app (`ogre::console`, `app/src/sdl_platform.cpp`) executes read/search/checksum/dump commands on the **main thread** (where a multi-megabyte write cannot race the game thread), triggered either by a watched command file (`OGRE_CONSOLE_FILE`, default `/tmp/ogre-console.txt`; lines run, file removed) or by the number keys `1`..`9` (`OGRE_KEY_<n>`). This exists because every "what is in RAM at the moment X happens" question in this project was previously answered by an exit dump that had already been overwritten (session 56's backdrop readback). Byte order follows `tools/rdram.py` (logical word = LE word at `addr-0x80000000`, logical byte = `addr ^ 3`) | `docs/guides/app-build.md` → "The live console"; `docs/HANDOFF-2026-09-16-session56.md` §4 |
| 2026-09-16 (55) | **Scene `0x07`'s form module is streamed code with its own bank unit (H), and a call into a *streamed module's* RAM must be dispatched like any other swappable range.** The module is ROM `0x712A0` (`0x8600`) → RAM `0x8019A7C0`, chunk-DMA'd by `func_8017B794`, the enter of the real scene-`0x07` descriptor **`D_8018FDAC`** (mask `0x00000002`) — session 54's `D_8018FB98`/`func_80177F04`/`0x801A578C` reading was the wrong table entry. Its RAM overlaps record 3 (unit A) and overlay C (the main ELF), so build-time bindings ran overlay C's bodies: of the module's 24 internal `jal` targets, **0** were main-ELF function entries. The fix is session 45's model applied to a module that is not in the segment table: new unit H, and `make recomp`'s `--only` list carries the six calls into it. `tools/cross_bank.py`'s `overloaded()` also had to be fixed (`hi = min(c.hi)` → `max(c.hi)`; it hid the module's whole range behind record 3's earlier start). The form renders | `docs/HANDOFF-2026-09-16-session55.md`; `docs/proofs/native-newgame-name-entry.png` |
| 2026-09-15 (52) | **The YUV→RGB conversion must sign-extend `G_SETCONVERT`'s 9-bit fields, scale them `2*K+1`, and pair `K1` with U.** The hardware coefficients are `K = 2*sext9(k)+1` (GLideN64 `gDPSetConvert`: `SIGN(k,9)<<1 + 1`; parallel-rdp `set_convert`: `2*sext<9>(k)+1`), giving `351/−85/−177/445` for the game's `k0..k3 = 175/469/423/222`, and the rows are `R = Y + K0*V'`, `G = Y + K1*U' + K2*V'`, `B = Y + K3*U'` (`U'=U-128`, `V'=V-128`). Session 51 used the raw **unsigned** fields directly and paired `K1` with V, so the green row exploded positive and the cathedral backdrop drew as blue banding; the geometry, the de-interleaved TMEM planes and the `[U, Y(even), V, Y(odd)]` source were all verified correct, so **session 51 §7's "the defect is in sampling/upload" is wrong**. `K4`/`K5` as *combiner* inputs stay the raw fields over 255 (GLideN64 `_FIXED2FLOATCOLOR(k,8)`) | `docs/HANDOFF-2026-09-15-session52.md` §1-§3 |
| 2026-09-15 (51) | **The `0x800A5110` njpeg display list is an S2DEX2 list and its `0xDA` command is the draw.** The ucode is `S2DEX 2.08` (`GBIUCode::S2DEX2` — the game's ucode text/data hashes match RT64's `S2DEX2_FIFO_2_08` database entries exactly), so per macroblock `0xDC` = **`G_OBJ_MOVEMEM`** (`gSPObjSubMatrix`, the 8-byte `uObjSubMtx`) and `0xDA` = **`G_OBJ_RECTANGLE_R`** (`gSPObjRectangleR`, the 24-byte `uObjSprite`), not F3DEX2 `G_MOVEMEM`/`G_MTX`. RT64's `GBI_S2DEX2` mapped neither, so the geometry was skipped and only the texture loads ran — **session 50 §9's "the list carries no geometry" is wrong and is corrected here**. Both commands (plus `G_OBJ_SPRITE`/`G_OBJ_RECTANGLE` and the four `objLoadTx*` handlers that `assert(false)`'d) are implemented in RT64; a live trace shows 300 rectangles tiling the 320x240 `G_SETCIMG` target exactly. **A YUV tile must also be loaded as the RDP's two-plane format** — one luma byte per texel in TMEM's upper half, one U/V pair per two texels in the lower — so a YUV `LOADTILE` de-interleaves, the sampler follows parallel-rdp's `sample_texel_yuv16`, and YUV16 tiles require raw-TMEM sampling. The source word is `[U, Y(even), V, Y(odd)]`, correcting session 50 §2. Still open: the sampled colours (blue banding, not the backdrop) | `docs/HANDOFF-2026-09-15-session51.md` §1-§7 |
| 2026-09-15 (50) | **RT64's YUV16 decode is implemented from the code that writes the format, not from a guessed byte pairing.** In the 32-bit word at `4*(2t + (s>>1))`: `byte0 = Y(s even)`, `byte1 = V`, `byte2 = U`, `byte3 = Y(s odd)` (mupen64plus-rsp-hle `jpeg.c` `GetUYVY` / `jpeg_decode_OB`); luma is sampled from the upper TMEM half (`OR 0x800`) with the XOR-3/XOR-4 swap, chroma from the lower half, converted with the matrix **the game programs via `G_SETCONVERT`** (`k0=175 k1=469 k2=423 k3=222`). The old stub returned black for every YUV texel, which is why the framebuffer the game read back was black. The remaining wall is **render timing**: the port completes the emulated RSP task as soon as the display list reaches RT64, so the game's CPU copy can run before the draw renders; wait on the **RSP worker** (`Application::waitForGameFramebuffers`), never on the game thread (it deadlocks). A follow-up static trace fixed the readback's identity: the four `func_ovlE_8019976C` stage-3 copies **are** the njpeg readback (sole caller bankE `0x80199D80` inside `func_ovlE_80199D30`), they are the four sub-images of one `'B5'` asset (ROM `0x7CADAC`), and they run in scene `0x02` as a preload because `0x0D` streams bankRec2 over unit E's RAM. ~~byte0 = Y(even), byte1 = V, byte2 = U, byte3 = Y(odd)~~ and ~~the sampler design (luma at `| 0x800` with an XOR-3 swap, chroma via a half-word XOR)~~ are **corrected by session 51**: the word is `[U, Y(even), V, Y(odd)]`, and the RDP keeps luma one byte per texel at `offset + stride*t + s` in the upper half with U/V pairs in the lower | `docs/HANDOFF-2026-09-15-session50.md` §2-§5; corrections in `docs/HANDOFF-2026-09-15-session51.md` §6 |
| 2026-09-15 (49) | **The cathedral background is still black.** A YUV16 decoder was written for RT64's `TextureDecoder.hlsli` (which does return black for `G_IM_FMT_YUV`) and **reverted**: it produced a corrupt blob, not the backdrop, so the hypothesis is unproven. The game's CPU readback copies a zero framebuffer, upstream of any decode. Next: check GLideN64 — the emulator upstream closed [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102) with | entry below; `docs/HANDOFF-2026-09-15-session49.md` §2/§7 |
| 2026-09-15 (48) | ~~The njpeg readback copies the buffer its own YUV draw landed in~~, recorded by RT64 in an RDRAM scratch word; `tools/njpeg_readback.py` (run by `make bank-recomp`) patches the stage-3 copy source so it survives regeneration, and is a no-op on renderers that do not maintain the scratch word. **Corrected by session 57**: the *mechanism* (a regeneration-surviving patch) stands, but the scratch word is not a valid source selector — it is stale at the first pass of every assembly, and preferring it is what produced the intermittent stale-backdrop rectangle. The patch now keeps the game's own `state[0x64]` and uses the scratch word only as a fallback. ~~The cathedral background renders~~ — **superseded by session 49**: harmless, but the readback source was never the wall | entry below (with its banner); `docs/HANDOFF-2026-09-15-session48.md`; correction in `docs/HANDOFF-2026-09-16-session57.md` §1 |
| 2026-09-15 (48b) | The game's framebuffer table is at **guest `0x800A8204`** = `{0x80000400, 0x80025C00, 0x8004B400}` — **session 47 read `0x800B8204`, which is padding inside the data segment**. RT64 does draw the `0x800A5110` YUV macroblocks and does write framebuffers back to RDRAM; session 47's two candidate causes are disproved | `docs/HANDOFF-2026-09-15-session48.md` §1 |
| 2026-09-15 (47) | **RSPRecomp's `text_address` is a label base, not an address**: it must be the RSP/IMEM DMA address the microcode was *assembled* for (masked `0x1FFF`), e.g. `0x1080` for a ucode the boot loader loads at IMEM `0x080`; getting it wrong rotates every `j` target by the difference and silently walks the wrong blocks. The njpg decoder is therefore **on by default** (`OGRE_NJPEG=0` forces the stub) | entry below; `docs/HANDOFF-2026-09-15-session47.md` §1-2 |
| 2026-09-15 (46) | Non-gfx RSP microcode is recompiled with RSPRecomp (`make rsp-recomp` → `RspFuncs/`) and dispatched by ucode address in `app/src/rsp.cpp`; the gfx ucode still goes to RT64. ~~The M_NJPEGTASK decoder is **opt-in** (`OGRE_NJPEG=1`) until its output is correct~~ — **superseded by session 47**: the output is correct and it is on by default | entry below; `docs/HANDOFF-2026-09-15-session46.md` |
| 2026-09-15 (45) | A shared RAM range can hold **several records**: compile each one (units F/G) and keep them **out of the unit whose code calls into it**, so N64Recomp emits `LOOKUP_FUNC` and the game's DMA selects the resident module | entry below; `docs/HANDOFF-2026-09-15-session45.md` §2 |
| 2026-09-15 (44) | Mirror the N64's low-window (KUSEG) RDRAM alias in `recomp_mem_addr` (`a < 0x80000000` → `a & 0x003FFFFF`); keep it narrow (4 MiB) — **justification weakened by session 45** (it masked a mis-binding); A/B it | entry below (see its session-45 banner) |
| 2026-09-15 (44b) | Record hygiene: `DECISIONS.md` keeps a **durable-decisions table** at the top and a `> Superseded by …` banner on disproved entries; findings go in handoffs | `DECISIONS.md` top |
| 2026-09-15 (44b) | Watch tools: `tools/midfunc.py` (`make midfunc`) for shared tails, `tools/rdram.py` for dump reads, `make handoffs`; synthetic taps can be scoped to a scene (`OGRE_TAP_SCENE` / `OGRE_TAP_NOT_SCENE`) | `docs/HANDOFF-2026-09-15-session44.md` addendum |
| 2026-09-15 (43) | Records 17/18 live in **bank unit B** (they overlap every other unit's RAM); the movie-path word is **`0x80197794`**, not `0x8019F794` | entry below; handoff s43 |
| 2026-09-14 (38) | Zero a streamed record's **BSS** from the segment table on load; dispatch `0x801AFC2C`/`0x801980A0` in the default build | entries below |
| 2026-09-14 (37) | Repair the tail-call emission at dispatched sites; `revert` never restores a call to a same-address fragment | entries below |
| 2026-09-14 (36) | `OGRE_SCENE` pokes **early** (`OGRE_SCENE_AFTER_MS` defaults to 0); `kScenes` names the captured screens | entry below |
| 2026-09-12 (33) | Streamed bank records are recompiled as **separate units partitioned by RAM**, and the swap is driven by the game's PI DMA | entry below |
| 2026-09-12 (32) | `osViSetMode = 0x800955C0`, `0x80095820 = __osViSwapContext`, `osViBlack = 0x80095B30` | entry below |
| 2026-08-29 (10) | RT64 auto-detects F3DEX2 — no GBI override | entry below |
| 2026-08-25 (9) | KMC merges epilogues: N64Recomp emits **fall-through tail calls**; `make midfunc` now lists those tails and their register contracts | entry below, `tools/midfunc.py` |
| 2026-08-24/25 (2–6) | libultra is bridged **by name** (`symbol_addrs.txt` + the runtime's `osXxx_recomp`); the scheduler, PI-DMA and byte-order fixes are load-bearing | `docs/HANDOFF-2026-08-24*.md`, `-session6.md` |

Two house rules that this log learned the hard way: a **finding** belongs in the
session handoff, not here; and when a later session disproves an entry, add a
one-line `> Superseded by …` banner to it instead of deleting it.

---

## 2026-09-17 (session 67) — the mission scene renders: three record sets, one mis-binding, and a guard that was not checking

The developer's suspend save (`assets/save-mission-1.srm`, the third SRAM slot at
`0x30B0`) resumes at **scene `0x03`**, the mission (descriptor `0x8018F350`,
mask `0x38C` = records 2, 3, 7, 8, 9). Four separate things had to land, and the
third is the most transferable.

**Decision (1): compile the records the mission streams as their own bank unit.**
`config-bankN.yaml` holds records 7/8/9 (`0x101D00`/`0x145230`/`0x14EC00` → RAM
`0x801AD5C0`/`0x801F4050`/`0x801FDA90`); they are RAM-disjoint from each other and
overloaded by other units' records, and their bss ranges went into
`tools/gen_bank_funcs.py`'s `RAM_END` so the runtime zeroes them on load. Same
treatment as sessions 45/55/59. Without it the first call into record 8
(`0x801F8530`) hit the streamed stub and the run died in `do_send` on a wild
queue pointer.

**Decision (2): a unit may not hold two records a scene can load separately,
even when their RAM is disjoint.** Unit A held records 2, 3 and 6. Scene `0x03`
loads record 3 *without* record 6, and record 3 calls `0x801AD6BC`, which is
inside record 6's RAM. Because both records were in unit A, N64Recomp bound that
call directly to unit A's own record-6 body (`BankAFuncs/funcs_0.c:20637`,
`jal` at `0x8019F4AC`) instead of emitting `LOOKUP_FUNC`; with record 7 (unit N)
resident in that RAM the wrong bank ran, and the process died on wild pointers
that **moved between runs** (the crash-handler chain named it:
`func_8008AFE0 -> func_80072398 -> func_800765D8 -> func_ovlA_8019EE70 ->
func_ovlA_801AD6BC -> func_ovlA_801AEC60 -> func_8007A110`). Record 6 moved to
**unit O**; the call now dispatches and the runtime's DMA-driven bank map picks
the resident module (record 6/unit O in scene `0x0B`, record 7/unit N in the
mission). The session-45 rule is about RAM *ownership*, not only overlap.

**Decision (3): a `LOOKUP_FUNC` whose target is not a function entry is a silent
no-op, so cross-record entries must be declared.** `get_function`
(`librecomp/src/overlays.cpp:698`) returns `streamed_stub_generic` for any
unregistered address in the streamed range — it has **no fallback to the
containing function**. Spimdisasm analyses each record as its own segment, so a
`jal` that only exists in *another* record (or in another unit's caller) is never
seen while the target record is disassembled, and the address is emitted as a
label inside the preceding body. The fix is a `symbol_addrs_path` file per unit
(`symbol_addrs-bank{N,O,P,Q}.txt`, `name = 0xADDR; // type:func`). Forcing a
**subsegment split** at the same address does *not* work: the assembler pads each
`.text` subsegment to 16 bytes and these addresses are not 16-aligned, so the ELF
stops being byte-identical to the ROM (session 65's check: `bankO.elf: 24265
differing bytes`).

**Decision (4): an arena's banks are found from the port's own stub log, not from
the segment table.** Record 9's arena (`0x801AD5C0`-style reuse) has two more
banks the segment table does not describe — units **P** (ROM `0x171EC0`,
`0x6200`) and **Q** (ROM `0x165FE0`, `0xBEE0`), both → RAM `0x80214FA0`. The
`[overlays] streamed function stub called @ …` log named the addresses first
(they are past record 9's declared size, in its bss, with real MIPS at them), and
`OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1` named module P. The trace **misses
module Q** because its own overhead changes which path the run takes — a light
probe inside `recomp::do_rom_read` (destination in the arena, or source in
`0x160000..0x179000`) printed `rom=0x165FE0 ram=0x80214FA0` directly. When a
trace and a run disagree about what was loaded, suspect the trace's effect on the
run.

**Decision (5): fix `check-banks` and give it the scene masks.** The guard's YAML
parser matched `- name: bankRecX` with its item-line branch and dropped the name,
so every record was `"?"` and the same-record test was vacuously true — this
session's own wall (`unit A rec3 -> rec6`) was exactly what it exists to catch.
It now parses names, flags a same-unit cross-record call only when **some scene's
mask loads the caller without the target** (the masks are `SCENE_DESCRIPTOR_MASKS`
from `tools/scenemap.py scenes`; 1728 calls reduce to 23), and carries an explicit
allowlist of the pre-existing unit-C backlog
(`tools/cross_bank_known_hazards.txt`) so a *new* violation still stops
`make bank-recomp`.

Result: the mission enters, runs and draws —
`docs/proofs/native-mission-scene.png` (3D terrain, rivers, cliffs, the 2D party
sprite, the `Stronghold` tooltip) and
`docs/proofs/native-mission-unit-panel.png` (`No. / FRIENDLY / STATUS`,
`1. Magnus`, `STRONGHOLD / Zemio`, `START ^ FATIGUE`). No crash, no stub calls,
and `OGRE_SCENE=story`/`title` regression runs are clean.

---

## 2026-09-16 (session 60) — the map scene renders: two cross-bank calls were aborting the scene update/hook and killing the frame pump

**Decision (1): a `jal` from the main ELF into RAM a scene module occupies must
be dispatched, and the reason is not only "the wrong bank's body runs" — the
recompiler's binding for a `jal` into a *size-overridden body interior* is
`containing_body(rdram, ctx); recomp_trace_return(...); return;`, i.e. the call
**abandons the caller's frame**. On the hardware the callee returns to the
instruction after the delay slot; here the caller's epilogue never runs, so
every call leaks `$sp` by the caller's frame size and the caller's
callee-saved registers are read back from the wrong slots. In the map scene
(scene `0x05`) the two sites were the scene update's `jal 0x8019AF0C`
(`func_8017B858` @0x8017B8A0, leak 0x20) and the scene hook's `jal 0x801A103C`
(`func_8017B9C8` @0x8017BA10, leak 0x18). The frame-pump thread (t4,
`func_8008AFE0`) keeps its `$s0`/`$s1` on its own stack; `$s1` is the
message-type comparator `1`, so once it is corrupted the thread's
`msg->type == 1` test stops matching and **the pump stops after two frames**.
That is the "frozen black screen": `D_800AEFA4` (frame counter) stops while
`D_800C4BCC` (VI retrace) keeps counting, and only two display lists are
submitted. `cross_bank.py dispatch` already detects this shape (early return
plus the duplicated delay slot) and repairs it to call-and-continue; adding
`0x8019AF0C` and `0x801A103C` to `make recomp`'s `--only` list is the fix.
`0x8019AF0C` resolves to unit **M**'s state-1 handler — which is what the game
asks for, since scene `0x05`'s enter sets `0x801977E8 = 1` while scene `0x07`'s
sets `3` and takes the already-dispatched `0x8019B340`.
`docs/HANDOFF-2026-09-16-session60.md` §1-§3.

**Decision (2): the map scene is scene `0x05`, and it is where the Controller
Pak save becomes reachable** (developer, session 60: *"the next scene after this
prologue movie is an important one: it's the map scene. from there, it should be
possible to save the game"*). The port now draws it
(`docs/proofs/native-newgame-map-scene.png`); the save device itself is still
unimplemented (session 58 §3c).

**Diagnostic recorded:** the way to find a leaking call is to log the guest
`$sp` (`ctx->r29`) before and after each call in the chain — a leaking callee
shows as a lower `sp` after it returns, and the caller then reads garbage into
its callee-saved registers. Grep the generated C for
`recomp_trace_return(...); return;` followed by more statements in the same
function. `OGRE_PROFILE=1` alone shows only idle threads; it is the state words
(`D_800AEFA4`/`D_800C4BCC`) that identify the stall as "no frames produced".

**Experiment (not adopted):** `python3 tools/cross_bank.py dispatch` with no
`--only` clears the whole resolvable backlog (89 call sites, 7 extra targets,
including calls into overlay C itself from streamedB) and also makes the map
render; it is the eventual fix but changes many bindings at once, so only the
two targets this wall needed were added and the full opening was re-verified
instead.

---

## 2026-09-16 (session 59) — the record-14 arena has a *third* bank; scene `0x05`'s module; and the ROM end of a bank is its DMA's, not the segment's

**Decision (1): every bank of a streamed RAM gets its own unit, and the unit
whose code calls into that RAM defines none of them.** The New Game sequence's
chapter-animation step (1073, command `-8`) streams **ROM `0x286BA0` (0x138F0)
→ RAM `0x8022ACB0`**, a module the port had no code for. `bankRec14a` — the
*other* bank of that same RAM — was in unit C, so the record-14 interpreter's
`jal 0x8022C270`/`jal 0x8022C6E4` and the command-`-8` callback's
`jal 0x8022E3F0` were bound at build time to rec14a's bodies; when the chapter
module was resident the interpreter ran rec14a's layout, left the engine state
(`0x8022A970`/`0x8022A978`/`0x8022A994`) at **0**, and walked guest 0 until it
took an opcode with a wild argument — the developer's end-of-sequence `SIGBUS`.
The banks are now **unit K** (the chapter module, `bankRec14d`) and **unit L**
(`bankRec14a`), so unit C's calls compile as `LOOKUP_FUNC` and the runtime's
DMA-driven bank map picks the resident module. This is session 45's rule with a
*third* bank in the same arena, plus the third time a scene's next module was
found this way (sessions 45, 55, 58, 59). Evidence: at the crash
(`/tmp/s58-crash.bin`) RAM `0x8022ACB0` is byte-identical to ROM `0x286BA0`.
`docs/HANDOFF-2026-09-16-session59.md` §1; `config-bankK.yaml`,
`config-bankL.yaml`).

**Decision (2): a chunk-DMA bank's extent is the DMA's own, and a truncated
segment can be a *build* failure rather than a silent one.** `config-bankC.yaml`
ended `bankRec14a` at ROM `0x2A82F0` (size `0xDE60`), but the enter's `subu` at
0x80226A28 shows the game DMAs `0x2A8CF0 - 0x29A490 = 0xE860`. The missing
`0xA00` bytes (RAM `0x80238B10..0x80239510`) hold the module's jump tables, so
splat named `jtbl_ovlL_80239270` without emitting it and N64Recomp aborted with
`Failed to determine size of jump table at 0x80239270`. Read the size out of the
DMA call site, not out of a previous session's rounded chunk count (unit H
carried `0x8600` for the same reason; its real size is `0x84B0`, its enter's
`subu` at 0x8017B7E4). `docs/HANDOFF-2026-09-16-session59.md` §1b/§3.

**Decision (3): scene `0x05`'s module is unit M** (`bankRec05`, ROM `0x79750`
(0xDAD0) → RAM `0x8019A7C0`), the *other* bank of unit H's scene-`0x07` form
module. Scene `0x05`'s enter `func_8017B60C` DMAs it and calls `0x8019A7C0`;
before the unit existed that call hit the runtime's streamed stub and the scene
produced no frames. `docs/HANDOFF-2026-09-16-session59.md` §3.

**Decision (4): "is this DMA a module the port knows?" must compare the chunk's
rom→ram delta, not just the ROM range.** `app/src/bank_overlays.cpp`'s
`is_known_module` matched any record whose ROM range contained the DMA's source
and whose RAM base was the destination. Unit H's over-claimed `0x8600` range
therefore covered unit M's ROM `0x79750`, so the port believed it had the module
and never printed `[bank] UNKNOWN module` — the report that normally names the
next wall. It now requires `rom_offset - record.rom_start == ram_addr -
record.ram_start` (`same_stream_delta`). `docs/HANDOFF-2026-09-16-session59.md`
§3b.

**Correction to session 58 (recorded, not deleted).** The engine/interpreter
globals session 58 read at `0x8023A970`/`0x8023A978`/`0x8023A994` are at
**`0x8022A970`/`0x8022A978`/`0x8022A994`**: the generated code uses `lui
at,0x8023` with a negative immediate, and the addition wraps
(`0x80230000 + 0xFFFFA994 = 0x8022A994`). The `0x8023A9xx` reads were the
**DMA'd arena image**, not the state; the crash dump has the three words at 0.
Session 58's "engine init does not run on the faulting visit" and "the handler
dereferences a stale global" describe the same crash from the wrong address —
the mechanism is decision (1).

---

## 2026-09-16 (session 58) — checkpoints (save/load) and scene `0x16`'s streamed module

**Decision (1): the port gets save states, and a save state is the RDRAM image
plus the runtime's overlay state.** `save`/`load` in the live console
(`app/src/sdl_platform.cpp`) write and restore the whole 8 MiB image together
with the loaded-section/function-bank records
(`recomp::overlays::get_overlay_state_blob` / `restore_overlay_state_blob`,
`librecomp/src/overlays.cpp`). Function pointers are process-local, so the blob
stores extents and the function map is rebuilt from the records of the current
process; magic/version/size/fnv-1a reject a file from another build.

**Why RDRAM alone is not enough.** Which recompiled body runs at each RAM address
is `func_map`, host state. A restore that rewinds only the game's data leaves the
map pointing at a *later* bank — so the restored scene would run the wrong
module's bodies at those addresses, exactly the session-45/55 mis-binding class.
The checkpoint closes that hole by construction.

**Why the pause is required.** The recompiled N64 threads are 1:1 native threads,
so the console's main thread writing 8 MiB raced the game thread that was
mid-frame. The first attempts produced **torn images**: the stored checksum did
not match the file's own bytes, the first `0x300` bytes were zeros, and stderr
showed the game still submitting RSP tasks during the write.
`ultramodern::checkpoint_pause_begin()` sets a flag that every recompiled
function entry checks (the `recomp_trace_entry` hook N64Recomp already emits),
parks the executing threads for the duration, and `checkpoint_pause_end()`
releases them. Verified: a save's checksum now matches its bytes; a `load` 20 s
later rewinds scene/step and the game continues from there (a checkpoint at the
personality questions replays into scene `0x16`); a checkpoint from an earlier
process loads too. `OGRE_CONSOLE_AT_MS` was added so a scripted run can leave the
command file in place at launch instead of racing a background writer.

**Decision (2): scene `0x16`'s module is bank unit I, and `bankRec10a` is unit J.**
Scene `0x16` (descriptor `0x8018FC00`, mask `0x400`) chunk-DMAs ROM `0x244770`
(0x7500, 59 × 0x200) → RAM `0x801D0860` — record 10's arena. `config-bankC.yaml`
carried that ROM as a `bin` gap while unit C's `bankRec10a` owned the RAM, so the
three calls resident code makes into it (`0x801D40D0`, `0x801D410C`,
`0x801D62A0`) missed the function map and hit the streamed stub (session 57 §2).
Session 45's model applies: the module is its own unit, and the other *bank* of
the same RAM (`bankRec10a`) is another unit, because a unit cannot hold
overlapping records and unit C may not define a range its own code calls into.
The module's code/data split is splat's (`func_ovlI_801D7484` ends ROM
`0x24B3E0`; data to the module end `0x801D7D60`).

**Result.** Scene `0x16` loads, plays and **advances out of it** — repeated
`0x02 → 0x0D → 0x16` cycles at 4x with no stub lines, `20 streamed-overlay
record(s), 1975 function(s) armed`. It then reproduces the developer's
end-of-sequence crash: `SIGBUS`, `func_ovlC_8022C270 + 0x53A`, faulting guest
`0x7FFF43E8`, with `D_8018F1C0 = 0x0431` (step 1073, past the decoded 19-step
table).

**Build hygiene this exposed (all three are traps, not scene work).** `splat
split` does not delete a segment that moved to another unit, and the bank ELF
rule globs its inputs — so clearing `build/bank<U>/{asm,assets}` before the split
is required. N64Recomp likewise never deletes a previous run's output, so
`RecompiledFuncs/` and `Bank*Funcs/` are cleared before regenerating (a symbol
that changed section left a conflicting definition behind). And a `data`
subsegment needs an explicit following `bin` gap: splat extends it to the next
segment, which re-emitted record 10's friends as data. `tools/gen_bank_syms.py`
also had to learn spimdisasm's `.Lovl<U>_<addr>` local-label references.

---

## 2026-09-16 (session 57) — the stale New Game backdrop is a stale readback *source*; the sequence end streams an uncompiled module

**Decision (1): the njpeg stage-3 copy's source is the game's own framebuffer
choice.** `tools/njpeg_readback.py` now keeps `state[0x64]` whenever the word the
game derives its index from (`D_800C4BB8`) matches one of the three entries of
the framebuffer table at `0x800A8204`, and falls back to RT64's scratch word
(`0x807FFC08`, then `+12`) only when it does not.

**Why.** Session 56 concluded the intermittent "cathedral backdrop shows a
rectangle of the previous form" was a render-vs-readback timing race. It is not.
At the instruction level the game computes the source itself
(`func_ovlE_80199A08`: the index is built at `0x80199AF8`-`0x80199B68` and stored
into `state[0x64]` at `0x80199B98` as `D_800A8204[index]`; index 1 exactly when
the display word equals table[0], else 0 — i.e. the copy reads the buffer that is
*not* displayed). RT64's scratch word `+8` is only *re*set when a display list
sets a YUV texture image followed by a colour image, and is **never cleared**, so
at the **first pass of every assembly** it still names the previous step's
target — the buffer that by then holds the previous screen. Preferring it
unconditionally replaced a correct selection with the name-entry form or the
date-of-birth form.

Measured with a temporary per-pass probe (`OGRE_NJREAD_LOG=1`, kept as a
diagnostic) over 10 runs / 120 stage-3 passes, against the expected sub-image per
pass (the four sources are the deterministic frames `573FF47B8A5A3279`,
`48892A76C362A4BA`, `3F79B6732153B2FD`, `38BCBEBE5B555C3C` in order). Every stale
read is **pass 0** (destination `0x801AAE90`, 240x320); the developer reports the
broken on-screen tile as the **top-left** of the 2x2 backdrop, and pass 0 is the
only stale pass, so they are almost certainly the same chunk — inferred, not read
out of the blit, and the framebuffer's own chunk order is *not* the scene order
(`docs/guides/njpeg-backgrounds.md`). Confirmed end to
end with the **live console** (dump while the dispatcher reports scene-`0x0D` step
`0x021D8002`): `0x80243E28` is the name-entry form under the old rule (5/5 dumps)
and the cathedral with the fix (5/5) —
`docs/proofs/native-newgame-backdrop-old-rule.png` / `-backdrop-fixed.png`:

| rule | wrong source |
|---|---|
| scratch word first (the old patch) | **13** / 120, always pass 0, always the previous screen |
| game's `state[0x64]` first (this session) | **0** / 120 |

**Decision (2): no extra readback handshake.** `sp_complete()` does run before
`renderer_context->send_dl()` (`ultramodern/src/events.cpp:422` vs `:429`), but
the game does not act on that event — it waits for the **DP** completion, which
`:432` posts *after* `send_dl`. A game-thread "wait for the in-flight display
list" handshake was implemented, and it never blocked once, even with the njpeg
display list deliberately delayed 400 ms inside `send_dl`; with it disabled the
copy's content was still correct in 36/36 passes. It was reverted, and
`git -C tools/N64ModernRuntime diff` is byte-identical to
`n64modernruntime-ob64.patch`. `ogre_sync_framebuffers()` → `State::syncFramebuffers`
(waits for the submitted workload, then `copyLastNativeToRAM` on every framebuffer)
is what actually puts the RDP's pixels in RDRAM before the copy; the game issues
no further display list until the copy returns, so the buffer cannot be recycled
mid-copy.

**Decision (3): scene `0x16` ends at an uncompiled module, and that is the next
job.** The crash is not reproduced (200 s run, exit 0), but after `0x16` starts
the game spins — 17 439 calls each — on `0x801D40D0`/`0x801D410C`, which
`[bank] UNKNOWN module rom=0x244770 ram=0x801D0860` explains: scene `0x16`
chunk-DMAs ROM `0x244770` (≈`0x7500`) over the RAM `bankRec10a` owns, and
`config-bankC.yaml` carries that ROM range as a `bin` **gap**, so the port has no
code for it. Fix with the session-55 treatment: compile the record into a bank
unit other than `bankRec10a`'s, so the calls emit `LOOKUP_FUNC` and the DMA-driven
bank map picks the resident module.

---

## 2026-09-16 (session 55) — scene `0x07`'s streamed form module is bank unit H, and its calls are dispatched

**Decision.** Compile the code module scene `0x07` (the New Game name-entry
form, descriptor `D_8018FDAC`) streams as its own bank unit **H**:

    bankRec07  ROM 0x712A0 (0x8600) -> RAM 0x8019A7C0

and route the six calls from resident code into it through the runtime bank map
by adding them to `make recomp`'s `cross_bank.py dispatch --only` list
(`0x8019A7C0`, `0x8019A884`, `0x8019B060`, `0x8019B340`, `0x8019C4A8`,
`0x8019C69C`).

**Why.** Scene 0x07 entered but rendered black (session 54). Two corrections to
that session's diagnosis, both at instruction level:

* The descriptor is `D_8018FDAC`, **not** `D_8018FB98`. `func_80075BC0` builds
  the accessor table at startup (`0x80075C44`: `D_800AF028[7] = func_8017B600`),
  and `func_8017B600` is `lui $v0,0x8019 / jr $ra / addiu $v0,$v0,0xFDAC`. Its
  `+0x10` mask is `0x00000002`, and the live log agrees
  (`scene=0x0007 descriptor=0x8018FDAC mask=0x00000002`). `func_80177F04` and
  `0x801A578C` belong to the *other* table entry — session 54 matched an address
  against the wrong layout, which is this project's most common bug class.
* The module is not missing and is not zeroed: `func_8017B794` (the real enter)
  chunk-DMAs `0x8600` bytes, ROM `0x712A0` → RAM `0x8019A7C0` (67 × `0x200`
  chunks in `OGRE_DMA_TRACE`), and then calls `0x8019A884`, the first function
  after the module's 0xC4-byte header.

The port *did* have code at every module address — the main ELF's `.streamedC`
(overlay C / record 15) and record 3 (unit A) both map there. N64Recomp binds a
`jal` to a function it knows as a direct C call, so the resident code ran
overlay C's bodies at the module's addresses. Measured: the module's 24 internal
`jal` targets are **none** of them function entries in the main ELF (which has
67 different entries in the same range). No crash, no display lists: a black
screen.

**A tooling bug this exposed.** `tools/cross_bank.py`'s `overloaded()` computed
each region's ambiguous span as `hi = min(r.hi, min(c.hi for c in competitors))`.
Because a region's competitor list includes regions that start *before* it, the
span collapsed to the lowest competitor end: unit H's overlap was reported as
`0x8019A7C0..0x8019F450` (record 3's start of overlap) instead of
`0x8019A7C0..0x801A2DC0`, so `dispatch --only 0x8019A884` reported "matched no
call site" and the module's calls were invisible to both `report` and `check`.
The fix is `hi = min(r.hi, max(c.hi for c in competitors))`. The swappable
ranges go 5 → 6 and the main unit's entries in swappable RAM go 78 → 250: the
same addresses were always at risk; the model could not see them.

**Result.** The form renders — name box (`Magnus`), `A–Z`/`a–z` grid,
`◀ ▶ INS BS DEL END`, and the `Is the name Magnus acceptable? / Yes No` prompt —
and confirming it hands the opening on to `0x02`/`0x0D`. Proof:
`docs/proofs/native-newgame-name-entry.png`. The cathedral is unchanged
(re-verified in the same run).

**Corrects.** `docs/HANDOFF-2026-09-16-session54.md` §2-§5 (descriptor,
`func_80177F04`, and "the code is not resident").

---

## 2026-09-15 (session 52) — the YUV→RGB conversion is `2*sext9(k)+1` with `K1` on U, and that is what made the cathedral backdrop

**Decision.** In RT64's `sampleTMEMYUV16`, apply `G_SETCONVERT` the way the
hardware does: sign-extend each 9-bit `k0..k3` field, scale it `2*K+1`, and use

```
R = Y + K0*(V-128)/256
G = Y + (K1*(U-128) + K2*(V-128))/256
B = Y + K3*(U-128)/256
```

`K4`/`K5` as **combiner** inputs keep the raw fields over 255.

**Why.** Session 51 implemented the S2DEX2 geometry and the RDP two-plane YUV16
tile correctly, but drew the backdrop as blue horizontal banding. Three
independent references agree on the conversion and RT64 was doing neither half of
it:

* **GLideN64** (the plugin that fixed the upstream
  [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102)
  "Missing backgrounds in Ogre Battle 64 battles and also some cutscenes")
  `gDPSetConvert` stores `SIGN(k,9) << 1 + 1` for `k0..k3`
  (`src/gDP.cpp:959`), and `glsl_CombinerProgramBuilderAccurate.cpp`'s
  `YUV_Convert` does `icolor.rg -= 128` for `format == 1`, then
  `r = b + (P0*g + 128)/256`, `g = b + (P1*r + P2*g + 128)/256`,
  `b = b + (P3*r + 128)/256`.
* **parallel-rdp** `set_convert` is `constants.convert[i] = 2*sext<9>(k)+1`
  (`rdp_renderer.cpp:3521`), and `texture_convert_factors`
  (`shaders/texture.h:487`) is the same formula.
* The sampled YUV texel is `i16x4(u-0x80, v-0x80, luma, luma)` in both, i.e.
  `.r = U-128`, `.g = V-128`, `.b = Y` — so `K0` multiplies **V**, `K1` and `K2`
  multiply **U** and **V** respectively, and `K3` multiplies **U**.

Session 51's code passed the raw unsigned fields (`OGRE_CONVERT_TRACE=1`:
`k0=175 k1=469 k2=423 k3=222`) straight in: the "green" row became
`Y + 1.83·V' + 1.65·U'` instead of `Y − 0.33·U' − 0.69·V'`, and `K1` was paired
with `V`. The geometry, the loader's de-interleaved TMEM planes and the
`[U, Y(even), V, Y(odd)]` source were each verified correct with a temporary
loader probe, so the blue was the matrix, not the sampling.

**Result.** The backdrop is the cathedral (red carpet, stone walls, pillars,
statues, candles) on the RT64 build, and the assembled `'B5'` image at
`0x80243E28` renders as the full scene. `docs/proofs/native-newgame-cathedral.png`
and `docs/proofs/native-newgame-cathedral-background.png` are **replaced** — the
previous files were the black/blue-band broken renders.

**Corrects.** `docs/HANDOFF-2026-09-15-session51.md` §7 ("the remaining defect is
localised to the YUV sampling/upload step") and its "pure blue is impossible"
argument, which assumed the corrected coefficient scale.

---

## 2026-09-15 (session 51) — the njpeg display list *is* an S2DEX2 draw: implement the object commands, and the RDP's two-plane YUV16 tile

**Decision.** Stop treating the `0x800A5110` display list as an F3DEX2 list with
no geometry (session 50 §9). It is an **S2DEX2** list, and it contains one draw
per njpeg macroblock. Implement the missing S2DEX2 object commands in RT64, and
implement the RDP's YUV16 tile layout on both the load and the sample side.

**Why (the identification, hash-level).** The game's own ucode table at ROM
`0x3A2B0` pairs text `0x800A5110` with the data block at ROM `0x3D990`, whose
ASCII string is `"RSP Gfx ucode S2DEX       fifo 2.08  Yoshitaka Yasumoto"`.
`func_ovlE_80199588` submits that text as a 0x18C0-byte ucode with a 0x390-byte
data block. XXH3-64 (over the word byte-reversed bytes, which is what RT64 sees)
of both regions reproduces RT64's database entries
`S2DEX2_FIFO_2_08` (`0x9300F34F3B438634` / `0x50EF0DFBD3A8CD0F`) exactly, so RT64
*was* selecting `GBIUCode::S2DEX2` — the GBI selection was never wrong; the
S2DEX2 **command map** was incomplete.

The SDK's `gs2dex.h` (`F3DEX_GBI_2` branch) makes the two per-macroblock commands
unambiguous: `G_OBJ_MOVEMEM = 0xdc` (`gSPObjSubMatrix`, selector in the low 16
bits — `0xDC070002` → `2`) and `G_OBJ_RECTANGLE_R = 0xda`
(`gSPObjRectangleR`). DaedalusX64's HLE, the reference that runs this game's
backgrounds, reads the same selector (`cmd0 & 0xFFFF`) and special-cases
`imageFmt == G_IM_FMT_YUV` for Ogre Battle.

**Why (the YUV half).** Session 50's sampler read the luma plane from TMEM's
**upper** half while RT64's `LOADTILE` wrote the tile linearly into the lower
4 KiB, so luma was always zero; and parallel-rdp's `sample_texel_yuv16` shows the
RDP keeps one luma **byte** per texel in the upper half and one U/V pair per two
texels in the lower half, both at the tile's row stride — i.e. a YUV `LOADTILE`
de-interleaves. The decoder's source word, read live out of RDRAM, is
`[U, Y(even), V, Y(odd)]` (mupen64plus-rsp-hle `GetUYVY`), not session 50's
`[Y, V, U, Y]`.

**Result.** The geometry is right — a live trace shows 300 object rectangles
tiling the 320x240 colour image exactly, one per macroblock, with the sprite
matching the game's ROM constant, and RT64 now builds game framebuffers for
these lists (`[njwait] found=0` → `found=1`). The de-interleaved TMEM planes are
verified correct byte for byte. **The sampled colours are still wrong** (the
backdrop draws as blue banding), so the fix is incomplete; the defect is
localised to the YUV sampling/upload step and the ranked next checks are in the
handoff. `rt64-ob64.patch` was refreshed and verified against a clean worktree.

---

## 2026-09-15 (session 49) — the black cathedral background: a YUV16 decode was tried and reverted

**Decision.** No fix. The background is still black.

The njpeg backdrop is drawn as one 16x16 **YUV16** texture per macroblock
(`SETTIMG fmt=1 siz=2`, verified) and RT64's `TextureDecoder.hlsli` does return
`float4(0,0,0,1)` for `G_IM_FMT_YUV`, so "the renderer decodes it black" was a
reasonable hypothesis. A decoder (`sampleTMEMYUV16`) was written, and it is
**wrong**: an A/B against the pristine shader at the same forced-step timing shows
the frame becomes a corrupt magenta/green blob (1 259 distinct values, 64 439 of
76 800 near-black) instead of the backdrop, while the baseline is black (5
distinct, 76 314 near-black). **The decoder was reverted**; the RT64 shader is
back to its original state. The mid-session claim that the backdrop drew was based
on bright pixels in a present capture that are actually the dialogue box and the
sprites (RGBA16, always rendered) — not evidence.

**What this session establishes instead:** the game's CPU readback
(`func_ovlE_8019976C` stage-3, `0x80199884`) runs 4×, all in scene `0x02`, and
every copy reads a **zero** framebuffer; the blit's source `0x80243E28` is zeros
when sampled. That is **upstream of any decode**, so the renderer's YUV handling
was never proven to be the wall. Session 48's scratch-word patch and
`tools/njpeg_readback.py` are harmless but were not the wall either.

**Next lead (before another decoder).** [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102)
("Missing backgrounds in Ogre Battle 64 battles and also some cutscenes… a known
issue with many emus") was closed in 2012 by Bobby Smiles' **RSP-side**
`jpeg_decode_OB` — the half this port already has (sessions 46/47). If GLideN64
renders this backdrop correctly, its renderer path is the reference to diff
against RT64; and the readback/draw ordering question must be settled first. See
`docs/HANDOFF-2026-09-15-session49.md` §2, §4 and §7.

**Housekeeping (same session).** `rt64-ob64.patch` had drifted since session 47
(it was missing the `OGRE_NJPEG_SCRATCH` bridge and the session-48 diagnostics), so
the documented "apply the patch over a clean submodule" recipe would not reproduce
the current port. It was regenerated from `git -C tools/RT64 diff` (excluding the
`src/contrib/plume` submodule entry) and verified with `git apply --check`.

---

> **Superseded by session 49** (above): this entry's premise is wrong. The readback
> source was *not* the wall — both the game's own index and the njpeg target resolve
> to `0x000400` — and the shader's YUV16 decode is **not** the wall either (a
> decoder was tried, produced a corrupt blob, and was reverted). The background is
> still black. The patch below is harmless and still applied.

## 2026-09-15 (session 48) — the cathedral background renders: the njpeg readback was copying the wrong buffer

**Decision.** The New Game step-2 background (the New Game opening's cathedral
scene) is an N64 JPEG whose decoded YUV macroblocks the game renders into a
framebuffer and then **copies back with the CPU** (`func_ovlE_8019976C`'s stage-3
row loop, `0x80199884`: `memcpy(state[0x70], state[0x64], 2*width)` per row). That
copy's source, `state[0x64]`, is chosen by `func_ovlE_80199A08` from the game's
framebuffer table at **guest `0x800A8204` = `{0x80000400, 0x80025C00, 0x8004B400}`**,
using an index derived by comparing the display word `D_800C4BB8` against those
entries — and when it matches none (routine: the game also swaps the VI to
non-framebuffer targets, observed `0x80250800`) the index defaults to **0**, the
table's placeholder first entry. The port therefore copies a buffer the njpeg draw
never landed in, and the background comes out uniform (`0x0843`).

The fix makes the readback copy **the buffer its own draw landed in**, which RT64
knows (it sees the `G_SETCIMG` for the YUV macroblock draw):

* RT64 maintains a scratch area in RDRAM's last 64 KiB (`0x807FFC00`, verified
  untouched in a live dump): `+0x00` the current colour image, `+0x04` a "YUV
  texture image seen" handshake, `+0x08` the colour image set right after that
  handshake (the njpeg target), `+0x0C` the most recent of the three game
  framebuffers.
* `tools/njpeg_readback.py` rewrites the stage-3 copy source to that word (with
  fallbacks), and `make bank-recomp` runs it right after `gen_bank_funcs.py` so
  the window survives regeneration — the same model as `cross_bank.py dispatch`.
  On a renderer that does not maintain the scratch word (the null build) the patch
  is a no-op and the game's own value is used unchanged.

**Corrections to session 47** (which this session verified at data level):
the table is at `0x800A8204`, **not** `0x800B8204` (which is alignment padding
inside the data segment, and reads as code-looking bytes / zero); RT64 **does**
draw the `0x800A5110` YUV macroblock draws (colour-image histogram: each of the
three buffers ends a run with ~2 500 distinct 16-bit values); and the framebuffer
writeback **does** run (`OGRE_FB_WRITEBACK=1` fires for `0x025C00` with real
pixels). Session 47's two candidate causes — "RT64 drops the YUV draw" and "the
framebuffer never receives the draw" — are therefore both disproved.

**Why the fix is at this layer, not in the game's index selection.** The game's
choice is measured (a `-w` watchpoint on `0x800C4BB8` with `--value 0x80000400`
caught `func_8007307C ← func_80089540`, the VI-manager command handler), but
*why* the display word matches no table entry — and whether retail ever reaches
that state — is not established. Making the readback use the buffer its own draw
landed in is what the copy is for, and sidesteps the question.

**Evidence.** `docs/HANDOFF-2026-09-15-session48.md`;
`docs/proofs/native-newgame-cathedral-background.png` (the capture: Archbishop
Odiron over the cathedral background). The assembled image at `0x80243E28` goes
from 2 distinct 16-bit values (uniform `0x0843`) to ~2 500.

---

## 2026-09-15 (session 46) — non-gfx RSP microcode is now recompilable (`make rsp-recomp`); the cathedral background needs it, and the M_NJPEGTASK decode is opt-in until it is correct

**Decision.** The port runs **non-graphics RSP microcode through RSPRecomp**
instead of always stubbing it. `rsp-njpeg.toml` + `make rsp-recomp` regenerate
`RspFuncs/njpeg_ucode.cpp` (gitignored, like `RecompiledFuncs/`), which
`app/CMakeLists.txt` builds into `ogrebattle64_rsp`; `app/src/rsp.cpp`
dispatches by ucode address (`task->t.ucode == 0x8009ED80`, the game's Nintendo
JPEG decoder, `M_NJPEGTASK` = type 4) — behind **`OGRE_NJPEG=1`**.

**Why opt-in.** The recompiled microcode runs and returns
`RspExitReason::Broke`, but a pre/post RDRAM diff around the decoder shows it
writes only **one `0x300`-byte block** per task (not the stream its loop
structure implies). The game's image assembler then copies an empty resource,
the scene stops submitting display lists at the step-2 entry, and the null
build dies with SIGFPE in the runtime's `do_recv` (`osRecvMesg` with a
null-derived queue `0x80000010`, `msgCount = 0`). Enabling it by default would
regress step 2 from "renders with a black background" to "does not draw", so
the stub stays the default.

**What it establishes.** The cathedral background is **not** a renderer bug:
the background is a 320x240 pre-rendered image at guest `0x80243E28` (the
step-2 display list's first 48 `G_TRI2` quads), and the game decodes it through
four `M_NJPEGTASK` tasks (asset `0x00183352` = ROM `0x7175A2`, magic `'HU'` +
`'HUFF'`) that the port stubbed. Any future fix must either make the recompiled
microcode correct (find why it stops after one block; check whether the skipped
`ucode_boot` at `0x8009ECB0` matters) or reproduce the decode another way —
fabricating the image is not an option (AGENTS §7).

**RSPRecomp note.** The declared ucode region is `0x7C0` bytes, but its last 8
bytes are data (`0x0900060E` recompiles as `j 0x1838` to a nonexistent label);
`text_size = 0x7B8` ends after the final `break` + delay slot.

**Tooling decision (same session, after the developer asked whether we need
better tooling).** Session work must use `tools/runlog.py` (one-screen run
summary + `--check`), `tools/guestmap.py` (offline rom/vram/record/function-entry
lookup), `tools/rdram.py image`/`diff`, and `tools/watch.sh`, rather than
re-writing those loops per session; `AGENTS.md` §4 points at them and
`docs/guides/app-build.md` → "Diagnostics toolkit" documents them. The app, the
recompiled code and `librecomp`/`ultramodern` are built with
`-fno-omit-frame-pointer` (RT64 is not) so a fault inside runtime code reached
through a bridge unwinds to the guest caller; without it `bt` printed a single
frame.

Entry: `docs/HANDOFF-2026-09-15-session46.md`. Durable table row added.

> **Superseded by session 47** on two points: the decoder's "writes only one
> `0x300` block" was an **RSPRecomp label-base bug** (`text_address` must be the
> IMEM DMA address `0x1080`, not the RDRAM address), and the decoder is now
> **on by default** (`OGRE_NJPEG=0` forces the stub). Also: the task's input is
> produced by `func_8008B250` (not `func_ovlE_80197E5C`), and `0x80243E28` is the
> pixel field of an assembled image at `0x80243E10`, not the memcpy destination.

---

## 2026-09-15 (session 47) — RSPRecomp's `text_address` is a label base; the njpg decoder is correct (and on by default)

**Decision.** `rsp-njpeg.toml` sets `text_address = 0x1080` — the RSP **IMEM DMA
address** the microcode was assembled at, which is what RSPRecomp's
`text_address & 0x1FFF` label base must equal — and `app/src/rsp.cpp` runs the
decoder **by default** (`OGRE_NJPEG=0` forces the stub). `text_offset` (`0x2F180`)
is what selects the ROM bytes; `text_address` only names labels.

**Why.** The game's RSP boot loader (ROM `0x2F0C0`, vram `0x8009ECC0`) is
`addi $7,$0,0x1080` / `mtc0 $7,SP_MEM_ADDR` / `ld`s the `0xF80` bytes at
`task->ucode` / `jr $7`, so the text runs at **IMEM `0x080`**; the ucode's own
`j` targets are `0x1000`-based (`j 0x84001190`). With the previous value
(`0x8009ED80`, low 13 bits `0x0D80`) every target resolved **0x300 bytes late**:
the loop's output-pointer path was never reached and the decoder wrote a single
`0x300`-byte block per task, then diverged. With `0x1080` all `mbs` blocks are
written for all four images and the task returns `Broke`; the scene no longer
stalls, and the decode costs 0–1 ms per image (so default-on is free).

**What it also establishes.** The full step-2 background pipeline: `'HU'`
container → CPU Huffman decode `func_8008B250` → this microcode (YUV16 in place)
→ a **second gfx ucode `0x800A5110`** drawing one 16x16 YUV16 texture per
macroblock → CPU framebuffer readback → assembly of a `'B5'`-headed image at
`0x80243E10` whose pixels (at `0x80243E28`) the step-2 blit reads. The background
is still uniform because that readback source is uniform in the port — the next
wall, and it is a renderer/readback question, not a decode one.

Entry: `docs/HANDOFF-2026-09-15-session47.md`. Durable table row added.

---

## 2026-09-15 (session 45) — a shared RAM range can hold several records: compile every bank, and keep them out of the caller's unit

### Finding: the step-2 wall was a missing streamed module (rec14c)

Scene `0x0D` streams **two different modules** into record 14's arena at RAM
`0x802395E0`: `bankRec14b` (ROM `0x2AE390`, first visit — the movie) and
`bankRec14c` (ROM `0x2A8CF0` size `0x56A0`, steps ≥ 2). The port had only
rec14b (session 37, unit C), so N64Recomp bound unit C's **95 calls** from
records 14/14a into `0x802395E0+` as direct C calls to rec14b's bodies. The two
modules have different layouts at those addresses: `0x80239C24` is a body
interior of rec14b's `func_ovlC_80239874` (no prologue, restores `s0`-`s8` from
the caller's frame, `sp += 0x238`) but a real function in rec14c
(`addiu sp,sp,-0x60`). The step-descriptor interpreter's opcode-42 handler
(`jal 0x80239C24` at `0x80229004`) ran rec14b's interior with the interpreter's
0x78-byte frame, leaking `+0x238` of stack and clobbering `s3`/`s6` to 0; the
interpreter then walked **guest address 0** as its descriptor, read an
out-of-bounds word at `malloc_base + 0x40000` (null build 0; RT64 build
`0x00010001`), and the RT64 build entered a ~2.5 GB ROM DMA that overwrote the
PI globals (`D_800AA400`/`D_800AA408`) and produced session 44's `do_send`
SIGBUS. Evidence: an lldb watchpoint on `rdram + 0xAA400` (the writer is
`do_rom_read` in the runtime's PI lambda), the `func_80089F80` DMA log (the two
builds are identical for 668 transfers), interpreter-frame probes, and the live
RDRAM at `0x80239C24` matching ROM `0x2A9334` (rec14c) rather than rec14b's
`0x2AE9D4`.

### Decision: units F and G, and the "keep the bank out of the caller's unit" rule

* `bankRec14b` moves out of unit C into **unit F**; `bankRec14c` is compiled as
  **unit G**. Two records that occupy the same RAM cannot share an ELF, and
  keeping **both** out of unit C is what makes unit C's arena calls compile as
  `LOOKUP_FUNC(0xADDR)` — N64Recomp only emits a lookup when the target is not
  defined in the unit's ELF.
* The runtime needs no change: `load_function_bank` already calls
  `unload_overlapping_overlays`, so the game's DMA of rec14c drops rec14b's
  entries and `get_function` resolves to whichever bank is resident.
* The general rule this session adds to the bank model (session 33): a
  **swappable** RAM range must not be defined in the ELF of the code that calls
  into it. Otherwise the call is bound at build time to one bank's layout, and
  the other bank's callers run bodies with the wrong register/frame contract.
* `Makefile`: `BANK_UNITS := A B C D E F G`, and the bank-ELF rule now tolerates
  a unit with no `asm/data` or `assets` objects (the unmatched glob was passed
  to `as`/`ld` literally and aborted the build).

Superseded in part: session 44's read of the same crash (see the banner on that
entry). Full evidence: `docs/HANDOFF-2026-09-15-session45.md`.

### Decision: an unknown-module check is now part of the bring-up loop

The module was found by logging every PI DMA destination in a run and diffing
against `app/src/bank_funcs.inc`'s record table
(`grep -o "dev=0x… dram=0x… size=0x…" | sort -u`). That is cheap and should be
repeated for each new scene: a module-shaped DMA whose ROM range is not a record
means a bank the port has not compiled, and the port will silently run a stale
bank's bodies there.

---

### Decision: record hygiene — durable table, superseded banners, and watch tools

`DECISIONS.md` is append-only and averages ~75 lines per entry, so the decisions
that still bind the port were buried under session reasoning. It now opens with a
**durable-decisions table** (the ~15 rows that matter, each with a pointer), and
entries later sessions disproved carry a one-line `> Superseded by …` banner
instead of being deleted (the session-38 `0x8019F794` address and "command mode"
label, the session-34 "body interiors" blocker, the session-9 shared-epilogue
finding, the session-43 wall). Findings keep going in the handoffs.

Two docs join the record because the code cannot supply them:
`docs/symbols.md` (address → proposed name → evidence → confidence, the naming
convention, and the cost of a rename: size overrides are keyed by name, `asm/`
is committed, cross-bank seeds) and `docs/scenes.md` (what each screen should
show, from the developer — the AGENTS §1 oracle data, including the New Game
opening's five parts). `AGENTS.md` gains the mental model that unlocked this
session: uninitialised guest state is the game's own leftovers, so "the port has
0 where retail does not" means *check the address map and the runtime bridges*.

### Decision: scene-scoped synthetic input (`OGRE_TAP_SCENE` / `OGRE_TAP_NOT_SCENE`)

Wall-clock tap schedules drift with `OGRE_SPEED`, load times and capture readback;
two runs this session drove the attract loop instead of New Game because of it.
`automation_buttons()` now gates the press on the dispatcher's scene
(`D_800E810E`, `ogre::active_scene_id()`), with the list taking `kScenes` names or
hex. `OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game` presses Start through the
title and goes silent the moment New Game is confirmed — verified: `0x02` at
`t=10968 ms`, step 2 entered, exit 0, no `OGRE_TAP_MAX` tuning. Gate-out means
"no press this interval"; the schedule index still advances.

### Decision: `make midfunc` is the first stop for an unexplained `jal`

Session 44 lost hours to `jal` targets that are the *tail* of the function above
them (no `jr $ra` between them), so the callee inherits a frame and registers the
caller never set up. `tools/midfunc.py` finds them by "the previous function does
not end with a transfer", and reports each tail's `jal` sites plus the frame
slots, callee-saved registers and inherited caller-saved registers it reads
before writing. It names all 67 today, including both of this session's
(`0x802399AC` → `$fs3/$fs1/$fs0`; `0x8023C894` → `$a3/$t0/$v1`) and session 41's
`func_801AFC2C` (`0x2C/0x46/0x4E($sp)`) in about two seconds. AGENTS §4 now says
to run it first.

## 2026-09-15 (session 44) — step 2's crash is a shared-tail call, the step table is decoded, and an asset's LZ block starts at `rom+4`

> **Superseded in part by session 45.** The two low-address stores (guest `0`/`8`) were **not** the game's own zeros: they came from `jal`s into `bankRec14b`'s body interiors while the *other* bank of that arena (`bankRec14c`, ROM `0x2A8CF0`) was resident. With rec14c compiled and the calls dispatched (units F/G), the interpreter's frame stays intact, the stores do not happen, and RDRAM `0x0..0x40` is zero after a New Game run. The KUSEG mirror below therefore fixed a *symptom*; it is left in place but its justification is weak and it should be A/B'd. The step table, the `rom+4` LZ rule, and the command-handler decode all stand. See `docs/HANDOFF-2026-09-15-session45.md`.

### Finding: the New Game step table, decoded from the ROM

`func_80227E64(n)` resolves step `n` as: asset `0x19A8804` (ROM `0x1F3CA54`)
is a table whose payload (at `rom+4`) is a u32 array; entry `n` is the step's
asset id; the step asset's first word is its payload size and its LZ block
starts at `rom+4`; the **low byte of the descriptor's last word** is the
command opcode (`0xFF0000xx` → `jtbl_ovlC_8022ABE0[xx-1]`, ROM `0x286B60`).
Steps 1..19 resolve to `-7, -3, -10, -10, -10, -10, -10, -10, -3, -4, -3, -3,
-3, -10, -10, -10, -3, -3, -3`. The command handler table
`jtbl_ovlC_8022ABA0` (ROM `0x286B20`) sends `-3`/`-10` to the same handler
(`sel=2` + callback `80226110`) and only `-7` to `sel=0` (`80225A3C`), so
**every step after the movie takes `func_ovlC_8022D1CC`'s path B** — it is
normal code, not a debug branch. Full table:
`docs/HANDOFF-2026-09-15-session44.md` §1. Entry 0 of the table
(`0x019AA27C`) is unused: `n` is 1-based.

### Decision: `tools/ogrelz.py` gets an `--asset` mode and the `rom+4` rule

The decoder was previously pointed at `rom`, where every block begins
`00 00 xx xx` and decodes nothing. `func_8009DBB8` copies the payload (first
word = size) from `rom+4`, so the LZ block that `func_8007A110` reads starts
there. `ogrelz.py --asset <rom> <id>...` encodes both steps of the resolution
(`(id & 0x0FFFFFFF) + 0x594250`, then `+4`), so future sessions decode assets
without re-deriving it.

### Finding: step 2 fails on two shared-tail calls, both with unset state

`0x802399AC` is the tail of the function whose prologue is `0x80239874` (no
`jr $ra` between them; raw ROM verified). Its epilogue calls
`func_800988A0(a0 = sp+0x190, a1 = *(sp+0x1EC))`, which *writes* 16 words to
`a1`; `sp+0x1EC` is the slot the prologue fills from its `a2`. Path B reaches
it with the slot 0 (and every word `sp+0x1DC..sp+0x218` zero), so the port
faults writing guest address 0. Behind it, `func_80227030 → func_ovlC_802282D8`
(the descriptor interpreter) reaches its first opcode (`0x80000006`) and calls
`func_ovlC_8023C894` — another shared tail, a display-list emitter fragment
that needs `a3` = the DL write pointer — with `a3 = 0`, faulting at guest
address 8. A/B with one variable per run (skip path B; force the `a2` slot)
shows both are independent: fixing path B lands on the second crash.

### Finding: the zeros are the game's own, so retail must tolerate the stores

`a3` was traced through the crashing frame (probes, reverted): the queue `jalr`
leaves `0x800E7A30` (the copied slot), `func_80227FF8`'s descriptor relocation
leaks its loop tag `0x08880000` (`0x80228138`-`0x8022817C` — a real ABI leak,
never restored), the asset-load chain leaks `0xFFFFFFFF`, and
`func_802329D0` (malloc + bzero) leaves `0`. All of those are *game* functions,
so retail arrives at the display-list emitter with `a3 = 0` too, and the path-B
call with `*(sp+0x1EC) = 0`. Retail runs the opening, so a store to guest `8`
(and `0`) must be harmless there.

### Decision: restore the N64's low-window (KUSEG) RDRAM alias in `recomp_mem_addr`

The R4300i's KUSEG (`0x00000000-0x7FFFFFFF`) is TLB-mapped, and the boot ROM
maps the low window of RDRAM into it — the alias N64 emulators implement by
masking the top three bits. The game's own boot installs only two TLB entries
(`func_8009AAA0` at `0xC0000000`, `func_8009AB00` clearing 0..30), so the low
window is not the game's. The port's `recomp_mem_addr` computed
`a - 0x80000000` for a low address, faulting ~2 GiB below the RDRAM buffer.
`tools/N64ModernRuntime/N64Recomp/include/recomp.h` now returns
`a & 0x003FFFFF` for `a < 0x80000000`; the change is recorded in
`n64modernruntime-n64recomp.patch` (the submodule working tree is invisible to
git).

This is a hardware-fidelity fix argued from "retail runs the sequence and the
zeros are the game's own code" (AGENTS §8), not from a hardware watchpoint. It
is narrow (4 MiB) on purpose, and it means genuine null-pointer writes will now
land in RDRAM `0..0x3FFFFF` instead of faulting. Effect: the step-2 enter
completes — the null build runs the scene for 110 s with no crash (it died in
one frame before), the movie still renders, and the Tutorial/attract runs are
unchanged. The RT64 build then dies *in the runtime*
(`do_sendP + 0xC4`, guest `0xFE6E2C89`), which is the next wall.

### Decision: keep the crash handler's host-PC print

`app/src/bank_overlays.cpp`'s SIGSEGV handler now prints the faulting host PC
and `dladdr`'s symbol+offset. It is app code (not a probe in generated code),
it localised all three crashes (`func_800988A0 + 0x166`,
`func_ovlC_8023C894 + 0xCA`, `do_sendP + 0xC4`), and it costs one `fprintf` on
the fatal path.

---

## 2026-09-15 (session 43, correction) — the first `0x0D` visit IS the New Game movie; the wall is step 2

> Session 44: this wall is **fixed** (the writes landed in KUSEG; see the session-44 entry
> below). The wall after it is RT64-only: `do_sendP + 0xC4`, guest `0xFE6E2C89`.


### Correction, from a capture rather than from code

Session 43 first recorded "the developer confirmed the movie is first, so
`F1C0` should be 0 on the first `0x0D` visit". A capture of the *natural* first
visit (tap `Start` only until New Game is confirmed, then stop tapping) shows
that is wrong: with `F1C0 = 1` the port renders a multi-shot sepia cutscene in a
castle courtyard with the subtitle `I promise I'll make you proud.`
(`docs/proofs/native-newgame-cutscene.png`), for the full 28.7 s
(`0x0D` at `t=11063 ms` → `0x02` at `t=39780 ms`). Session 42's "command mode"
label is a misnomer: `F1C0 != 0` is the **cutscene engine**, driven by the step
descriptor table; the `F1C0 == 0` branch (records 10a/10b, vtable
`&D_801E5AC0`) is a different engine that no New Game step selects. The
developer's parenthetical in that question was the agent's own inference.

Consequence: the script VM's `D_8018F1C0 = var[0] = 1` is correct for step 1;
nothing in the VM needs repair, and the corrected word `0x80197794` /
`func_ovlC_801B7EBC` is not on the critical path until a later step selects the
other engine.

### The scene after the movie is step 2, and step 2 is the wall

The developer supplied the reference for the scene the opening should show
*after* the movie: a cathedral interior, dialogue box
`Archbishop Odiron` / `"He who has learned the way of` /
`the sword and god's teachings,`. The port never reaches it — visit 1 ends,
`0x02` reloads, and visit 2 (`0x0D` at `t=39901 ms`) dies in
`func_ovlC_8022D1CC`'s path B (`D_8018FC39 == 2` → `jal 0x802399AC`, session
42 §3). So session 42's frame-contract wall gates an identified scene, and
fixing it is the next session's single goal.

### Dialogue text is LZ-compressed

`Archbishop`, `learned`, `promise`: none appear as ASCII anywhere in the ROM.
The name table (ROM `0x64810`) and the attract-story text (ROM `0x100710`) are
uncompressed exceptions; cutscene/dialogue text lives in LZ blocks decoded by
`func_8007A110`. `tools/ogrelz.py` implements that format (5 tokens, read off
`0x8007A110..0x8007A7D4`); the asset base is `rom = (id & 0x0FFFFFFF) +
0x594250` (`func_8009DAF4`).

---

## 2026-09-15 (session 43) — bank unit B for records 17/18, scene 0x17 is the Tutorial, and two address/VM corrections

### Decision: one new bank unit for two RAM-disjoint records

Records 17 (`rom=0x069920`, RAM `0x80197B90`, size `0x4D60`, ram_end
`0x8019C950`) and 18 (`rom=0x1BA020`, RAM `0x80220F60`, size `0x92B0`, ram_end
`0x80230600`) are what scene `0x17` asks for (descriptor mask `0x00060000`).
They cannot join any existing unit: rec 17 overlaps records 0/1/2/15 (all at
`0x80197B90`) and rec 18 overlaps unit C's records 12/13/16. They are
RAM-disjoint from **each other**, and a unit only partitions RAM — the runtime
registers each record's functions from its own DMA (`is_function_bank_loaded` is
keyed by ROM start) — so one new unit (`B`, previously unused) holds both
rather than two units of one record each. `RAM_END` in `tools/gen_bank_funcs.py`
gained both ends from the segment table (ROM `0x387C0`).

### Correction: the movie-path word is `0x80197794`, not `0x8019F794`

Sessions 38, 39 and 42 all cite `0x8019F794` as the flag scene `0x0D`'s movie
path reads. The instruction is `lui $v0,0x8019` + `lw $v0,0x7794($v0)` at
`0x801B80B4`/`0x801B80B8` (`build/bankC/asm/1F0A00.s`), i.e. **`0x80197794`**;
`0x7794 = 30612` was mis-expanded to `0xF794`. It is a cached hand-off pointer
written by scene `0x02`'s record-0 code (`func_ovlE_801980A0`,
`func_ovlE_801988C8`), cleared by record 14a (`0x80234C58`, `0x80234DE8`) and
read by record 10's `func_ovlC_801B7EBC`. Any replay of the session-38/42
decompressor work must use the corrected address.

### Correction: the VM dispatch byte is `sp+0x60`; `sp+0x61` is the operand

`func_80170974` reads the opcode with `lb $v1,0x0($v0)` and the operand with
`lb $a0,0x1($v0)` at `0x801709D0`/`0x801709D4`, storing them to `sp+0x60` /
`sp+0x61`. Session 42's "opcode byte" probe logged `sp+0x61`, so its `op=0x00`
readings were operands. Also `jtbl_80190758` is indexed by `opcode - 1`
(`addiu $v1,$v0,-1` at `0x801709E4`), so index 0 is unused: **opcode 0x10** is
the `D_8018F1C0 = var[0]; D_8018F1C2 = 0x8002` handler at `0x80170AC0`.

### Decision: scene `0x17` is the Tutorial (verified), and the title-menu counter mapping

Scene `0x17` renders Deneb's `"Welcome! / Is this your first time here?"`
dialogue box. Driving the real title menu with `OGRE_TAP_BUTTON` (`Down` then
`Start`) enters it, so `func_80177A58` state 3 → scene `0x17` is the Tutorial.
The title menu capture is `New Game` / `Tutorial` / `Stereo`. With no save the
highlight bitmask has bits `{New Game, Tutorial}` (Stereo is left/right, not a
cursor entry) and `func_8019B8CC`'s `if (a0 == 2 && counter == 2) counter = 3`
shift maps Tutorial to counter 3; with a save the third cursor entry is
`Load Game`, so counter 2 → scene `0x12`. That makes **scene `0x12` = Load
Game** (unreachable without Controller Pak save state, which is why its forced
runs die on a near-NULL).

---

## 2026-09-14 (session 38) — two more cross-bank dispatches, record-BSS zeroing, and why the forced new-game path stops in scene 0x0D's init

> Session 43: the word this entry calls `0x8019F794` is really **`0x80197794`**
> (`lui`+`lw 0x7794`); session 44 reads the same path as the cutscene engine, not
> "command mode".


Session 37's `0x0D` wall moved twice. First, the forced run died right after
record 10a loaded in main-unit `func_801AFAF4` — the same bug class as session
37 (resident `jal 0x801AFC2C` bound to a containing overlay-C body while bank
unit C's record 10 owns the address). Dispatching `0x801AFC2C` (2 sites, same
tail-call repair) moved execution into real bank code, where it died one asset
later: `func_8007A110` (decompress) storing to N64 `0x0` after the allocator
legitimately refused a 1.2 GB request parsed from a wrongly-selected asset.
Probes (temporary, reverted) traced the wrong asset id (`0x148`) to scene
`0x0D`'s init reading flag `0x8019F794 == 0`, which scene `0x02`'s record-0
code never wrote: `0x02`'s loader entry runs once and never reaches its writer
branch. A natural-boot RDRAM dump shows the flag region (and the record-3
state the `0x02` entry branches on) is nonzero after the intro, i.e. the flag
is residue built by scenes the forced jump skips. The forced `0x0D` crash is
missing pre-state, not wrong code or wrong bytes (every transfer verified
byte-correct against the ROM). The natural path — title → menu → `0x02` →
`0x0D` — is blocked at menu `0x18`'s pre-existing unit-D crash, unchanged.

### Decision: dispatch `0x801AFC2C` and `0x801980A0` in the default build

`make recomp` now runs `dispatch --only 0x80198D28,0x801AFC2C,0x801980A0`
(idempotent, verified). `0x801980A0` is the same class again: two resident
call sites bound to the overlay-C fragment `func_801980A0` (no prologue) while
bank unit E (record 0, resident in scene `0x02`) owns the real entry
`func_ovlE_801980A0`; both sites are already call-and-continue, so no tail
repair. Regression battery (boot + five forced scenes exit 0, zero stubs;
menu still crashes at its known unit-D site) shows no new breakage.

### Decision: zero record BSS on streamed load, from the segment table

The hardware loader zeroes `[ram_start + rom_size, ram_end)` (BSS). The port
never did: `load_function_bank` only registers map entries while `do_rom_read`
copies just the ROM bytes, so game code read stale bytes as empty
lists/tables (scene `0x0D`'s list anchors live in record 10's BSS; record 14's
BSS holds the arena slots and the gap session 37 found). `ram_end` comes from
the game's segment table at ROM `0x387C0` via a static map in
`tools/gen_bank_funcs.py` (chunk-DMA records like 10a/10b/14a/14b default to
no BSS); the hook zeroes the span and evicts overlapping map entries, the
same as the ROM span. Verified: record-14 BSS reads back zero after load.

### Decision: crash handler reports the N64 fault address and dumps RDRAM

`on_fatal_signal` is now a `sigaction` handler: it prints the host fault, the
rdram base, and their difference, which reads back as the faulting N64
address for word accesses (half/byte accesses xor the low bits — see
`recomp_mem_addr`), plus per-thread last functions. `OGRE_DUMP_RDRAM=<path>`
also works on the crash path (same whole-image dump as the exit path), so a
crash's data pointers can be followed offline. These three lines located both
of this session's walls. See `docs/HANDOFF-2026-09-14-session38.md`.

## 2026-09-14 (session 37) — New Game plays into scene 0x0D: first targeted cross-bank dispatch, plus that scene's arena banks

The title → Start → New Game crash was the session-34 canonical case firing
for real: scene `0x02`'s update `func_80178920` does `jal 0x80198D28`, which
N64Recomp binds to the containing overlay-C body `func_801989AC` — whose
prologue reads `3($a0)` with `$a0` unset (NULL), i.e. a read of N64 address 3.
Bank unit E (record 0, resident in that scene) owns the real entry, so the
single call site is now dispatched through the bank map. The game reaches
scene `0x0D` with all of that scene's code resident (record 4 and record 14's
two arena modules are newly compiled into unit C) and dies there in list
management on a wild data pointer — the next wall.

### Decision: dispatch `0x80198D28` only, in the default build

`tools/cross_bank.py dispatch --only ADDR` rewrites just the named targets
(67 exist, 9 resolvable, 58 blocked — only this one is dispatched). An
`--only` target with no bank entry or no call site is a hard error; with no
bank data at all it warns and leaves the tree alone; re-runs are no-ops. The
`recomp` Makefile target applies it after every regen, because
`RecompiledFuncs/` is generated and a hand edit would be wiped. A lookup that
misses the map is a no-op stub, and the call site only executes in scene
`0x02`, so boot/attract/title are unaffected (verified exit 0).

### Decision: repair the tail-call emission at the dispatched site

N64Recomp emits a `jal` to a body-interior target as a call to the containing
body followed by an early `return`, abandoning the caller's epilogue (here
including the `D_800C4C26 = 0x800D` store). `dispatch` converts that shape to
the standard call-and-continue (`goto after_N` over the duplicated delay
slot) whenever the premature return plus the duplicate are present; a true
tail call at a function's end has no duplicate and keeps its `return`.

### Decision: `revert` never restores a call to a same-address fragment

Reverting an experimental dispatch invented a third binding at our site:
`func_80198D28` names a splat fragment (`and` first op), never the original
call. `revert` now restores only definitions that open like a function and
leaves fragment targets dispatched (`func_80198D28`, `func_801AB740`); `make
recomp` regenerates pristine C. This also protects the 58 blocked targets'
future dispatches.

### Decision: scene 0x0D's record 4 and record-14 arena go in unit C

Record 4 (all code, overlaps record 3's RAM so it cannot live in unit A) and
the two arena modules the PI-DMA trace showed scene `0x0D` streaming above
record 14 (`bankRec14a/b`) join unit C; five `function_sizes` in
`config-bankC.toml` extend splat-capped pieces past their switch/cut points
(N64Recomp redirects swallowed symbols into the extended body, so no map
entry is lost). Next session starts at the `0x0D` list-unlink wild pointer
(`func_80071950` via `func_800712C4` ← arena code), which reproduces with no
input, at 1× speed, with zero stub calls — and separately at the
`OGRE_NO_AUDIO=1` early-boot crash in the queue-snapshot diagnostic. See
`docs/HANDOFF-2026-09-14-session37.md`.

## 2026-09-14 (session 36) — scene jumps that work, and the title fog repair

Two reports: `docs/proofs/native-title-fog-isolation.png` is a crop of the
story/lore screen rather than the title, and testing needs a way to jump
straight into the title screen. The second report led into the first: with the
title reachable the fog could actually be measured, and the repair turned out to
be drawing nothing at all.

### Decision: `OGRE_SCENE` pokes early, and its default delay becomes 0

The scene dispatcher `func_80075BC0` copies the pending id `D_800E8214` into the
state block (`*(u16*)(*(u32*)D_800C4BBC + 4)`) at the top of every call and then
runs `block->id`. A poke is therefore only seen while the boot still owns the
block: once scene `0x09` is entered at ~1.1 s, the running scene's update
function re-establishes the block every frame and the write is discarded.
`OGRE_SCENE_AFTER_MS` was documented as 3000 with the claim that poking earlier
crashes the boot — but 3000 is *after* the window, so `OGRE_SCENE` had been a
silent no-op. The default is now **0**; the delayed behaviour is still available
for A/B runs by setting it explicitly. Both the block id and `D_800E8214` are
written, from the streamed-DMA hook and from a per-frame retry that stops as soon
as `D_800E810E` reports the target active.

The "poking before ~2 s crashes the boot" observation was the `OGRE_SCENE_LOG`
bug: `poll_scene` dereferenced `D_800E8294 + 0x10` unconditionally, and before a
scene exists that word holds a stale non-KSEG0 value, so the read went off the
end of RDRAM (SIGBUS at ~1 s). The descriptor is now only dereferenced inside
KSEG0/RDRAM.

### Decision: the scene names in `kScenes` are its captured screens

Each id was forced with `OGRE_SCENE=<hex> OGRE_SCENE_AFTER_MS=0` and captured:
`0x04` title (logo + PRESS START, after its prologue text), `0x09` boot intro,
`0x0A` publisher screens, `0x0B` world-map story, `0x0C` the unit-description
book, `0x18`/`0x02` still SIGSEGV (cross-bank work). Session 35's
`title = 0x0C` was the book, not the title.

### Fix: the `OGRE_FOG` repair wrote tile extents in the wrong units

`G_SETTILESIZE` and `G_LOADTILE` carry `uls/ult/lrs/lrt` in **quarter-texel**
units, not texels — the game's own tiles use `lrs = (width - 1) * 4` (320 texels
→ `lrs = 1276`). The repair wrote `((width - 1) << 12) | (height - 1)`, so a
64x64 layer image was declared as a ~16x16 tile; the six logo-band rectangles
sample `s = 63..0`, outside that tile, and RT64 clamped them onto a transparent
corner. The pass rewrote the display list and then drew nothing, which is why
`OGRE_FOG=1` and `OGRE_FOG=0` frames were byte-identical. `OGRE_RECT_STATE`
showed the wrong tile (`lrs=63 lrt=63`) and session 35 read it as correct.

### Fix: every white group is repaired, not just the first

The walker cleared `fog_white` after the first white group, on the theory that
later white groups are cloud wipes that do not fit the layer image. But the title
has two: the layer wrap behind the logo (`s = 63..0`) and the **bottom sweep** —
64 one-screen-row-tall rectangles at N64 rows 176-239 with `s = 288..1920`,
`t = 0..2016`, the `©1999 QUEST` fog the report was about. The sweep inherits the
cloud loop's zero-area tail tile (row 159 of the *cloud* texture); the old
one-texel fallback let RT64 read that dead row, which draws nothing. Both groups'
scroll fits a 64x64 layer image, so the repair now runs for every white group
whose scroll fits, and only falls back to a one-row tile when it does not.

Measured on the title, fog on vs off at the same present (runs verified in
lockstep outside the fog bands):

| region | before | after |
|---|---|---|
| logo band (N64 rows 64-128) | `0.00` | mean 4.6, max 41 |
| `©1999 QUEST` band (rows 176-216) | `0.00` | mean 3.1, max 37 |

The `©1999 QUEST` band now also *moves* (consecutive title frames differ by mean
1.0-2.0, max 24-36), matching the emulator captures above the copyright line.

The sweep is repaired to the layer image rather than the 320-wide cloud texture
its `SETTIMG` names: a 320x64 RGBA32 texture does not fit in 4 KB of TMEM, so the
original per-strip uploads cannot be replayed verbatim. The result is the right
kind of moving haze in the right place; the exact wisp shape is the layer
image's. Synthesising a per-rectangle `LOADTILE` for the cloud-texture row each
sweep rectangle names would close that gap.

### Fix: the sweep blinked because its scroll ran off the image

The first version of the repair refused any group whose first rectangle's `s`
passed the image width and fell back to the dead one-row tile. The sweep walks
`s` from 0 to ~118 texels (it is written against a 320-wide cloud texture), so
every time `s` crossed 64 the `©1999 QUEST` fog disappeared for a few frames.
`OGRE_FOG_SUMMARY=1` shows the sweep group reporting `used=0x00000000` on exactly
those frames. The draw tile now **wraps** (`masks`/`maskt` = log2 of the image
size, 6 for 64x64) and only a negative coordinate falls back. After the fix the
band is flat to within 0.84/255 across the title (it alternated 63.8/67.5
before).

### Decision: the fog's level is the game's own asset (no port constant)

The opacity is not a number the port picks. The combiner the game uses for every
white fog group is `RGB = ONE, ALPHA = TEXEL0`, alpha compare is off and the
blend colour alpha is 0 (`othermode_l = 0x00504240`), so the overlay is white
modulated by the sampled texel's alpha — and for the layer table's 64x64
`G_IM_FMT_I` images that alpha is the image's intensity (mean 12.9/255 = 5.1%,
peak 52/255 = 20.4%). `OGRE_FOG_SCALE=100` therefore means "the asset as
authored" and is the default; the knob only stages a scaled copy in scratch RDRAM
for A/B runs.

That default is checked against a retail emulator capture of the lower half of
the screen (just after the fade-in): fitting the emulator's brightness offset on
the fog-free cloud band (N64 rows 129-175, which reproduces zero fog to ±0.02)
and measuring the fog's contribution in the clean cloud region below the
copyright (rows 214-236) gives retail **+8.1**, a 45% build **+3.2** and the
100% build **+7.2**. Six phase-matched presents put the 45% build at 112-116% of
the needed value, i.e. the raw asset. The 45% it briefly defaulted to was a
port-side over-correction from comparing overall brightness rather than the
fog's contribution; it is gone, and `OGRE_FOG_SCALE=45` remains as a taste
option.

### Proofs

* `docs/proofs/native-title-fog-isolation.png`: the title logo band with the fog
  off, on, and the difference ×4.
* `docs/proofs/native-title-fog-quest-region.png`: the `©1999 QUEST` sweep, same
  treatment.
* `docs/proofs/native-title-band-isolation.png`: the zero-texel band fix,
  pre-fix white band / fixed / difference ×4.

---

## 2026-09-14 (session 35) — the title screen's fog: a zero-area texture tile

The retail title screen has clouds scrolling behind the "Ogre Battle 64" logo,
with a **very faint white fog** moving over them. The port drew a solid **white
band across rows 64-128** instead. The guess on the report was a 3D/2D
compositing or shader problem. It was neither.

### Finding: the opaque band and the missing fog are the same six rectangles

`OGRE_DL_DECODE=<n>` of the title scene's display list shows, right after the
layer-2 cloud strip loop, six left-going rectangles:

```
TEXRECT ulx=0    uly=256 lrx=24   lry=512 tile=0 s=160  t=0 dsdx=-1024 dtdy=1024
TEXRECT ulx=24   uly=256 lrx=280  lry=512 tile=0 s=2016 t=0 dsdx=-1024 dtdy=1024
...                                         (six pieces tiling the full width)
```

with the combiner `FCFFFFFF FFFF73B9` immediately before them. Decoding that
combiner with the N64 `GCCc0w0`/`GCCc0w1` packing (`include/PR/gbi.h`) gives
`RGB = (0 - 0) * 0 + ONE` and `ALPHA = (0 - 0) * 0 + TEXEL0`, i.e. **a white
overlay modulated by the sampled texel's alpha** — the fog. The tile those
rectangles name is the one the layer-2 loop left behind:
`SETTILESIZE t0 uls=0 ult=420 lrs=1276 lrt=420` — `lrt == ult`, so the tile
covers **zero texels**.

Bisecting with the new `OGRE_NOP_RECT=flip` diagnostic (which NOPs matching
`G_TEXRECT`s in the submitted display list) removed exactly the band and left the
rest of the frame bit-identical, which is how the six rectangles were found
without guessing at the renderer.

### Why RT64 painted white instead of fog

`State::loadDrawState` computes the sampling rectangle as
`sampleHeight = max((lrt - ult + 4) / 4, 1)`. The `max(..., 1)` is necessary (an
empty texture cannot be decoded or sampled), but for a zero-height tile it
*invents* a one-texel-tall image out of whatever TMEM holds. The `LOADTILE` that
ran just before loaded texture row 105 of `0x801E2918`, and rows 60-105 of that
texture are fully opaque (alpha 255 once the RDRAM is read with the runtime's
`^3` byte order), so `RGB = ONE`, `ALPHA = 255` painted an opaque white band.

### The fog's image: the layer's own, from the game's layer table

OB64 draws each scrolling layer as a wrapped "layer image": the six rectangles
are the layer's horizontally scrolling wrap (a 64x64 image, mirror-sampled 1:1,
repeated across the width). `func_8019C5D4(0)` is the wrap pass for layer 0 of
the game's *effect* layer table, and it intends to upload that layer's image
before drawing — the upload is skipped in the display lists the port sees, so
the rectangle inherits the stale tile instead.

The image is reachable: the game keeps its layer table at `D_801B80E0`;
`+0xFC + i*0x24` is layer `i`'s record, whose first word points at an image
record (`+8` = the data, then height, then width, then the siz/fmt code bytes).
For the title screen layer 0 that is **`0x801D2F88`, a 64x64 8-bit intensity
image** whose mean intensity is 0.051 — "very faint", matching the retail
measurement. Layer 2 is the same kind of image at `0x801D51E8`.

### Decision: repair the texture setup the rectangle inherits

`app/src/renderer.cpp` gains an `OGRE_FOG` pass (on by default) that runs on the
game's display list before RT64 parses it. It walks the list with the proven
`OGRE_NOP_RECT` walker, and whenever a `G_TEXRECT` is about to sample a tile
that covers no texels while the *white* combiner is active, it rewrites the last
`SETTIMG` / `SETTILE t7` / `LOADTILE` / `SETTILE t0` / `SETTILESIZE t0` so the
rectangle samples the layer's own image (found through the layer table). The
rectangle then samples the 64x64 fog image, mirrored and tiled, exactly as
intended. `OGRE_FOG=0` restores the previous behaviour and `OGRE_FOG_TRACE=1`
reports each repair.

RT64 keeps its own guard: `RDP::drawTexRect` skips a rectangle whose tile covers
no texels (a one-texel tile is `lrs == uls + 4`, so this cannot catch a
legitimate single-texel tile; a tile with `line == 0` keeps RT64's existing
"no texture" handling). That guard is what removed the opaque band, and it still
catches degenerate rectangles the repair does not recognise.
`app/src/web_renderer.cpp`'s `draw_texrect` has the same guard.

### Booting straight into a screen

`OGRE_SCENE=<name|hex>` replaces the hex-only `OGRE_FORCE_SCENE`: it pokes the
game's scene id (`u16(D_800C4BBC + 4)`, the word the dispatcher at
`asm/1060.s @0x80075DB8` runs the per-scene update function from) from the
streamed-DMA hook until the scene is entered. Names are in `kScenes`
(`title` = 0x0C, `menu` = 0x18, `new-game` = 0x02) and `OGRE_SCENE_LOG=1` logs
every scene change with its descriptor and record mask, which is how new names
are found. Poking *every* frame corrupts the scene state machine (the game
rewrites the id itself), so the poke stays on the scene-load path.

### Diagnostics added (all env-gated)

| knob | where | what it does |
|---|---|---|
| `OGRE_NOP_RECT=<selectors>` | app | NOPs matching `G_TEXRECT`s in the submitted DL before RT64 sees them (`any`, `tex:<hex>`, `flip`, `x:a-b`, `y:a-b`, `n:<i>`) — "which draw is this?" bisection |
| `OGRE_FOG` / `OGRE_FOG_TRACE` | app | the fog repair above |
| `OGRE_SCENE` / `OGRE_SCENE_AFTER_MS` / `OGRE_SCENE_LOG` | app | boot into a named scene / log scene changes |
| `OGRE_RECT_STATE=<y0>-<y1>` | RT64 | per-rectangle cycle type, both combiner cycles, blender inputs, prim colour and tile descriptor, decoded by RT64 itself |
| `OGRE_TILE_TRACE=1` | RT64 | every tile whose sampling rectangle is degenerate, with the texture the cache returned |
| `OGRE_DUMP_TEX=<dir>` | RT64 | writes the 4 KiB `.tmem` and `.tile.json` of every decoded texture |

### Note: the checked-in RT64 patch was stale

Regenerating `rt64-ob64.patch` picked up `OGRE_CAPTURE_EVERY` (added in session
33/34 and documented in `guides/app-build.md`) which the patch file did not
carry. The patch is now current with the working tree.

---

## 2026-09-12 (session 34, addendum) — the missing intro smoke and logo characters

Reported after the cross-bank work: the opening soldiers' attack had no "smoke",
and the 3D characters in the "Ogre Battle" logo did not show. Both were the same
bug, and neither was caused by the bank work — they are session 31/32 leftovers.

### Finding: the main unit's linker script was stale, so overlay C was never loaded

`build/ogrebattle64.elf`'s section table placed overlay C at ROM `0x0E4910`
instead of `0x1CE040`. `0xE4910` is bank record 2's ROM start, so the script still
described the abandoned "bank records inside the main unit" layout. As a result
`load_overlays(0x1CE040, 0x80197B90, 0x22A00)` matched **zero** sections: overlay
C's function-map entries never existed at all, and the game's lookups into its
tail returned the generic stub. In a 45 s run that is 5140 stub calls from exactly
two addresses — `func_8019E588` (the smoke puffs, identified in session 29) and
`func_801A34FC` (the logo's 3D characters).

`make` never re-runs `splat split` for the main unit, so nothing regenerated the
script: `ogrebattle64.ld` and `config.yaml` had identical timestamps.

**Fix:** remove the generated `ogrebattle64.ld` and the stale `assets/*.bin`,
re-run `tools/venv/bin/splat split config.yaml`, then rebuild. The map now shows
`streamedC` at `0x1CE040` and `load_overlays` matches 1 section.

**Lesson for the build:** any change to `config.yaml`'s segments needs an explicit
re-split; `make` alone will silently link against the previous layout.

### Decision: a partial bank swap must not drop the evicted overlay's whole map

`recomp::overlays::unload_overlapping_overlays` erased **every** function-map
entry of a section it evicted. A bank record is usually much narrower than the
overlay it replaces (record 2 is `0x72C0` bytes of overlay C's `0x229C0`) and the
RAM above it is not overwritten, so the overlay's code there still runs. The
over-broad erase is what made the effects vanish *after* the first bank swap (and
what made the cross-bank target space look far larger than it is).

**Fix:** erase only the entries the incoming load actually covers, for both real
sections and registered function banks. `n64modernruntime-ob64.patch` regenerated.

### Outcome

| check | before | after |
|---|---|---|
| intro stubs, 45 s | 5140 | **0** |
| `load_overlays` matches for overlay C | 0 | 1 |
| `OGRE_SPEED=4` 120 s attract run | — | exit 0, 0 stubs, 0 crashes, 9000+ display lists |

---

## 2026-09-12 (session 34) — cross-bank `jal` routing: mechanism built, blocked on the recompiler

Session 33 left one task: route the main unit's cross-bank calls through
`get_function` and seed each bank unit with the cross-bank entry points, because
N64Recomp binds a `jal` to a known function as a *direct C call*. This session
built the mechanism (`tools/cross_bank.py`), fixed two real defects in the bank
build, and found why the routing cannot ship yet. **The default build keeps
session 33's behaviour**; the routing is an opt-in experiment
(`make cross-bank-dispatch`). The attract loop is unchanged and scene
`0x18`/`0x02` is still open.

### Finding: the bank build silently linked stale objects

`build/bank<U>.elf` used to depend only on the generated `build/bank<U>.ld`, and
its recipe discovered the `.o` files by **globbing**. `splat` rewrites an `.s`
file whenever it re-splits but only rewrites the `.ld` when the *layout* changes,
so a re-split that changed the code (exactly what added symbols do) left `make`
thinking the ELF was up to date: the change was compiled into `.s`, never
assembled, and the ELF kept the old code. Two session-34 experiments reported
"seeds applied" while the ELF contained none of them.

**Fix (kept):** the ELF now depends on the config (not the `.ld`), with
`build/bank%.ld` order-only, so any config or source change re-assembles and
re-links every unit.

### Finding: the dispatch address must be the `jal` source address, not the callee

Turning a cross-bank `jal` into `LOOKUP_FUNC(<callee address>)` is wrong for a
**size-overridden** function: N64Recomp emits `jal 0x80198D28` (an address inside
`func_801989AC`) as a call to `func_801989AC`, deliberately, so the continuation
the caller meant to reach runs. Rewriting it to `LOOKUP_FUNC(0x801989AC)` broke
overlay B's menu path. The generated C's `// 0xADDR: jal 0xTARGET` comment is the
authoritative source address; the rewrite keys on it.

### Finding: a call is only cross-bank when the caller and callee can differ

Two addresses inside the same *overlapped group* (the intersection of two regions
that can occupy the same RAM) belong to the same resident bank, because a bank's
records are loaded and evicted as one. The first version used the *merged*
swappable spans, which made a record-3 caller and a record-2 callee look like a
cross-bank call and mis-rewrote ~320 harmless call sites.

### Finding (the blocker): most cross-bank targets are body interiors in the bank

> Session 44: those body interiors are the **fall-through tails** of the function above
> them. `make midfunc` (`tools/midfunc.py`) lists all 67 with the frame slots and
> registers each tail inherits before it writes them.


The mechanism needs the target address to be a function entry in the bank that is
resident when the call runs. Of the 67 distinct cross-bank `jal` targets the main
unit has, only **9** are entry points the bank units' own disassembly found (and
9 seed symbols per unit are not enough to matter). The other **58** are body
interiors in *every* bank that covers the address — record 1's `0x80198D28` is
the session-33 case (`splat` merged it into `func_ovlD_80198A6C`).

The same gap from the other side: **62 of the 67 function entries in overlay C
have no bank registration at all** (only 1395 addresses are registered across all
four units). With session 33's bindings, a 130 s `OGRE_SPEED=4` run still makes
5140 stub calls, from two addresses only (`0x8019E588` 2024 times, `0x801A34FC`
3116 times) — both reachable only from a resident bank's code, so the same class
of failure as the menu crash.

**Diagnostic that is now automatic:** `tools/cross_bank.py` refuses to seed an
address whose code does not open like a function (a stack frame, a jump-table
dispatch, or a bare `jr $ra`), so a bad seed is a printed warning rather than a
fragment-compiling failure.

Forcing splat to split at a body interior does not work: N64Recomp then compiles
the fragment with `goto after_4;` whose label lives in the sibling fragment, and
the app does not compile:

```
BankDFuncs/funcs_0.c:3843:14: error: use of undeclared label 'after_4'
```

Rewriting only the 9 resolvable targets and leaving the rest bound still crashes
the boot (SIGBUS before the first present, `bank loads: 0`), so the dispatch does
not merely under-deliver — it is not yet correct. It stays off.

### State after this session

| item | result |
|---|---|
| `cross_bank.py report` | 67 cross-bank `jal` targets; 9 with a bank entry, 58 without |
| `make bank-recomp` + app build | clean, session-33 function table (1398 bank functions) |
| attract loop, scene `0x0C` | unchanged (session 33's behaviour) |
| scene `0x18`/`0x02` | still open |
| `make cross-bank-dispatch` | experiment only; breaks the boot today |

### Next: make the 58 targets entry points *by detection*

Both routes are in the bank units' own configuration rather than new runtime
code:

1. Give each unit's disassembly the addresses explicitly (a `functions`/
   `function_sizes` list in `config-bank<U>.toml`, like the main `config.toml`
   already carries, or a splat symbol with the right attributes), so they are
   function entries by detection and the seeds are pure renames again. This is
   the minimal, in-model fix and should also remove the `after_N` failure.
2. Move overlay C out of the main ELF into a bank unit, which removes the
   cross-bank bindings by construction (session 33 §14's alternative). Larger,
   and it touches the working attract loop.

Keep the invariant the tool prints: `cross_bank.py` always reports exactly which
targets are dispatched and which are still bound, so a run shows the remaining
work by name.

---

## 2026-09-12 (session 33) — Phase 4: streamed-overlay banks swap on the game's DMA

### Finding: the streamed-overlay *segment table* and *bank descriptors* are now decoded

The "streamed-segment table at ROM `0x387C0`" that session 7 found is an array of
**19 records of 0x28 bytes**, one per loadable segment:

| word | meaning |
|---|---|
| +0x00 | RAM start (also the DMA destination) |
| +0x04 | RAM end (including bss, 16-aligned) |
| +0x08 / +0x0C | ROM start / ROM end (the bytes `func_8009DA50` copies) |
| +0x10 / +0x14 | bss start / end (`func_80093380` zeroes this) |
| +0x18 / +0x1C | code range (`func_800900C0`, icache invalidate) |
| +0x20 / +0x24 | data range (`func_80090010`, dcache invalidate) |

`func_800761E4`'s inner loop loads one record: flush/invalidate the code and data
ranges, `func_8009DA50(rom_start, ram_start, rom_end - rom_start)`, then
`bzero(bss_start, bss_end - bss_start)`. The **bank descriptors** are byte lists
of record indices terminated by `0xFF`, at ROM `0x38AB8`; the 11-entry pointer
array is at ROM `0x38AFC` and `func_80076430` matches a requested bitmask to one
of them. So there are **11 banks over 19 records**; records are shared between
banks, and banks overlap in RAM.

The bank the t≈95s run swapped in is `{2, 3, 6}`: ROM `0xE4910` → RAM
`0x80197B90` (0x72C0), ROM `0xEBBD0` → RAM `0x8019EE70` (0xE440), ROM
`0xFA600` → RAM `0x801AD5C0` (0x7700). Record 6's ROM size and code/data split
match the table exactly, and `0x801AD5C0` is its entry point.

### Finding: splat's `exclusive_ram_id` overlay mode is unusable here

splat/spimdisasm do have first-class overlay support: `exclusive_ram_id` groups
segments that share RAM, `overlayCategory` namespaces their spimdisasm symbols,
and references are resolved by ROM address within the group. It looked like the
intended mechanism for overlaying C and the new bank in **one** ELF.

**It does not work with this splat/spimdisasm version.** Tagging `streamedC`
with an `exclusive_ram_id` makes splat emit `asm/1CE040.s` with **zero branch
label definitions** (1650 `.Lxxxx:` labels before, none after; the references to
them remain), so the link fails with ~3843 undefined `.L` symbols. The same
happens whether C shares a group with the new records or has its own. It is not
an option for the working overlay C.

### Decision: recompile the bank records as a *separate unit*

`config-bank.yaml` recompiles records 2/3/6 at their true RAM addresses in
isolation (so spimdisasm follows their `jal` targets and finds 204 real
functions), with `symbol_name_format: "ovl_$VRAM"` so their symbols cannot
collide with overlay C's when both units are linked into the app. The leading
and inter-record ROM gaps are read as `bin`s and never linked. `make bank`
builds `build/ogrebank.elf`; `config-bank.toml` recompiles it into `BankFuncs/`.

The bank sections are deliberately **not** relocatable: the game pre-links its
overlays with absolute pointers, so the linked addresses *are* the RAM
addresses, and every emitted reference is absolute — no `section_addresses` /
`RELOC_HI16` involvement at all. `tools/gen_bank_funcs.py` flattens the bank's
`recomp_overlays.inl` into `app/src/bank_funcs.inc`
(`{ rom_start, ram_start, size, {ram addr, func}[] }`).

### Decision: the bank swap is driven by the PI DMA

`recomp::do_rom_read` now calls `recomp::overlays::notify_rom_read(rom_offset,
ram, size)`, which:

1. scans the main unit's section table for a streamed section (`ram_addr >=
   0x800E0000`) whose ROM range contains `rom_offset` **and** whose
   `ram - rom` delta matches — i.e. this DMA is a chunk of that overlay — and,
   if it is not already resident, `unload_overlapping_overlays()` +
   `load_overlay()` at the section base;
2. then calls an app-installed hook, which does the same for the bank unit's
   records via `load_function_bank()`.

`unload_overlapping_overlays` (new) allows *partial* overlap, unlike the
existing `unload_overlays` (which asserts): a bank swap overwrites most of the
previous bank. It drops both real sections and registered function banks, so
re-loading overlay C later cleanly replaces the bank. `loaded_function_banks`
records each bank's RAM extent for that purpose.

**Bug found while bringing this up:** the first version compared the *chunk's*
destination against the section's loaded base, so every 0x200-byte chunk after
the first looked like a new load and re-registered overlay A/B at shifted
addresses (and unloaded the real one) — boot died immediately with a NULL
function pointer in overlay B. The check must be "is this section loaded at
all" (`is_section_loaded`), and the load address must be the section base, not
the chunk destination.

### Finding: the next attract scene asks for records {10,11,12,13}

The attract loop is title → "lore" (the story movie) → title → the next variant
screen. A 430s unattended run reached that second transition at t≈360.7s and
recorded the scene's own request. The scene dispatcher (asm/1060.s @0x80075E50)
stores the scene id at `D_800E810E` and its descriptor at `D_800E8294`; the
descriptor's `+0x10` word is the mask handed to the record loader
`func_800761E4`. `app/src/bank_overlays.cpp` now dumps it when it first sees an
uncompiled record:

```
[bank] UNCOMPILED streamed record 10: rom=0x1F0A00 ram=0x801AD5C0 size=0x230E0
[bank]   scene=0x000C descriptor=0x8018FB58 record mask=0x00003C00
[bank] UNCOMPILED streamed record 11: rom=0x24BC70 ram=0x801F7100 size=0x131F0
[bank] UNCOMPILED streamed record 12: rom=0x25EE60 ram=0x8020A300 size=0x169C0
[bank] UNCOMPILED streamed record 13: rom=0x275820 ram=0x802210E0 size=0x47D0
[overlays] streamed function stub called @ 0x801E6FD0 (not yet loaded)
[overlays] streamed function stub called @ 0x801EE3E8 (not yet loaded)
[overlays] streamed function stub called @ 0x801EE600 (not yet loaded)
[overlays] streamed function stub called @ 0x801EE4A0 (not yet loaded)
```

`mask=0x3C00` is bits 10,11,12,13; `func_80076430` matches it to descriptor id 2
(`{0,2,4,10,11,12,13,14}`), and only the masked records are copied. That is why
the scene bounced: its code was never recompiled, so the entry returned through
the generic stub, and the four calls that follow (all in record 7's address
range, i.e. a different bank's layout) also hit the stub. Records 2,3,6 were
re-loaded afterwards, i.e. the game fell back to the previous attract scene.

### Decision: bank units are partitioned by RAM, not by game bank

One ELF cannot link two records whose RAM ranges overlap — they are different
banks of the same RAM. `make bank-recomp` therefore loops over `BANK_UNITS`,
each a `config-bank<U>.yaml` + `.toml` compiling a **RAM-disjoint** record set:

| unit | records | RAM |
|---|---|---|
| A | 2, 3, 6 | `0x80197B90` / `0x8019EE70` / `0x801AD5C0` |
| C | 10, 11, 12, 13 | `0x801AD5C0` / `0x801F7100` / `0x8020A300` / `0x802210E0` |

Records that overlap go in different units. The partition is invisible to the
runtime: `app/src/bank_overlays.cpp` registers whichever record the game DMA's,
so a new record can be added to any unit with a free range. Each unit namespaces
its symbols (`ovlA_`, `ovlC_`) so the units can be linked beside each other and
the main unit; `tools/gen_bank_funcs.py` merges every
`Bank*Funcs/recomp_overlays.inl` into one `app/src/bank_funcs.inc`. Unit C adds
848 functions (records 10–13); the app now arms 7 records / 1052 functions.

### Finding: overlays stream *more code into their reserved arena* at runtime

Registering records 10–13 did not fix scene `0x0C`: the same four calls
(`0x801E6FD0`, `0x801EE3E8`, `0x801EE600`, `0x801EE4A0`) still hit the streamed
stub. A stub-path diagnostic that dumps the first words at the address showed
**real MIPS code** there, and a DMA trace (filtered to the arena) showed where it
came from:

```
[pi] inline DMA dram=0x801E6FD0 dev=0x0023B1F0 size=0x200   <- the stub address
[pi] inline DMA dram=0x801EE3D0 dev=0x002425F0 size=0x200
[pi] inline DMA dram=0x801EE5D0 dev=0x002427F0 size=0x200
```

So a segment-table record is only the **resident part** of an overlay. The
record's `ram_end` word (`+0x04`) is much larger than its code+data+bss because
the space above the record is an **arena** the game fills on demand with further
code modules from the ROM gap between that record's `rom_end` and the next
record's `rom_start`. For record 10:

| module | ROM | size | RAM |
|---|---|---|---|
| table record 10 | `0x1F0A00` | `0x230E0` | `0x801AD5C0` |
| `bankRec10a` | `0x213AE0` | `0x16770` | `0x801D0860` |
| `bankRec10b` | `0x23B1F0` | `0x09580` | `0x801E6FD0` |

All four stubs land in `bankRec10b`. Both modules are plain 0x200-byte-chunk DMA
copies (constant rom→ram delta), so they are compiled as ordinary RAM-disjoint
sections of unit C and registered by the same DMA hook — no new runtime
mechanism was needed. One wrinkle: as `asm` sections, splat sometimes references
a data label inside them without emitting its definition;
`tools/gen_bank_syms.py` turns every referenced-but-undefined `D_ovl<U>_<vram>`
into an absolute linker definition (the name encodes the VRAM, and the unit links
at true RAM addresses).

**Consequence for future banks:** a record is not "done" when its table entry is
compiled — the arena modules in the following ROM gap must be compiled too. The
app's DMA hook already registers them, and `[bank] loading overlay record …`
lines name them.

### Decision: debug knobs for iterating on a scene that is minutes away

Waiting ~6 minutes for scene `0x0C` made this slow, so two knobs were added:

| knob | what |
|---|---|
| `OGRE_SPEED=<n>` (`ultramodern/src/timer.cpp`) | multiplies the emulated clock: the CPU counter *and* the VI retrace schedule run `n`× faster, so a timed attract sequence completes in `1/n` of the wall time (clamped to 64). Semantics are unchanged because every timer scales together (audio is off in these runs). |
| `OGRE_FORCE_SCENE=<hex>` (+ `OGRE_FORCE_SCENE_AFTER_MS`, `app/src/bank_overlays.cpp`) | pokes the attract scene id (`*(u16*)(D_800C4BBC + 4)`) until `D_800E810E` reports the scene active, so a run switches to the wanted scene as soon as the scene's own state machine allows. |

Scene `0x0C` is reached at t≈42s wall with `OGRE_SPEED=8 OGRE_FORCE_SCENE=0x0C`
instead of t≈360s, which is what made the arena modules findable in one session.

### Finding: the title menu (New Game / Tutorial) asks for records 1, 0 and 14

Pressing Start opens the menu; selecting an item crashed (SIGSEGV). With
`OGRE_TAP_MS` the diagnostic names both scenes: scene `0x18` (the menu) asks for
**record 1** (`mask 0x2`, descriptor `{2,1}`) and scene `0x02` (New Game /
Tutorial) asks for **records 0 and 14** (`mask 0x4001`, descriptor `{0,14}`).
All three are now compiled — record 1 is **bank unit D**, record 0 is **bank
unit E** (both load at `0x80197B90` like record 2, so each needs its own unit),
and record 14 joined unit C (RAM-disjoint). The app arms 12 records / 1398
functions across units A, C, D, E.

### Finding: N64Recomp binds cross-bank `jal`s at build time

Compiling those records did not stop the crash. lldb shows the fault inside
**overlay C's** recompiled `func_801989AC` while a different bank is resident at
that address. N64Recomp compiles a `jal` to a known function as a *direct* C
call, so resident code that calls into a swappable bank's address range is bound
to the main unit's overlay C body:

```
RecompiledFuncs/funcs_1.c:  func_80178920 (overlay B)
    // 0x8017892C: jal 0x80198D28
    func_801989AC(rdram, ctx);      <- overlay C, not the resident bank
```

The main unit's generated C has **58 such call edges (53 distinct targets)**
into `[0x80197B90, 0x801BA550)`. In the real game the callee is whatever bank is
resident, so these must go through `get_function`. Patching that one call to
`LOOKUP_FUNC` moved the crash, confirming the mechanism (the lookup returned the
stub because record 1's disassembly has no entry at `0x80198D28` — splat merged
it into `func_ovlD_80198A6C`).

**Fix to apply** (not yet done): (a) a post-step over `RecompiledFuncs/*.c` that
rewrites calls crossing into a bank-swappable range as
`LOOKUP_FUNC(addr)(rdram, ctx)` (or move overlay C out of the main ELF into a
bank unit, which removes the bindings by construction); and (b) seed each bank
unit's disassembly with those cross-bank targets as `func_<vram>` entries via a
generated `symbol_addrs`, so `get_function` can resolve them. Until then scene
`0x18`/`0x02` still segfault; the attract loop is unaffected.

A `SIGSEGV`/`SIGBUS`/`SIGABRT` handler now dumps the runtime's shadow call chain
on a crash (`app/src/bank_overlays.cpp`), because a raw segfault otherwise loses
the recompiled stack (the Release build omits frame pointers, so lldb shows only
frame #0).

### Outcome

| run | session 32 | session 33 (unit A) | session 33 + unit C + arena |
|---|---|---|---|
| 5s | exit 0 | exit 0 | exit 0 |
| 170s unattended | exit 134, dl 2770 at t≈94.8s | **exit 0, dl 4850 at t≈168s** | — |
| 430s unattended | — | exit 0, but scene `0x0C` stubbed at t≈360s | **scene `0x0C` runs** |
| bank swap | `streamed function stub called @ 0x801AD5C0` | `[bank] loading overlay record …` ×3 at t≈95.1s | + ×4 (records 10-13) + ×2 (arena modules) |
| stubs at scene `0x0C` | — | 4 (`0x801E6FD0` …) | **0** |
| unit-info screen | — | bounced to the title | `docs/proofs/native-unit-info-{dragon-tamer,griffin}.png` |
| natural unattended loop (`OGRE_SPEED=4`, 280s wall ≈ 1100s game) | — | — | **exit 0, 29080 display lists, 13 bank/arena loads, 0 stubs**; the unit-info screen comes up on its own (`docs/proofs/native-unit-info-fighter-natural.png`) |
| `build-null`, `build-wasm` | clean | clean | clean |

The app arms **9 records / 1289 functions** (unit A: records 2,3,6; unit C:
records 10,11,12,13 + arena modules `bankRec10a`/`bankRec10b`).

Remaining uncompiled records: 0, 1, 4, 5, 14, 16, 17, 18 (plus their arena
modules). They overlap records already placed, so each needs a unit with a free
RAM range (0/1/17 all share `0x80197B90` with record 2, so they need further
units); `kAllStreamedRecords` in `app/src/bank_overlays.cpp` names whichever one
an unattended run asks for, together with the scene id and mask that requested
it.

---

## 2026-09-12 (session 32) — the publisher screens were a mis-named `osViSetMode`; the late crash is an overlay bank swap

### Finding: `0x80095820` is `__osViSwapContext`, not `osViSetMode`

The "Licensed by Nintendo" / ATLUS / QUEST stills rendered as the top-left
quarter of the image, stretched over the window. Rendering was *correct*: an
`OGRE_CAPTURE_TARGET` dump of the framebuffer the VI samples showed a perfect
640x480 still. The fault was in the VI scanout: the runtime was presenting with
its `dummy_mode` geometry (`width=320`, `hRegion=0x006C02EC`, `xScale=0x200`)
the whole run, and RT64's video-interface shader crops sampling to
`videoResolution/textureResolution`, i.e. the top-left quarter of a 640x480
target.

The game *does* switch to a 640x480 hi-res mode (`D_800AB9B0`: `ctrl=0x324E`,
`width=640`, `xScale=0x400`, `yScale=0x800`) for those screens, but the call
never reached the runtime. `symbol_addrs.txt` named `0x80095820`
`osViSetMode`, and session 10 had already shown that function is really
`__osViSwapContext` (it reads `__osViNext->modep/framep` and writes the VI
MMIO registers; it ignores `$a0`). The **real** `osViSetMode` is
`func_800955C0`, which stores the mode pointer at `__osViNext+0x8` and copies
`modep->comRegs.ctrl` to `+0xC`, exactly like libultra — and it was unnamed, so
it was compiled verbatim and never bridged.

**Decision:** `func_800955C0` is `osViSetMode` (bridged to the runtime);
`0x80095820` is `__osViSwapContext`, reimplemented as a no-op, because the
port's VI thread (`update_vi`) already writes the registers from its own
`ViState` and the game-side `__osViNext` context is never populated. The
mislabeled function stays out of the way and the game's mode switching now
reaches RT64. Verified: the three publisher screens render full-frame
(`docs/proofs/native-{licensed,atlus,quest}-screen.png`), the intro/title are
unchanged, and a 15s run still reaches ~400 display lists (~28fps).

### Finding: the 0.2-0.5s of noise before each still was an unbridged `osViBlack`

Once the mode switch worked, the 320x240 → 640x480 transition still showed a
short burst of colour static (presents 276–278 in a capture from present 250):
the VI is already hi-res while the new 640x480 framebuffer is only partly drawn.
The game blanks the screen for exactly that window —
`func_80093CB0` = `osViSetMode(hi-res)` + `osViBlack(1)`, and `func_800942C0` =
`osViBlack(0)` + `osViSwapBuffer` when the new frame is ready — but `osViBlack`
was another unnamed libultra function (`func_80095B30`, the real one: it
tests/sets `__osViNext->state` bit `0x20`). Its neighbour `func_80095780` is
`osViFade` (stores the fade level at `+0x24`, sets bit `0x4`).

**Decision:** `osViBlack = 0x80095B30` (already in `reimplemented_funcs`, with
`osViBlack_recomp` in `librecomp/src/vi.cpp`). `update_vi` sets `hStart = 0`
while `VI_STATE_BLACK` is set, RT64's `VI::visible()` goes false, and the
presenter clears rather than uploading stale RDRAM. `osViFade` is deliberately
left verbatim for now (no runtime implementation; `update_vi` still has
`TODO implement osViFade`), so fades snap instead of ramping.

### Finding: the late crash is a streamed-overlay *bank swap*, not a missing overlay-C address

The ~95s abort (`streamed function stub called @ 0x801AD5C0` then
`Failed to find function at 0x00000000`) was attributed in session 31 to
"overlay C's upper reach". Instrumenting `get_function` with the recompiled
call chain, and logging every inline PI DMA (`OGRE_DEBUG_TRACES`), shows
otherwise:

- the stub is called from `func_800765D8`, via `jalr $a1` where
  `$a1 = *(0x800E7A40)` (an object template's callback field);
- immediately before the crash (t≈95s) the game DMA's a **new overlay over
  overlay C** in three pieces: ROM `0xE4910` → RAM `0x80197B90` (`0x72C0`),
  ROM `0xEBBD0` → RAM `0x8019EE70` (`0xE440`), ROM `0xFA600` → RAM
  `0x801AD5C0` (`0x7700`);
- ROM `0xFA600` starts with a normal prologue (`addiu $sp,$sp,-0x30`), so
  `0x801AD5C0` is that overlay's entry point; in the linked overlay C the same
  address is a mid-function label (`D_801AD5C0` inside `func_801AD558`).

So the port is running overlay C's `func_map` while the game has swapped to a
different bank. The stub returns without initialising the object the callback
belongs to, and the caller's next function-pointer call is NULL.

**Decision (scope):** this is the Phase-4 boundary proper. Overlays C and D
overlap in RAM (`0x80197B90..0x801B8090` vs `0x80197B90..0x801B4CC0`), so they
cannot both be linked at their fixed RAM addresses in one ELF; the fix is a
relocatable/overlay-section scheme with runtime load/unload, not another
`app/src/overlays.cpp` registration. Session 32 stops at documenting the
evidence.

---

## 2026-09-12 (session 31) — a size override only resizes a symbol; calls into it must be redirected

### Finding: the intro crash was a split symbol called without its frame

The session-30 wall (`SIGSEGV` in `func_801A1FCC`, `$s3 = 0x80000318`) was
reproduced and instrumented frame-by-frame. The saved `$s3` slot held
`0x80000310`: the high halfword of `0x800A81C0` had been overwritten by a 16-bit
store at the *wrong stack depth*, and `$sp` came back `0x10`-`0x38` high after
each `func_801A1170` call.

`func_801989AC` has `function_sizes = 0x520` (its real epilogue at `0x80198EC4`
restores `$s3` from `0x9C($sp)` and adds `0xB8`), so the `func_80198D28` symbol
inside it is a false split. But `func_80198D28` was *also* emitted and *called*
as a standalone function: its first instruction is the label `L_80198B40` and it
ends with `func_801989AC`'s epilogue, so it computed with a frame that was never
allocated and restored `$ra`/`$s3` from garbage. That is the leak the whole call
chain inherited.

**Decision:** N64Recomp now treats a `jal`/`j` into a body extended by
`function_sizes` as a jump into that body, never as a call to the split symbol.
`Function::has_size_override` is set in `elf.cpp`, and `recompilation.cpp`
resolves such targets to the innermost containing function
(`find_containing_size_override`), emitting a tail call to it (its prologue sets
up the frame the target expects and its epilogue unwinds it) instead of
compiling/calling the split symbol again. 24 sites redirect at recomp time.
This makes the `function_sizes` workaround self-enforcing: an override is now
enough on its own, with no per-symbol hand patching.

**Also decided:** session 30's `func_80197C94 = 0x948` override is wrong and is
back to `0x40C`. `0x948` swallowed the real function `func_801980A0` (own
prologue and epilogue), turning it into a ~0x10-byte stub; the genuine
cross-function jumps at `0x80197C94`'s tail (`j 0x801984BC` / `j 0x801985D8`)
are shared code and are handled by the fall-through fix below, not by moving the
boundary.

### Finding: a body that runs off its end never unwinds its frame

`static_16_801984BC` (reached by `j 0x801984BC`) ends with a plain `sw` and
falls through into the discovered static `static_16_801985D8` (the shared
epilogue `lw $s3, 0xC($sp) ... jr $ra / addiu $sp, 0x10`). N64Recomp emitted the
store and closed the function, so the `0x10` frame leaked per call.

**Decision:** when a function body's last instruction is not a control transfer
(`j`/`b`/`jr`/`syscall`), emit the tail call the fall-through performs plus
`return`: call the body at the next address when one is known, otherwise
register that address as a static in the same list `print_branch` uses and call
it by name. 1726 sites were emitted; they are unreachable wherever the preceding
instruction already returned, and real where a body genuinely ran off its end.

**Consequence:** with both fixes the intro no longer crashes. An 8s run exits 0
with 205 display lists (was SIGSEGV at display list 1, t~845ms); a 15s run
reaches 395 display lists at t=14.2s (~28fps, the game's intended 30fps); the
per-frame lists carry 12-17 triangles and 27-32 `SETTIMG`.
`docs/proofs/native-intro-impact.png` (swap-chain readback at t~3.3s) shows the
twelve soldiers, the impact smoke and the block falling between them.
`OGRE_CAPTURE_PRESENT=<path> OGRE_CAPTURE_AFTER=<n>` writes
`<path>.<n>.ppm`; do **not** combine it with `OGRE_PRESENT_ALWAYS=1`, which
captures a black frame (the session-27 RDRAM-vs-render-target finding).

---

## 2026-09-12 (session 29) — the frame rate was a recompiler poll-loop false positive

### Finding: `yield_self` in ordinary loops

`OGRE_PROFILE=1` (new: a sampling profiler over the recompiled-function entry
hook, plus per-thread entry counts) showed the game was *not* CPU-bound during
the 0.3-2.8 s stalls: `sample(1)` on the running process put the thread in
`func_8008AFE0 -> func_80072398 -> func_800EAC24 -> func_80081B08` with the time
split between `yield_self` and `wait_for_resumed`, not in game math. Every
`yield_self` with an empty running queue sleeps for the next VI retrace
(~16.7 ms), so a loop that yielded per iteration ran at ~60 iterations/s.

N64Recomp's poll-loop heuristic had flagged those loops as spins on a
loop-invariant address. Two holes let ordinary loops through:

1. **The branch delay slot was not part of the loop body.** The body was
   `[head_idx, instr_index)`, so `bne $v0, $zero, head` / `addiu $v1, $v1, 0x13C`
   (the pointer bump in the delay slot) was invisible and `$v1` looked constant.
   `func_80081B08` scans a 36-entry object array that way.
2. **A load's destination did not count as a write.** That made a pointer chase
   (`lw $a0, 0x10($a0); bne $a0, $zero, head`) look like `while (flag) ;`. The
   game's malloc free-list search (`func_80071A3C`) and the rest of the
   allocator family are exactly that.

### Decision: fix the heuristic, do not special-case the functions

The body is now `[head_idx, instr_index + 2)` (branch and delay slot included),
and a load is only a poll load if its base is the constant produced by a `lui`
in the same iteration; a base re-derived from a load (a chase) is rejected. The
destination of a load still counts as a write, so the classic
`lui $v0, 0x800E; lw $v0, 0x79A4($v0); bne ...` spin (`func_80089A10`) is still
detected - its base is written by `lui` immediately before the load.

Regenerating drops `yield_self` from 99 sites to 14, all of them real poll loops
(`func_80089A10`, `func_80075BC0`, overlay polls). Measured effect: **171 display
lists in 6.2 s, median gap 34 ms (~30 fps), no gap over 500 ms**, versus 8
display lists in 8 s with 0.3-2.8 s stalls before. `n64recomp-ob64.patch`
regenerated; `RecompiledFuncs/` is a build artifact regenerated by `make recomp`.

Two smaller fixes were kept from the same investigation because they make the
scheduler ring usable: its dump compared `ev.seq` against the *slot index*, so
every event past the first 16384 was silently dropped; it now compares against
the global sequence the slot should hold. Each event also carries a
`trace_millis` timestamp.

### Not the cause

- The renderer: `processDisplayLists` is 0.6-8 ms.
- The scheduler `yield_self` implementation: replacing it with an
  osYieldThread-style "hand off to the running-queue head before sleeping"
  variant changed nothing once the recompiler was fixed, so it was reverted.

### New wall (same session): the title's object table is never filled

At display list 171 the game SIGSEGVs in `func_801A1170` on a NULL object
pointer. The same crash happens with the session-28 output run long enough
(150 s), so it is pre-existing, not a regression - the old frame rate just hid
it. Entries 10-12 of `D_801B81D0` are filled only by `func_801A103C`, which is
reached only from `func_8017B9C8` (the draw callback of scene states 5-7); the
game boots into state 9, whose draw is `func_801A1FCC`. That is why the cube -
the object-table entries - never appears while the soldiers/smoke (the parallel
`func_801A0E44`/`func_8019FC68` path) do. See
`docs/HANDOFF-2026-09-12-session29.md`.

---

## 2026-09-12 (session 28) — the game renders natively: a scheduler handoff bug, not a game or renderer bug

### Context

Session 27 proved the native render path end to end with a synthetic display
list, and left one open item: the game itself still submits only its boot
blanking display list (1 gfx task per run) and the boot thread (t3) never returns
from `func_80089804`. Four candidate explanations had been on the table across
sessions 15/24/26/27: `func_80089A10`'s spin, the frame-completion handshake, a
missing asset, or the presenter.

### Finding: the boot thread is *stranded* by a spurious scheduler wakeup

`OGRE_SCHED_TRACE=1` records every scheduler operation into a lock-free ring with
a global sequence number (`[sched-ring]`), because the `[Sched]` printfs come
from several host threads and interleave through a buffered stdout - useless for
an ordering question. The ring shows the boot thread parking once inside
`func_80089804`'s `osSendMesg` (`swap_to_thread` -> `wait_for_resumed`) and then
**never being popped, resumed or woken again** for the rest of the run, while the
running queue drains to empty and every other thread blocks on `recv`.

The wakeup it was waiting for was the one `wake_blocked_head` sends when a
message is delivered to a queue a thread is blocked on. That code signalled the
same `running` semaphore that `wait_for_resumed` waits on. A thread parked in a
*handoff* wait could therefore consume a *message poke* and continue without
having been handed execution - and so could the thread it had just swapped to.
Two game threads then run at once, which breaks the cooperative scheduler's
single-runner invariants and is what lets the running queue lose an entry. The
existing comment ("every scheduler semaphore wait re-checks its condition, so a
surplus count is harmless") was the mistaken premise: `wait_for_resumed` does
not and cannot re-check.

### Decision: a strict handoff token (`running`) and a separate idle poke (`poke`)

`UltraThreadContext` gets a second `LightweightSemaphore`. Only a real grant of
execution (a pop from the running queue via `run_next_thread`/
`run_next_thread_and_wait`, or `resume_thread_and_wait`) signals `running`;
`wake_blocked_head` signals `poke`, which is reaped by the idle path of
`run_next_thread_and_wait` exactly as before. `wait_for_resumed` can no longer
return without a handoff.

This is a *runtime* fix, not a game-specific patch, and it is what unblocked the
boot: 6/6 native runs now submit the game's real display lists (`0x801C1520` /
`0x801C80A0` alternating, ~2.5/s) where 4/6 used to stall with 1.

### Decision: scheduler forensics get a race-free ring, not more printfs

The event ring (`debug_sched_event*`, `debug_dump_sched_ring`) records
insert/pop/remove/resume/park/wake/swap *and the running queue after each event*.
It is enabled by `OGRE_SCHED_TRACE=1` and dumped at `OGRE_EXIT_AFTER_MS`
(`OGRE_SCHED_TRACE_TID` filters it). `debug_printf` was previously compiled out;
it is now gated on the same flag, so the thread-queue traces that were already
written become usable. This is the diagnostic to reach for on any future
"a thread stopped running" question.

### Finding: the diagnostics were reading out of the committed RDRAM mapping

Once the boot progressed with input, the VI thread's periodic queue snapshot
crashed (`EXC_BAD_ACCESS`, 3 of 4 tap runs). A queue's `blocked_on_recv` held
`0x8606FF09`; the guard accepted anything in `0x80000000..0xA0000000` (the whole
KSEG0 range), the read resolved past the 512 MiB committed RDRAM, and the *VI
thread* died. Only the game's RDRAM window (`0x80000000..0x80800000`) can hold an
`OSThread`.

### Decision: snapshot guards use the RDRAM window, and `NULL` queues are refused

The guard in `debug_dump_queue_snapshot` (and its `valid_ptr`, assettab,
frame-dispatch and title-dispatch siblings) is now `0x80000000..0x80800000`.
Separately, `queue_to_ptr` in `threadqueue.cpp` refuses a `NULL`/non-KSEG0 queue
instead of resolving `TO_PTR(NULLPTR)` out of bounds: `osSetThreadPri` and
`osDestroyThread` can legitimately be called on a *running* thread, whose `queue`
is `NULLPTR`.

### Note: `func_80089A10` was never the wall

Its generated code already contains the `yield_self` poll-loop yield (N64Recomp's
poll-loop detection), so the boot thread's spin yields. The session-15 runtime
override of the same name remains dead code (the recompiled symbol is strong and
direct calls bind to it), but it is not load-bearing: the boot thread was parked
*inside* the frame submit's `osSendMesg`, not spinning in `func_80089A10`.

### Open (recorded, not decided)

- The 512 MiB RDRAM mapping is a recurring foot-gun; `thread_ptr_valid` in
  `threadqueue.cpp` and the mesgqueue `mq` list reads still use `0xA0000000`.
  Safe today only because 512 MiB is committed.
- The periodic queue snapshot (with its ~20 ms `debug_sample_hot_loop`) runs
  unconditionally every ~1.5 s; it should be env-gated and off by default.
- "Start advances the title to the menu" is untested - this session proves the
  game renders and accepts input, not that input progresses the game.

---

## 2026-09-12 (session 27) — the native window renders: GPU readback, and four display-list encoding bugs

### Context

Session 26 ended with "RT64 receives, parses and executes a real display list,
but the window never shows the result" and two candidate explanations: the
presenter never composites the framebuffer, or `screencapture` cannot see the
Metal layer. Both were wrong, and the way they were wrong mattered: every
session-26 capture-based conclusion rested on `screencapture` output.

### Finding: window contents *are* capturable, but the session-26 captures were of a locked display

`CGWindowListCopyWindowInfo` shows a `loginwindow` window at layer 2004 covering
the whole screen: the display is locked. `screencapture -x` (whole screen) then
returns wallpaper only, while `screencapture -l<windowid>` (per window) *does*
return window contents - a Chrome window captures perfectly. The app's own window
captures as its title bar plus a black canvas.

So a window capture is usable for a still frame, but it is not a trustworthy way
to see a *changing* frame on a locked display: with a cycling clear colour the
window capture stays on one colour for seconds. It reports the last frame the
window server committed, which on a locked display is rare.

### Decision: trust the presented swap-chain texture, not the window (RT64 + plume)

RT64 now has `OGRE_CAPTURE_PRESENT=<path>`: at present time it copies the exact
swap-chain texture this frame presents into a `RenderBufferDesc::ReadbackBuffer`
and writes `<path>.<n>.ppm`. `OGRE_CAPTURE_TARGET=<path>` does the same for the
render target the VI renderer sampled (`renderParams.texture`) and
`OGRE_CAPTURE_AFTER=<n>` skips the first `n` frames.

Plume's Metal `copyTextureRegion` only implemented buffer -> texture, so the
mirror case (texture -> buffer, what a readback needs) was added. It also has to
go through `ExtendedRenderTexture::getTexture()` because a swap-chain texture is
a `MetalDrawable`, not a `MetalTexture`, and reading `MetalTexture::mtl` off one
is undefined behaviour (it crashed inside `copyFromTexture:toBuffer:`).

This readback is the session's ground truth: it is what RT64 presents, with no
window server, permission or compositing in the path.

### Finding: RT64's presenter is change-driven, so a stalled boot freezes the window

`State::updateScreen` pushes a present only when the VI changed, the RDRAM copy
of the VI framebuffer changed, or a framebuffer operation was recorded. A stalled
boot does none of those: the traced present count stops after ~10 VIs and the
window keeps whatever it last showed forever. `OGRE_PRESENT_ALWAYS=1` forces a
present per VI. Separately, the presenter looks its framebuffer up in the
framebuffer manager, which only knows framebuffers it has seen through tile
operations; a display list that just sets a colour image and fills it leaves its
render target unregistered, and the presenter would upload the (empty) RDRAM copy
instead. `OGRE_PRESENT_FBTARGET=1` falls back to a non-empty render target at the
VI address.

### Finding: the synthetic display list was never drawing — four encoding bugs

The renderer half looked "proven" because RT64's logs showed `setFillColor` and
`fillRect` firing. Firing is not rasterizing. The hand-built F3DEX2 list in
`app/src/synth_frame.cpp` was wrong in four independent ways:

1. **Every two-word RDP command was emitted as two `emit()` calls** (four words).
   `G_SETSCISSOR` became `w1 = 0` (a null scissor) followed by a garbage opcode,
   and `G_FILLRECT` became `lrx = lry = 0` (an empty rectangle). RT64's
   `RDP::drawRect` returns early for an empty rect and only merges
   `drawColorRect` when a scissor is set, so nothing was ever recorded as drawn.
2. **`G_FILLRECT` operand halves were swapped.** RT64's `GBI_RDP::fillRect`
   decodes `ulx/uly` from `w1` and `lrx/lry` from `w0` (libultra's
   `gDPFillRectangle` does the same); the probe had the reverse, which is what
   the session-26 note recorded.
3. **Coordinates were not 10.2 fixed point.** The RDP wants `pixel << 2`; passing
   raw pixels made a "full screen" fill 80x60 and clipped everything to the top
   60 rows.
4. **`G_RDPSETOTHERMODE` was pre-shifted.** RT64 keeps the 24-bit mode0 verbatim
   in `w0` and tests fields at their full-word positions
   (`OtherMode::cycleType() == H & (3 << G_MDSFT_CYCLETYPE)`, i.e. bits 20-21 of
   `w0`). Sending `mode0 >> 8` made the cycle type `G_CYC_1CYCLE`, so the fill
   path in `FramebufferRenderer` never ran. The fill colour also has to be
   RGBA16 (`0xF801`), not RGBA8888, because the colour image is 16-bit.

With all four fixed, `G_CYC_FILL` draw calls reach `FramebufferRenderer`, the
target contains the seven bars, and the presented swap-chain texture is
`docs/proofs/native-synth-frame-rdp.png`.

### Decision: the probe turns on the presenter diagnostics it needs

`RT64Renderer` calls `setenv("OGRE_PRESENT_ALWAYS"/"OGRE_PRESENT_FBTARGET", "1",
0)` when `OGRE_SYNTH_FRAME` is set, so one command shows a frame. With
`OGRE_SYNTH_NO_DL` (the raw RDRAM path) only `OGRE_PRESENT_ALWAYS` is set: the
render-target fallback would otherwise bypass the RAM upload path being tested,
which made the raw probe black. The RT64-side knobs stay env-gated so the default
(no-env) run behaves exactly as before.

### Decision: the probe pins the VI

The probe first came out black in about one run in three. The cause is that the
VI is the game's: on some boots the game has repointed it at one of its own
(black) framebuffers by the time the capture runs, and the presented frame is the
game's, not the probe's. `synth_frame_vi_tick` now calls
`osViSwapBuffer(rdram, 0x80700000)` every VI (the VI thread is outside the
runtime's `message_mutex` at that point), which makes both probe paths
deterministic across runs. `OGRE_SYNTH_FORCE_VI=0` disables the pin.

---

## 2026-09-12 (session 26) — a real call stack for game threads, and the renderer half proven

### Context

Session 25 concluded that the wall was `func_8008AFE0` (thread 4) blocking on
`0x800C4C28`, a queue "nothing ever sends to". That came from a snapshot that
knows only *block kind + resume count + last function entered* — and "last
function entered" is the callee, not the caller, so it cannot say where a thread
actually is. This session replaced the diagnostic and re-answered the question.

### Decision: measure the stack, not the last entry (N64Recomp codegen + runtime)

The code generator now emits

```c
recomp_trace_entry(rdram, 0x80089804, "func_80089804", (uint32_t)ctx->r31);
```

in every function prologue and `recomp_trace_return(rdram, <func_index>);` before
every return. At prologue time `ctx->r31` still holds the caller's return
address (the function has not saved it yet), so each frame knows both its own
name and the call site that created it. The runtime keeps a per-thread shadow
chain of live frames (`function_trace.cpp`), so entry/return pairs make a *stack*
rather than an ever-growing ring of entries.

Consequences that made the session:

* the boot thread's live chain is stable across snapshots and readable;
* `debug_dump_call_chain()` also runs for every parked thread in the VI
  snapshot, and for all threads in the `OGRE_EXIT_AFTER_MS` dump;
* `OGRE_CHAIN_HISTORY=<tid>` records the ordered sequence of live-frame sets, so
  a wedge reads as a path (the frames it passed through), not a single state.

The alternative — logging `[func] enter/exit` lines — was rejected: the VI
snapshot and the game threads write the same stream and the interleaving destroys
the ordering that is the whole point.

### Finding: the earlier localisation was wrong

The send trace (`do_send`) shows `func_800891A0 → mq=0x800C4C28 msg=0x800E8B10`
dozens of times, and t4's chain shows it parked in `func_8008AFE0 →
func_80089054`. `func_80089054` is the *retrace-subscriber registration*
(`asm/1060.s` 0x80089054), so t4 is a per-frame service thread, not the wall.

The boot thread (`func_8007F8E4` → `osCreateThread(t3, func_80071EB0)`), by
contrast, goes `func_80071EB0 → func_8008A1B0 → {func_80089AB0 → func_80089A30}
→ func_80089660 → func_80089804` and never returns from `func_80089804`, the
frame-submit function. That is the handshake the boot is waiting on.

### Finding: the runtime's `func_80089A10` override is dead code

`librecomp/src/recomp.cpp`'s `func_80089A10_recomp` (a yielding spin) is never
called. The recompiled `_func_80089A10` is a strong symbol defined in
`funcs_5.c.o`; direct calls link to it, and only *function-pointer* lookups go
through the overlay map that the runtime can patch (`get_function`). Session 15
recorded the override as "the fix that unblocked the boot"; it was inert, which
is why the stall kept surviving every "fix". This defines the actual boot gate:
a high-priority thread executing `while (D_800E79A4 != 0);` with no yield, in a
cooperative scheduler whose only preemption is voluntary.

### Decision: a synthetic display list as a renderer-path probe

`app/src/synth_frame.cpp` (`OGRE_SYNTH_FRAME=1`) builds a small F3DEX2 list in
RDRAM and submits it through the same path the game uses
(`ultramodern::submit_rsp_task` from the VI callback). It exists because "the
canvas is black" cannot distinguish "the renderer never got a list" from "the
renderer dropped it", and because a stalled boot only ever submits its blanking
list. Three traps are documented in the file: `TO_PTR` sign-extends `int32_t`
pointers (so KSEG0 only), `OSTask::t.data_ptr` must be a *physical* offset
(`send_dl` masks with `0x3FFFFFF`), and RT64's fill path needs a scissor set or
the rect never reaches `drawColorRect`.

Result: the probe reaches `send_dl` and RT64 executes every command (verified by
instrumenting `setFillColor`/`fillRect`). The captured window is still black, so
the open gap is presentation, not display-list handling — a much narrower claim
than "the renderer is not proven".

### Finding: use a memory dump, not snapshot words

`OGRE_DUMP_RDRAM=<path>` writes the whole 8 MiB image at the scripted exit.
Reading a handful of words through the snapshot repeatedly produced
contradictory readings during this session; the dump settles questions offline in
one shot. Note the byte rules (`recomp.h`): the word at game address `a` is a
little-endian word at file offset `a - 0x80000000`, and the logical byte at `a`
is at `(a ^ 3) - 0x80000000`.

---

## 2026-09-12 (session 25) — the native app drives itself, and the idle trajectory is reproduced + localised on it

### Context

Session 24 left two open threads: the title sprites now render correctly in the
browser, and the native RT64 build runs but paints a black canvas because it
never receives input. The web probes feed input (`tap()` in
`debug/lib/harness.cjs`, one Enter press every 5 s), so no native measurement
could be reproduced without a human at the keyboard, and none of the web
probes' tooling (screenshots, pixel counts, `tasks=`) had a native counterpart.

### Decision: `OGRE_TAP_MS` / `OGRE_EXIT_AFTER_MS` instead of an external driver

A scripted native run is now self-driving, with two env vars read once in
`configure_automation()` (`app/src/sdl_platform.cpp`):

* `OGRE_TAP_MS=<n>` — controller 0 presses Start for the first 150 ms of every
  `n` ms window, contributing **button state only**. This mirrors the web
  harness's Enter tap so the two platforms' input is the same in kind.
* `OGRE_EXIT_AFTER_MS=<n>` — the main thread prints the per-thread last-function
  table after `n` ms and exits 0 immediately (`_Exit`, after a 100 ms grace so
  the VI thread can finish a snapshot it is printing). The graceful path
  (`ultramodern::quit()` → `recomp::start` unwinding) instead tears down the
  renderer and the runtime's workers while the game's threads are mid-call,
  which segfaults intermittently (seen with and without
  `OGRE_DEBUG_TRACES`); a scripted run wants the log and an exit code, not the
  unwind.

Two alternatives were rejected:

* **Synthesising SDL key events** (`SDL_PushEvent` from the main thread). It
  would work, but it puts the button state through SDL's keyboard layer, which
  fights the main-thread-only `SDL_PumpEvents` constraint documented in session
  24; button state cannot violate it.
* **Driving the app externally** (AppleScript/`cliclick` keystrokes). It needs
  Accessibility permissions and a focused window, i.e. it is not headless, and
  it would be the only part of the project's verification that is not
  self-contained.

Both default to 0, so an interactive run behaves exactly as before.

### Finding: the idle trajectory is not input-gated, and not platform-specific

| run | input | outcome |
|---|---|---|
| `ogrebattle64`, 40 s, no env | none | 1 display list (the boot blanking DL), no RSP task at all |
| `ogrebattle64`, 60 s, `OGRE_TAP_MS=5000` | 11 Start taps | 1 display list, **1085** type-2 RSP tasks, `bootstate=0x00000060` |
| `ogrebattle64`, 45 s, `OGRE_TAP_MS=3000` | 14 taps | 1 display list, no RSP task, `bootstate=0xBF880415` |
| `progress.cjs`, 4 x 70 s (web, taps Enter) | 4 boots | `maxTasks=1` on all four |

So the taps *do* reach the game — they moved the boot state from the
`0xBF880415` stall to `0x00000060` and started the audio path — but they are not
what gates frames. The web build, with its own input pump, failed to reach a
real display list on 4/4 attempts in this session, where session 21's six
attempts reached 1, 9, 19, 19, 35, 39. The distinct stall points
(`0xBF880415`, `0x00000060`) and the run-to-run spread say this is a **race in
the game's boot**, not a renderer or input problem, and it is shared by both
platforms.

### Finding: the stall is localised — the frame/RSP worker never gets a message

With `OGRE_DEBUG_TRACES=1` the whole 25 s message-queue trace is only five
successful sends:

```
T=147  do_send OK queue=0x800C6490 msg=0x800E8B10 count=1/1   (PI-manager handshake)
T=177  do_send OK queue=0x800E8B4C msg=0x800E7D90 count=1/8   (boot frame -> t17)
T=178  do_send OK queue=0x800E8BBC msg=0x0000029B             (SP done)
T=182  do_send OK queue=0x800E8BF4 msg=0x0000029C             (SI done)
T=193  do_send OK queue=0x800E9BA8 msg=0x800E7D90 count=1/8   (boot frame -> t5)
```

and the per-thread exit table:

```
t1  last func 0x80099740   (PI/DMA busy-wait)
t3  last func 0x800901C0   (osCont family)
t4  last func 0x80089054   (osSetEventMesg wrapper, func_8008AFE0's queue loop)
t5  last func 0x80089540   (queue worker: osCreateMesgQueue + osRecvMesg)
t16 last func 0x80089358   (RSP task thread: osRecvMesg + osSpGetStatus)
t17 last func 0x800901C0   (osCont family)
t18 last func 0x80089200   (RSP task thread)
t19 last func 0x800891A0   (VI-retrace worker, receives 0x29A ~60/s)
```

What this establishes:

* The VI path is healthy: `[vi-debug] retrace -> mq=0x800E8B84 msg=0x0000029A`
  fires at ~60 Hz and t19 consumes it, so "the VI thread is stalled" is not the
  story (session 21's measurement said the same).
* `func_8008AFE0` (thread 4, priority 50) creates its own queue
  `0x800C4C28` (buf `0x800BE1A0`, count 8), registers an event (`func_80089054`
  with type 3), and then blocks on `osRecvMesg` forever. **Nothing ever sends to
  it** — zero sends in 25 s of trace, and it re-blocks ~45 times/s.
* The frame message `0x800E7D90` is delivered exactly once, at T=193, to
  `0x800E9BA8`; t16's RSP thread (`func_80089358`) is blocked on a *different*
  queue (`0x800B9C40`) and never runs its `osSpGetStatus` → submit path again.
  Since gfx tasks are submitted by these RSP threads, that is why exactly one
  display list exists.
* `func_8008AFE0`'s thread is created from `func_80071EB0` (the boot/init
  function, via `func_8008A1B0` → `func_8008B0B0`), so the worker exists from
  boot init and is simply never fed.
* t16's resume counter is stuck at 3 across every snapshot (`t17=3`,
  `t16=3`), i.e. it has not been scheduled since boot.

### Correction: `[snap]`'s `bootstate(D_800AEF98)` is read as a word, but the game stores a byte

`func_80071EB0` (the boot/init function) writes `D_800AEF98` with `sb`:

```
/* 23EC 80071FEC */  lui  $at, %hi(D_800AEF98)
/* 23F0 80071FF0 */  sb   $zero, %lo(D_800AEF98)($at)
```

and every other reference in `asm/1060.s` is `lbu`/`sb`. The snapshot prints it
with `snap_w` (32-bit), so `bootstate=0xBF880415` is one byte of game state plus
three bytes of adjacent memory — the low byte (`0x15`) is the state, the rest is
noise. It is still useful as a coarse "which stall point" fingerprint (it is
reproducibly `0xBF880415` or `0x00000060`), but it must not be read as a word
value. The other fields on that line come from globals whose access width has
not been checked either; treat the line as a fingerprint, not as decoded state.

### Verified

| check | result |
|---|---|
| `cmake --build build-app -j 8` | clean (only the pre-existing sdl2-compat deployment-target link warning) |
| interactive run (no env) | unchanged: window opens, `api=3`, boot DL 1, stalls at ~3 display lists |
| `screencapture -l <window id>` on a live run | works (2560x1496 PNG); the canvas area is black on a stalled boot (`colorful=0`), which matches "one blanking DL" |
| bounded run | `OGRE_EXIT_AFTER_MS=60000` exits on time and prints the per-thread table |

### What this does not establish

The **taps' effect on the boot trajectory** is not proven causal. They changed
`bootstate` and started the audio path in one run, but the next run with
*different* tap timing stalled earlier, so the input tap and the boot race are
still confounded; a controlled A/B (same boot count, taps on/off, several runs
each) is the way to separate them.

---

## 2026-09-11 (session 24, native bring-up) — the native RT64 build works on macOS: two SDL-glue bugs

### Context

The web build had become the project's only renderer: `app/src/web_renderer.cpp`
is 3 355 lines against `app/src/renderer.cpp`'s 357, and sessions 19-24 were all
web-renderer work. The plan was to move the renderer phase to Linux/Vulkan
(`docs/guides/linux-migration.md`), partly because the macOS window path was
believed to be blocked. Before committing to that, the native app was brought up
on the current machine (macOS 15.7.9, x86_64, Metal).

It runs. RT64 initialises its Metal backend (`api=3`, `AMD Radeon Pro 560X`), the
window opens, the game boots and stays up. Two bugs in the app's own SDL glue
were in the way, both macOS-only.

### Bug 1: `create_window` handed RT64 the SDL_Window*, not the NSWindow*

`renderer.cpp` fills `RT64::Application::Core::window` from
`ultramodern::renderer::WindowHandle`. On Apple that struct is
`{void* window; void* view;}`, and plume casts `window` straight to an
`NSWindow*` - `plume_apple.mm`'s `CocoaWindow::updateWindowAttributesInternal`
does `[[nsWindow contentView] frame]` and `[nsWindow screen]`. `create_window`
passed the `SDL_Window*` and left `view` null, so the first main-thread event
pump died in `objc_msgSend` with `EXC_BAD_ACCESS (code=1, address=0x18)`.

The null `view` was a deliberate stub whose comment said `SDL_Metal_GetLayer`
"segfaults with Homebrew's sdl2-compat". That does not reproduce: a standalone
probe (`SDL_Init` -> `SDL_CreateWindow(SDL_WINDOW_METAL)` -> `SDL_GetWindowWMInfo`
/ `SDL_Metal_CreateView` / `SDL_Metal_GetLayer`) returns valid pointers for all
three under sdl2-compat 2.32.70.

`create_window` now takes the Cocoa window from `SDL_GetWindowWMInfo` and the
CAMetalLayer from `SDL_Metal_CreateView` + `SDL_Metal_GetLayer`, and owns the
`SDL_MetalView` in `Platform` so it is released before the window.

This is *not* what RT64's own `ApplicationWindow::create` does - it sets
`windowHandle.window = sdlWindow` as well. The frontend is expected to supply the
handles, which is what this app does.

### Bug 2: `poll_input` pumped SDL events from the game thread

```cpp
static void poll_input() {
    SDL_PumpEvents();   // "Safe to call from any thread" - it is not
}
```

ultramodern calls `poll_input` from `osContStartReadData`, i.e. the game thread.
On macOS `SDL_PumpEvents` -> `Cocoa_PumpEvents` -> `[NSApp nextEventMatchingMask:]`
raises

```
NSInternalInconsistencyException: 'nextEventMatchingMask should only be called from the Main Thread!'
```

which terminates the process: 2 of 3 runs died there, immediately after the boot
display list. The main thread already pumps every frame through
`pump_sdl_events` (from `update_gfx`), and reading SDL's keyboard/controller
*state* off-thread is fine - only pumping is not. The call is gone; 3 of 3 runs
then survived 45 s.

### Verified

| check | result |
|---|---|
| `cmake --build build-app` | clean (one pre-existing sdl2-compat deployment-target link warning) |
| RT64 setup | `[renderer] RT64 renderer initialized (api=3)`, `Device Name: AMD Radeon Pro 560X` |
| window | opens (1280x748); captured with `screencapture -l <window id>` |
| stability | 3/3 runs alive after 45 s (1/3 before bug 2) |
| display lists | 1 per run: `type=1 ucode=0x8009F540 data=0x800C6500` - the boot blanking DL |
| canvas | black |

### What this does and does not establish

The **environment is not a blocker**: macOS + Metal + sdl2-compat is a workable
native platform, so `linux-migration.md`'s premise (switch for the renderer phase
because macOS is blocked) no longer stands on its own. Linux is now a preference
- RT64's most-tested backend is Vulkan - not a requirement.

The **game-side stall is unchanged and platform-independent**: every native run
submits only its boot blanking display list and then idles, exactly like the web
build. The web probes get past that wall by pressing Enter every 5 s
(`debug/lib/harness.cjs`, `tapMs: 5000`), and session 17 found that input advances
the title. These native runs sent no input, so the black canvas is expected and
is not evidence of a renderer failure.

Two diagnostics were added to `renderer.cpp`: the RT64-init line now goes to
stderr (it was `printf`, and stdout is block-buffered when piped, so a successful
setup looked like a silent failure), and `send_dl` logs the first few display
lists and then every 50th - nothing on the native path reported whether the game
submits gfx tasks at all.

---

## 2026-09-11 (session 24) — the "green smudges": texture bytes read at the wrong offset, and a blender cycle folded when it should cancel

### Symptom

The twelve title-screen soldiers rendered with green and purple speckle
"smudges" over the helmet area, and a dim colour cast everywhere. The sprites
were recognisable and their palette was broadly right, so this read like a
combiner, filtering or blending problem. It was two separate bugs: a texture
decode that read the wrong byte, and a blender cycle whose overflow simulation
should not have applied.

### Finding: `decode_texture_rect` read texture bytes at their *logical* offset

The runtime's rdram stores every N64 32-bit word byte-reversed, so the logical
byte at address `a` lives at physical `a ^ 3` (`recomp.h`'s `MEM_B`). RT64's TMEM
load states the rule exactly (`hle/rt64_rdp.cpp`, `loadWord`):

```
TMEM[(tmemAddress + i) ^ tmemXorMask] = RDRAM[(textureAddress + i) ^ 3];
```

`decode_texture_rect` used a raw `rd16()`/`p[0]` at the logical offset.
`read_matrix` has always dodged this with its `c ^ 1` column index and session 23
added `n64h`/`n64b` for `Vtx`; the texture decoder was the one place left. Two
distortions followed, and only the second is obvious:

- **16-bit texels** (TEXEL1, the RGBA16 colour): `rd16()` at logical offset `o`
  returns the halfword at `o ^ 2`, i.e. the *adjacent* texel, so every horizontal
  pair came out swapped. Harmless inside flat regions, wrong wherever the art has
  detail.
- **I4/CI4 bytes** (TEXEL0, the 32x34 mask): a byte read at logical offset `b`
  returns logical `b ^ 3`, so the four bytes of every word came back reversed -
  eight texels per group, in 2-texel units. The mask's alpha therefore landed in
  the wrong places and uncovered colour texels the mask exists to hide. OB64's
  colour data really does carry green under the mask, which is where the green
  smudges came from.

The mask's alpha *histogram* is identical before and after (the bug is a
permutation, so the multiset of nibbles is preserved) - only the spatial
arrangement changes. Comparing histograms would have missed this entirely.

### Decision: apply RT64's byte rule in the texture decoder

`n64be16(rdram, addr)` joins `n64h`/`n64b` next to `rd16`. `decode_texture_rect`
reads I4/I8 via `n64b` and 16/32-bit texels via `n64be16`, and `OP_LOADTLUT`
reads its entries the same way. Nothing about the tile model changes - sizes,
strides, formats and rects are untouched; only the byte index is compensated.

### Finding: the viewport was correct only by accident, and its comment said the opposite

`set_viewport` read `rd16(rdram_ + addr + i * 2)` and then took x from index 1, y
from index 0 and z from index 3. Because the raw reads return the other halfword,
the local array held `[y, x, 0, z]` and those index choices undid the swap: the
code logged `vscale=[480 640 0 511]` and was right. The comment, though, asserted
that `Vp_t` is laid out `(y, x, pad, z)`. That is not true - it is `(x, y, z, 0)`
and OB64 submits `[640 480 511 0]` - and a reader who believed it would have
"fixed" the index juggling into a transposed viewport.

It now reads through `n64h()` with the canonical indices. Equivalent, and the
`[GFX-VP]` log shows the same transform with the swap gone:

```
before: vscale=[480 640 0 511] -> scale=[160.00 120.00 127.75]
after:  vscale=[640 480 511 0] -> scale=[160.00 120.00 127.75]
```

### Finding: the blender's overflow simulation folded a cycle that cancels exactly

With the decode fixed the decoded pair was clean but the rendered sprite was
still a dim, speckled mess, so the fault had to be downstream of the sampler.
`[GFX-UV]` ruled the sampler out (`k1` found, a 20x34 colour texture bound to
unit 1, `scl1=(0.0500 0.0294)`, vertex `uv1=(0.000 0.000)`, `glGetError` clean),
and `[GFX-CMD]` gave the sprite's exact state:

```
cyc=2 omh=0x182CF0 oml=0x00184240 cmb=0xFCFFFFFFFFFD7238
mux0=[rgb=(ZERO-ZERO)*ZERO+TEXEL1 a=(ZERO-ZERO)*ZERO+TEXEL0]
mux1=[rgb=(ZERO-ZERO)*ZERO+COMBINED a=(ZERO-ZERO)*ZERO+COMBINED]
prim=[1.00,1.00,1.00,0.00] ablend=1 force=1 approx=0 ccyc=2
b0=[P=CC M=CC A=CC_A B=ONE]   b1=[P=CC M=FB A=CC_A B=1MA]
```

So the combiner is `rgb = TEXEL1, a = TEXEL0` - correct - and the colour goes
through the blender. That mode has `forceBlend` set with a 2-cycle combiner, so
`blendCycleCount` is 2 and cycle 0 (`P=CC, M=CC, A=CC_A, B=ONE`) is evaluated
before cycle 1 does the framebuffer blend. Cycle 1 feeds cycle 0's colour
through as its `CC` input, so cycle 0 must be an identity.

It is: `(P*a + M*b) / (a + b)` with `P == M` is exactly `P`. But our cycle is a
field-for-field port of RT64 `Blender::runCycle`, which wraps the numerator with
`fmod(numerator, 1 + 8/255)` to simulate the hardware's accumulator overflow -
and that fold is applied unconditionally. With `P=M=CC` and `B=ONE` the
numerator is `CC*(a+1)`, which exceeds `1.031` for ordinary colours, so the fold
fires on almost every pixel. Because the fold is per channel, it changes hue:

```
mask alpha=0.0: orange(255,166,33) -> (255,166, 33)
mask alpha=0.1:                    -> ( 16,166, 33)   green
mask alpha=0.5:                    -> ( 80,166, 33)   green
mask alpha=1.0:                    -> (124, 34, 33)   dark red
```

Red is the first channel to exceed the limit, so mid-alpha pixels lose red and
come out green, and fully opaque pixels come out about half brightness. The mask
histogram shows most of the helmet/detail texels sit at intermediate alpha, which
is exactly where the green appeared.

Note this is not a porting slip: RT64's `runCycle` computes a `numeratorOverflow`
flag (`!passthrough && (B != B_ONE_MINUS_A)`) and then never uses it. RT64
already special-cases the analogous identity - `(P == M) && (B == B_ONE_MINUS_A)`
- as a passthrough, so treating `P == M` as an identity is consistent with its
own intent.

### Decision: skip the fold when `P == M`

`blender_run_cycle` now computes the numerator and applies `mod(..., Overflow)`
only when `P != M`. When `P == M` the division cancels the numerator exactly, so
the fold can only ever corrupt the result and skipping it cannot change any
correct output. The fold is left in place for `P != M`, where the pinned RT64's
behaviour is a separate question (the same reasoning suggests it should not fold
there either, since a weighted average of two in-range inputs is in range - the
example above with `P=CC, M=0.5, a=b=1` folds `1.5` to `0.234` instead of
`0.75` - but nothing in this scene exercises it, so it is left alone and flagged
here rather than changed blind).

### Verified

| check | before | after |
|---|---|---|
| `textures.cjs` mask silhouette | ragged, 8-texel block steps, detached fragment | smooth plume + figure |
| `spritecheck.cjs` rendered side | dim green/purple speckle, no resemblance to the composite | the same character as the composite (orange body, white plume, green helmet band, magenta sash) |
| full settled canvas | tangle of green/purple/blue slivers | twelve readable soldiers in two mirrored clusters of three |
| `shots.cjs` settled `nonBlack` | `239425` | `239425` (unchanged - only the colours changed, so this is not a brightness effect) |
| `testdraw.cjs` | `nonBlack=12312 colorful=10237` | unchanged |
| `logmode.cjs` | PASS | PASS |
| `[GFX-VP]` scale | `[160.00 120.00 127.75]` | unchanged |

This closes session 23's open item 0: a single isolated sprite now matches its own
`colour x mask` composite.

Still open from session 23: raw TEXEL1 (`--flags 9`) was reported to sample black
while its decoded image is bright. That was not re-measured here, and with the
blender fixed the composite comparison no longer shows a discrepancy, so it may
have been an artifact of the isolation probe (which compares the *first* sprite
against the *last* textured draw's texture pair - two different sprites). It
needs a measurement before it is either fixed or dismissed.

---

## 2026-09-11 (session 23, follow-up) — the rectangle commands were decoded word-swapped, so nothing ever cleared the canvas

### Finding: `G_FILLRECT`/`G_TEXRECT` take `lrx/lry` from w0 and `ulx/uly` from w1

`gDPFillRectangle`/`gSPTextureRectangle` emit the lower-right corner in **w0**
and the upper-left in **w1**; RT64 `GBI_RDP::fillRect`/`texrect` read `p0` for
the lower-right and `p1` for the upper-left. This renderer read the upper-left
from `w0`, so a rectangle from `(0,0)` to `(w-1,h-1)` - a full-screen clear, the
most common rectangle there is - decoded as *inverted* and was dropped by the
empty-rect guard session 20 added.

Session 20's note ("OB64's title DL contains a TEXRECT with xl=319, yl=239,
xh=0, yh=0 - decoding that span as a quad produced a full-screen black cover")
is that same mis-decode seen from the other side: the inverted rect *was* the
real full-screen rect.

Consequences: the game's per-frame `G_FILLRECT` clear and its full-screen fade
rect were both discarded, so **frames accumulated on the canvas** - the title
sprites looked like thin streaked columns because many frames of a slowly
changing scene were stacked on top of each other.

### Decision: decode rectangles the way the SDK and RT64 do, and carry the extra rules

`G_FILLRECT` and `G_TEXRECT`/`G_TEXRECTFLIP` now read `ulx/uly` from `w1` and
`lrx/lry` from `w0`. Two related rules came with them:

- a rectangle samples **its own tile** (`G_TEXRECT` w1 bits 24-26), not
  `G_TEXTURE`'s - RT64 `RDP::drawTexRect` takes the tile as a parameter, so the
  draw scopes `active_tile` to the rectangle's;
- fill/copy mode rounds the origin down and the far corner up to the 4-pixel
  group (RT64 `RDP::fillRect`'s `lrx |= 3`/`lry |= 3` and `RDP::drawRect`'s
  `ulx &= ~3`).

`test_draw`'s synthetic list had been built with the swapped layout (it put both
corners in w1), which is why it kept "working" until this fix and then drew
nothing; it now emits the SDK layout.

### Note: the reference screenshot is a zoomed capture

The reference frame from a real session that this work was checked against is
**zoomed**, so it is only valid for zoom-invariant comparisons: layout, palette,
and whether a sprite is recognisable. Its pixel counts and absolute scale say
nothing about our render. The scene's layout does match it (two mirrored groups
per side, three characters above and three below in a diagonal).

### Finding: the intro fades in from black, so the first frame is not a fidelity measure

The full-screen rectangle drawn *after* the sprites each frame is a fade whose
alpha is `G_SETPRIMCOLOR`'s (`rgb = PRIM`, `a = PRIM`, `FORCE_BL`). Every probe
that captures "the first rendered frame" therefore captures a black frame.

Measured over 48 display lists the sprites never change: the first quad stays
0.1463 x 0.3253 NDC (23.4 x 39.4 px), the vertex UV range stays 0..19 x 0..33
texels, and the tiles stay a 32x34 I4 mask plus a 20x34 RGBA16 colour. The
bounding box that appeared to "grow" was the fade revealing a static picture.

### Decision: probes must wait for a fresh display list, not a fixed delay

`debug/probes/spritecheck.cjs` now (a) waits for the `exec: task=` counter to
advance after it changes the debug flags - the game stalls for seconds and a
stale canvas reads a faded (black) frame - and (b) compares the rendered sprite
against `ogre_gfx_debug_last_tex`, the exact pair the last textured draw
sampled, because `ogre_gfx_debug_tex`'s ring of decodes spans frames. It also
takes `--settle MS`.

---

## 2026-09-11 (session 23) — the title sprites are decoded the way the RDP samples them

### Finding: every vertex field was read with the wrong byte order

The runtime stores each N64 32-bit word byte-reversed and compensates with
`MEM_W` (native load), `MEM_H` (`^ 2`) and `MEM_B` (`^ 3`) in
`N64Recomp/include/recomp.h`. So the 16-bit field at logical offset `o` lives at
`o ^ 2`, and a direct `rd16(p + o)` returns the *other* half of the word.
`read_matrix()` has always applied that compensation through its `c ^ 1` column
index; `OP_VTX` did not, so every `Vtx` was decoded as
`(y, x) (flag, z) (t, s) (a, b, g, r)` instead of `(x, y) (z, flag) (s, t)
(r, g, b, a)`.

That single mistake transposed the sprite quads (39x23 where the sheet is
20x34) *and* their texture coordinates, which is why the title sprites looked
like scrambled slivers while the ring they form still looked like a ring. It had
survived five sessions of renderer work because a near-symmetric composition
hides a transpose.

### Decision: one `n64h()`/`n64b()` helper pair, used with named N64 offsets

`gbi.hpp`'s `rd16()` reads a halfword out of the byte-reversed buffer; the new
`n64h(p, off)`/`n64b(p, off)` in `web_renderer.cpp` apply the `^ 2`/`^ 3` rule.
`OP_VTX` now reads `n64h(p, 0/2/4/6/8/10)` and `n64b(p, 12..15)` — the N64 `Vtx`
layout as written — so a future field addition cannot reintroduce the transpose
silently.

### Decision: apply `G_TEXTURE`'s scale, and only to vertices

RT64 `RSP::setVertexCommon` computes `tc = (s * sc) / (65536 * 32)` with `sc`
zero-extended from the 16-bit field; OB64's title sprites use `0x8000` (0.5), so
the renderer was sampling twice the texture it should have. `RenderState` now
carries `tex_scale_s`/`tex_scale_t` and `transform_vertex` applies them.
Rectangles deliberately do **not** apply the scale: RT64's `RDP::drawRect` uses
`uls/32` and `lrs/32` straight. The field must also be read *unsigned* (the
session-21 code cast it to `int16_t`, turning `0x8000` into `-32768`).

### Finding: I8/I4 texels must carry their intensity into alpha

RT64's `I8ToFloat4`/`I4ToFloat4` return `i` for all four channels. The decoder
set alpha to 255 for I formats, so a mask sampled as `TEXEL0_ALPHA` was
constant `alpha = 1` and did nothing. Fixed, and `RGBA16` now uses RT64's
`(c << 3) | (c >> 2)` replication rather than `c * 255 / 31` so the two
renderers agree bit for bit.

### Decision: port `rt64_blender.h`, do not guess the blend from `oml`

The blend was approximated from `othermode_l & 0xFFF` with a two-entry
heuristic that read the wrong bits, so OB64's title sprites
(`oml = 0x00184240`, `FORCE_BL` set, `M1 = FRAMEBUFFER_COLOR`, `B1 =
ONE_MINUS_A`) were drawn with blending **disabled** and the alpha mask thrown
away.

`Blender` now mirrors RT64: `OtherMode::blenderInputs` (L bits 16–31, packed
`P1 P0 A1 A0 M1 M0 B1 B0`), `checkEmulationRequirements` (including both
approximations), `usesAlphaBlend`, and a field-for-field port of
`run`/`runCycle` into the fragment shader. GL blending is enabled exactly when
`usesAlphaBlend` is true, with `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` — RT64 uses
dual-source blending with the factor in the secondary output, which is the same
thing here because the primary output's alpha carries that value and the canvas
has no alpha channel.

The title render mode decodes to the classic two-cycle XLU:
`b0=[P=CC M=CC A=CC_A B=ONE]`, `b1=[P=CC M=FB A=CC_A B=1MA]`.

### Decision: decode a tile's image from the *render tile*, not from the load

The RDP samples TMEM through the render tile, whose `fmt`/`siz`/`line` need not
match the load's. OB64's mask is the proof: it loads 16 bytes/row with 8-bit
addressing (the standard `gDPLoadTextureBlock` idiom) and then declares the
render tile as **I4**, making the sampled image 32x34, not 16x34 (each byte is
two 4-bit texels). Decoding the load's rect with the load's format produced a
thin silhouette that did not match the character at all.

`RenderState` now keeps a *pending load* (set by `G_LOADTILE`/`G_LOADBLOCK`,
claimed by the next `G_SETTILE` that names a different tile) and each
`TileState` carries the source address and a lazily decoded image.
`ensure_tile_image()` decodes the tile's rect at the tile's format with
`line * 8` as the row stride — the same three inputs RT64 uses (`RDPTile` +
`GPUTile`) — and queues one upload per key per display list. This replaces the
`recent_loads[2]` "the last two loads are TEXEL0/TEXEL1" model, which paired
the images correctly but decoded them wrongly.

`decode_texture_rect()` now takes an explicit `row_bytes`; the old
`width * bytes_per_texel` was only accidentally right when the load's format
matched the tile's. The stride itself comes from the **SETTIMG** state at load
time (`bytesPerRow = width << siz >> 1`, RT64 `loadTileOperation`), not from the
tile's `line`: `line` is the TMEM stride the load writes with, which is the same
number here (`line = 2` words = 16 bytes vs `16 << 1 >> 1` = 16) but is a
different quantity.

### Finding: the synthetic `test_draw` probe had been drawing black

`test_draw` wrote its scratch display list and texture at `0x1FE00000`, and
`resolve_address()` (RT64 `fromSegmented`) reads the top nibble of an address as
a *segment index* — so the texture address resolved through segment 15 (base 0)
to `0x00E01000`, zeroed memory. Its combiner word also set the cycle-1 C
selector to ZERO, which evaluates to 0. The probe reported "rendered" while
drawing a black rectangle, so it proved nothing about the texture path.

Scratch memory moved to `0x00C00000` (segment 0, above the N64's 8 MiB, below
the 32 MiB walk-escape threshold) and the combiner is now the SDK's
`G_CC_TEXEL0` (`rgb = (TEXEL0 - 0) * SHADE + 0`). `testdraw.cjs` now shows the
four quadrants of the synthetic 2x2 RGBA16 texture, so it is a real isolation
test again. Note that color selector C has no `ONE` (`15` is `K5`, which the
shader returns as 0); "TEXEL0 only" has to be `A=TEXEL0, C=SHADE` with a white
shade.

### Decision: verify a sprite by rendering one and comparing it with its textures

Screenshots were actively misleading here (a 20x37 character at 1:1, through a
2x canvas and a JPEG re-encode, reads as noise). Two probes were added instead:

- `debug/probes/textures.cjs` dumps the last 24 decoded images (a frame's
  `mask, colour` pairs) as RGB and alpha views, plus the `colour × mask`
  composite of each pair.
- `debug/probes/spritecheck.cjs` sets `ogre_gfx_debug_flags(2)` so the renderer
  draws only the first sprite, reads the canvas back, and writes the rendered
  crop beside that composite. Matching means the whole path
  (vertex → decode → combiner → blender → GL) is right.

Two debug entry points exist for this and only this: `ogre_gfx_debug_tex(i,
&w, &h, &tile, &fmt, &siz)` and `ogre_gfx_debug_flags(bits)` (`1` = ignore
alpha blending, `2` = draw only the first sprite).

---

## 2026-09-11 (session 22) — game audio is muted by default

### Decision: silence the output, keep the ring draining

The audio microcode is not emulated (the RSP audio task is only auto-completed),
so the samples the game hands to the AI are not real audio: playing them is a
wall of screeching. The page therefore **does not play game audio unless it is
asked to**.

Two things had to stay exactly as they were, because the audio path is
load-bearing for the game's flow (session 15 had to unblock it to get the boot
moving):

- the **AudioWorklet is still created, connected and draining** the ring buffer,
  so the game's `get_frames_remaining()` sees identical backpressure;
- the ring bookkeeping in the worklet (`tail`, `readPos`, the underrun snap) runs
  unchanged - a muted worklet only zeroes the samples it writes to the output.

Silencing at the *page* layer rather than in the app/runtime means no wasm
rebuild, no change to the game's view of the host, and no risk of re-breaking
the boot path that the audio plumbing feeds.

### How to enable it

`?audio` on the page URL (also `?audio=1`), or at runtime:

```js
window.ogreAudio.enabled()        // false by default
window.ogreAudio.setEnabled(true) // unmute (screeches until the audio ucode runs)
```

The page logs the state on connect (`game audio MUTED - add ?audio to the URL to
hear it`) and still logs when the game starts queueing samples
(`game is queueing audio (N frames buffered, muted)`), so a silent page is
distinguishable from a stalled audio path.

### Why this is a deferral, not a fix

Making audio real is Phase 6 work with two independent halves: run the game's
audio microcode (RSPRecomp/`rsp_callbacks` instead of the auto-response that
satisfies the task), and only then is `queue_audio_samples()` fed actual PCM.
The switch above is exactly what that work needs: unmute and listen.

---

## 2026-09-11 (session 21) — `G_SETOTHERMODE_H/L` never applied anything; the combiner is now evaluated the way RT64 does

### Finding: the othermode decode double-shifted its data, so OTHERMODE_H stayed 0

`G_SETOTHERMODE_H`/`_L` carry the field's **already shifted** data word (the SDK
macro emits `val << sft`), and the RSP only clears the target field:
`H = (H & ~(((1 << len) - 1) << sft)) | w1` (RT64 `RSP::setOtherModeH`). The
renderer instead computed `((w1 & mask) << off)`, i.e. it masked the data down
to `len` bits *and* shifted it again, which zeroed every bit of every field. The
observable consequence: `OTHERMODE_H` was always 0, so **every draw was
`G_CYC_1CYCLE`** with no dithering, no texture filtering and no perspective
correction, whatever the game asked for.

With the decode fixed, OB64's title sprites report
`omh=0x182CF0` = `G_CYC_2CYCLE` + `G_TF_BILERP` + `G_TP_PERSP` +
`G_AD_DISABLE` + `G_CD_DISABLE`, `oml=0x00184240`. The same fix is what made the
combiner's mux question answerable (below): the sprites *are* a two-cycle
combiner, so the second-cycle path is exercised on every title frame.

### Decision: evaluate the combiner exactly as `rt64_color_combiner.h` does

The session-19 selector decode was position-independent and therefore wrong for
the top of every mux field (color selector 6 is `ONE` for A/D, `KEY_CENTER` for
B, `KEY_SCALE` for C; `8..15` are C-only alpha-ish sources; alpha D reads `H` in
both cycles, not `L` in the first). The renderer now mirrors RT64's
`parseColorInput*`/`parseAlphaInput*` bit fields, `colorInputA/B/C/D`,
`alphaInputABD/C` and `runCycle` field for field, including:

- **which mux a cycle type evaluates** — 1-cycle evaluates the *second* mux
  (`run()` calls `runCycle(inputs, twoCycle ? 0 : 1, twoCycle, ...)`), 2-cycle
  evaluates mux 0 then mux 1, and `G_CYC_COPY` bypasses the combiner entirely
  (`texel0` passthrough);
- the **TEXEL0/TEXEL1 value swap** in the second cycle of a two-cycle combiner;
- the **wrap** rules for the second cycle's `COMBINED` inputs
  (`wrapInputC`/`wrapInputABD`, then `wrapClamp`) rather than a plain clamp.

Evidence that this matters for OB64: the sprite combiner is the single ROM word
`0xFCFFFFFF 0xFFFD7238` (file offset `0x1EE828`), which decodes to
`cycle0 = rgb TEXEL1, alpha TEXEL0` and `cycle1 = COMBINED` — i.e. a real
two-cycle combiner whose colour comes from the second texture tile and whose
alpha mask comes from the first. All of it is now visible in the `[GFX-CMD]` log
as an N64 expression per cycle.

### Finding: OB64 loads every image into tile 7 while `G_TEXTURE` selects tile 0

`[GFX-TEX]` (tile index added this session) shows every `G_LOADTILE`/`G_LOADBLOCK`
targeting **tile 7**, while the game's `G_TEXTURE` names **tile 0**
(`D7000002 80008000`). That is not a decode bug (RT64 decodes both the same way);
all eight tiles in OB64's state block are programmed identically with `tmem = 0`,
so the tile index does not select the image — the *load order* does. The RDP
samples TMEM, and the pair of loads per object (an I8 alpha mask, then the
RGBA16 colour image) overwrite the same TMEM region.

### Decision: model the two texel units as the last two loads

A draw needs two images (the combiner takes colour from TEXEL1 and alpha from
TEXEL0), so `RenderState` keeps `recent_loads[2]`: `[1]` = most recent load,
`[0]` = the one before it, with its own UV transform (loaded size + `uls/ult`
origin). The shader gets `v_uv0`/`v_uv1` and two sampler units, and an unloaded
unit binds a 1x1 white fallback instead of texture 0. This replaces "the last
loaded texture is the only texture", which silently sampled the colour image for
TEXEL0 as well. (A real TMEM model — per-tile tmem address, line and the tile's
`uls/lrt` rect as the UV basis — is the principled replacement; see the handoff.)

### Decision: the walker and the executor share one address resolver, and the walker tracks segments

`gbi::walk_dl` picked the next command while `WebGLRenderer::exec_command`
decoded it, but each had its own copy of `resolve_address` and only the executor
applied `G_MOVEWORD G_MW_SEGMENT`. The walker's segment registers therefore
stayed zero, so a `G_DL` to a segmented address was followed as if it were
physical — the walker and the executor could disagree about where the walk *is*.
Both now call `gbi::resolve_address` and `gbi::set_segment_from_moveword`.

---

## 2026-09-11 — the `/tmp` browser probes are consolidated into `debug/`

### Decision: keep the probe harness in the repo, not in `/tmp`

Every browser session so far drove the wasm build with one-off Playwright
scripts under `/tmp/ogre-probe` (18 files, ~1 200 lines), each hard-coding the
Playwright install at `/Users/momo/dev/fatos/node_modules/playwright`, the ROM
path, and the `:8931` URL. They were never committed, and loss had already
started: session 16's `server.py` was gone from disk while its process was still
serving `:8931`, and the base `probe.cjs` that `probe-delay.cjs` documents itself
as extending no longer existed.

The reusable part is the harness, not the individual scripts: the boot-retry
rule, the page contract (`#rom-input`, `#gfxstats`, `#gfx-status`,
`#game-canvas`, `#status`, `window.ogreLog`), and the measurement methods. So
`debug/` now holds a shared `lib/harness.cjs`, eight curated probes (`boot`,
`shots`, `fps`, `vi`, `stats`, `diag`, `logmode`, `testdraw`), the reconstructed
COOP/COEP `server.py`, and a Playwright `package.json`. Paths and budgets come
from flags (`--url`, `--rom`, `--out`, `--out-dir`, `--attempts`, `--secs`) or
`OGRE_URL`/`OGRE_ROM`/`OGRE_OUT`; artifacts go to the gitignored `debug/out/`.
See `docs/guides/web-probes.md`.

### Decision: the "idle trajectory" retry lives in the harness, not in each probe

Roughly one in two boots stalls at ~3 display lists (`tasks=1` in `#gfxstats`)
instead of submitting the game's display list (`tasks>=2`). `attemptLoop()`
re-runs the whole boot until a real list arrives; a probe without it reports a
false negative about half the time. That rule, plus reading `window.ogreLog`
instead of the DOM, is the main reason the probes belong in the repo rather than
in a session's shell history.

The one-off investigation scripts (the freeze/locate/tap variants) are not
carried over; their findings are in the session handoffs. Rebuild them from the
harness if those bugs recur.

---

## 2026-09-10 (session 20) — The browser renderer now draws the title scene; seven WebGL2-prototype bugs found (viewport, matrix decode, matrix stack, empty rects)

### Finding: the black screen was seven independent renderer bugs, not a game/runtime problem

`app/src/web_renderer.cpp` had never applied the RSP viewport, was decoding
every `Mtx` with its column pairs swapped, ignored the projection `MUL`, composed
modelview matrices in the wrong order, read F3DEX2's inverted `G_MTX` PUSH bit
literally (so the title DL's per-object matrices accumulated into one another),
hit `push_back(vector.back())` UB once the PUSH path became live, and drew a
`TEXRECT` that decodes as an inverted/empty rectangle as a full-screen cover.
Fixing those takes the title DL from 18/24 triangles rejected and a black canvas
to 0 rejected and a stable ring of twelve character sprites. See
`docs/HANDOFF-2026-09-10-session20.md` for the evidence and the bisection.

### Decision: match RT64's HLE semantics exactly where they are observable

Each fix was validated against `tools/RT64` rather than guessed:
`RSP::setViewport` (viewport field indices), `FixedMatrix::toFloat` (the `j ^ 1`
column swap forced by the runtime's byte-reversed rdram), `RSP::matrixCommon`
(compose `MUL` as `m * old`, keep one composite view-projection matrix),
`GBI_F3DEX2::matrix` (`p0(0,8) ^ pushMask`, i.e. the PUSH bit is stored
inverted - the game's own `0xDA380000`/`0xDA380001` words confirm it), and
`RDP::fillRect` / `FixedRect::isEmpty` (empty and inverted rectangles draw
nothing).

### Deliberately not applied: the `G_TEXTURE` `sc`/`tc` scale

RT64 converts vertex texcoords as `(s * sc) / (65536 * 32)` and OB64 submits
`sc = tc = 0x8000`, so the hardware UVs are half of this renderer's `s/32`.
Applying it visibly truncates the sprites (the vertical span drops below the
loaded texture height), so it is left unapplied and recorded as the top open
question with the per-draw values logged in `[GFX-V]`.

### Decision: no per-line DOM logging; the runtime log lives on `window.ogreLog`

The page used to append every `printf` line to `#status` with
`textContent += line`. Emscripten proxies wasm-thread output onto the browser
main thread, so that O(n) write per line blocked the printing threads and cost
roughly 2x of the frame rate once the log passed 100 KB. Lines now go into a
bounded ring buffer exposed as `window.ogreLog`
(`tail`/`text`/`find`/`contains`/`save`/`clear`/`show`), `#status` renders one
line on a 200 ms timer, and `?log` restores the full console for debugging. Keep
the page free of per-line DOM work whenever wasm threads can print.

---

## 2026-08-29 (session 10) — The VI-thread segfault was a runtime pointer-translation bug; osViSetMode now validates its argument; the GBI question is resolved (auto-detected F3DEX2 is correct)

### Finding: OB64's `osViSetMode` is really the swap-context routine and ignores its argument; the runtime translated a garbage value into an out-of-bounds pointer

The game's `osViSetMode` at `0x80095820` (bridged to the runtime's native
`osViSetMode_recomp`) is actually `__osViSwapContext` — it reads
`__osViNext->mode`/`framep` from its own context and writes the VI hardware
registers, never using its incoming `$a0`. Its callers pass leftover registers:
`func_8009AB50` (VI init, called by `osCreateViManager`) passes the MMIO address
`0xA4400010` (VI_STATUS), and `viMgrMain` passes a leftover message-queue
pointer. On hardware this is harmless.

The runtime's `osViSetMode` translated that argument with `TO_PTR`, producing a
host pointer ~4.58 GB past the start of the 512 MiB rdram buffer
(`rdram[0x124400010]`), stored it as the next VI mode, and the VI thread's
`update_vi()` dereferenced it (`next_mode->comRegs`) → the long-standing flaky
SIGSEGV. It reproduced under both the Intel hasvk driver and Lavapipe, so it is
a general port bug.

### Decision: validate the mode pointer in `osViSetMode`

`ultramodern/src/events.cpp` now translates the mode argument only when it is a
KSEG0 address (`0x80000000..0xA0000000`), which maps 1:1 into the rdram buffer;
otherwise it keeps the current mode (matching real hardware, where the argument
is unused). Also added null-guards in `update_vi()` (fall back to `dummy_mode`)
and `osViSetSpecialFeatures`. Result: the VI-thread crash is gone; under Lavapipe
the port boots, submits and completes 1864 RSP gfx tasks, and renders without a
single crash.

### Finding/Decision: RT64's auto-detected F3DEX2 GBI is correct — remove the session-9 "force F3DEX" experiment

Dumping the game's real display lists settled the open GBI question:
- First gfx task DL: `DE000000 / 000A9EF0` = G_DL branch, `E9000000` =
  G_RDPFULLSYNC, `DF000000` = G_ENDDL — exactly RT64's F3DEX2 map
  (`0xDE`=G_DL, `0xDF`=ENDDL).
- Branch target `0xA9EF0` is standard RDP color/rect setup (`0xFB..0xF7`,
  `0xFC`, `0xF5`); later boot DLs branch to real KSEG0 targets (`0x801869E8`).

So `GBIManager::getGBIForUCode` (which matches OB64's ucode hash to
`GBIUCode::F3DEX2`) is right, and the session-9 "force `gbiCache[F3DEX]`"
override misparsed the DLs (plain F3DEX does not map `0xDE/0xDF`). The override
was removed; `renderer.cpp` documents this.

### Outcome

Boot is stable under Lavapipe (software rasterizer). The remaining crash
(`submitRasterScene` → `libvulkan_intel_hasvk.so`) is specific to this dev
machine's Intel Haswell GPU (the driver itself warns its Vulkan support is
incomplete) and is not a general issue; the next session should run on a
supported GPU. The vendored runtime changes are snapshotted in
`n64modernruntime-ob64.patch` (new).



## 2026-08-25 (session 9) — The `func_8019FC68` DL-build crash was a recompilation bug; N64Recomp now emits fall-through tail calls; boot reaches real rendering

### Finding: KMC shared-epilogue functions lose their register restores

> Session 44: the same phenomenon also produces `jal` targets that are plain tails
> (not epilogues) — e.g. `0x802399AC` (the `0x80239874` cutscene render tail) and
> `0x8023C824`/`0x8023C894` (RDP emitters needing `a3`). `make midfunc` lists them.


OB64's compiler (KMC) merges identical function epilogues into a separate symbol
that the preceding function **falls through into**. `func_8019ABC4`'s epilogue
ends with `lw $ra/$fp/$s7/$s6` at `0x8019AEFC..0x8019AF08` and then falls into
`func_8019AF0C`, a separately-symbolized shared epilogue that restores
`$s5..$s0` and `$sp`. N64Recomp treated the fall-through as the end of
`func_8019ABC4` and never ran the shared epilogue, so the callee-saved
registers and `$sp` were left clobbered. In the title path,
`func_8019FC68` (session-8 crash site) captured a heap pointer in `s0`, called
`func_8019ABC4`, then `free(s0)` — freeing a garbage value (`0x801bc666`) and
SIGSEGV'ing in `func_800712C4`.

### Decision: emit a fall-through tail call for shared-epilogue chains

`N64Recomp::recompile_function_impl` now, after processing a function's
instructions, emits `next_func(rdram, ctx); return;` when the function's code can
fall off the end of its range and the next function starts exactly at
`func.vram + words*4`. "Can fall off the end" means the last instruction is not
`jr`/`j`/`syscall`/`break`/`eret` (nor the delay slot of one of those, which
already emitted a return). Detected 23 genuine cases across all segments.

### Decision: process N64Recomp static functions to a fixpoint and register them by address

Static functions (created for cross-boundary branches) were neither registered in
`context.functions_by_vram` nor created in an order that guaranteed a static's
fall-through target (another static) existed before it was recompiled. `main.cpp`
now registers statics in `functions_by_vram` and processes them in a two-phase
fixpoint loop (create all known statics, then recompile the batch, repeat until no
new statics). This fixed `static_16_8019AB84` → `static_16_8019AB94` (a shared
epilogue chunk), which was otherwise lost. Total fall-through tail calls: 35.

### Outcome

Boot now passes the title-screen DL build (the `func_8019FC68` crash is gone),
runs ~1528 PI DMAs and 64 RSP tasks (including new `type=1` tasks), and reaches
real RT64 frame rendering. The next crash is in the **RT64 render thread** calling
`libvulkan_intel_hasvk.so` (the Intel Haswell Vulkan driver) inside
`FramebufferRenderer::submitRasterScene` — see
`docs/HANDOFF-2026-08-25-session9.md` for leads (driver vs. RT64 GBI vs. the
pre-existing flaky renderer-init race).



## 2026-08-25 (session 8) — The post-boot spin was a cooperative-scheduler deadlock; N64Recomp now emits `yield_self` for poll loops


### Finding: Thread 3's `.L80075FB8` spin on `D_800C4C26` starves the drainer (deadlock)

The session-7 "N64 threads 1+3 spin" is Thread 3 (`func_80075BC0`, the system
loop) waiting in a **pure spin** for the 16-bit word `D_800C4C26` to change
from `0xFFFF`/`0xFFFD`. The word is set to `0xFFFC` by the boot state-machine
callback `func_80072398` after 13 invocations. The callback is driven by the
game's own event chain — Thread 19 (`func_80088F08`) wakes on VI-retrace/AI
messages (`0x29A`/`0x29D` on `D_800E8B84`), dispatches via `func_800891A0` to
Thread 4 (`func_8008AFE0`, queue `D_800C4C28`), which calls the callback. The
VI thread enqueues the messages, but delivery requires the cooperative
**drainer** (pri 5) to run, and Thread 3's pure spin never yields → the whole
chain stalls forever. Same class as session 6's `func_80089A10` spin.

### Decision: make N64Recomp emit `yield_self` for poll loops (general fix)

Instead of reimplementing the spinning function natively (session 6's approach,
impractical for the 1390-byte `func_80075BC0`), N64Recomp's `print_branch` now
detects **poll loops** — backward conditional branches whose body has no
function calls and no stores, and contains a load whose base register is
loop-constant at the load site (cyclic last-write is `lui`, or never written in
the body) — and emits `yield_self(rdram);` before the loop-back goto.
`yield_self` (already in the runtime) waits for one external message and checks
the running queue, so the drainer can deliver the very event the poll awaits.

The heuristic was tuned against the whole codebase: it flags ~17 loops (the
`D_800C4C26` spin, the PI/DP status polls, overlay game-logic polls) while
correctly excluding data loops (strlen/list-walk/memcpy) whose load bases are
written by non-constant ops.

### Outcome

Boot now passes the old spin, runs through the overlay-D data loads, the
controller path, and the RSP pipeline, and reaches **title-screen display-list
building** (`func_801A1FCC`/`func_8019FC68`, overlay C). The next crash is
`func_8019FC68` at `0x8019FFF4`: it stores through `0x803ffa7b +
entry->[0x34]` where a graphics-object entry in the heap array at `0x803fefa0`
has a garbage `[0x34]` (varies per run), wrapping the address to unmapped
RDRAM. The array is allocated + zeroed by `func_801A1A2C`, so the garbage is
written later — root cause open (see session-8 handoff leads: likely an
audio-init / record-population step or a wrong splat function boundary for the
`nonmatching func_8019FC68, 0xF58` region).


## 2026-08-25 (session 7) — Phase 4: streamed overlays are plain linked code (no relocation); A+B+C recompiled and registered

### Finding: the streamed overlays need no runtime relocation

The session-6 handoff framed the next wall as "overlay relocation". That was
wrong. OB64's streamed overlays are **plain linked MIPS code+data** DMA'd to a
**fixed** RAM address (per the streamed-segment table at ROM `0x387C0`). All
pointers inside them (function-pointer tables at e.g. ROM `0x65200`, RDP
display lists, data tables) are absolute addresses already correct for that
load address. The real problem was that overlay *functions* were log-and-return
stubs, so the data structures they build at boot (e.g. the descriptor that
`func_80075BC0` dispatches through) were never created, and the game read raw
code bytes as function pointers (`0x3C028019` = `lui $v0, 0x8019`).

### Decision: disassemble + recompile the overlays into the same ELF, register them eagerly

- Added `streamedA` (ROM `0x3F1B0` → RAM `0x800E9C20`), `streamedB` (ROM
  `0x40E80` → RAM `0x8016AF80`), and `streamedC` (ROM `0x1CE040` → RAM
  `0x80197B90`) as splat `code` segments; bin gaps (`0x66E30`, `0x1F0A00`) are
  pinned to their ROM address as VMA so they don't collide in RDRAM VMA space.
- Recompiled the whole ELF together (807 → ~1520 functions). Cross-overlay
  absolute references (overlay B takes the address of overlay C's
  `.L8019EE70`) need the label exported globally; `tools/fix_cross_overlay_labels.sh`
  re-applies that after every `splat split` (wired into the Makefile).
- The app registers A+B+C via `load_overlays()` from the GameEntry
  `on_init_callback`; the runtime's `recomp::init()` now loads only the base
  sections (`entry`+`main`) so overlays aren't registered at the wrong
  linear-mapped addresses (which would corrupt `section_addresses`, read by the
  recompiled overlay code through `RELOC_HI16`/`LO16`).

### Decision: a `load_overlays` DMA hook was tried and REVERTED

Hooking `func_8008BC40_recomp` (the game's DMA request, reimplemented as a sync
ROM read) to call `load_overlays(dev_off, dramAddr, size)` is the "obvious" way
to register overlays as the game streams them. It fails for OB64 because the
game streams in **0x200-byte chunks**: `load_overlays` computes a bound range
from `rom`+`size`, and a chunk-sized range makes `lower_bound > upper_bound`
(inverted), so the load loop walks off the end of the section table and
segfaults. Registration therefore happens eagerly at boot instead.

### Outcome

The game now boots through the old `func_80075BC0` function-pointer-table crash
and runs ~1520 real functions through the whole streamed-overlay load sequence
before **spinning on N64 threads 1 and 3** after loading the next overlay's data
blocks into RAM `0x801BD930`. No stub calls, no `get_function` hard-fail.
Remaining: find that spin (overlay D at ROM `0x1F0A00` → RAM `0x801F7100` is the
likely next recompilation target), fix the flaky VI-thread segfault, then the
RT64 GBI fix.


## 2026-08-25 (session 6) — The boot stall was a scheduler busy-spin deadlock; three fixes unblock boot into the main loop

### Finding: "waits for a second RSP task" was wrong — the game thread busy-spins and starves the drainer

The session-5 conclusion (callback slots at 0x800B9E84 all zero → the sync task's
completion "does nothing" → the game waits for a second task) was **incorrect** on two
counts:

- The real callback-slot address is **0x800A9E84** (0x800B9E84 was a dump misrecord, off
  by 0x10000). The type-4 slot is registered at boot (`func_8008A1B0` →
  `func_800899D0(func_8008B110)`), and the sync task is **type 0**, which never dispatches
  to any callback anyway — so "dispatch does nothing" is expected, not a bug.
- The actual deadlock: after the sync task, the game thread enters `func_80089A10`, a tight
  `bnez` spin on the task-done counter `D_800E79A4`. The runtime's cooperative scheduler
  (higher priority numbers first) cannot preempt a spinning thread, so the spin (pri 10)
  starves the external-message **drainer** (pri 5) that must deliver the SP/DP completion
  events which eventually decrement the counter.

### Decision: reimplement the spin (`func_80089A10`) as a yielding wait

N64Recomp only emits its `pause_self` yield for *exact self-loops* (`j`/`b` to the
instruction's own address); this spin branches to the *function start*, so it was compiled
with no yield. Rather than special-casing the generator, we added `func_80089A10` to
N64Recomp's `reimplemented_funcs` and implemented `func_80089A10_recomp` in the runtime:
loop while `MEM_W(0, 0x800E79A4) != 0`, calling `wait_for_external_message` +
`check_running_queue` each iteration. This lets the game thread itself drain the SP/DP
completion events and hand off to the higher-priority RSP threads.

### Finding: the game's PI manager is dead, so every DMA deadlocks

`osCreatePiManager_recomp` is an **empty stub**. The game's verbatim `osCreatePiManager`
(0x8008B8C0) — the only writer of `D_800AA400`/`D_800AA408` — is dead code. The game's
DMA-request function `func_8008BC40` checks `D_800AA400` and bails when 0, so the blocking
DMA helpers (`func_80089F80`, `func_8008A0F0`) wait forever on their completion queue; the
first streamed-code load (`func_8009DA50`) deadlocks.

### Decision: reimplement `func_8008BC40` (DMA request) as a synchronous ROM read

`func_8008BC40_recomp` reads the game's `OSIoMesg` (mq@+4, dram@+8, dev@+0xC, size@+0x10),
calls `recomp::do_rom_read`, and completes the request via `osSendMesg` so the caller's
`osRecvMesg` returns immediately. One function fixes all game DMA paths without the PI
thread. **Important**: the ROM copy must go through `recomp::do_rom_read` (which
byte-swaps big-endian ROM bytes into host-order RDRAM words via `MEM_B`); a plain `memcpy`
produces byte-swapped garbage pointers.

### Decision: generic stub for not-yet-loaded streamed functions

`get_function` now returns a logging no-op stub for unknown addresses in the streamed
ranges (`0x8016A000..0x80200000`, `0x84000000..0x84200000`) instead of hard-failing. This
lets boot reach the main loop. It is a bring-up measure; Phase 4 will replace the stubs
with recompiled overlay functions.

### Outcome

The game now boots through the RSP pipeline, controllers, 703 streamed-overlay DMA loads,
and into its main loop (18 distinct streamed functions called, all stubbed). The next
crash is an **indirect call through a streamed-overlay function-pointer table that needs
relocation** (`func_80075BC0` reads `*(u32*)(*(u32*)(0x800E8294))` = `0x3C028019`, a MIPS
instruction, not a pointer). That is the start of **Phase 4 (overlay loading +
recompilation)**. See `docs/HANDOFF-2026-08-25-session6.md`.


## 2026-08-25 (session 5) — RT64 renderer integrated; the boot stalls waiting for a second RSP task

### Decision: replace the null renderer with RT64 (Vulkan on Linux)

The game has been submitting real gfx tasks since session 4, so the null
renderer has served its purpose. `app/src/renderer.cpp` now wraps
`RT64::Application` (the current RT64 HLE architecture with `Interpreter` /
`GBIManager` / `State`) behind ultramodern's `RendererContext`. Wiring copied
from N64Recomp/RecompFrontend's `rt64_render_context.cpp`: `Application::Core`
gets RDRAM, DMEM/IMEM slots, DPC registers, and the VI registers straight from
`ultramodern::renderer::get_vi_regs()`. RT64's render hooks are optional
(null-checked) and not wired.

Verified on the Intel HD 4400 (Mesa): RT64 sets up Vulkan, presents frames at
~60 Hz, and the game boots through the renderer unchanged. This also fixed the
SDL window: it must be created with `SDL_WINDOW_VULKAN` on Linux (and
`SDL_WINDOW_METAL` on macOS) or `SDL_Vulkan_CreateSurface` fails and RT64
segfaults during setup.

### Decision: gfx display lists are interpreted by RT64, not recompiled microcode

The runtime routes gfx tasks (`M_GFXTASK`) to the renderer's `send_dl`, never
through `get_rsp_microcode` — so `RSPRecomp` is **not** needed for the gfx
ucode. RT64 ships its own GBI interpreters (`src/gbi/`) and parses display
lists itself via `loadUCodeGBI` + `processDisplayLists`. RSPRecomp remains for
the **audio** ucode (task type 5) once audio tasks flow.

### Finding: OB64's ucode is "F3DEX fifo 2.08" (short-format opcodes) and RT64 misidentifies it as F3DEX2

Runtime OSTask dump: `ucode=0x8009F540 ucode_data=0x800AC140` →
ROM `0x2F940`/`0x3C540`; the data block contains
`"RSP Gfx ucode F3DEX fifo 2.08 ... Yoshitaka Yasumoto 1999 Nintendo"`.
The game's display lists use the F3DEX **short-format** opcode set
(`G_SETOTHERMODE_H=0xDE`, `G_SETOTHERMODE_L=0xDF`, `G_RDPFULLSYNC=0xE9`, ...).

RT64's `GBIManager::getGBIForUCode` (XXH3 hash database) matches this ucode to
`GBIUCode::F3DEX2` (verified in-app: `GBI matched: 7`). F3DEX2 semantics differ
(`0xDE` = `G_DL`, `0xDF` = `G_ENDDL`), so RT64 misparses OB64's DLs — the first
(sync) DL "branches" into MIPS code at RDRAM `0xA9EF0`. RT64 has no GBI map for
the F3DEX-2.08 short set. **Action item**: add/force the correct GBI before
real DLs flow (see handoff).

### Finding: the boot stalls after the first gfx task — the task-done dispatch has zero callbacks

The whole RSP task pipeline works end-to-end: `osSpTaskStartGo` → `sp_complete`
(→ `0x800E8BBC`) + `send_dl` + `dp_complete` (→ `0x800E8BF4`) →
`func_80089200` sends the task-done message to the task's `done_mq`
(`0x800E9BA8`) → the done pump `func_80089540` consumes and dispatches on the
message's command type. But the dispatch callback slots (`0x800B9E84/88/8C`)
are all **zero** at boot, so the first (sync) task's completion does nothing
and the game waits for a second task that never comes. The game's next boot
steps (`func_8009DA50` streamed DMA, then streamed funcs `0x800E9CEC`/
`0x800E9C20`) are never reached. See the handoff for the thread map and the
investigation path.

### Decision: keep the boot-time diagnostics in the vendored runtime for now

The gitignored runtime carries temporary `[sp]`/`[ev]`/`[mq]` debug prints
(task submission, event registration, SP/DP completion, targeted queue
activity). They are cheap, and removing them needs a full librecomp/ultramodern
rebuild; they are documented in the handoff and should be stripped once the
stall is fixed.

---

## 2026-08-25 (session 4) — First RSP task + real VI mode: boot is past the libultra bridge

### Decision: correct osSendMesg/osJamMesg identification

Session 3 named `0x800935A0 = osSendMesg`, but that function does a **front
insert** (`mq->first = (first+msgCount-1) % msgCount`), i.e. `osJamMesg`. The
game's real back-insert `osSendMesg` is `func_80093810` (36 call sites; the
audio/event request system uses it). Renaming `func_80093810 = osSendMesg`
makes audio-request responses go through the runtime's `osSendMesg`, which
**wakes blocked threads** — the missing piece that let the game thread proceed
past its first audio request.

### Decision: add an external-message drainer thread to the runtime

With VI events being delivered every frame, the boot still deadlocked: the
runtime only drains its external-message queue when a game thread calls
`osSendMesg`/`osRecvMesg`, and OB64's boot blocks every game thread on a queue
fed by the VI retrace event (its main thread busy-spins). Added a hidden
"drainer" game thread in `librecomp/src/recomp.cpp` (spawned after
`on_init_callback`) that loops `wait_for_external_message` +
`check_running_queue`. It must be **lower** priority (5) than the threads it
wakes, because this runtime schedules **higher** priority numbers first —
priority 30 starved the game thread (pri 10) in the running queue.

### Decision: name the osCont family, timers, events, and osSpGetStatus

- The PFS (Controller-Pak) cluster at `0x80096B90+` is *not* the osCont family
  (`__osSumcalc`/`__osIdCheckSum` etc.); the osCont cluster is the SI-touching
  one at `0x800900C0..0x800906C0`. Named `osContInit`, `osContStartQuery`,
  `osContGetQuery`, `osContStartReadData`, `osContGetReadData`.
- Timers: `osGetTime` 0x80094C90, `osSetTime` 0x80094D20, `osSetTimer`
  0x80094D40 (the handoff's `0x80094E34` is the timer *interrupt handler*, not
  osSetTimer).
- Events: `osViSetEvent` 0x80095560 and `osSetEventMesg` 0x80093940 — the game
  registered its VI-retrace→audio-queue message (0x29A) through its verbatim
  osViSetEvent, so the runtime never delivered it.
- `osSpGetStatus` 0x800939F0: the RSP task threads busy-wait on SP_STATUS; not
  dead code as session 3 assumed. Added to N64Recomp's `reimplemented_funcs`
  (N64RecompCLI rebuild required) + runtime `osSpGetStatus_recomp` (returns
  SP_STATUS_HALTED).

### Outcome

The game boots without crashing for the full observed window (~25 s), submits
its **first RSP gfx task** (`send_dl frame=1 type=1`), and sets a **real VI
mode** (dummy workloads stop; the null renderer swaps the game's framebuffers
at ~50-60 Hz). Next watch point: the streamed-code ROM DMA (`func_8009DA50`)
before Phase 4 overlay loading.

---

## 2026-08-24 (session 3) — First boot achieved: libultra bridging is working

### Decision: name-based libultra replacement confirmed and applied (3 batches)

Applied 24 names to `symbol_addrs.txt` (see `LIBULTRA-BRIDGING.md` "Progress").
The boot path now runs the runtime's native `osXxx_recomp` services:
`osInitialize`, `osCreateThread`/`osStartThread`/`osSetThreadPri`/
`osGetThreadPri`, `osCreateMesgQueue`/`osSendMesg`/`osRecvMesg`,
`osCreatePiManager`/`osCartRomInit`, `osSetIntMask`,
`osAiGetLength`/`osAiGetStatus`/`osAiSetFrequency`/`osAiSetNextBuffer`,
`osSpTaskLoad`/`osSpTaskStartGo`/`osSpTaskYield`/`osSpTaskYielded`,
`osCreateViManager`/`osViSetMode`/`osViSetSpecialFeatures`/`osViSwapBuffer`/
`osViGetCurrentFramebuffer`/`osViGetNextFramebuffer`, `osDpSetNextBuffer`,
`osGetCount`, `__osSetFpcCsr`, `osVirtualToPhysical`.

**Outcome:** the game **boots without crashing** on Linux — 7 N64 threads start,
the null renderer swaps VI buffers at ~50-60 Hz, and the game runs its init.
No MMIO shim (handoff option 3) was needed; naming the RSP-task family made the
remaining verbatim MMIO readers (`osSpGetStatus`, `osSpSetStatus`,
`__osAiDeviceBusy`, `osSiGetStatus`, `osDpGetStatus`) unreachable dead code.

### Decision: fix the runtime's initial-1MB DMA sign-extension bug

`recomp::init()` passed the entrypoint VRAM address to `do_rom_read` as a
zero-extended `uint64_t`, but recompiled memory accesses expect sign-extended
32-bit addresses (the `MEM_B`/`MEM_W` macros subtract `0xFFFFFFFF80000000`).
The DMA wrote ~4 GiB past rdram, so the game's data section never loaded and a
later verbatim VI helper null-deref'd. Fixed by sign-extending:

```cpp
recomp::do_rom_read(rdram, (gpr)(int32_t)entrypoint, 0x10001000, 0x100000);
```

This is a genuine N64ModernRuntime bug that other projects would hit with a
high `0x800xxxxx` entrypoint; our fix stays local to the vendored runtime.

### Decision: new analysis tool

Added `tools/libultra_scan.py` — parses `asm/1060.s` and reports, per function:
MMIO registers, cop0 registers, and direct callees. This replaces the one-off
manual scans with a reproducible inventory.

### Next session (see `docs/HANDOFF-2026-08-24.md` / `LIBULTRA-BRIDGING.md`)

- Name the `osCont*` family (`osContInit` etc.) — the game may be waiting on
  controller init; the game thread has not yet hit streamed-code stubs or
  submitted RSP tasks in the observed window.
- Watch for the first RSP task and the game's first real `osViSetMode`
  (the null renderer currently shows the runtime's dummy framebuffers).

---


## 2026-08-24 (session 2) — Libultra bridging: name-based replacement + platform decision

### Decision: adopt handoff Option 1 — name OB64's libultra functions in the ELF

The next milestone (see `LIBULTRA-BRIDGING.md`) is to make the runtime's native
`osXxx_recomp` services replace OB64's verbatim libultra. Mechanism is purely
name-based in N64Recomp (`reimplemented_funcs`), so the work is building the
OB64 libultra **address→name table** and adding it to `symbol_addrs.txt`,
then re-splat → rebuild ELF → re-run `make recomp`.

Rejected for now:
- **MMIO shim first (handoff option 3)**: only to be used surgically for
  registers the game still reads after reimplementation.
- **Address-based N64Recomp config**: viable fallback, but the canonical
  `symbol_addrs.txt` route also names the disassembly for future debugging.
- **Skipping the game's libultra entirely (option 4)**: too risky.

### Decision: this milestone is macOS- and Linux-equivalent; switch for the renderer phase

The libultra-bridging work is CPU-side and platform-independent — no reason to
switch machines for it. The RT64/renderer phase should move to Ubuntu (Vulkan
primary backend). Documented in `guides/linux-migration.md` with the exact
macOS-specific code to remove (`app/CMakeLists.txt` APPLE blocks, the
`__APPLE__` branch in `sdl_platform.cpp::create_window`, the RT64 DXC `libz.dylib`
symlink, Metal toolchain download).

### Decision: correct the function count

PLAN.md's "3659 functions" is stale; the recompiled output has **807 functions**
(802 `func_800xxxxx` + `main_recomp`/`recomp_entrypoint`/others).

---

## 2026-08-24 — Phase 3: first-boot app

### Decision: static recompilation pipeline (unchanged, restated)

Confirmed the project continues on the `N64Recomp` + `N64ModernRuntime` stack
(Zelda 64: Recompiled approach), not a matching decomp.

### Decision: app project layout

- New `app/` CMake project produces the `ogrebattle64` executable.
- `RecompiledFuncs/*.c` (recompiler output) is compiled into a static lib
  (`ogrebattle64_recomp`) and linked into the app.
- `tools/N64ModernRuntime` (ultramodern + librecomp) and `tools/RT64` are pulled
  in via `add_subdirectory`.
- `tools/RecompFrontend` (RmlUi-based menu UI) is intentionally **not** used yet;
  it adds heavy dependencies (RmlUi, lunasvg, GamepadMotion) and is only needed
  for the config/mod menu (Phase 7). Input is wired directly via SDL2 for now.

Rationale: keep the first-boot milestone minimal and debuggable. Adding the
frontend later is purely additive.

### Decision: RT64 as the renderer

RT64 (MIT) is the recommended renderer for N64ModernRuntime projects and the
same one used by Zelda64Recomp. It interprets RDP commands and provides the
`RendererContext` used by ultramodern. Added as submodule at `tools/RT64`.

Build options (same as Zelda64Recomp): `RT64_STATIC=ON`, `RT64_SDL_WINDOW_VULKAN=ON`,
`HLSL_CPU` compile definition. On macOS RT64 uses its **Metal** backend (the
`RT64_SDL_WINDOW_VULKAN` option is only meaningful on Linux).

### Decision: phased bring-up (null renderer first)

The app is structured so the renderer is swappable. For the first boot
validation we use a **null renderer** that:
- acknowledges VI register updates and display-list submissions without drawing,
- logs boot progress (VI swaps, RSP tasks, thread activity).

Rationale: Ogre Battle 64 renders everything through RDP, and RDP commands only
exist after the game's RSP microcode is recompiled. A null renderer lets us
validate the whole boot path (thread scheduler, libultra shims, PI DMA) before
RT64/RSP work lands; it also avoids coupling renderer bring-up with microcode
bring-up.

### Decision: ROM hash constant

`GameEntry.rom_hash` uses XXH3-64 of the full big-endian ROM
(`librecomp` hashes the post-byteswap contents):

```
XXH3_64(assets/ogre64.z64) = 0xbe6adaa5c3f8f7a9
```

### Decision: entrypoint

`GameEntry.entrypoint_address = 0x80070C00` (cart header entry), entrypoint
function = `recomp_entrypoint` (recompiled boot stub). `recomp::init()` already
handles IPL3 variable setup and the initial 1MB ROM DMA, and the boot stub
clears BSS and jumps to the game's `main_recomp`.

### Decision: streamed/overlay code stubs during bring-up

Functions outside the ELF (`0x8016C900+`, `0x84001120+`, etc.) are referenced
via runtime `get_function` lookups, which hard-fail if a function is unknown.
Until Phase 4 implements real overlay loading, the app registers **log-and-return
stubs** for all unresolved addresses via `recomp::overlays::add_loaded_function`.
This lets the boot path survive early references to streamed code.

---

## 2026-08-24 — First boot findings (critical)

### Decision: record the MMIO / libultra-verbatim problem

The app now builds and boots the runtime, but the game crashes on its first
hardware access:

- `recomp::mem_size` is **512 MiB** (not GB); the recompiled `MEM_W` macro maps
  MMIO reads like `0xA4800018` (SI status) to `rdram + ~0x24800000`
  (~580 MiB), which is outside the RW region → SIGBUS.
- More fundamentally, OB64's libultra is recompiled **verbatim** (generic
  `func_800xxxxx` symbols), so N64Recomp's name-based libultra reimplementation
  never fired and the runtime's `osXxx_recomp` services are not called by the
  game. Bridging this (see `HANDOFF-2026-08-24.md` options 1–4) is the next
  milestone.

### Decision: `start_game` must be delayed

Calling `recomp::start_game` before `recomp::start` makes the VI thread skip
its dummy-mode phase and crash on a null VI mode. The app now starts the game
from a thread delayed 500 ms.


### Decision: SDL2 dependency

The app uses SDL2 for window/input/audio. On macOS it is provided by Homebrew
(`brew install sdl2`, which today installs `sdl3` + `sdl2-compat`).

### Decision: licensing

- N64Recomp: MIT
- N64ModernRuntime: GPL-3.0
- RT64: MIT
- RecompFrontend: (see repo)

Because N64ModernRuntime is GPL-3.0 and is statically linked, the final
`ogrebattle64` app is **GPL-3.0**.

### Decision: OGRE_STEP seeds the step once instead of holding it (session 53)

`OGRE_STEP=<n>` used to write `D_8018F1C0 = n` on every frame while the selected
scene was active. The scene-script VM writes the same word when the sequence
advances, so the hold overwrote the advance and the New Game opening re-entered
the same step forever ("the cathedral scene restarts" — developer, session 53).

The step is now **seeded once** on the first frame the target scene is active and
released as soon as the live step differs from the seed (or after a 1500 ms
window). A shortcut must not be able to pin game state the game itself writes.

### Decision: syncFramebuffers reads the last written native slot (session 53)

`State::syncFramebuffers` writes every framebuffer in the framebuffer manager
back to RDRAM, because the game's CPU readback can pick a stale source. It called
`Framebuffer::copyNativeToRAM`, which consumes the same per-framebuffer write
buffer slots the game already consumed when it retired the workload; for a
framebuffer the current workload did not touch, the consumer index points past
the history and `NativeTarget::copyToRAM` dereferenced null (the name-entry
form's crash).

`NativeTarget` now tracks `lastWrittenBufferSlot` and `copyLastNativeToRAM`
reads that slot without consuming it, skipping framebuffers that were not
rendered since their last reset. This preserves the writeback's purpose (every
rendered framebuffer reaches RDRAM) without reusing a consumed slot.

### Decision: drive New Game from the title, never by seeding a step (session 54)

`OGRE_STEP=<n>` seeds the scene-script step to skip earlier steps. Session 54
showed that this *changes the sequence's own state*: entering scene `0x0D` at
step 2 with no step-1 visit leaves the exit on its `otherwise` arm, which
re-enters `0x0D` at step 0 and selects the movie-mode branch — the 1 GiB
`memset` crash session 53 recorded. The crash is therefore a property of the
shortcut, not of the opening.

The maintained New Game repro drives the real flow instead:

```sh
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,…" ./build-app/ogrebattle64
```

A Start press summons the title menu (developer, session 54), the cursor starts
on `New Game`, and a second Start confirms it; `A` then advances the movie and
the cathedral dialogue. The engine's own exit resets `D_8018F1C0` to 0 and sets
`next = 0x0007`, so a step word of 0 **at that point** is correct game
behaviour. `OGRE_STEP` stays for reaching a later step when the pre-state does
not matter, and its documentation now says so.

### Decision: `OGRE_DMA_TRACE` is the tool for "a module is not resident" (session 54)

The `[bank]` log only fires for streamed records the port has compiled, so a
module the game loads but the port has no record for is invisible — that is the
scene-`0x07` black screen: its enter calls `0x801A578C`, which lives in overlay
C's RAM window and is zero at runtime.

`OGRE_DMA_TRACE=1` accumulates **every** PI DMA the game issues, keyed by
`(rom, ram)` base with first/last event indices, and dumps it from the
bounded-run exit path (`_exit`, so no `atexit`). It answers "which module is
resident in this RAM, and which one overwrote it" without trusting
`config.yaml` — the same class of question `tools/rdram.py banks` answers for a
dump. Reuse it before writing another throwaway PI-DMA logger.

### Decision: a sprite *size* bug is proved by an entry-vs-rect A/B, not by the window (session 61)

> **PARTLY CORRECTED BY SESSION 64.** The A/B method stands (the builder really
> does emit `rect = entry's (W,H)`, so the rect names the slot the caller asked
> for — still the fastest discriminator). But the corollary at the end is
> **wrong**: there is **no second writer** of a tile-size command. The builder
> `func_ovlM_8019F83C` emits no `G_SETTILESIZE` at all — the word at
> `0x8019F990` read here as "the builder's UV arithmetic" is the TEXRECT's
> **s,t** half (`u<<21 | v<<5`), and the caller's `0x0001C028` is the only
> `SETTILESIZE`. See `docs/HANDOFF-2026-09-17-session64.md` §1b. Also note the
> texture slot the party uses is **not an asset** (session 64 §3).

When a 2D sprite renders at the wrong scale on this port, the fastest
discriminator is **to compare the emitted `G_TEXRECT` with the sprite-table entry
the builder actually read**, not to reason about the `G_SETTILESIZE` window.

Measured (session 61): `func_ovlM_8019F83C` (`bankRec05`) always emits a rect
equal to the entry's `(W,H)` from `0x801A6FE0 + a2*8`, so the rect's size names
the slot the caller asked for. The map's party marker therefore reads slot 11
(`144x23`, a HUD-width entry) while the knight frames are slots 12-15
(`16x24`) — and the developer's retail measurement (~10% of 240 px tall, ~5% of
320 px wide = `16x24`) matches those slots. So: **measure the rect, read the
entry, and compare against the developer's stated on-screen size**; the
tile-size window may be a separate writer and is not evidence about which slot
was requested.

Corollary recorded because it cost time: the `7x10`-texel window that session 60
read as "a descriptor the map never finished filling" **is** the game's own
emitted value (byte-confirmed in the display list), so it is not an app/renderer
bug — but it is also **not** produced by the builder's own UV arithmetic, so its
writer is still unidentified (session 61 §4). Do not assume the builder is the
only writer of a sprite's tile-size command.

---

## Session 63 — the map party's two draws are identified, and the knight samples a zeroed texture buffer

> **CORRECTED IN PART BY SESSION 64 — the draw/role identification stands; the
> "zeroed texture buffer / find the pass that fills it" framing does not.** The
> **knight is not an un-filled asset**: the map's enter `malloc(0x18000)`s
> `state[+0x34]` and **composites** it at runtime (a 32-texel-wide RGBA32 sheet
> of 24 frames, `0x1000` bytes each, selected by `3*state[0x1DC] + f`), and it
> **does** contain the knight. The buffer that is genuinely zero is the
> **shadow's** `state[+0x04]+0x1068`, and `state` is a **heap pointer** — read it
> from `*(0x80197B18)`; the `0x801F1570` used here is run-specific. Session 63's
> "the caller's delay-slot store wins on hardware too" and its shadow `+0x10E0`
> are also superseded. Full correction:
> `docs/HANDOFF-2026-09-17-session64.md` §1, §3.

**The correction (developer).** Sessions 61/62 identified the party's two builder
calls backwards. In `func_ovlM_801A2A7C` the **second** call (ROM `0x81BFC`,
`a2=0xA`, rect `(a0-16, a1-24)`) is the **knight** — the rect *above* the other —
and it must be `16x24`; the **first** call (ROM `0x81B50`, `a2=0xB`, rect
`(a0-8, a1)`) is the **shadow** and must be `16x11`. The geometry says the same:
`a0`/`a1` are the party's map position, call 2's rect sits 24 px above call 1's
and 8 px left, i.e. the body above its shadow. So the two calls today name each
other's entries, which is exactly why call 1's `144x23` over a `7x10`-texel
window produced the row of ~18 repeated ellipses while the knight stayed a small
dark blob. Live entries: slot 12 = `(16,24)`, slot 10 = `(16,11)`, slot 11 =
`(144,23)`; session 61's table printout inverts those pairs.

**The window mechanism, settled.** The caller's hardcoded `0x0001C028`
(`lrs=28 lrt=40`) is written by the `jal` **delay slot** to the *same*
display-list word the builder writes its own window to (`dl+0x3C`), and it wins
on **hardware** as well as in N64Recomp's output (the delay slot runs after the
callee returns). It is the game's authored value either way: **not a port
artifact and no unidentified second writer** (corrects session 61 §4(a) and
session 62 §1's framing). Any fix must derive the window from the sprite-table
entry the builder is about to read, and must sit at the **call sites**, not
inside `func_ovlM_8019F83C` (19 callers; several entries carry a non-zero `u`/`v`
and legitimately want a sub-rect window — changing the builder regressed them and
produced a zero window, because its store runs before `$a2` is set).

**The wall (new, and the reason nothing is landed).** With the rects and
per-entry windows made consistent and slot 12 on the knight call, both party
sprites are clean and correctly shaped — and both are flat **dark blocks**. A
live probe at the draw reads the texture's own words:

```
[probe70] call1 tex=8021AC88  w0=00000000 w1=00000000 w4=00000000
```

The pointer address is faithful (the emitted list says `F1 … @0x8021AC88`), but
the buffer is **zero in RDRAM**, and a zero RGBA32 texture under the map's
alpha-blend combiner *is* the dark block. The shadow's buffer (`0x80264D00`,
`+0x10E0`) is zero too. So the party's art is missing at the point of use — the
next question is which pass fills `0x8021AC88` (the scene entry decodes ~20
assets through `func_8009DD38`) or which field of the state table `0x801F1570`
names a populated buffer, not which entry index or window is wrong.

**Generalisable rules.** (1) When a caller's constant and a callee's table entry
both write the same display-list word, the *caller's delay slot* is the one the
RDP sees — on hardware too. (2) A sprite's rect is the authoritative statement
of its size; derive its window from the same entry. (3) Before theorising about
a sprite's geometry, check the texture's **bytes** at the draw: a rect and window
can be perfectly consistent and still render flat if the buffer they name is
empty.

**Nothing is landed.** `tools/map_sprite_fix.py` and its `Makefile` hook were
removed after the experiments; every probe was regenerated away.

## Session 62 — the map's `G_SETTILESIZE` window is the caller's `jal` delay slot, and N64Recomp runs delay-slot display-list stores before the call

> **CORRECTED BY SESSION 64 — do not act on this entry's mechanism.** Two of its
> claims are false and were disproved at instruction level (see
> `docs/HANDOFF-2026-09-17-session64.md` §1, and the session-64 entry below):
> **(1)** N64Recomp does **not** execute a `jal` delay slot twice — the generated
> C emits the delay-slot store *before* the call and the duplicate after
> `goto after_N` is **dead code**; the delay slot runs once, *before* the callee,
> as MIPS requires. So nothing "clobbers" the caller's store. **(2)** There was
> never a second window writer: `func_ovlM_8019F83C` emits **no
> `G_SETTILESIZE` at all**, and the word at `0x8019F990` this entry (and session
> 61 §4a) read as "the builder's own window" is the TEXRECT's **s,t** half
> (`u<<21 | v<<5`). The caller's `0x0001C028` is simply the game's own
> `SETTILESIZE`, written once. The general rule this entry drew from it — *"check
> the `jal` delay slot of the call site first"* — is withdrawn.

> **Superseded in part by session 63.** The mechanism below is right (the
> caller's delay-slot store is the window the RDP uses), but two of its
> conclusions are corrected there: the caller's window wins on **hardware** too
> (not because of a recompiler quirk), and this entry's sprite-table printout is
> endianness-inverted relative to the live image — the party's slot 11 is
> `(144,23)` and the knight is slot 12 `(16,24)`, confirmed live at the consumer.
> The window-from-the-entry fix belongs at the party's **call sites** rather than
> inside the shared builder, but it is **not** landed (session 63 found the
> texture buffer empty, so the form is right and the art is not).

Session 61's open question ("the window has a second writer that is still
unidentified") is answered at instruction level. The map's party draw
`func_ovlM_801A2A7C` (`bankRec05`, unit M) sets the window itself, in the `jal`
**delay slot** of each builder call:

```
ROM 0x81B44   sw    $t5, 0x44($v0)     ; $t5 = 0x0001C028 -> F2 uls=0 ult=0 lrs=28 lrt=40
ROM 0x81B48   jal   0x8019F83C         ; func_ovlM_8019F83C (the sprite builder)
```

`0x0001C028` is a **28x40-texel** window. It is written to `$v0+0x40`..`0x44`,
while the builder writes *its own* window to `$t2+4` = `$t1+0x14` — a different
slot. So the value the RDP samples with is the caller's constant, and the
builder's UV-derived window is irrelevant for this draw.

**Why the constant looks stale:** the entries the map uses have `u=v=0`, and the
builder's window arithmetic (`(u+W)`/`(v+H)` packed at
`0x8019F990`) therefore degenerates to `0` for them — it cannot produce a sane
window for this table. The caller's constant is also not this draw's sprite size
(the party entry is `16x24`, the rectangle is `144x23`). Whatever `0x1C028` was
authored for, it is wrong for every map marker on the port, and with it a 144x23
rect over a 28x40 window *wraps the atlas* — which is exactly the row of repeated
ellipses sessions 60/61 recorded.

**Decision (reverted the same session, at the developer's instruction).** The
first fix tried was to derive the window from the sprite-table entry the builder
is about to read (`0x801A6FE0 + (a2 & 0xFFFF)*8`, `lhu 0` = W, `lhu 2` = H) and
emit `((W & 0xFFC) << 12) | (H & 0xFFC)`. It removed the tiling but sampled the
wrong atlas region, and it regressed a better mid-session probe frame, so it was
**reverted** and is **not** in the tree.

**What is decided and kept:** the map's window must be taken from the *emitted
display list*, never inferred from the tile-size arithmetic. The builder's
UV-derived window is discarded for these entries (`u=v=0` makes it degenerate)
and the caller's hardcoded `28x40` is what the RDP uses. Any future fix must be
A/B'd from a clean `make bank-recomp && make recomp` tree against a capture of
the *same* frame, and the party must come out as the developer described: one
`16x24` knight with one **static soft oval shadow below it**.

**The recompiler behaviour behind it, which is general.** N64Recomp emits a `jal`
whose delay slot writes memory as *delay-slot-store, then call*, and duplicates
the delay-slot block after the call behind a `goto after_N` (its tail-call
"repair" shape, the same one `cross_bank.py` looks for). The generated C
therefore performs the delay-slot store **twice**, and because the callee
re-reads its own display-list cursor, the caller's store is what survives in the
buffer. On hardware the delay slot runs *after* the callee, so the callee's store
wins. **When a callee's output is missing from a display list but present in its
generated C, check the `jal` delay slot of the call site first** — do not go
looking for a second writer in the game code.

**Also recorded.** The party body's sprite-table index is `0xB` in the ROM
(entry 11 = `16x11`); the knight's `16x24` entries are 12-15. Raising it to `0xC`
was part of the reverted fix and is **not** applied. The shadow's entry and the
shadow's atlas region are still unidentified — the shadow is a *static* sprite,
which rules out the "it is another animation frame" hypothesis.

---

## Session 64 (2026-09-17) — **correction**: the map's `G_SETTILESIZE` "two writers" story is wrong, and the emulator-first rule

**What was decided.** Stop treating the port's own decoded display lists as
evidence about the game's intent. Before any theory about what the game "meant"
by a list, the port's faithfulness has to be established at four layers: the
ucode/GBI dispatch (hash it), the RSP (was a task swallowed?), the recompiled vs.
ROM instructions, and the decoded data (independent decoder + boundary
invariant). Only if all four pass is a difference a *state* question, and a state
question is answered against a reference emulator, not by editing generated C.
Written up as `docs/guides/emulator-first.md`; applied in
`docs/HANDOFF-2026-09-17-session64.md`.

**Corrections to the two entries above.**

1. **N64Recomp does NOT execute a `jal` delay slot twice.** The generated C emits
   the delay-slot store *before* the call and then duplicates the block after a
   `goto after_N` where it is **dead code** (the duplicate exists so a
   fall-through entry at `jal+4` still works). The delay slot runs once, before
   the callee — MIPS-correct. So nothing "clobbers" the caller's constant: the
   caller's `0x0001C028` is written once and is what the RDP uses. The claim
   *"On hardware the delay slot runs after the callee, so the callee's store
   wins… check the `jal` delay slot of the call site first"* is **false** and is
   withdrawn.
2. **The builder emits no `G_SETTILESIZE`, so there was never a "second writer"
   to find.** `func_ovlM_8019F83C` emits `G_RDPPIPESYNC`, the `G_TEXRECT`
   (w0 = lower-right, w1 = upper-left), `G_RDPHALF_1` carrying **s,t**
   (`u<<21 | v<<5`), `G_RDPHALF_2` with `dsdx = dtdy = 1.0`, and another
   `G_RDPPIPESYNC`. The word at `0x8019F990` that session 61 §4(a) read as
   "the builder's own window" is the TEXRECT's `s`/`t` half, not a tile size. The
   "the builder's uv-derived window is degenerate for `u=v=0`" reasoning built on
   it is void.

**New, ROM-verified facts that bind the map work from here.**

* `state[+0x34]` is **not a decoded asset**: the map's enter `malloc`s `0x18000`
  bytes (`0x8019A9E4`) and **composites** an RGB555 LUT over an 8-bit index image
  into it, producing a 32-texel-wide RGBA32 sprite sheet of 24 frames
  (`0x1000` bytes each = 8 directions x 3 frames, selected by
  `3*state[0x1DC] + f`). `state[+0x04]` *is* an asset (`0x01DD210A`, byte-verified
  against an offline decode), and `state[+0x08]` is **never written** by the
  enter, so its zero is not a failed decode.
* The party's two draws are `func_ovlM_801A2A7C`'s calls at ROM `0x81B50`
  (`a2=0xB` entry 11 `(144,23)`, texture `state[+0x04]+0x1068`, static, `line=2`,
  `cms=WRAP masks=3`) and `0x81BFC` (`a2=0xA` entry 10 `(16,11)`, texture
  `state[+0x34]+frame*0x1000`, `line=8`, `SETTILESIZE 31x31`). All constants
  verified against the raw ROM, not just the ELF.
* **`state` is a heap pointer**, not `0x801F1570`; read it from `*(0x80197B18)`.
  Any note that hardcodes `0x801F1570` (sessions 60/63) is run-specific.
* The map's gfx ucode `0x8009F540` hashes (XXH3-64, raw bytes, length `0x1390`)
  to `0xCF55FAE288BFE48D` = RT64's `F3DEX2.fifo 2.08`, so RT64's GBI choice for
  the map is correct.

## Session 65 (2026-09-17) — the map's sprites were an 8-byte linker misplacement in unit M; compare every ELF with the ROM

**Decision: a bank ELF must be byte-identical to the ROM, and a misalignment is
fixed in the *config*, never in generated code.** `make elf-rom-check`
(`tools/elfcheck.py`) now asserts it for every unit and `make bank-recomp` runs
it before recompiling.

* **What broke.** Unit M's `.data` subsegment was linked 8 bytes above its ROM
  address. `mips-linux-gnu-as` aligns `.text` to 16 bytes and pads the section to
  that boundary; the module's code/data boundary (ROM `0x85838` = RAM
  `0x801A68A8`) is only 8-byte aligned, so the 0xC0E8-byte code object assembled
  to 0xC0F0 and the data subsegment began at 0x801A68B0 instead of 0x801A68A8.
  **Only the `lui/%lo` immediates that name data labels changed** — 150 of unit
  M's 480 address-named symbols were +8, and nothing else was — so the generated C
  looked plausible and every data read in the recompiled module was 8 bytes high.
* **The symptom.** The map's sprite table base is `D_ovlM_801A6FD8` (the raw ROM
  instruction at `0x7E804` is `addiu v0,v0,0x6FD8`); the port read `0x801A6FE0`,
  **shifting every sprite descriptor by one entry**. Sessions 60–63 spent four
  sessions on the result: the party drew a 16x11 crop of the 32x32 knight sheet
  ("rect with random colors"), the 16x11 shadow drew as a 144x23 band of repeated
  ellipses ("multiple shadow sprites"), the cursor drew as 16x16 instead of 24x23,
  the date plate as a 16x24 sliver (with `masks=2` wrap → the "striped box"), and
  the panel's month came from the wrong string-table entry (`Flama`, not
  `Sombra`). With the corrected table: `a2=10` = `(32,32)` party, `a2=11` =
  `(16,11)` shadow, `a2=12` = `(144,23)` date plate, `a2=6` = `(16,16)` cursor.
* **The fix.** `config-bankM.yaml`: the data subsegment starts at `0x85840`, not
  `0x85838`. The 8 bytes at `0x85838` are zeros, are not referenced by any code
  (the symbol count drops 480 → 479), and are the module's own alignment padding;
  absorbing them into the `asm` subsegment makes its size 16-aligned so the
  assembler emits no pad and every label lands on its ROM address.
* **Correction to the session-64 entry above.** Its "new, ROM-verified facts"
  bullet lists the party's calls as `a2=0xB` → entry 11 `(144,23)` and `a2=0xA` →
  entry 10 `(16,11)`. Those are the *shifted* table readings (they came from the
  bank ELF). The ROM's table at RAM `0x801A6FD8` has entry 10 `(32,32)` (party),
  entry 11 `(16,11)` (shadow), entry 12 `(144,23)` (date plate). Reading a bank's
  data table from its ELF was only unsafe because of this bug; with the check in
  place the ELF equals the ROM again, but the *live RDRAM dump* remains the right
  source for anything the game patches.
* **Withdrawn: session 64 §4's renderer-side `line = 8` lead.** For a 32-bit RGBA
  texture the RDP stores 2 bytes per texel in each TMEM half, so a tile `line` of
  8 (64-bit words) is a 64-byte half-row = **32 texels** — the sheet's row. RT64's
  `loadBlockOperation` (`0x800 / dxt` words per TMEM row ⇒ `dxt = 0x80` = 128
  source bytes) and its sampler (`pixelAddress = t*(line<<3) + x*2`, high half at
  `| 0x800`) agree with angrylion's `get_tmem_idx`. No renderer change is needed
  for the party, cursor or panel.
* **New rule for the record:** when a scene draws the wrong *size*, the wrong
  *slice* or the wrong *string* from a table, check the ELF's layout against the
  ROM (`make elf-rom-check`) before theorising about the renderer or the game's
  data. AGENTS §8 check 5 says "compare the generated C, the bank ELF **and the
  raw ROM bytes**"; this session is why the ELF is in that list.

## Session 65 addendum — checkpoints are process-local: `OSThread::context` is a host pointer inside the image

**Decision: the `save`/`load` recipe is a *same-run* tool until the runtime
rebuilds its host state on load; a checkpoint is not handoff material.** Recorded
because sessions 58's verification note ("cross-process load") and the project's
habit of handing checkpoints to the next session both assume otherwise.

* **Measured (session 65).** `save` then `load` **in the same run** on the live
  map (scene `0x05`, every N64 thread active) restores and continues cleanly.
  Loading `/tmp/map-fixed.ckpt` — written moments earlier by the *developer's*
  process, same binary and build id — reports `checkpoint loaded … (1 thread(s)
  parked)` and `c` confirms scene `0x05`, then dies deterministically:
  `SIGSEGV` at `do_send + 0x54C` (`lock incq 0x18(%rbx)`) on an N64 thread, with a
  garbage host address. The same happened for `/tmp/map-real.ckpt`.
* **Cause.** `ultramodern/ultra64.h` declares `OSThread::context` as
  `UltraThreadContext*` — "An actual pointer regardless of platform" — i.e. a
  **host** address stored in guest RDRAM, set in `osCreateThread`
  (`ultramodern/src/threads.cpp`). The checkpoint writes the whole RDRAM image,
  so the image carries host addresses (session 65 found 7
  in `/tmp/map-fixed.bin`, one per thread, at `OSThread::context` = `base+0x20`) that are meaningful only in the writing process. After a
  rewind in another process, `schedule_running_thread`/`wake_blocked_head`
  dereference them. It is the *only* host pointer in guest memory (checked: no
  other field is declared as a real pointer, and no other host object is stored
  into RDRAM), which is why the pointer rebind is a tractable fix.
* **Fix sketch (not implemented).** Keep a registry of `(PTR(OSThread),
  UltraThreadContext*)` filled by `osCreateThread` and pruned by
  `osDestroyThread`; add `ultramodern::rebind_thread_contexts(RDRAM_ARG1)` that
  walks it and rewrites `t->context`; call it from the app's `load` path right
  after `read_checkpoint`, while the checkpoint pause still holds every thread at
  a function entry. Expect a second half: the host-side scheduler/blocked-thread
  state (parked external sends, threads waiting on a queue) is derived from the
  *old* timeline and is not restored by the image, so a rebind alone may turn the
  crash into a hang. Also note `build_id` remains necessary regardless: the
  overlay state (which recompiled body is mapped where) is the other thing the
  image cannot carry across builds.
* **Operational corollary.** The live console's watched file is
  `OGRE_CONSOLE_FILE` (default `/tmp/ogre-console.txt`) and is *global*: with two
  instances running, the other one silently consumes your commands. Use a
  per-instance file.

## Session 65 (2026-09-17) — Controller Pak: bridge the three SI functions, not the filesystem; flat 32 KiB image

**Decision: the port owns the Controller Pak *device*, and keeps the game's own
recompiled PFS above it.** Three findings drove it:

1. **The PFS is already in the ROM and already recompiled.** `osPfsInitPak`,
   `osPfsAllocateFile`, `osPfsFindFile`, `osPfsReadWriteFile`, `osPfsChecker`, …
   are libultra functions in the main segment (`0x8009616C`..`0x80097DC0`) that
   `make recomp` compiles. Porting a filesystem would have been strictly worse
   than leaving the game's own code in place.
2. **Only three functions touch hardware the port lacks.** `__osContRamRead`
   (`0x80097BD0`, 23 call sites), `__osContRamWrite` (`0x80097DC0`) and
   `__osPfsGetStatus` (`0x80096EC0`) build PIF/SI commands and wait on the SI
   message queue; the port has no SI emulation (KSEG1 MMIO is aliased into a
   scratch page by `recomp.h`), so they are reimplemented in
   `librecomp/src/pak.cpp` and bound via `symbol_addrs.txt` + N64Recomp's
   `reimplemented_funcs`. Everything else — inode tables, directory, notes,
   checksums, the bank probe — stays the ROM's own code.
3. **The device model is flat 32 KiB.** mupen64plus
   (`src/device/controllers/paks/mempak.c`) and cen64 (`si/pak.c`) both implement
   the pak as a flat 32 KiB byte space accessed in 32-byte blocks, where
   addresses `>= 0x8000` are the bank/enable register and are ignored. That is why
   libultra's `__osRepairPackId` bank probe (write block 0 with bank *j*, re-read
   it with bank 0) finds bank 0 disturbed and reports **`banks == 1`** — the value
   its layout expects (inode tables at page 1, mirror at page 2, directory at
   page 3, first data page 5). A fresh image is therefore formatted exactly like
   mupen's `format_mempak` with `banks = 1`, which makes the port's `.mpk` files
   interchangeable with emulator/console dumps in both directions.
4. **The write guard is the ROM's, not a guess.** `__osContRamWrite` protects
   the ID/label blocks: `address < 7 && address != 0` unless the caller passes
   `force == PFS_FORCE`, i.e. **`PFS_LABEL_AREA = 7`, `PFS_FORCE = 1`**, read out
   of this ROM's instruction at `0x80097DF4` (`sltiu v1,a0,7`) rather than from a
   header. The device implements the same guard so it cannot diverge from the
   game's expectations.
5. **The image is a separate device file**: `<config>/saves/<game id>.mpk`
   (32 KiB), written with the runtime's temp+`.bak` helpers. It is *not* the
   cartridge-save `save_buffer`/`SaveType` plumbing, which stays `None` — a
   Controller Pak is a different device with its own image.

**Where it is reachable from (new):** the pak API is called by **unit H** only
(the form/UI module), and the Controller Pak menu is **scene `0x07`'s descriptor
callback `+0x18`** (`0x8018FDAC` → `func_8017BB28` → `jal 0x8019C69C`, unit H's
`func_ovlH_8019C69C`); `+0x14` of the same descriptor is the name/birthday form.
The map's module (unit M) never calls the pak API, and a *forced* map entry's
Save never reaches scene `0x07` (no DMA, no scene change, no device call), so the
device can only be exercised from a **natural** map run — the same "a forced
entry has no army/save state" caveat as the map's mission markers.

## Session 65 (2026-09-17) — **correction**: OB64's save is a battery-backed cartridge save; the Controller Pak is the *copy/backup* device

**The developer (end of session 65): "ob64 saved the game in a battery, not in a
controller pak."** Session 58's recon read the Controller Pak strings
(`Controller Pak Menu`, `1 note 25 pages to save.`, `Insufficient pages to copy
game.`, `Data saved to Controller Pak.`) and the PFS cluster and concluded the
save device was the pak. The string that gives it away is
**`Data loaded to Game Pak.`** — "Game Pak" is the cartridge, and those strings
are the *copy between the cartridge save and a pak*, a backup feature. So:

* the **primary save** (the map's R → Save → Yes, and whatever the title reads) is
  **battery-backed cartridge save** = SRAM / FlashRAM / EEPROM, and the port's
  `entry.save_type = recomp::SaveType::None` (`app/src/main.cpp`) is why that flow
  silently does nothing: the runtime reports no save device, so the game never
  issues the PI DMA. Everything session 65 measured on that flow (dialog + prompt
  drawn, then no device call, no DMA, no scene change) follows from this.
* the **Controller Pak device implemented in session 65 is not wasted**: it is the
  copy/backup device, and the "Controller Pak Menu" (Save/Load/Erase/Exit) plus
  the titie's `Load Game` entry ("when a Controller Pak save exists") belong to
  that path. With `SaveType` fixed, the *next* question is whether the copy
  feature's own scene is reachable.
* **Next step**: identify the chip (`osEepromProbe`, `osFlashInit`/`osFlashReadId`,
  or a raw PI DMA to the `0x08000000` save window), set `recomp::SaveType` to it,
  rebuild, and re-drive the map's Save; the runtime's existing `save_context`
  (`librecomp/src/pi.cpp`) then persists `<config>/saves/<game id>.bin`.

## Session 66 (2026-09-17) — the save chip is SRAM, and the *save* is the battery; the title's `Load Game` reads it

Session 65 ended with the right correction (battery, not Controller Pak) and the
right next step (identify the chip). This session found the chip and wired it.

### The chip: 32 KiB SRAM

Two independent sources agree, and neither is inference from a generated file:

* **mupen64plus's game database** (`~/Documents/RetroArch/system/Mupen64plus/mupen64plus.ini`)
  gives `SaveType=SRAM` for this exact ROM (`CRC=0ADAECA7 B17F9795`, entry
  `EBB4B4D2808DF427AAA3085A41B8A954`, "(U) (V1.1) [!]"), with `Mempak=Yes` for the
  unrelated Controller Pak copy/backup device. Emulators carry this per-game
  knowledge precisely because it cannot be read off the ROM header.
* **The game's own code.** `func_8008A040`, called once at boot from
  `func_80071EB0` (`0x80071F4C`), builds an `OSPiHandle` at `0x800BE110` with
  `baseAddress 0xA8000000` (physical `0x08000000`, the SRAM window) and publishes
  it at `0x800E79AC`; `func_8008A0F0(devOffset, dramAddr, size, dir)` DMAs through
  it. The boot accessors `func_80074CF0`.. (13 of them, each with the same
  "read the whole image once into `*(0x800B83B8)`" prologue) read 0x8000 bytes in
  256-byte DMAs at device offsets `0..0x7F00`, and the commit
  `func_80074BF0` → `func_80074C58` writes the same 0x8000 bytes back with
  direction 1. Slot `n` is `0x1850` (6224) bytes at `0x10 + n*0x1850`; the device
  signature (0x04) and slot-0 magic (0x14) are the ASCII `QuestOG3`. There is no
  EEPROM/FlashRAM protocol anywhere (no `osFlashInit`/`osEepromProbe` call, no
  FlashRAM command DMA), so `SaveType` is not ambiguous.

### The mechanism: the device is the handle, not the address

`func_8008BC40` queues an `OSIoMesg` to `D_800AA408` and the runtime completes it
inline (`librecomp/src/pi.cpp`). The old handler treated *every* transfer as a
ROM read at `devAddr`, so the boot save read pulled ROM bytes `0..0x7FFF` and the
game "repaired" that garbage — which is what made the battery path look absent.
The fix is one rule: **resolve the device from the `OSPiHandle` the message
carries** (`OSIoMesg.piHandle` at +0x14 → `OSPiHandle.baseAddress` at +0xC).
`baseAddress 0xA8000000` → the SRAM window → `save_read`/`save_write`
(`save_context`, 0x8000 bytes for `SaveType::Sram`); anything else → ROM at
`devAddr`, exactly as before. Direction comes from `OSIoMesg.hdr.type`
(`0xF` = EDMAREAD, `0x10` = EDMAWRITE) or from `func_8008BC40`'s `a2`. Handles of
0 (the AI helper `func_8008C360` records none) keep the old ROM behaviour, and
the game's *cart* handle (`osCartRomInit`, bridged) has base 0 or 0x10000000, so
its `devAddr` is still a raw ROM offset — verified over a run: 1290 ROM DMAs
unchanged, 384 SRAM reads and 128 SRAM writes.

### Verification (all on the built app)

* **No file** → `[save] no save file … starting with a blank image (32768 bytes)`;
  the game reads the window, finds it blank, formats it, and the port writes a
  real 32768-byte file whose 0x04/0x14 magics are `QuestOG3`.
* **Restart with that file** → `[save] loaded …`; the game reads it and does
  **not** rewrite it (accepted; file hash unchanged).
* **File overwritten with `0xA5`-garbled bytes** → the game reads, rejects and
  rewrites it; the magics come back.
* **A real emulator save with progress** (developer, parallel-n64) → converted
  with `tools/sramsave.py` and loaded: the game accepts it and does not rewrite
  it, and the **title menu's `Load Game` entry appears with the cursor on it**.
  A/B on the same `title` + Start tap schedule: no save → New Game
  (`0x04 → 0x02 → 0x0D`); the progress save → `0x04 → 0x12` (Load Game) →
  `0x05` (the map), i.e. **the loaded game comes up**, dev-confirmed.
  This corrects `docs/scenes.md`'s "when a Controller Pak save exists": the
  title's `Load Game` reads the **battery** save; the pak is only the
  copy/backup device.

### Emulator interchange: `tools/sramsave.py`

Every port/emulator holds the same 32 KiB of logical bytes, but the file
wrapping differs, so a save cannot be dropped in blind:

* port / mupen64plus-RetroArch `.srm`: the bare 32 KiB, "QuestOG3" literal;
* some `.sra` writers and **parallel-n64**: every 4-byte group reversed
  (`seuQ3GOt`), and parallel-n64's dump is a **296960-byte combined image with
  the SRAM at 0x20800** (FlashRAM 0x20000 + EEPROM 0x800 before it).

The tool finds the SRAM by magic at any offset and byte order (`check`),
converts to the port's logical image (`import`) or to a byteswapped `.sra`
(`export`), and refuses a file with no magic rather than inventing an image. It
is how the developer's save above was imported; `import` of it is byte-identical
to the image the port then loaded.

### Two operational notes

* `OGRE_PREF_DIR=<dir>` relocates the runtime config dir (saves + mods). It was
  added because a **failed save write is a modal raised from the saving thread**,
  which stalls the game and (in that first sandboxed attempt) ended in the
  periodic `[snap]` dump crashing. The runtime now also prints
  `[save] wrote <path>` / `[save] FAILED to write <path>` to make the outcome
  unambiguous. **Open, not investigated: a failed write should not be able to
  take the process down** — the modal-off-the-main-thread plus the `[snap]` race
  are the suspects.
* The save is rewritten even when its content is unchanged: a load-then-play run
  printed `[save] wrote …` and the file hash was identical afterwards. Harmless
  (the runtime's saving thread coalesces writes and uses temp+`.bak`), but it
  means "the game wrote the save" is not by itself evidence of new progress —
  compare the bytes.

