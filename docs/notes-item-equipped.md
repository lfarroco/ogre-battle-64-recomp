# Owned-item list: the `x/y` pair, the `+0x02` byte, and the equip test

Read-only research. No tracked file was changed except this one. Every address is guest VRAM.
Evidence is `mips-linux-gnu-objdump -d` of `build/ogrebattle64.elf` and `build/bank*.elf`, plus the
raw ROM at `assets/ogre64.z64`. The generated C was not used as evidence. The game was not run.

Scope of the question: the developer's item-randomizer mod writes only the `+0x00` id halfword of a
record in `0x80196B20`, and then saw an item ("Durandel") shown as `1/1` that could not be equipped.
Session 122 recorded that as open work item 1.

## 0. Summary

The `x/y` pair on screen is `+0x02 / +0x03` of the same 4-byte record, printed left to right in that
order. `+0x02` is a count of roster references to the item, and `+0x03` is the number owned. The equip
test is `+0x02 < +0x03` on the record found by searching `0x80196B20` from index 0. No code derives
`+0x02` from the id, and no code recomputes `+0x02` while a screen is open. An id-only write therefore
carries the record's existing `+0x02` to the new item's name, and it can leave a unit's equipment
referring to an id that the list no longer contains.

## 1. Where the pair is printed (question 1)

Site `func_ovlS_801A6A70` (`0x801A6A70`), bank unit S, scene `0x06` (Organize Screen), the unit's
equipment rows:

* `0x801A6D84` `lw v0,27416(v0)`; the record index for row `n` comes from
  `lhu v0,7150(ptr+s1)` with `ptr = *(0x80196B18)` and `s1 = 2n`, i.e. the u16 array at `ptr+0x1BEE`.
  That array holds indices into `0x80196B20`, proven by `func_ovlS_801CFC44` (`0x801CFC8C`
  `jal 8016B6FC`, `0x801CFCBC` `sh a0,7150(v1)`). `ptr` is a heap block of 9512 bytes that scene
  `0x06`'s module entry allocates (`0x801C19B4` `li a0,9512`, `0x801C19D4` `sw a0,27416(at)`), so
  `0x80196B18` holds a pointer to that block and not a static table.
* `0x801A6D94` `sll v0,v0,2`, then `0x801A6DA0` `lbu v1,27427(...)` = `+0x03` of `0x80196B20 + idx*4`.
* `0x801A6CE0`-`0x801A6D28`: draws `+0x02` (`0x80196B22 + idx*4`) at x = `s5+113`,
  then glyph 25 at x = `s5+128`, then `+0x03` (`0x80196B23 + idx*4`) at x = `s5+137`.
  The two draws are `func_ovlS_8019B8C4` (an integer of `a3 = 2` digits) and the middle one is
  `func_ovlS_8019A7C0` (glyph index 25 from the sheet at `0x801EE8A8`). Inference: glyph 25 is the
  separator character, because the developer reads the two numbers with a `/` between them. Candidate
  check: render glyph 25 of the sheet at `0x801EE8A8`.
* The item name in the same rows comes from `func_8016F5D0` at `0x801A7340`, with the id taken from
  the 4-entry stack array at `sp+0x18` (`0x801A733C` `lhu a0,0(s0)`). That array is filled at
  `0x801A6C14` from the table at `0x801EF2A8` (bank S data; the four pointers are `func_8016DFA8`,
  `func_8016DFFC`, `func_8016E050`, `func_8016E0A4`), so the name comes from the unit's
  class-derived equipment id, and the counts come from the record at the cached index. These are two
  different sources.

Same pair, other sites:

