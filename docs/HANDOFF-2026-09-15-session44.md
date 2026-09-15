# Handoff — 2026-09-15, session 44: step 2 unblocked by restoring the N64's low-RDRAM (KUSEG) alias

## Goal and result

Session 43 left one goal: fix command-mode step 2 (the cathedral dialogue with
`Archbishop Odiron`), which dies in `func_ovlC_8022D1CC`'s path B.

**Result: the wall is understood and the enter now runs.** Both crashes were
NULL-ish writes — `*(sp+0x1EC) = 0` for path B, `a3 = 0` for the descriptor
interpreter's display-list emitter — and every value on the way to them is
produced by *game* code (traced), so retail reaches the same stores with the
same zeros. Retail runs the whole opening, so on hardware those stores must
land somewhere harmless: the N64 maps the low window (KUSEG, `0x00000000-…`)
into RDRAM through the boot TLB mapping, and the port's `recomp_mem_addr`
computed `a - 0x80000000` for it, faulting ~2 GiB below the RDRAM buffer.
Adding that 4 MiB mirror in `recomp.h` (recorded in
`n64modernruntime-n64recomp.patch`) makes the step-2 enter complete: the
null-renderer build runs the scene for 110 s with no crash (previously SIGSEGV
within one frame), and the movie still renders. The RT64 build now reaches the
same point and fails a *different*, renderer-path way (§4).

Side results, all verified: the New Game **step table** is decoded, and
`tools/ogrelz.py`'s decoder is corrected (the LZ block starts at `rom + 4`,
not `rom`).

The finding, in one line: **the two crashes are stores to guest address 0 and
guest address 8; retail tolerates them because the N64 maps the low window into
RDRAM, and the port did not.**

## 1. The New Game step table (new, verified from the ROM)

`func_80227E64(n)` reads step `n`'s descriptor:

1. asset `0x19A8804` (`rom = (id & 0x0FFFFFFF) + 0x594250` = ROM `0x1F3CA54`) is
   a *table*: its first word is the payload size (`0x1A74`), and the u32 array
   starts at `rom + 4`. Entry `n` is the step's asset id.
2. the step asset's first word is its own payload size; **the LZ block starts at
   `rom + 4`** (the loader `func_8009DBB8` copies `size` bytes from `rom + 4`,
   then `func_8007A110` reads the decompressed size there and the tokens after
   it).
3. the **low byte of the descriptor's last word** is the command opcode:
   `0xFF0000xx` → `jtbl_ovlC_8022ABE0[xx-1]` (ROM `0x286B60`) → command.

| step | asset id | ROM | payload → decompressed | last word | cmd |
|---|---|---|---|---|---|
| 1 | `0x019AA5B6` | `0x1F3E806` | 664 → 3328 | `FF000005` | -7 |
| 2 | `0x019AA852` | `0x1F3EAA2` | 429 → 1512 | `FF000001` | -3 |
| 3 | `0x019AAA04` | `0x1F3EC54` | 233 → 680 | `FF000008` | -10 |
| 4 | `0x019AAAF2` | `0x1F3ED42` | 272 → 928 | `FF000008` | -10 |
| 5 | `0x019AAC06` | `0x1F3EE56` | 252 → 804 | `FF000008` | -10 |
| 6 | `0x019AAD06` | `0x1F3EF56` | 229 → 672 | `FF000008` | -10 |
| 7 | `0x019AADF0` | `0x1F3F040` | 281 → 940 | `FF000008` | -10 |
| 8 | `0x019AAF0E` | `0x1F3F15E` | 175 → 612 | `FF000008` | -10 |
| 9 | `0x019AAFC2` | `0x1F3F212` | 1219 → 6624 | `FF000001` | -3 |
| 10 | `0x019AB48A` | `0x1F3F6DA` | 2377 → 12540 | `FF000004` | -4 |
| 11 | `0x019ABDD8` | `0x1F40028` | 251 → 696 | `FF000001` | -3 |
| 12 | `0x019ABED8` | `0x1F40128` | 620 → 3116 | `FF000001` | -3 |
| 13 | `0x019AC148` | `0x1F40398` | 640 → 3268 | `FF000001` | -3 |
| 14 | `0x019AC3CC` | `0x1F4061C` | 524 → 2112 | `FF000008` | -10 |
| 15 | `0x019AC5DC` | `0x1F4082C` | 244 → 780 | `FF000008` | -10 |
| 16 | `0x019AC6D4` | `0x1F40924` | 155 → 380 | `FF000008` | -10 |
| 17 | `0x019AC774` | `0x1F409C4` | 587 → 2352 | `FF000001` | -3 |
| 18 | `0x019AC9C4` | `0x1F40C14` | 560 → 2192 | `FF000001` | -3 |
| 19 | `0x019ACBF8` | `0x1F40E48` | 891 → 4752 | `FF000001` | -3 |

