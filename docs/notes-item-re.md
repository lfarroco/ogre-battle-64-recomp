# Item grant research (paths A: battle drop, B: map pickup)

Read-only research. Every claim below is from `mips-linux-gnu-objdump -d` of `build/*.elf`
(the recompiled C is not evidence). ROM = `assets/ogre64.z64`. Addresses are guest VRAM
unless marked ROM. Scanner: an abstract interpreter over each ELF's disassembly that tracks
constant GPR values, so a `lui`+`addiu`/`addu` base is resolved; note that MIPS sign-extends
the 16-bit displacement, so `lui r,0x8019; lhu v,-15312(r)` is `0x8018C430`, and
`lui r,0x8022; lhu v,-14496(r)` is `0x8021C760`, not `0x8022C760`.

## 1. Item id space and the item struct table

- Table base RAM `0x8018C42C` = ROM `0x6232C`, stride `0x20`, 279 records (ids 0..278).
  Proof: accessors do `sll a0,a0,5` then `lui v0,0x8019; addu v0,v0,a0` and load with
  negative displacements `-0x3BD4..-0x3BEA` (`lbu v0,-15312(v0)` at `0x8016F5FC`
  = `0x8018C430` = record+4).
- `+0x00` u32 name pointer into the packed name pool at RAM `0x8018B4D0` (ROM `0x613D0`,
  streamedB data, 4-byte-aligned `.asciz`). Reader: `func_8016F5D0` (`0x8016F5D0`,
  `lw v0,-15316(v0)` at `0x8016F5E4`) = get item name pointer.
- ids: search loops use 278 entries (`func_8016B6FC` `slti v0,v1,278` at `0x8016B720`,
  `func_80172820` `sltiu v0,v0,278` at `0x801728C0`); id bounds checks elsewhere use
  `sltiu v0,v1,279` (`bankX 0x801F4984`, `bankJ 0x801D1448`, `bankAF 0x80218980`,
  `bankC 0x8021F0A4 li a2,279`). So 279 records; id 0 is the empty-slot value
  (`func_80172820` returns early on id 0 at `0x8017282C`).
- Other fields (all in streamedB accessors): `+0x04` u8 category (`func_8016F5E8`),
  `+0x05` u8 special-cased `==16` reads `0x80193C32` (`func_8016F600`), `+0x06` u8 rank
  (`func_8016F634`), `+0x08` u16 price (`func_8016F64C`, `lhu v0,-15308(v0)`; the shop
  reads it directly, e.g. `bankAB 0x80215618`), `+0x0A` s8 attack-ish (`func_8016F67C`,
  special-cases id 0x84/132), `+0x0B..+0x0F` (`func_8016F6CC..8016F80C`), `+0x10` s8
  (`func_8016F664`), `+0x11..+0x1A` flag bytes (`func_8016F85C..8016F9F4`), `+0x1B`/`+0x1C`
  (`func_8016F5A0`/`8016F5B8`).

## 2. Owned-item list

- `0x80196B20`, stride 4, 278 slots: `+0x00` u16 item id, `+0x02` u8 (in use),
  `+0x03` u8 owned count. Cap 99. Search: `func_8016B6FC` (`0x8016B6FC`, base materialised
  at `0x8016B704-0x8016B708`, returns 511 when absent). Rebuild of the `+0x02` column:
  `func_8016B774` (`0x8016B774`, clears at `0x8016B7BC`, clears `0x80193AE2` at
  `0x8016B7A0`).
- A second, 40-slot list `0x80193AE0` has the same 4-byte shape and is written by the same
  grant routine in its other mode (see 4).
- Money (goth) is a u32 at `0x80196A8C` (cap constant `9999999` = `0x98967F`).

## 3. Path A — item received after a battle

Proven chain, all in **bank unit N record 7** (ROM `0x101D00` -> RAM `0x801AD5C0`):

- `func_ovlN_801E49D0`, at `0x801E522C`-`0x801E525C`:
  `lhu v1,14040(v1)` = `lhu(0x801936D8)` `0x801E5230`; if non-zero it stores the flag
  `sb 1,0x8021C76C` (`0x801E5240`) and `sh v1,0x8021C760` (`0x801E524C`); else
  `lhu v0,14042(v0)` = `lhu(0x801936DA)` (`0x801E5254`) and `sh v0,0x8021C760`
  (`0x801E525C`). So the pending reward item id comes from the saved game-state halfword
  `0x801936D8` (fallback `0x801936DA`).
- Goth: the reward is computed from the Chaos Frame (`lbu 0x801936C9`, `0x801E5284`) and
  values from `func_ovlN_801D34DC` (`0x801D34DC`) and table `0x801E7E3A`, then
  `sw v0,0x8021C764` (`0x801E5358`/`0x801E5390`); applied at `0x801E53B0-0x801E53D0`
  (`lw v0,0(a1)` = money `0x80196A8C`, `addu`, cap `0x0098967F`).