| site | function | bank | pair drawn | memory |
|---|---|---|---|---|
| `0x801A6CE0`-`0x801A6D28` | `func_ovlS_801A6A70` | S | `+0x02` at x+113, glyph 25, `+0x03` at x+137 | record at `ptr+0x1BEE` index |
| `0x801B0DB4`-`0x801B0DFC` | `func_ovlS_801AFECC` | S | `+0x02` at x+111, glyph 25, `+0x03` at x+135 | record index from the same cache |
| `0x801A97A0`-`0x801A97D8` | `func_ovlS_801A8818` | S | `+0x02` at x+111, glyph 25, `+0x03` at x+135 | `s4*4`, the same index as the name at `0x801A8FB4` |
| `0x80217DD0`-`0x80217E08` | `func_ovlAB_80217A54` | AB (shop) | `+0x02` at `s3+12`, `+0x03` at `s3+38` | `func_8016B6FC`/`func_8016B738` result |
| `0x80218994`, `0x802189A0` | `func_ovlAF_80218910` | AF | reads `+0x03` then `+0x02`, compares | `func_8016B6FC` result |
| `0x801D3184`, `0x801D3190` | `func_ovlS_801D2F88` | S | reads `+0x02` then `+0x03`, compares | `func_8016B6FC` result |
| `0x801B350C`, `0x801B3518` | `func_ovlS_801B3030` | S | reads `+0x02` then `+0x03`, compares | `func_8016B6FC` result |
| `0x801D2B2C`-`0x801D2B50` | `func_ovlS_801D2A18` | S | computes `+0x03 - +0x02` and keeps the maximum | `func_8016B6FC` result |

In all four draw sites the `+0x02` draw is issued first and the `+0x03` draw second, at the lower x.
The printed pair is therefore `used / owned`. `1/1` means the record claims one used and one owned.
The number printer that the shop rows call is `0x802004E4`, which unit N's record 7 defines as
`func_ovlN_802004E4` (`0x802004E4`) and bank AB reaches as an external through the shared arena.

The `x` and `y` are both read from the same record. In `func_ovlS_801A8818` the name is read from the
record id at `0x801A8FB4` and the pair from the same index, so on that screen name and counts agree
with the record.

Callers of the id/`+0x03`/`+0x02` accessors `func_8017069C`, `func_80170688`, `func_801706B0`:
no `jal` to any of the three exists in the whole build. Their only reference is the pointer table at
`0x8018F2C0` in `.streamedB`, which holds
`{0x80170654, 0x80170668, 0x80170688, 0x8017069C, 0x801706B0, 0, 0, 0x801706C4}`. An exhaustive scan
of every ELF for a load of `0x8018F2C0` (any `lui`+`addiu`/`ori` producing the address, and any
displacement `-3392` or `0xF2C0` from a `0x8019` base) found no reader, so the item screens read the
bytes directly. Bank S does use its own data copies of the item accessors: `0x801EF2A8` (called at
`0x801A6C14`, `0x801A0094`, `0x801CFC70`, `0x801D2AF8`) and `0x801EF2B8` (called at `0x801A7E28`,
`0x801A7E40`). Those entries are the `.streamedB` functions `0x8016DFA8`-`0x8016E0A4` and
`0x8016F67C`-`0x8016F8EC`.

Callers of the item-name accessor `func_8016F5D0` (12 sites, whole build):

| site | function | bank |
|---|---|---|
| `0x80180484` | `func_8017FC48` | base |
| `0x801A00A8` | `func_ovlS_8019FEF0` | S |
| `0x801A7340` | `func_ovlS_801A6A70` | S |
| `0x801A8FBC` | `func_ovlS_801A8818` | S |
| `0x801B0998`, `0x801B09C0` | `func_ovlS_801AFECC` | S |
| `0x802158B0` | `func_ovlAB_80214FA0` | AB |
| `0x802164DC` | `func_ovlAB_80216184` | AB |
| `0x8021B4DC` | `func_ovlAE_8021B464` | AE |
| `0x802183D4` | `func_ovlAF_802178A4` | AF |
| `0x802360E8` | `func_ovlL_80235C98` | L |
| `0x801DE3F0` | `func_ovlN_801DDE18` | N |