`tools/ogrelz.py --asset assets/ogre64.z64 <id>...` reproduces every row.
Note entry 0 (`0x019AA27C`, ROM `0x1F3E4CC`, 822 → 3264) is **not** the first
step: `n` is 1-based, so step 1 is table entry 1. (Session 42's "descriptor id
`0x019AA27C`" for `n=1` and the raw dumps in this session's own working notes
were off by one entry; the ids above are read from the table at `0x1F3CA58`.)

**Every step ≥ 2 selects selector 2.** The command handler table
`jtbl_ovlC_8022ABA0` (ROM `0x286B20`) is:

| cmd | handler | effect |
|---|---|---|
| -10, -3 | `0x80226E30` | DMA records 14a/14b, `func_80227700(2)`, enqueue callback `80226110` |
| -9 | `0x802268B8` | DMA records, `func_80227700(2)`, register `80226344` |
| -8 | `0x802269DC` | DMA records, `func_80227700(2)`, register `80226344` |
| -7 | `0x80226AC4` | DMA records, `func_80227700(0)`, register `80225A3C` (**step 1 only**) |
| -6 | `0x80226BEC` | DMA records, register… |
| -5 | `0x80226F98` | epilogue (no-op) |
| -4 | `0x80226D10` | … |
| -2 | `0x80226F50` | register `8022645C`, set `D_800E7A33 |= 8` |

So `D_8018FC39 = 0` happens **only** on step 1; steps 2–19 all set 2 and take
`func_ovlC_8022D1CC`'s path B. Path B is therefore normal code, not a debug
branch — which is what makes the crash a real port-vs-hardware question (§3).

## 2. Where step 2 dies, at instruction level

**The callback is queued, not called.** `func_80076F5C` (main segment) only
*stores* `{id, callback, args}` into the 6-slot table `D_800E82C8` (stride
`0xA8`, `+0x10` = callback). The consumer is `func_800765D8`, called from the
frame function `func_80072398` at `0x8007266C` (after the scene update at
`0x8007265C`); it copies the slot to `D_800E7A30`, takes the callback from
`D_800E7A40`, and calls it with **a0 = slot index** (`jalr` at `0x800766B4`).

Measured call chain at the crash (`OGRE_TRACE_HOOKS=1`):

```
func_80072398 → func_800765D8 → func_ovlC_80226110 → func_ovlC_8022D1CC
  → func_ovlC_802399AC → (static_80239BA0 / static_80239A38 loop) → func_800988A0  ✗
```

`func_ovlC_8022D1CC` (the task body) does: `malloc(0x1CB8)` → `D_8022A994`,
`bzero` it, `func_80227FF8(step, 0)` (loads + LZ-decompresses the step
descriptor into `D_8022A978`), then branches on `D_8018FC39`:
`0` → `func_ovlC_8023A9AC` (a **proper** function: prologue, epilogue),
`2` → **`jal 0x802399AC`**.

