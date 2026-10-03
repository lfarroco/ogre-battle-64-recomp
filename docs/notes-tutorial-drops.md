# Tutorial (scene 0x17) item drops — where the granted id comes from

Read-only research. Evidence: `mips-linux-gnu-objdump -d build/*.elf` plus raw ROM bytes
(`assets/ogre64.z64`). Generated C was not used. No tracked file changed.

## 0. Scene setup (verified from the ROM, not from prose)

- Scene `0x17` accessor `func_801862F0`: `0x801862F8 andi v0,v1,0x8` on `D_80196B0C`; bit 3 clear
  returns `0x8018FE50` and also clears bit 5 of the mode byte (`0x80186300 andi v0,v1,0xdf`,
  `0x80186314 sb`); bit 3 set returns `0x8018FE64`. Descriptor words at ROM `0x65250`/`0x65264`:
  masks `0x00060000` (records 17,18) and `0x00020000` (record 17).
- Scene `0x03` accessor `func_80173700` (`0x80173700-0x80173720`): `lbu(0x80193700)==0` ->
  `0x8018F350` mask `0x0000038C` (records 2,3,7,8,9); non-zero -> `0x8018F364` mask `0x0004038C`
  (same plus record 18) — the tutorial practice stage. Descriptors at ROM `0x651D0`/`0x651E4`.
- Mask semantics: `func_800761E4` (`asm/1060.s:6273`, called at `asm/1060.s:6012` from `0x80075E68`)
  walks the record-id byte list at `D_800B86FC[index]` (`0x80076208-0x8007623C`) and tests
  `1 << id` against the mask (`0x80076240 sllv`, `0x80076248 and v0,s3,v0`). Bit N = record N.
- Segment table (ROM `0x387C0`, 0x28-byte rows): row 7 = RAM `0x801AD5C0..0x801F4050`, ROM
  `0x101D00..0x145230`, DMA data ends `0x801F0AF0`; row 18 = RAM `0x80220F60..0x80230600`, ROM
  `0x1BA020..0x1C32D0`, nested mode module RAM `0x8022A860..0x802305F0` (bank unit T, ROM
  `0x1C32D0`, size `0x5D50`).

## 1. Q1 — `func_ovlN_801DC740` in the tutorial

- Scene `0x17` itself loads only records 17 and 18, so `0x801DC740` is absent there.
- The practice stage mask `0x0004038C` has bit 7 set, so **record 7 is loaded and
  `func_ovlN_801DC740` (`0x801DC740`) is resident**. Bank T sits at `0x8022A860` and does not
  overlap record 7.
- So a failing record-7 fingerprint at `0x801EDB38` does not prove record 7 absent. Either the sample
  was in scene `0x17`, or another DMA covered that RAM: record 10 (`0x801AD5C0`, ROM `0x1F0A00`,
  `0x230E0`), record 10a (`0x801D0860..0x801E6FD0`), record 10c / unit X (`0x801E6FD0`, ROM
  `0x22A250`, `0x10120`) all overlap it and hold code there. None is in mask `0x4038C`; the battle
  code loads them.
- Grant arms: `0x801DC744 andi t0,a0,0x7FFF`, `0x801DC748 andi a0,a0,0x8000`; bit 15 set ->
  278-slot list `0x80196B20` (`0x801DC76C-0x801DC808`); bit 15 clear -> 40-slot list `0x80193AE0`
  (`0x801DC814-0x801DC8E0`, `a1 & 0xFF` gates an extra counter at `+0x02`).

## 2. Q2 — the tutorial's reward path

### 2a. Drop reader and its two item sources

`func_ovlN_801AD6BC` (record 7, RAM `0x801AD6BC`) is called from the **scene-0x03 enter**
`func_ovlA_8019EE70` at `0x8019F4AC` (`jal 801ad6bc`), so the practice stage runs it.

- `0x801ADCE8-0x801ADD18`: `s4 = lbu(0x80197708)`, `v1 = lbu(0x801976FC)`,
  `lbu(0x801976F8) & 0x40` gates the path (`0x801ADD30`, branch `0x801ADD34`).
