# The tutorial crash: SIGBUS in the scene dispatcher

Read-only investigation. No game run by this session; the evidence is four macOS
crash reports and the linked ELF. No tracked file changed except this one and the
record files.

## 0. What was observed

The developer opened the Tutorial (scene `0x17`) from the suspend save
`build-app/saves/ogrebattle64-us-rev1.bin` and the process died. The run log ends
immediately after the scene's two bank records load and a save read:

```
[bank] loading overlay record rom=0x069920 ram=0x80197B90 size=0x4D60 (21 functions)
[bank] loading overlay record rom=0x1BA020 ram=0x80220F60 size=0x92B0 (125 functions)
[mod] save-read
```

There is no crash-handler output in the log, so the crash reports are the
evidence.

## 1. The four reports are one fault

| report | time | fault address | thread | frames 1-2 |
|---|---|---|---|---|
| `ogrebattle64-2026-10-03-165921.ips` | 16:59:21 | `0x199ab25f0` | N64 Thread 3 | `func_80075BC0`, `func_80071EB0` |
| `ogrebattle64-2026-10-03-173838.ips` | 17:38:38 | `0x1a4e015f0` | N64 Thread 3 | `func_80075BC0`, `func_80071EB0` |
| `ogrebattle64-2026-10-03-174634.ips` | 17:46:34 | `0x1a4c195f0` | N64 Thread 3 | `func_80075BC0`, `func_80071EB0` |
| `ogrebattle64-2026-10-03-174657.ips` | 17:46:57 | `0x19dd635f0` | N64 Thread 3 | `func_80075BC0`, `func_80071EB0` |

Every report is `EXC_BAD_ACCESS` / `KERN_PROTECTION_FAILURE` on N64 Thread 3, with
frame 1 at the same offset (`+895732`, symbolised `func_80075BC0`) and frame 2 at
`+2028309` (`func_80071EB0`). Frame 0 has no symbol in any of them, and the fault
address differs between runs, which is what a computed address looks like rather
than a fixed one.

`func_80071EB0` is the streamed-overlay loader (it is the function that DMAs
`0x40E80` -> `0x8016AF80` at boot, `config/config.yaml`). `func_80075BC0` is the
scene dispatcher, which the developer's own record describes as the function that
looks up `D_800AF028[scene_id]()` and then runs the descriptor's enter and
per-frame hooks.

## 2. What the fault address is not

`0x19DD635F0` and the three others are all around `0x19xxxxxxx`..`0x1Axxxxxxx`.
That is far above every guest mapping this port defines:

- RDRAM is `0x80000000`..`0x80800000`.
- KSEG3 and the RSP IMEM window end at `0xE0000000`.
- The recompiler maps guest code through `get_function`, which returns a stub for
  an unknown address rather than jumping to it
  (`docs/HANDOFF-2026-08-25-session6.md`, `overlays.cpp`'s
  `streamed_stub_generic`).

So the guest is dereferencing a value that is not a guest pointer or a guest code
address at all. The faulting address is a *data* address, and the guest
instruction that computes it is in `func_80075BC0`.

## 3. What this is not

**The Item Randomizer is in no frame of it, and the mechanism does not fit.** The
earliest report predates the session-124 change to the mod: `ogrebattle64-2026-10-03-165921.ips` is
from 16:59, before the roster-scan rewrite and its 17:00-17:26 builds. The mod's C
code writes to guest RAM at `0x80196B20`, `0x80193AE0`, `0x801EDB38`,
`0x801F1002`/`0x801F1004`, `0x801936D8` and the `+0x02` byte of those records, and
it cannot produce a dereference at `0x19xxxxxxx` because it never installs a
pointer the game follows. It also cannot make the dispatcher jump: an unknown
guest code address resolves to a stub.