The name is `*(u32 *)(0x8018C42C + id*0x20)` (`func_8016F5D0` at `0x8016F5D0`, `lw v0,-15316(v0)` at
`0x8016F5E4`). The name always follows the record's own id on the screens that read the id from the
record.

## 2. What `+0x02` is (question 2)

`func_8016B774` (`0x8016B774`, size `0x29C`) is a full rebuild of the `+0x02` byte of both lists from
the roster. It is answer (a): a count of how many roster entries currently reference the item. It is
not a cache and not an index. Evidence, in order:

1. `0x8016B798`-`0x8016B7A8`: loop `at = 0x80190000 + v0`, `sb zero,15074(at)` at `0x8016B7A0` with
   `v0` running from 156 down by 4. That clears `0x80193AE2 + i*4` for the 40 records of the
   consumable list.
2. `0x8016B7B0`-`0x8016B7C4`: the same loop with `v0` from 1108 down by 4 and
   `sb zero,27426(at)` at `0x8016B7BC`. That clears `0x80196B22 + i*4` for the 278 records.
3. `0x8016B7CC`-`0x8016B870`: loop over 30 unit/battalion records at `0x80197210 + i*0x19`
   (`0x8016B870` `addiu a3,a3,25`). `0x8016B7DC` `lbu v0,1(a3)`, `0x8016B7E0` `andi v0,v0,0x1`; skip
   when clear. Then 10 bytes at `a3+0x0D+n` (`0x8016B7F4` `lbu a2,13(v0)`), each searched in the
   40-slot list (`0x8016B810`-`0x8016B830`, bound `0x8016B824` `slti v0,v1,40`), then
   `0x8016B848`-`0x8016B850` `lbu v1,15074(...)`, `addiu v1,v1,1`, `sb v1,2(v0)`.
4. `0x8016B874`-`0x8016B9E8`: loop `s2 = 1..99` over the 100 character records at
   `0x80193BE0 + s2*0x38` (`0x8016B9EC` `addiu s4,s4,56`). `0x8016B890` `lbu v0,17(s1)`; skip when
   zero. For `s0 = 0..3` the id is `lhu a2,42/44/46/48(s1)` (`0x8016B8DC`, `0x8016B8E8`,
   `0x8016B8F4`, `0x8016B900`), each searched in the 278-slot list (`0x8016B910`-`0x8016B934`, bound
   `0x8016B928` `slti v0,v1,278`), then `0x8016B948`-`0x8016B954` increments `+0x02`.
5. `0x8016B958`-`0x8016B9CC`: for the same `s0 = 0..3` it loads a function pointer from the table at
   `0x80186FF4` (`0x8016B964` `lw v0,28660(at)`, `at = 0x80180000 + s0*4`) and calls it with
   `a0 = lbu 17(s1)`, `a1 = lbu 18(s1)` (`0x8016B968`-`0x8016B970`). A non-zero result is searched in
   the 278-slot list and its `+0x02` is incremented at `0x8016B9CC`. The four pointers are
   `0x8016DFA8`, `0x8016DFFC`, `0x8016E050`, `0x8016E0A4` (ROM dump of `0x80186FF4`).

So one increment per roster reference, for the 4 stored equipment ids and for the 4 class-derived ids.
The class-derived ids come from the table at `0x80187C62`/`0x80187C64`/`0x80187C66`/`0x80187C68`
(stride `0x48`, comparison byte at `0x80187C79`) and are not stored in the character record.

The byte is not saved. The save descriptor entry 18 at `0x80187448` is `(00,02,09)`, `(03,01,07)`,
so the save blob carries `+0x00` (9 bits) and `+0x03` (7 bits) only. The rebuild is therefore also the
restore path: the save-load function `func_8016CEC4` calls `func_8016B774` at `0x8016CF30`.

Every writer of the `+0x02` byte found by a store scan of the whole build (immediate displacements
`27426`/`15074`, plus every register-relative `sb ...,2(vX)` in the item and equipment banks, each
read in context):

