# Drop / pickup item-id tables, and how the game reads them

Read-only research; no tracked file changed. Every claim is from
`mips-linux-gnu-objdump -d build/*.elf` plus raw ROM bytes at the ROM offsets the game's
own segment table (ROM `0x387C0`, 0x28-byte entries) gives. `tools/guestmap.py` was used
for record ownership. Generated C was not used as evidence.

Arena layout that the addresses below depend on (all chunk-DMA'd into `0x801AD5C0..0x801F70F0`):

| record | ROM | RAM | size | what |
|---|---|---|---|---|
| 7 (bankN) | `0x101D00` | `0x801AD5C0` | `0x43530` | mission/map bank; DMA data ends `0x801F0AF0`, segment BSS ends `0x801F4050` |
| 10 (bankC rec10) | `0x1F0A00` | `0x801AD5C0` | `0x230E0` | first arena chunk (holds `0x801CE8DC`) |
| 10a (bankJ) | `0x213AE0` | `0x801D0860` | `0x16770` | battle-result bank |
| 10c (unit X) | `0x22A250` | `0x801E6FD0` | `0x10120` | `rec10` battle bank (its bytes at `0x801F1004` are code) |
| 10b (bankC) | `0x23B1F0` | `0x801E6FD0` | `0x9580` | second bank of the same window; holds `0x801E7E36` |
| 9d (bankR) | `0x195430` | `0x80214FA0` | `0x2380` | the map loader `func_ovlR_80215C38` |

## 0. Q1 correction: `func_ovlJ_801D5DFC` does not decide an item

`0x801D5DFC`-`0x801D61BC` returns `(int)ceil(f20)` clamped to `[1,100]`, or `0`:

- `0x801D5E30 sltiu v0,v0,30` on `lbu(0x801976FC)` (`0x801D5DFC-0x801D5E04`); `>= 30` returns 0
  (`0x801D5F50 j 0x801D6194`, `v0=0`).
- `0x801D5E44` `lbu v0,24717(v1)` with `v1 = *(0x801CE8DC)` = battle-state pointer: `+0x608D` is a
  count, `+0x608E` a byte array copied to `sp+0x10` (`0x801D5E60-0x801D5E8C`).
- 20-participant loop `0x801D5EA4-0x801D5F40`: `func_801C9008(i)`, filters `func_801C8BA4`,
  `func_801C8E50`, `func_801C8EBC`, `func_801C8B88`, `func_801C8C5C`, `lhu(s0+0x20)==0`; then
  `sp+0x10[s1] = lbu(s0+0x31)` and `sp+0x18[s1] = func_8016DE3C(lbu(s0+0x4B), lbu(s0+0x4F))` (`0x801D5F14-0x801D5F30`).
- `0x801D5FD0-0x801D6080`: `f20 += min(1.0,(v-level+8))^2 / (float)count * m` with `m = 0.8` (weight 0)
  or `1.5` (weight 2), the two doubles at `0x801E66F0 = 0.8` and `0x801E66F8 = 1.5`
  (bankJ data, ROM `0x229970`/`0x229978`, `ldc1` at `0x801D5FE8`/`0x801D5FF0`).
- `0x801D6094 jal 8009D3D0` is `ceilf` (`.main`, ROM `0x2D7D0`, `4600638e ceil.w.s`);
  `0x801D60A8-0x801D60F4` clamps to `[1.0,100.0]`; `0x801D6108-0x801D6114` adds `s0` (`0`,`1`,`2`);
  `0x801D618C trunc.w.s`.
- Caller state 1 of `func_ovlJ_801D61C0` (`0x801D629C-0x801D6344`): `s0 = func_ovlJ_801D5DFC(p)`,
  then `func_ovlJ_801E34F8(a0=p, a1=17, a2=s0)` (`0x801D631C`) and `lbu v0,50(p); v0 += s0; sb v0,50(p)`
  (`0x801D6324-0x801D6330`) — `p+0x32` is a 0..99 counter: state 2 (`0x801D6398 sltiu v0,v0,100`,
  `0x801D63b0 sb zero,50(s0)`) consumes it as experience and levels the unit up.
- No item-id table is read anywhere in `0x801D5DFC`. `func_ovlJ_801E34F8` (`0x801E34F8`) only enqueues
  `{type=+0x08, participant=+0x0C, payload=+0x10}` into the 0x84-byte record queue at `0x801D083C`.

So the battle banks (bankJ, unit X) contain no drop table and no drop roll. The premise that
`0x801D5DFC`'s return value is the dropped id is wrong; it is the experience/point award (message
type 17; that label is inferred from state 2's level-up consume, the message text itself was not
traced). The element that *does* match "battle reward" is the saved u16 block at `0x801936D8`
(read by bankN `0x801E5230-0x801E525C`, granted at `0x801E53DC`), and `0x801936D8` is the first of
20 u16 copied by the streamedB save-field loader `func_80185128` (`0x80185128`, `sh v0,0(a1)` with
`a1 = 0x801936D8`, saved by `func_80185984`; the pair is entry 3's `+0x10` load2 / `+0x14` save2 slots,
ROM `0x386B0`/`0x386B4`, offset2 `0x0ECC`, in the save-field table at ROM `0x3864C`). A
constant-base scan of every ELF finds no other writer of
`0x801936D8`/`0x801936DA`, and no ROM word holds those addresses — which contradicts the mod
README's "the battle result computes it" and needs the experiment in §5.