- bit 6 **set**: scan table O at `0x801EDB38` for `s4` (`0x801ADD3C-0x801ADD64`), `rand()%3`
  (`0x801ADD68-0x801ADD90`, `func_80092A60`), read `lhu(0x801EDB3A + (s1*4+r)*2)` (`0x801ADDB0`)
  into `s3`; then gate `lbu(0x80193690) & 0x10` (`0x801ADDB4`, zero -> `0x801AE1B0`).
- bit 6 **clear**: `s3 = lhu(0x8019369C + s4*2)` (`0x801ADD14`) — a per-unit pending id in
  game-state RAM, not a table.
- Both arms: `sh s3,0x801F36C2` (popup id; `0x801ADE84` / `0x801AE164`) and `jal 0x801DC740`
  with `a0 = s3` (`0x801ADEA8` / `0x801AE188`). Arm B then clears the slot:
  `sh zero,0x8019369C + s4*2` (`0x801AE190-0x801AE19C`).
- Table O in ROM: entry 0 at `0x142278` = `0027 8031 809b 80e8`; entry 34 at `0x142388` =
  `0050 8091 80b2 80df`, then a zero type. Stride 8.

### 2b. Writer of `0x8019369C + 2*unit`

`func_ovlN_801B15A0` (record 7), code at `0x801B2520-0x801B2580`:
`0x801B2558 mult s0,s2` / `0x801B2560 lui v1,0x801F` / `0x801B2568 lbu v1,-31321(v1)` reads the
**byte** at RAM `0x801E85A7 + s0*s2`; `0x801B256C-0x801B257C sh v1,0x8019369C + s3*2`. The same arm
reads `lbu(0x801E85A4 + s0)` / `lbu(0x801E85A5 + s0)` (`0x801B23B8`, `0x801B2404`) and converts them
to floats stored in the unit struct (`swc1` at `0x801B2548`). `0x801E85A4` is inside record 7's DMA
data = ROM `0x13CCE4`; bytes there: `c4 62 08 0b | 50 02 08 04 | e3 71 08 0d ...`, i.e. 4-byte groups
whose last byte is an item id (`0x0B`, `0x0D`, ...) and whose first two bytes (196,98 / 227,113) read
as coordinates. Item ids here are bytes, so bit 15 is clear and the grant fills the **40-slot list**.
`0x801B254C jal 0x8021592C` is bank unit R (`0x80214FA0`); that function loops `jal 0x801DC740` with
literal ids 1..6 and `a1 = 1` (`0x80215BE8-0x80215BEC`).

### 2c. The two inventories are separate id spaces

- 278-slot `0x80196B20` (stride 4: `+0 u16 id`, `+2 in-use`, `+3 count`), search `func_8016B6FC`
  (`0x8016B720 slti v1,278`). Names come from the 0x20-stride table at `0x8018C42C`
  (`func_8016F5D0` reads `+0`). Ids 1..277 are equipment in the pool at `0x8018B4D0`; 278 is
  `None` (`0x801906DC`). No id 1..278 names `Heal Leaf`.
- 40-slot `0x80193AE0`, search `func_8016B738` (`0x8016B75C slti v1,40`). `Heal Leaf` is row 0 of the
  0xC-stride table at RAM `0x8018E6F4` = ROM `0x645F4` (`{0, 0x801906D0 "Heal Leaf", 0xA}`; Heal Seed
  `0x32`, Heal Pack `0x78`, Power Fruit `0x50`, Angel Fruit `0xC8`, Revive Stone `0x1F4`, ...). A
  `Heal Leaf` acquisition is therefore a bit15-clear grant into `0x80193AE0`.
- Table P confirms the split: `func_ovlAE_8021B884` loads `lhu(0x801F1004 + index*2)` (`0x8021B8CC`),
  stores it in `0x801F0B70` (`0x8021B8DC`) and branches on `a0 & 0x8000` (`0x8021B8D4`): set ->
  278-list add (`0x8021B938-0x8021B970`), clear -> 40-list add from `0x801F0B70`
  (`0x8021B9F0-0x8021BA0C`).

### 2d. Literal grants in the map loader