`0x802399AC` is **the tail of the function whose prologue is `0x80239874`** —
there is no `jr $ra` between them (raw ROM `0x2AE754..0x2AE764` verified, and
splat's split at `0x802399AC` exists only because of this `jal`). Reading the
tail as one function:

* the prologue fills `a2` into `sw a2, 0x1EC(sp)` (`0x802398F0`);
* the `s5 < 28` loop reads object pointers from `D_8022A994 + 0x18 + s5*4`
  (fresh step ⇒ all zero ⇒ the body is skipped, only the counter advances);
* the epilogue calls `func_800988A0(a0 = sp+0x190, a1 = *(sp+0x1EC))`
  (`0x80239C1C`/`0x80239C30`) and returns at `0x80239CC0` with
  `addiu sp,sp,0x238`.

`func_800988A0(a0, a1)` reads 16 floats from `a0` and **writes** 8 words at
`a1` and 8 at `a1+0x20`. Path B reaches it with `a1 = *(sp+0x1EC) = 0` → the
port faults on the *store* at guest address `0` (host PC
`func_800988A0 + 0x166`, `fault = rdram - 0x80000000`, i.e. guest `0`).

Measured at path B (probe, later reverted): `sel=2`, `sp=0x800C22A8`,
`s0=0`, `f26=f20=f22=f24=0`, `*(sp+0x1EC)=0`, and **every word
`sp+0x1DC..sp+0x218` is zero**. The healthy per-frame path
(`func_ovlC_80239D68` → `0x80239874` → falls into `0x802399AC`) runs with
`sp=0x800C1BF0`, `s0=0x800C2040`, `*(sp+0x1EC)=0x800C1EC0` — i.e. the slot the
tail needs is the `a2` buffer of that frame, and path B has no such frame.

### A/B: path B is not the only wall (one variable per run)

1. **Skip the call** (`OGRE_SKIP_PATHB=1`, the branch's `func_ovlC_802399AC`
   call removed): the `func_800988A0` crash disappears and the enter dies one
   step later, writing guest address **8** in `func_ovlC_8023C894`.
2. **Give the slot a valid pointer** (`OGRE_PATHB_A2=800C6C00`, poked only when
   the slot is zero): path B completes and execution dies in the **same**
   `func_ovlC_8023C894`.

So the real blocker is the second crash, and path B is one instance of a class.

### The second crash is the same class

`func_80227030` (called at the end of `func_ovlC_8022D1CC`) calls
`func_ovlC_802282D8`, **the step-descriptor interpreter** (`s3 = D_8022A978`,
`s1 = D_8022A970` = the word pc; opcode = `desc[s1]`, `0x8000000x` and up). Step
2's descriptor starts `80000006 00000039 …`, so the **first** opcode is
`0x80000006`, whose `sel == 2` case is:

```
8022A07C: if (D_8018FC39 != 2) goto 8022A0A4
8022A090:   a0 = desc[s1+1]          ; = 0x39
8022A094:   jal func_ovlC_8023C894   ; ← a3 is NOT set
8022A0A4:   … jal func_ovlC_8023BF50(a0, 0); jal func_ovlC_802396FC()
```

`func_ovlC_8023C894` is `func_ovlC_8023C824`'s tail (again no `jr $ra` between
them) and immediately does `addiu v0,a3,0x28` / `sw v0,D_800E9BA0` /
`sw t0,0x8(a3)` — it is a **display-list emitter fragment** whose `a3` is the
DL write pointer (`D_800E9BA0`) and whose `v0`/`t0`/`t1` are the already-built
RDP words. The interpreter's case passes only `a0`.

Probes (reverted) show the emitters normally run with valid pointers —
`EMIT 8023C894 a0=0C184240 a1=E200001C a2=FC21FFFF a3=80346D58
t0=E7000000 t1=800CF9B0 v0=80346D78` (3211 such calls in one run; `a3`
alternates between the two framebuffer DL buffers) — but the **enter-time**
interpreter run starts at word pc 0 and reaches the emitter before anything
sets `a3`:

```
INTERP enter a0=801A1AD0 a1=0 a2=0 a3=0 t0=7 t1=0x135E v0=2
EMIT 8023C894 a0=00000039 a1=0 a2=0 a3=0 t0=801A2DE0 t1=0x135E v0=801A2DE0   ✗
```

i.e. the interpreter's own entry `a3` is irrelevant (healthy frames enter with
`a3 = 0xFF`); what matters is that the **first** opcode of the descriptor is a
`sel == 2` emitter call. Nothing in this call chain (`func_80072398` →
`func_800765D8` → `80226110` → `8022D1CC` → `80227030` → `802282D8`) sets `a3`.

## 3. What this means (and what it does not)

Both failures are "read a value the caller never wrote":
`*(sp+0x1EC)` for path B, `a3` for the emitter. Everything else on both paths is
the game's own code and data, byte-verified against the ROM:

* `jal 0x802399AC` at `0x8022D218` is real (`0C08E66B`); `jal 0x8022D1CC` in
  both selector callbacks is real (`0C08B473`).
* `func_80093380` is `bzero` (all-zero stores), so `D_8022A994` really is
  zeroed and the 28-entry object array really is empty on a fresh step.