## 1. Table O — the per-object drop table with the `rand()%3` roll (Q2, Q3)

RAM `0x801EDB38` .. `0x801EDC50`, **35 entries x 8 bytes, zero-terminated**, RAM = ROM
`0x101D00 + (ram - 0x801AD5C0)`, so ROM `0x142278`. It is plain ROM data of the mission bank, i.e.
identical for every map and *not* per-map (unit X's bytes at that address, ROM `0x230DB8`, are code).

Entry: `+0x00 u16 object/unit type`, `+0x02 u16 itemA`, `+0x04 u16 itemB`, `+0x06 u16 itemC`.
Item word: bit15 selects the list that `func_ovlN_801DC740` writes (set -> 278-slot player list
`0x80196B20`; clear -> 40-slot list `0x80193AE0`); low 15 bits are the item id (1..278).

Reader `func_ovlN_801AD6BC` (bankN record 7, entry RAM `0x801AD6BC`):

- `0x801ADD3C-0x801ADD64`: scan for the entry whose `+0x00 == lbu(0x80197708)`, `s1` = index,
  `addiu v1,v1,8` (stride 8), stop at a zero type.
- `0x801ADD68-0x801ADD90`: `func_80092A60` (rand, `.main`) `% 3` via magic `0x55555556`.
- `0x801ADD94-0x801ADDB0`: `lhu(0x801EDB3A + (s1*4 + r)*2)` — the `+2` base is table O's itemA field,
  so the roll picks itemA/itemB/itemC (`0x801ADDB0`).
- `0x801ADDB4`: gate `lbu(0x80193690) & 0x10`.
- `0x801ADE74-0x801ADE94`: `sh s3,0x801F36C2` (the popup's id field), `sh 0x1A0,0x801F36F0`,
  `sw type,0x801F0E20`, `sw 30,0x801F0E00`, `sw -1,0x801F0E08`, `sw func_801DA0C8(s2),0x8018F520`.
- popup: `func_ovlN_801AE880` reads that field at `0x801AF7C8` (`lhu a0,14018(a0)`) and calls
  `func_ovlN_801DE460`, which masks bit15 (`0x801DE47C andi v0,s2,0x8000`) and then indexes the icon
  sheet asset `0x01DEE4B0` at `0x100 + (id&0x7FFF)*0x100` (`0x801DE4CC-0x801DE4E4`); the sheet is a
  512-byte palette plus 256-byte 16x16 8bpp icons (asset header `us3.bg2`).
- grant: `0x801ADEA8` and `0x801AE188` `jal 801DC740` with `a0 = s3`.

Both the popup and the inventory use `s3`, so rewriting table O's `+0x02/+0x04/+0x06` makes them agree.
Example entry 0: type `0x27` -> `0x8031`(49 Halt Hammer), `0x809b`(155 Leather Armor),
`0x80e8`(232 Bandanna) — the three columns are three item *categories*, not three rolls of one kind.

## 2. Table P — the per-map item list parsed at map load (Q4)

RAM: `0x801F1002` u8 count, `0x801F1004` u16 id array (index*2), `0x801F1024`/`0x801F1034`/`0x801F1044`
byte arrays. These are in record 7's BSS (`> 0x801F0AF0`, zeroed on load) but also lie inside unit X's
DMA window, so they only mean this while record 7 (or the map chunk) is resident.

Writer, the only one: `func_ovlR_80215C38` (bankR record 9d, ROM `0x195430` -> RAM `0x80214FA0`),
called from `func_ovlN_801AE880` at bankN `0x801AEFE4`:

- `0x80215CC4-0x80215CF4`: `a0 = 0x021AF67C`, `a2 = lbu(0x801E7E36 + lbu(0x8018F4A1)*12)`,
  `jal 8007938C` — `func_8007938C` (`.main`, ROM `0x978C`) returns the map's data buffer.
- `0x80215D04-0x80215D18`: first byte of the buffer -> `0x801F1002` (count).
- `0x80215D28-0x80215DB4`: per entry `s3` read 4 bytes -> `0x801F1044[s3]`, `0x801F1024[s3]`,
  `0x801F1034[s3]`, then a flag byte (`==1` -> value `|= 0x8000`) plus two bytes `b1<<8|b2` added, and
  `sh` to `0x801F1004 + s3*2` (`0x80215D74`/`0x80215D7C`).
- `0x80215DB8-0x80215DC8`: `func_80079618(0x021AF67C,0,-17)` releases it.

Reader `func_ovlN_801B9694` (bankN record 7):

- `0x801BB8B8-0x801BB8FC`: `idx = lw(0x801F0E24)`; `v1 = lhu(0x801F1004 + idx*2)`; `sh v1,0x801F36C2`.
- `0x801BB914`: `func_ovlN_801DE460(lhu(0x801F1004 + idx*2))` (the same popup builder).
- `0x801BB930`: `func_ovlN_801DC740(lhu(0x801F1004 + idx*2), 0)` (the grant).
- `0x801BB93C-0x801BB988`: `flag = lbu(0x801F1044 + idx)`; set bit `flag%8` of `0x80196A58 + flag/8`.

Again one value feeds both the popup and the grant, so rewriting `0x801F1004[i]` after the parse makes
them agree. The per-map selector byte `lbu(0x801E7E36 + idx*12)` lives in the bank at `0x801E6FD0`
(record 10b, ROM `0x23B1F0`, byte ROM `0x23C056`); the same 12-byte record's u16 `+0x04`
(`0x801E7E3A`) is the battle-reward goth base (`0x801E529C`). `lbu(0x8018F4A1)` is the index; its
exact meaning (map id vs mission id) is unverified.

The known "battle reward" id `0x801936D8` is **not** read by either table's code.

## 3. Q3 and Q5 direct answers

- Whatever indexes drop item ids is read by the **mission bank, unit N record 7** (table O at
  `0x801ADD44/0x801ADD50/0x801ADDB0`; table P at `0x801BB8D8/0x801BB918/0x801BB930/0x801BB94C`).
  Nothing in bankJ or unit X reads either table. The only base-section involvement is
  `func_8007938C` (`.main`), which returns the raw per-map buffer that bankR parses — it never sees a
  parsed item id, and it is shared by other assets.
- A `.main`/streamedB **code hook cannot intercept the read**, but a base-section frame hook (the
  mod's `func_80072944`) can *write* the tables in RAM, which is what the mod needs: RAM writes are
  not section-restricted.
- Hazard: `0x801EDB38`, `0x801F1002..0x801F1044` are in windows shared with unit X (ROM `0x22A250`)
  and record 10b. Writing them while those banks are resident corrupts their code. Gate every write on
  a fingerprint of record 7's image, e.g. `lhu(0x801EDB38)==0x0027` and
  `lhu(0x801EDB38+0x110)==0x0050` (entries 0 and 34) — X's words there are code and never match.
- Cheapest confirming experiment (live console, `OGRE_LIVE_CONSOLE=1`, console file
  `printf 'rh 801EDB38 4\nrh 801EDC48 4\nrb 801F1002 1\nrh 801F1004 8\ndump /tmp/map.bin\n' > /tmp/ogre-console.txt`):
  with a map loaded, `0x801EDB38` must read `0027 8031 809b 80e8` and `0x801EDC48` (entry 34, offset
  `+0x110`, type `0x50`) `0050 8091 80b2 80df`; `0x801F1002` is the parsed per-map count and
  `0x801F1004` its ids. Then prove the mechanism with one write. `w` (app/src/sdl_platform.cpp:1584)
  memcpy's a 32-bit value into RDRAM at `addr & 0x1FFFFFFF`, so one `w` covers two halfwords and the
  halfword order must be confirmed with a following `rh`: write `0x801EDB38`/`0x801EDB3C` so that all
  three item fields of the target entry hold the wanted id (e.g. id 1 with bit15 clear), then confirm
  by `rh 801EDB38 4` that the popup and the inventory both name item 1. `tools/rdram.py half`/`byte` on
  the dump reads the same words offline.
- Discriminating experiment for §0: `w`-watch `0x801936D8` (`tools/watch.sh 0x801936D8`) or read it
  every frame across a full map battle. If nothing in the map/battle banks writes it, the mod README's
  battle-reward claim is wrong and the reward value can only arrive from a loaded save slot
  (`func_80185128`).

## 4. Not proven

- Which of the two tables covers which in-game event ("defeat an enemy unit" vs "search the map"):
  both are reached from `func_ovlN_801AE880` (`jal func_ovlN_801B9694` at `0x801AF47C`,
  `jal func_ovlN_801E49D0` at `0x801AF3E0`), and `func_ovlN_801AD6BC` from `0x8019F4AC`. That call site
  is in bankA record 3's image (ROM `0xEBBD0` -> RAM `0x8019EE70`), but the RAM is shared by the
  scene-0x07 module H (ROM `0x712A0`), the map module M (ROM `0x79750`) and the organize screen S
  (ROM `0x87220`), so the calling module is not pinned down here.
- The role of the 40-slot list `0x80193AE0` (bit15-clear mode). Its readers are the item/organize
  screens (`0x80218C84`, `0x80218DFC`, `0x8021B884`, `0x801A7960`), i.e. it is not obviously an
  inventory, so a rewrite should preserve each entry's bit15 mode rather than clear it.
- Whether the popup and the inventory can *ever* disagree for the same id: `func_ovlN_801DE460` masks
  bit15 before indexing its icon sheet, and the name comes from the item table (`0x8018C42C`,
  ROM `0x6232C`, pool ROM `0x613D0`), so absent a mod rewrite they agree.