| site | function | unit | operation |
|---|---|---|---|
| `0x8016B7BC` | `func_8016B774` | base `.streamedB` | `+0x02 = 0` for all 278, then increments |
| `0x8016B850`, `0x8016B954`, `0x8016B9CC` | `func_8016B774` | base | `+0x02++` per roster reference |
| `0x8016BC20` | `func_8016BA6C` | base | `+0x02--` per unequipped reference |
| `0x80170770` | `func_80170760` | base | setter: `+0x02(index) = a2` |
| `0x80170734` | `func_80170724` | base | setter for the 40-slot list |
| `0x80172CD4`, `0x80172D0C`, `0x80172D44`, `0x80172D7C`, `0x80172DCC`, `0x80172E98`, `0x80172F64`, `0x80173030` | `func_80172C64` | base | `+0x02--` (class-change transfer, remove half) |
| `0x801A4180`, `0x801A41B8`, `0x801A41F0`, `0x801A4228`, `0x801A4278`, `0x801A4344`, `0x801A4410`, `0x801A44DC` | `func_ovlM_801A4110` | M | `+0x02--` (the same transfer, scene `0x05` copy) |
| `0x801CEE04` | `func_ovlS_801CEC54` | S | `+0x02--` |
| `0x801CA6A4` | `func_ovlS_801CA5A8` | S | `+0x02--` (40-slot list) |
| `0x801CE884` | `func_ovlS_801CE7AC` | S | `+0x02--` |
| `0x801CEBF8` | `func_ovlS_801CEB44` | S | `+0x02--` (40-slot list) |
| `0x801D0620` | `func_ovlS_801D04A4` | S | `+0x02--` (40-slot list) |
| `0x801D120C` | `func_ovlJ_801D1014` | J | `+0x02++` (with `+0x03++`) |
| `0x801D1238` | `func_ovlJ_801D1014` | J | `+0x02 = s2` together with `+0x03 = s2` and the id |
| `0x801DD4DC`, `0x801DD53C` | `func_ovlN_801DD430` | N | `+0x02++` (clamp 99) |
| `0x801C4CAC` | `func_ovlN_801C4AAC` | N | `+0x02--` (40-slot list) |
| `0x80219564`, `0x8021958C`, `0x802195E4`, `0x8021960C`, `0x80219664`, `0x8021968C`, `0x802196E4`, `0x8021970C` | `func_ovlAF_80218E68` | AF | `+0x02++` (clamp 99) with no ownership test |
| `0x80216CBC`, `0x80217034` | bank AE code, enclosed by the label `func_ovlAE_80214FA0` | AE | `+0x02--` (40-slot list) |
| `0x80218E98`, `0x80218EB4` | `func_ovlAB_80218DFC` | AB | `+0x02 += s2` (40-slot list) |

`func_ovlN_801DC740` (the grant routine, bank N record 7) does not appear in this table. It writes the
id at `0x801DC7FC` and `+0x03 = 1` at `0x801DC808` for a new record, and `+0x03++` at
`0x801DC788` for a record that already holds the id. It never writes `+0x02`.

## 3. The equip availability test (question 3)

The test consults the `0x80196B20` record and compares the two bytes in place. It is
`used < owned`, computed with `sltu` on the two bytes. The candidate ids can come from the character
record, and the comparison that decides availability reads only the two stored bytes. No per-item
total and no subtraction is computed anywhere else.

Proven sites:

* `func_ovlS_801D2F88` (`0x801D2F88`, bank S, scene `0x06`):
  `0x801D3134` `li s7,511`; `0x801D3164` `jal 8016b6fc`; `0x801D3170` `beq v0,s7` returns
  `v1 = 0` for an absent item; `0x801D3184` `lbu v1,27426(v1)` = `+0x02`;
  `0x801D3190` `lbu v0,27427(at)` = `+0x03`; `0x801D3194` `sltu v1,v1,v0`;
  `0x801D31C4` `andi v0,v1,0xff`, `0x801D31C8` `bnez v0`. A spare exists when `+0x02 < +0x03`.