* Both records (`14a` ROM `0x29A490` → `0x8022ACB0`, `14b` ROM `0x2AE390` →
  `0x802395E0`) DMA at the segment table's addresses; no `UNCOMPILED` report
  appears in these runs.
* The callback really is invoked through the game's own queue consumer from the
  game's own frame function — not a port shortcut.

`a3` was then traced end to end in the crashing frame (probe, reverted):

```
func_800765D8 jalr cb=80226110  a3=800E7A30   <- the copied queue slot
80226110 enter                  a3=800E7A30
8022D1CC pathB                  a3=08880000   <- leaked by func_80227FF8 (its
                                                 descriptor-relocation tag!)
8022D1CC before func_802329D0   a3=FFFFFFFF   <- leaked by the asset-load chain
8022D1CC after  func_802329D0   a3=00000000   <- func_802329D0 = malloc+bzero
```

Every one of those writer functions is game code (recompiled 1:1), so **retail
reaches the emitter with `a3 = 0` as well**. `func_80227FF8` is the interesting
one: it sets `a3 = 0x08880000` as the loop constant of its descriptor
relocation (`(w & 0xFFFFFF00) == 0x08880000` → `u16 D_80196F80[(w & 0xFF)*2]`,
`0x80228138`-`0x8022817C`) and never restores it — a real ABI leak, and harmless
only because the later malloc/bzero overwrites it.