**The A/B that was meant to prove this did not.** The first attempt wrote an empty
`enabled_mods` list into the preference directory, but
`mods/item-randomizer/run-with-save.sh` copies `build-app/mods.json` over that file
(line 72), so the mod was loaded in both runs. Set `mods.json` **between** runs, or
move `mods/item-randomizer.nrm` out of `<pref>/mods/`, to actually veto a mod.

## 4. The open question

What in scene `0x17`'s dispatch reads a non-guest pointer. Candidates, in the order
they are cheap to test:

1. **The scene descriptor's accessor.** `func_80075BC0` writes 25 accessor
   pointers into `D_800AF028` from an immediate list, and ids 2, 3, 6, 8 and 23
   branch (`docs/README.md`). Scene `0x17`'s accessor is `func_801862F0`, which
   branches on `D_80196B0C` bit 3 and returns `0x8018FE50` or `0x8018FE64`
   (`docs/notes-tutorial-drops.md` §0). One of those two descriptors may hold a
   word the dispatcher treats as a hook.
2. **`func_80071EB0`'s return into the dispatcher.** Frame 2 is the loader and
   frame 1 the dispatcher, so the sequence is a load followed by a dispatch. The
   two records it loaded are the tutorial's, and record 18 is the nested mode
   module at `0x8022A860`.. (`docs/notes-tutorial-drops.md` §0).
3. **The game's own pre-state.** A forced scene enters without the state a natural
   path builds (`AGENTS.md` §9). If the tutorial is being entered from a suspend
   save rather than through its own flow, a field the dispatcher reads may never
   have been written. This should be tested before the port is blamed: enter the
   tutorial through the flow the game itself provides and see whether the crash
   survives.

The cheapest discriminating experiment is a live console `r`/`rh` of
`D_800AF028`, the resolved descriptor and its `+0x00`/`+0x04`/`+0x0C` words at the
frame before the crash, plus a `tools/watch.sh` on the descriptor word that is
non-guest. `OGRE_LIVE_CONSOLE=1` and `OGRE_CONSOLE_AT_MS` make that one run.

## 5. Status: resolved in practice, cause not identified

The developer reports that the tutorial runs fine after a rebuild. **The app
binary did not change**, so that rebuild cannot be the cause of the difference:
`build-app/ogrebattle64` is still `Oct 3 12:06:40`, size `27277408`, which is the
binary all four crash reports came from, and `find build-app -name ogrebattle64
-newermt "2026-10-03 12:07"` is empty. The tutorial is therefore reachable with
the current build, and the four crashes are not reproducible from the state the
developer is in now.

Two candidates remain, and the record should not prefer one:

1. **Save state.** `run-with-save.sh` restores the imported save at startup and
   the port writes the battery back while playing and on exit. A crash mid-write
   can leave the sandbox copy inconsistent with the file it was imported from, and
   the next run then loads that. Every one of the four reports is from a run that
   followed another run, and a fresh import is what the working run had.
2. **A flaky fault.** `func_80075BC0` reads a value that is not a guest address,
   and the fault address moves between runs. Intermittency is consistent with
   reading a field that some paths initialise and others do not, which is what
   `AGENTS.md` section 9 warns about for a scene entered without the pre-state a
   natural path builds.

If it returns, the first thing to capture is the save file at the moment of the
crash, and the live-console read of `D_800AF028`, the resolved descriptor and its
hook words from the frame before.

## 6. Not proven

- That the tutorial crash happens without the mod. The A/B as first written could
  not show it; the four reports only show that it happens with the mod loaded. The
  mod's binary is likewise unchanged between the crashing runs and the working one
  (`build/mods/item-randomizer.nrm`, 8591 bytes), so the mod did not change either.
- Why the fault stopped.
- Which guest instruction in `func_80075BC0` computes the bad address.
- Whether the tutorial was ever reachable in this port at all. `PLAN.md` recorded
  scene `0x17` as "= the Tutorial and runs (bank unit B, records 17/18)", which is
  evidence that it ran at some point; the reports here are all from 2026-10-03.