* `func_ovlS_801B3030` (`0x801B3030`): `0x801B34E8` `jal 8016b6fc`; `0x801B34F4` `li v0,511`;
  `0x801B34F8` `beq a0,v0` sets `v1 = 0`; `0x801B350C` `+0x02`; `0x801B3518` `+0x03`;
  `0x801B351C` `sltu v1,v1,v0`.
* `func_ovlAF_80218910` (`0x80218910`, bank AF, the record-9 arena): builds four ids from
  `func_8016DFA8`/`DFFC`/`E050`/`E0A4` with class 36 (`0x80218914`-`0x80218964`), then per id
  `0x80218974` `jal 8016b6fc`; `0x80218980` `sltiu v0,v1,279` rejects the 511 sentinel;
  `0x80218994` `+0x03`; `0x802189A0` `+0x02`; `0x802189A4` `sltu v0,v0,v1`; `0x802189A8` `bnez`.
  It returns 1 only when all four ids have a spare.
* `func_ovlS_801D2A18` (`0x801D2A18`): `0x801D2AF8` calls the four accessors from the table at
  `0x801EF2A8` (`0x801D2AEC` `lui s1,0x801f`, `0x801D2AF0` `addiu s1,s1,-3416` = `0x801EF2A8`), then
  `0x801D2B14` `jal 8016b6fc`, `0x801D2B2C` `+0x03`, `0x801D2B38` `+0x02`,
  `0x801D2B3C` `subu v1,v1,v0`, `0x801D2B40` `sltu v0,v1,s2`. It keeps the maximum of
  `owned - used` over the four ids.

The routines that raise `+0x02` are `func_ovlAF_80218E68` (bank AF) and `func_ovlJ_801D1014`
(bank J). The store scan finds five writers of that byte in bank S, and every one is a decrement
(`0x801CA6A4`, `0x801CE884`, `0x801CEBF8`, `0x801CEE04`, `0x801D0620`). So in the Organize screen
(bank S) the availability test can pass several times for one item while `+0x02` stays at its stored
value.

## 4. An id-only write of a `0x80196B20` record (question 4)

No mechanism exists by which the id write itself raises a count. `+0x02` and `+0x03` are read from
the record by the draw code and by the test. No instruction in any ELF computes either byte from the
id. A store of the id halfword leaves both bytes as they were.

Two mechanisms below do turn an id-only write into the reported symptom, one mechanism renames a
genuinely equipped item, and one further mechanism is a side effect on other memory. All of them are
consequences of the record's own stored bytes, and all four are proven at instruction level.

**Mechanism A: the record's existing `+0x02` is displayed under the new name.**
The grant routine reuses the first record whose id is 0 and writes the id and `+0x03 = 1` only
(`0x801DC7FC`, `0x801DC808`). An id of 0 with `+0x02` non-zero is reachable, because `+0x02` is
maintained by separate increments and decrements and the unequip path clears the id whenever `+0x03`
reaches 0 (`func_8016BA6C`, `0x8016BC14` `lbu +0x02`, `0x8016BC1C` `addiu v0,v0,-1`,
`0x8016BC20` `sb`, `0x8016BC2C` `lbu +0x03`, `0x8016BC40` `sh zero,27424(at)`). One decrement of
`+0x02` and one decrement of `+0x03` happen per unequip, so a record whose `+0x02` was 2 while its
`+0x03` was 1 ends with `+0x02 = 1` and id 0. `+0x02 = 2` with `+0x03 = 1` is reachable because
`func_ovlAF_80218E68` raises `+0x02` for the class-derived equipment without any comparison against
`+0x03` (`0x80219534` search, `0x80219554`-`0x8021958C` increment and clamp 99; no `sltu` in that
block). Bank AF has eight stores to `+0x02` and no store to `+0x03` at all, so that routine adds a
user without adding a copy. The same class-derived ids are counted once per character of that class
by `func_8016B774` step 5. The next grant into that record inherits `+0x02 = 1`, and the pair reads
`1/1` with no unit using the new item. The id write is what changes which item name carries that 1.
The rebuild that would clear it runs only at `0x8016CF30` (save load), `0x801728F8` (the count
add/remove routine `func_80172820`), `0x801D6368` (bank I `func_ovlI_801D62A0`), `0x801D8CAC`
(bank J `func_ovlJ_801D85E4`) and `0x8019F550` (bank M `func_ovlM_8019F374`). Bank S and bank AB
contain no call to `func_8016B774`, so the stored byte stays on screen.