- Grant: `0x801E53D4 lhu a0,-14496(a0)` = `lhu(0x8021C760)`, then
  `0x801E53DC jal 0x801DC740` = `func_ovlN_801DC740`. It then clears the pending slot:
  `0x801E53F4 sh zero,14040(at)` = `sh zero,0x801936D8`.
- So: **grant routine = `func_ovlN_801DC740` (`0x801DC740`, bank unit N record 7), not
  `func_80172820`.** Signature: `a0` = item id (with bit15 as a mode flag, see 4),
  `a1` is overwritten by the routine (`andi a1,t0,0xffff` at `0x801DC758`) and is
  effectively unused; no recipient register, the recipient is the single global list.
- NOT PROVEN: which instruction stores the dropped id into `0x801936D8`. A constant-base
  scan of every ELF finds only two writers of that address: streamedB `func_80185128`
  `0x80185168` (`sh v0,0(a1)` with `a1` = `0x801936D8`, 20 halfwords from a save record —
  a load-marshalling routine) and its inverse `func_80185984` `0x80185994` (read). The
  block `0x801936D8..0x801936FF` is a 20-u16 saved game-state block. The battle banks
  (unit X, unit J, unit C) contain no constant-base write to it, so either the battle
  writes it through a pointer this scan cannot resolve, or the value arrives through the
  save-load marshalling. Cheapest proof: `OGRE_LIVE_CONSOLE=1` write watchpoint on
  `0x801936D8` during a battle, or `tools/watch.sh 0x801936D8` (`base + (0x801936D8-0x80000000)`).
  The parent's own lead — `bankJ func_ovlJ_801D61C0` `0x801D6300-0x801D6320`:
  `jal func_ovlJ_801D5DFC(participant)` returns the id in `v0`, then
  `jal func_ovlJ_801E34F8(a0=participant, a1=17, a2=id)` — is the *display* half of the
  same id; the participant struct field it lands in is not the list `0x80196B20`, and I did
  not find a copier from that struct into the list.

## 4. Path B — item picked up on the map

Also **bank unit N record 7**. The grantee is the same `func_ovlN_801DC740`:

- `func_ovlN_801B9694`, `0x801BB900-0x801BB938`: index = `lw(0x801F0E24)`
  (`0x801BB904`); item id = `lhu a0,4100(a0)` with `a0 = 0x801F0000 + index*2` =
  `lhu(0x801F1004 + index*2)` (`0x801BB930`); then `0x801BB934 jal 0x801DC740`
  (and `0x801BB914 jal 0x801DE460` on the other arm). A third byte at
  `0x801F1044 + index` (`0x801BB94C`) is read and split into bitfields.
- `func_ovlN_801AD6BC`, `0x801ADE50-0x801ADEA8`: object pointer `s2`, type
  `lbu v0,4(s2)` (`0x801ADE50`); value `s3 = lhu(0x801F0000 + (s1*4+v0)*2 - 9414)` =
  `lhu(0x801EDB3A + v)` (`0x801ADD94-0x801ADDB0`); `jal 0x801DA0C8` (`0x801ADE5C`) then
  `sw v0,-2784(at)` = `sw v0,0x8018F520` (`0x801ADE70`); UI state
  `sb (x|4),0x801F367D` (`0x801ADE90`), `sh 0x1A0,0x801F36F0` (`0x801ADE7C`),
  `sh s3,0x801F36C2` (`0x801ADE84`); then `0x801ADEA8 jal 0x801DC740` (a0 = s3) or
  `0x801ADE9C jal 0x801DE460`. The near-identical second arm is
  `0x801AE130-0x801AE188`.
- `func_ovlN_801DC740` internals: `andi t0,a0,0x7FFF` (`0x801DC744`),
  `andi a0,a0,0x8000` (`0x801DC748`); bit15 set -> 278-slot list: linear search
  `0x801DC76C-0x801DC7B0` (`lhu v0,27424(v0)` = `0x80196B20 + i*4`), found ->
  `lbu v0,0(v1); addiu; sb v0,0(v1)` on `0x80196B23 + i*4` (`0x801DC780-0x801DC788`,
  clamp 99 at `0x801DC78C-0x801DC798`); not found -> first slot with id 0, then
  `sh t0,27424(at)` (`0x801DC7FC`) and `sb v0,27427(at)` with `v0 = 1` (`0x801DC808`).
  bit15 clear -> the 40-slot list `0x80193AE0`/`0x80193AE2`/`0x80193AE3`
  (`0x801DC814-0x801DC8DC`).