`func_ovlR_80215C38`'s tail (bank unit R, RAM `0x80214FA0`) has an unrolled run of
`jal 0x801DC740` with literal `a0` and `a1 = 0`: `0x80217050` compares `lbu(0x8018F4A1)` with 63/64
and grants `1,1,1,1,1,8,8,22,22` (`0x80217068-0x802170C4`), else `2x5,3x5,4x5,5x5,8x5,21,21,22,22,22,
23,23` (`0x802170CC-0x80217244`). Every `a0` is < `0x8000`, so these fill the 40-slot list, where id 1
is `Heal Leaf`. `0x80216ECC` compares the map index with `0x41` (65) and walks the byte list at RAM
`0x8021729C`. Adjacent code creates up to 30 units of stride 25 and equips class defaults through
`func_ovlN_801DD430` (`0x801DD430`, 278-list add with count maintenance).

## 3. Q3 — message id equals grant id

Yes on every path read:

- Table O arm: `s3` -> `0x801F36C2` (`0x801ADE84`) and -> grant `a0` (`0x801ADEA8`).
- `0x8019369C` arm: `s3` -> `0x801F36C2` (`0x801AE164`) and -> grant `a0` (`0x801AE188`).
- Table P: `func_ovlN_801B9694` stores `lhu(0x801F1004 + idx*2)` in `0x801F36C2` (`0x801BB8FC`) and
  grants the same word (`0x801BB930`); `func_ovlAE_8021B884` stores the same word in `0x801F0B70` then
  adds it.

A table rewrite keeps message and inventory consistent if it happens before the read and the entry's
bit 15 is preserved.

## 4. Q4 — cheapest experiment per open point

Live console (`OGRE_LIVE_CONSOLE=1`; the command file is read wholesale and removed, `OGRE_CONSOLE_AT_MS`
delays the single read). Tutorial route (session 71):
`OGRE_SCENE=tutorial OGRE_SPEED=4 OGRE_TAP_MS=1500
OGRE_TAP_SCENE_BUTTON="0x17:a:40,0x02:a:10,0x0d:a:80,0x03:a:60" OGRE_EXIT_AFTER_MS=60000`;
the practice stage appears at t≈9.8 s. Run it alone: a second instance falls into the ROM picker
(`[launcher] waiting for a ROM`).

1. Which source is live: `printf 'c\nr 80193700 1\nr 80196B0C 1\nr 8018F4A1 1\nr 801976F8 1\n
   r 80193690 1\nrh 801EDB38 4\nrh 801EDC48 4\nrh 8019369C 40\nrh 80193AE0 40\nrh 80196B20 40\n'
   > /tmp/ogre-console.txt` with `OGRE_CONSOLE_AT_MS=12000`. Expect `0x0027` at `0x801EDB38` and
   `0x0050` at `0x801EDC48` while the practice map is up. `lbu(0x801976F8) & 0x40` selects the arm;
   `lbu(0x8018F4A1)` against 63/64/65 tests §2d.
2. At the reward popup, `rh 801F36C2 1` gives the id whose name is displayed; its high bit names the
   list, and the same word in `0x80196B20`/`0x80193AE0` is the granted entry.
3. `tools/watch.sh 0x8019369C`, `0x801936D8`, `0x80193AE0` print the writer's backtrace and so name
   the module.
4. One-variable write test: with the fingerprint present, `w 801EDB38`/`w 801EDB3C` and check whether
   the popup name and the list entry move together; then repeat on `0x801E85A4`'s item bytes and on
   table P. Whichever write moves the tutorial popup is the table to randomize.

## 5. Not proven

- Which source produced the developer's tutorial `Heal Leaf` message. Candidates: the per-unit slot
  `0x8019369C` (filled from RAM `0x801E85A4`, byte ids -> special items), the map-index literal run at
  `0x80217050`, and table P entries with bit 15 clear. Table O only if `lbu(0x801976F8) & 0x40` is set.
- The `0x801E85A4` record layout (stride; whether `s0`/`s2` are a map and an object type).
- Whether `0x8018F4A1` is 63/64/65 in the practice stage.
- `func_801705BC` (`0x801705BC`, streamedB) writes a byte to `0x8019369C + index` with a byte index
  while the reader uses `+2*unit`; the two index spaces were not reconciled.