**Mechanism B: a duplicate id is credited to the first record only.**
`func_8016B774` and the availability test both search `0x80196B20` from index 0 and stop at the first
record whose id matches (`0x8016B910`-`0x8016B934`; `func_8016B6FC` `0x8016B70C`-`0x8016B720`). If
the mod's write makes a second record hold an id the party already owns, the whole used count lands on
the lower-numbered record and the other record keeps `+0x02 = 0`. The lower record then reads `1/1`
and the availability test refuses a new equip on it, because it finds that record first. This is the
case where one unit's equipment reference gives a `1` to a record the unit does not use. Both records
carry the same id, so this mechanism shows the count under the correct name.

**Mechanism C: the name is read from the record at draw time.**
The name accessor takes the halfword in the record (`func_8016F5D0` `0x8016F5D0`, `lw v0,-15316(v0)`
at `0x8016F5E4`), and a unit's equipment reference is an id. An id-only write of an occupied record
therefore renames that record's row to the replacement item and keeps the old counts in the row. That
is the answer to the "genuinely equipped item under a different name" half of the question for a
general id-only write.

For the mod's own trigger the target record is empty in the previous frame, so this rename cannot
happen through the grant. The name and the counts can still come apart on the unit-equipment screen,
because there the name is read from the class-derived id and the counts are read from the record at
the cached index (`0x801A6C14` for the name, `0x801A6D90`/`0x801A6DA0` for the counts).

**Mechanism D: a unit's equipment can point at an id the list no longer holds.**
`func_8016BA6C` does not test the 511 sentinel that `func_8016B6FC` returns for an absent id. At
`0x8016BB58` `li s0,511` and `0x8016BB5C` `andi v0,s0,0xffff` the value goes straight into the slot
address, so the decrement and the read land at `0x80196B22 + 511*4` = `0x8019731E`, inside the
unit/battalion records at `0x80197210 + i*0x19`. `func_8016B774` has the same shape for its
increment (the `li v0,511` at `0x8016B934` and `0x8016B9AC`, then `sb` at `0x8016B954`/`0x8016B9CC`).
An id-only write breaks the link between a unit's equipment id and the list, and the later unequip
then writes one byte of a unit record. This is a memory corruption path. It is a separate effect from
the `1/1` display.

The stale `+0x02` of mechanism A is a property of the grant routine and of the reused record. A
rewrite of the drop or pickup table has the same exposure, because the grant routine still writes the
id and `+0x03` into a reused record without clearing `+0x02`. The only path that avoids it is one that
clears `+0x02` itself, such as the setter `func_80170760` or a call to `func_8016B774`.

**A name and a count that come from two structures.** In `func_ovlS_801A6A70` the name comes from
the class-derived id array at `sp+0x18` (`0x801A6C14` fills it, `0x801A7340` calls
`func_8016F5D0` on it), while the counts come from the record at the cached index in
`*(0x80196B18)+0x1BEE+n` (`0x801A6D90`, `0x801A6DA0`, `0x801A6CF4`). A rewrite of the record's id
therefore changes the counts' owner without changing the name on that screen, and the same rewrite
changes the name without changing the counts on the screens that read the name from the record id.

## 5. Base section and streamed bank (question 5)

Base ELF `.streamedB` (`0x8016AF80`-`0x80190F30`), present in `mods/reference/dump.toml`, so
`RECOMP_HOOK` can bind to these:

* `func_8016B6FC` (0x8016B6FC), `func_8016B738` (0x8016B738), `func_8016B774` (0x8016B774),
  `func_8016BA6C` (0x8016BA6C).
* `func_8016F500`, `func_8016F540`, `func_8016F5D0`, `func_8016F5E8`, `func_8016F634`, `func_8016F67C`.
* `func_8016DFA8`, `func_8016DFFC`, `func_8016E050`, `func_8016E0A4`.
* `func_80170654`, `func_80170668`, `func_80170688`, `func_8017069C`, `func_801706B0`, `func_801706C4`,
  `func_80170724`, `func_80170738`, `func_8017074C`, `func_80170760`, `func_80170774`.
* `func_80172820`, `func_80172C64`.
* Data: the item struct table `0x8018C42C`, the name pool `0x8018B4D0`, the accessor table `0x8018F2C0`,
  the class-equipment table `0x80187C62`, the class-equipment accessor table `0x80186FF4`.

Streamed bank units, not hookable as base sections (the addresses are only valid while the unit is
resident):

* bank S (scene `0x06`, RAM `0x8019A7C0`): `func_ovlS_801A6A70`, `func_ovlS_801A8818`,
  `func_ovlS_801AFECC`, `func_ovlS_8019FEF0`, `func_ovlS_801CEC54`, `func_ovlS_801CFC44`,
  `func_ovlS_801D2A18`, `func_ovlS_801D2F88`, `func_ovlS_801B3030`.
* bank AB (record 9 arena, RAM `0x80214FA0`): `func_ovlAB_80214FA0`, `func_ovlAB_80216184`,
  `func_ovlAB_80217A54`, `func_ovlAB_80218DFC`.
* bank AE code inside the label `func_ovlAE_80214FA0`; bank AF `func_ovlAF_80218910`,
  `func_ovlAF_80218E68`, `func_ovlAF_802178A4`.
* bank N (record 7): `func_ovlN_801DC740`, `func_ovlN_801DD430`, `func_ovlN_801C4AAC`;
  bank M (scene `0x05`): `func_ovlM_801A4110`, `func_ovlM_8019F374`;
  bank J (record 10a): `func_ovlJ_801D1014`; bank I (scene `0x16`): `func_ovlI_801D62A0`;
  bank L: `func_ovlL_80235C98`.

## 6. Proven and inferred

Proven at instruction level:

* The displayed pair is `+0x02` then `+0x03` of one `0x80196B20` record, at the sites in section 1.
* `func_8016B774` clears `+0x02` for both lists and then adds one per roster reference, taking the 4
  stored equipment ids of each character (`+0x2A`-`+0x30`) and the 4 class-derived ids from the table
  at `0x80186FF4`, and the 10 ids at unit record `+0x0D` for the 40-slot list.
* The grant routine `func_ovlN_801DC740` writes the id and `+0x03` only.
* The availability test is `sltu(+0x02, +0x03)` on the record found by `func_8016B6FC`.
* `func_8016BA6C` decrements `+0x02` without testing the 511 sentinel, so the write lands at
  `0x8019731E`.
* No code in the build reads the accessor table at `0x8018F2C0`.

Inference, with the candidate check named:

* That the developer's screen is one of the four draw sites in section 1. Check: load the save with
  the Durandel record, enter scene `0x06`, and read `rh 80196B20 32` plus the cached array
  `rh <*(80196B18)+0x1BEE> 8` from the live console.
* That the record the mod rewrote carried `+0x02 = 1` before the grant (mechanism A). Check: log
  `+0x02` and `+0x03` for every slot at each acquisition in the mod, including the value before the
  write. The mod already walks the list every frame, so this is a two-line change.
* That two records hold the same id (mechanism B). Check: dump the 278 ids and look for a repeated
  value in the run that shows `1/1`.
* That `+0x02 = 2` with `+0x03 = 1` occurs in a real save. Check: `rh 80196B20 64` in the live
  console and compare each record's `+0x02` with its `+0x03`.