- Callers of `func_ovlN_801DC740` (whole game): `bankN 0x801ADEA8`, `0x801AE188`,
  `0x801BB934`, `0x801E53DC` (the battle reward above), `bankR 0x80215BEC`, `0x80216D90`
  and the unrolled unit-init run `0x80217068-0x802171F0` (grants starting items 1,1,1,1,1,
  8,8,22, ... with `a0` = the literal id, so path-independent).

## 5. Do A and B share the grant routine?

Yes for the final write. Both end at `func_ovlN_801DC740` (unit N, record 7, streamed bank,
not a base section). `func_80172820` (streamedB, base section) is a *different* add routine
(signature `(id, count)`, adds `count`, no bit15 mode); its only `jal` in the whole game is
`bankL 0x802356F8` inside `func_ovlL_80235334`, which stacks items from a 14-entry table
(u16 ids at +0x00, u16 counts at +0x1C) onto a defeated participant's list — the post-battle
item handling of the chapter bank, not the drop roll.

## 6. Every writer of `0x80196B20` found (address, unit, what it is)

Constant-base scan of `0x80196B20..0x80196B24`:

| address | unit | what it is |
|---|---|---|
| `0x8016B70C`, `0x8016B918`, `0x8016B990`, `0x8016BB3C`, `0x8016BBB4` | main / streamedB | reads inside `func_8016B6FC`/`func_8016B774`/neighbours (search + rebuild of the `+0x02` column) |
| `0x80172DFC/0x80172E0C/0x80172E10/0x80172E18`, `0x80172EC8..`, `0x80172F94..`, `0x80173060..` | main / streamedB `func_80172C64` | class-change transfer: append `sh s1,0(v1)` and zero both counts; `func_80172820` also writes `+0x03` at `0x80172874` (increment) and appends at `0x801728E8`/`0x801728F4` |
| `0x801A42B8/0x801A42BC/0x801A42C4`, `0x801A4384..`, `0x801A4450..`, `0x801A451C..` | bank M (`func_ovlM_801A4110`) | the same class-change transfer, copy inside the scene-0x05 module |
| `0x801DC780-0x801DC798`, `0x801DC7FC`, `0x801DC808` | bank N (`func_ovlN_801DC740`) | **the grant routine**: increment `+0x03`, else append id and count 1 |
| `0x801C25FC`, `0x801C2614` | bank S (`func_ovlS_801C214C`) | debug/cheat: sets `+0x03` to 98 for every non-empty slot, gated on `lbu(0x800E79B0)&8` and `func_801707B4` |
| `0x801D3488` | bank S | read only |

`0x8019...` writes with a register-relative base (not constant-base visible): `bankN
0x801DC7FC`/`0x801DC808` are the ones above.

## 7. Does the granted id reach the message through the list?

- Path A display: the id is in a register. `bankJ func_ovlJ_801D61C0` state 1
  (`0x801D6300-0x801D6320`): `jal func_ovlJ_801D5DFC(participant)` -> `v0` -> `s0`, then
  `jal func_ovlJ_801E34F8(a0=participant, a1=17, a2=s0)`. No read of `0x80196B20` in the
  battle banks at all (scan of `0x80196B20` returns only bank M/N/S and the main unit).
- Path B popup: `func_ovlN_801AD6BC` writes the id into the UI state
  `sh s3,0x801F36C2` (`0x801ADE84`) and passes `s3` in `a0` to the grant; the name for the
  popup is drawn from the register/UI field, not from `0x80196B20`
  (`func_ovlN_801DDE18` `0x801DE3F0` reads `func_8016F5D0` on `lhu 0x801F0B70`, a UI field).
- Consequence: a post-hoc rewrite of `0x80196B20` changes the owned set but not the already
  posted id; rewriting the id register/UI field is what changes the displayed item.

## 8. Base section vs streamed bank (matters for mod hooks)

- `0x8018C42C` item struct table, `0x8018B4D0` name pool, `func_8016B6FC`, `func_8016B774`,
  `func_80172820`, `func_80172C64`: **streamedB**, a base section of `build/ogrebattle64.elf`
  (config `streamedB`, ROM `0x40E80` -> RAM `0x8016AF80`). Hookable.
- `0x80196B20` list and `0x80196A8C` money: plain RAM, no owning segment.
- `func_ovlN_801DC740` (`0x801DC740`), `func_ovlN_801E49D0` (`0x801E49D0`),
  `func_ovlN_801B9694` (`0x801B9694`), `func_ovlN_801AD6BC` (`0x801AD6BC`): **bank unit N
  record 7**, ROM `0x101D00` -> RAM `0x801AD5C0` (`k_N_bankRec7`). Replaced per scene, not
  hookable as a base section.
- `func_ovlL_80235334` (`0x80235334`): bank unit L (record 14a), ROM `0x29A490` ->
  `0x802395E0`. Streamed.