So both crashes are stores to a *low* address (`0` and `8`), reached with zeros
that hardware shares. Retail runs the opening, therefore the low window must be
writable there. The R4300i's KUSEG (`0x00000000-0x7FFFFFFF`) is TLB-mapped
([N64 Programming Manual 1-4-3](http://ultra64.ca/files/documentation/online-manuals/man-v5-1/kantan/step1/1/1_4.htm)),
and the game's own boot installs only two TLB entries (`func_8009AAA0`, at
`0x8009AAA0`, maps `0xC0000000`; `func_8009AB00` clears 0..30), so the low
window comes from the boot ROM (IPL3) — exactly the "mask the top three bits"
alias N64 emulators implement.

### The fix

`tools/N64ModernRuntime/N64Recomp/include/recomp.h`, `recomp_mem_addr`:

```c
if (a < 0x80000000u) {          // KUSEG: the boot TLB mapping of the low window
    return a & 0x003FFFFFu;     // target of the stores above (0x0 / 0x8)
}
return a - 0x80000000u;
```

Recorded in `n64modernruntime-n64recomp.patch` (the vendored patch is the only
copy the repo keeps — the submodule's working tree is invisible to git).

**Effect (null renderer, `OGRE_SPEED=4`, taps):** the run that previously died
with `SIGSEGV` in `func_800988A0` one frame into the second `0x0D` visit now
enters that visit (`[scene] t=41363ms id=0x000D`) and keeps running for the
remaining 110 s wall with **no crash** and exit 0 — it sits in the step, which
is what an interactive cathedral/name/birthday/question sequence should do with
no input. With continuous `Start` taps the null build is still clean after
60 s (`0x0D` at `t=15672 ms`, exit 0). The movie still renders (the sepia
courtyard capture below).

**Caveat (AGENTS §7/§8):** this is a *hardware-fidelity* change, argued from
"retail runs the sequence + the zeros are the game's own", not from a hardware
watchpoint. It also means genuine null-pointer writes elsewhere will now land in
RDRAM offset `0..0x3FFFFF` instead of faulting — useful to remember when
debugging. Keep the mirror narrow (4 MiB) if a later path argues for more.

## 3b. What the sequence is supposed to show (developer, this session)

The developer supplied the missing *intent* for the post-movie flow — it is one
multi-step scene, and the port dies on its first step:

1. a **cathedral screen**: the player's character walks to Archbishop Odiron,
   with a few dialogue lines;
2. Odiron asks the player's **name** → the character-table name-entry form
   (`Magnus` + `A–Z`/`a–z`, `INS`/`BS`/`DEL`/`END`);
3. Odiron asks the **date of birth** → a form (`BIRTHDAY Jul. 25`, `Trueno 12`);
4. a series of **personality questions** (`"What dost thou hold within thy
   sword?"` → `ardor`/`passion`/`vigor`/`talent`/`belief`/`hatred`); the answers
   determine the player's initial units and items;
5. an **intro movie**.

So the "scene" is steps 2..n of the `0x02`/`0x0D` sequence, one step per visit,
and `PLAN.md`'s "step 2 = the cathedral dialogue" is the *first* of five parts.
The name/birthday/question screens are the game's form module — the same
look (dialogue/form box over the dark blue textured backdrop) as the Tutorial
that already renders in the port (`docs/proofs/native-tutorial-dialogue.png`),
so that module is present and working. A reasonable (unverified) mapping of the
step table onto the five parts: steps 2 and 9 are the dialogue parts (`-3`),
the `-10` steps (3–8, 14–16) the forms, and the lone `-4` step 10 the closing
movie. Confirming it needs the steps to run, which the §3 fix now allows (the
next session should capture them).

## 4. The wall that the fix exposes: the RT64 build dies in the runtime's queue bridge

With the mirror, the **RT64 build** (`build-app`) now reaches the second `0x0D`
visit and immediately dies *in the runtime*, not in game code:

```
[bank] loading overlay record rom=0x0E4910 ram=0x80197B90 size=0x72C0 (40 functions)
[crash] host pc _Z7do_sendPhiibb + 0xC4
[crash] signal 10 on N64 thread 4 fault=0x18eb3dc89 rdram=0x11045b000 offset=0x7E6E2C89
```

`do_sendP` is `ultramodern`'s message-queue bridge, and the guest pointer it was
handed translates to `guest 0xFE6E2C89` (KSEG3) — i.e. a queue address the
bridge's `addr >= 0x80000000 && addr < 0x80800000` validation
(`ultramodern/src/mesgqueue.cpp:255`) does not accept, after which it
dereferences it anyway. The **null build never hits this** (60 s of the same
`Start`-tap schedule, exit 0), so it is on the renderer path — plausibly the
first display-list task of the step. Two hypotheses to separate next:
(a) game code legitimately uses a KSEG3/KSSEG pointer (the game maps
`0xC0000000` in `func_8009AAA0`) and the runtime bridges need the same
segment translation the `MEM_*` macros now have; (b) the mirror lets a bad
pointer survive where it used to fault, and a structure is corrupted.

## 5. What's next (session 45)

1. **Chase the new `do_sendP` SIGBUS (RT64 only).** Dump the RDRAM at that crash
   (`OGRE_DUMP_RDRAM`) and read the queue structure the game passed; watch the
   guest register that holds the pointer at the `osSendMesg` call. Compare with
   the null build at the same point (it does not crash) to see whether the
   difference is the display-list task submission itself.
2. **Check that the mirror is not masking a real structure bug.** The two
   stores the fix tolerates are at guest `0` and `8`; verify no game code
   *reads* those offsets (`OGRE_DUMP_RDRAM` after the enter shows RDRAM
   `0x0..0x40`; the emitter writes a 16-word matrix there).
3. **Look at the cathedral visually.** `build-app` with a *slower* tap schedule
   (the capture readback slows the emulated clock, so a 1.5 s schedule misses
   the menu — session 43's schedule needed the run at full speed) or with taps
   stopped after New Game is confirmed, then capture `OGRE_CAPTURE_PRESENT`.
4. Unchanged and open: menu `0x18` natural entry, `OGRE_NO_AUDIO=1` early-boot
   crash, `osViFade`, scene `0x12` (Load Game, needs Controller Pak state), the
   movie-engine path (`0x80197794`).

## Verification (final binaries, all probes reverted by `make recomp` + `make bank-recomp`)

* `make recomp` and `make bank-recomp` clean; `gen_bank_funcs.py` reports 5
  unit(s), **17 record(s), 1867 function(s)** (unchanged from session 43).
* `grep -rl probe44 probe45 RecompiledFuncs/ Bank*Funcs/ app/` → empty.
* `build-null` and `build-app` rebuild clean (exit 0).
* **Before the fix**, the baseline New Game repro died as session 43 recorded:
  `0x02` @12401 ms → `0x0D` @12551 ms → `0x02` @41205 ms → `0x0D` @41339 ms →
  `[crash] func_800988A0 + 0x166`, fault = guest `0`, exit 139.
* **After the fix** (null, `OGRE_SPEED=4`, `OGRE_TAP_MS=3000 TAP_MAX=4`):
  `0x02` @12464 → `0x0D` @12617 → `0x02` @41222 → `0x0D` @41363, then **no
  crash** for the remaining ~110 s and exit 0. With continuous
  `OGRE_TAP_MS=2500` taps: `0x0D` @15672, exit 0 at 60 s.
* **After the fix** (RT64, continuous `Start` taps): reaches `0x0D` @15717 then
  `SIGBUS` in `do_sendP + 0xC4`, guest `0xFE6E2C89` (§4). Movie capture from the
  same run shows the sepia courtyard (screen `cath.1290`), i.e. visit 1 still
  renders.
* Regressions with the fix: forced scene `0x17` (Tutorial) 25 s → exit 0;
  attract run 45 s at `OGRE_SPEED=8` → `0x09 → 0x0A → 0x04 → 0x0B`, no crash.
* A/B before the fix: skip path B → `func_ovlC_8023C894 + 0xCA`, fault = guest
  `8`; force `a2` → same `func_ovlC_8023C894` crash. This is what showed path B
  was one of two instances of the same low-address store.
* Step table re-derived twice (this session's script and
  `tools/ogrelz.py --asset`) with identical results.

No repo test harness exists; per the durable-test skill this is disclosed, not
fixed with a framework: each check is a ROM-dependent 12–170 s game run, so the
repro commands below are the maintained verification.

## Files changed (vs session 43)

* `n64modernruntime-n64recomp.patch` — regenerated to carry the `recomp_mem_addr`
  low-window (KUSEG) mirror; the submodule working tree
  (`tools/N64ModernRuntime/N64Recomp/include/recomp.h`) holds the change, and
  the patch is the only copy the repo keeps (AGENTS §10: vendored changes are
  listed, not committed).
* `app/src/bank_overlays.cpp` — the SIGSEGV handler now prints the faulting
  **host PC** plus `dladdr`'s symbol and offset. This is how all three crashes
  were localised (`func_800988A0 + 0x166`, `func_ovlC_8023C894 + 0xCA`,
  `do_sendP + 0xC4`); kept (it is app code, not a probe).
* `tools/ogrelz.py` — `--asset <rom> <id>...` mode, and the docstring now
  records that an asset's LZ block starts at `rom + 4` (the first word is the
  payload size).
* `PLAN.md`, `docs/DECISIONS.md`, `docs/README.md`, `AGENTS.md` (walls list),
  this file.
* Probes, **all reverted by `make recomp` + `make bank-recomp`**: `funcs_0.c`
  (`0x80239874` entry), `funcs_1.c` (`func_ovlC_8023C894` entry and
  `func_ovlC_80226110` entry), `funcs_6.c` (`func_ovlC_802282D8` entry),
  `funcs_12.c` (`func_ovlC_8022D1CC` path B slot dump + the `OGRE_SKIP_PATHB`
  skip and `OGRE_PATHB_A2` poke, and the `a3` prints around `func_802329D0`),
  `funcs_14.c` (`func_ovlC_802399AC` entry), `RecompiledFuncs/funcs_12.c`
  (`func_800765D8`'s `jalr`). Each also gained
  `#include <stdio.h>`/`<stdlib.h>` for the probe.

## Repro commands

```sh
# the step table (ids -> decompressed descriptors -> command byte)
python3 tools/ogrelz.py --asset assets/ogre64.z64 19AA5B6 19AA852 19AAA04

# step 2 now runs (no crash; the scene waits for input)
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_MAX=4 OGRE_SCENE_LOG=1 \
  OGRE_EXIT_AFTER_MS=60000 ./build-null/ogrebattle64

# the RT64-only wall the fix exposes
OGRE_SPEED=4 OGRE_TAP_MS=2500 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=60000 \
  ./build-app/ogrebattle64

# regressions
OGRE_SCENE=0x17 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=25000 ./build-null/ogrebattle64
OGRE_SPEED=8 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=45000 ./build-null/ogrebattle64

# the call chain at the crash (slow boot: keep retuning the taps)
OGRE_TRACE_HOOKS=1 OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_MAX=7 \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=180000 ./build-null/ogrebattle64

# raw RDRAM at the crash, plus the host PC the handler prints
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_MAX=4 OGRE_DUMP_RDRAM=/tmp/rdram.bin \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=60000 ./build-null/ogrebattle64
# read it with: struct.unpack_from('<I', data, a & 0x1FFFFFFF)  (docs/guides/app-build.md)
```
