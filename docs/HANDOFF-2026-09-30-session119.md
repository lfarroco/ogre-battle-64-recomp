# Handoff — 2026-09-30, session 119: on Windows the crash report captures nothing in an Explorer launch

**Goal (developer):** session 107 §7 left the captured `stdout`/`stderr` sections
of every Windows report empty, and the developer asked what this Windows machine
can do that the Mac cannot. This session took that item.

**Result:** the capture records **nothing at all** when the process starts with
NULL standard handles, which is the Explorer (double-click) launch a player uses.
A six-second boot puts 101 `stdout` and 53 `stderr` lines into the report with
valid handles and **0 lines** with NULL handles. That is why every Windows report
so far has said `(empty)` in both sections, including the v0.4.0 report that
started session 113.

The observable is the same in both halves of the split, so two candidate
mechanisms survive and this session could not separate them without a build.
Section 3 proposes one change that covers both.

---

## 1. The measurement

`OGRE_CRASH_TEST=segv` writes one `[boot] ...` line to stderr and then raises the
signal, and the report is written by the signal path. Same packaged binary
(2026-09-28) in every row:

| how the child was started | fds 1/2 of the child | report's captured sections |
|---|---|---|
| `cmd /c "…exe > out 2>&1"` | redirected file | stderr has the line, `out` is 0 bytes |
| `Start-Process -RedirectStandardOutput` | redirected file | stderr has the line, the file is 0 bytes |
| `Start-Process`, parent console present | console | stderr has the line; the console shows 12 `[boot]` lines |
| `CreateProcess` `DETACHED_PROCESS` + redirection | redirected file | stderr has the line, the file is 0 bytes |
| `CreateProcess` `CREATE_NO_WINDOW` + redirection | redirected file | stderr has the line, the file is 0 bytes |
| `DETACHED_PROCESS` + `DEVNULL` | NUL device | stderr has the line |
| **`DETACHED_PROCESS`, own handles cleared** | **NULL** | **both sections `(empty)`** |
| NULL handles + `OGRE_CONSOLE=1` | the app's own console | both sections `(empty)` |
| **NULL handles, fault 6 s in** | NULL | **both sections `(empty)`** |
| valid handles, fault 6 s in | pipe | **101 `stdout` + 53 `stderr` lines** |

The v0.4.0 binary from `Downloads\ogre-battle-64-recomp-windows-x86_64 (4).zip`
behaves the same in the rows that were run on it.

Two conclusions:

* The split is not the destination and not a lost race with the capture thread.
  The last two rows fault the process six seconds into the boot, so the drain
  thread has seconds to read the pipe; the `(empty)` rows are genuinely empty.
* The launch state, not the destination, decides whether anything is recorded. A
  terminal, a file, a pipe, `DEVNULL` and a console all capture the line. NULL
  handles capture nothing.

A second, separate defect came out of the same runs: with a console present the
app retargets its own stdio to it. `Start-Process -RedirectStandardOutput file`
plus a parent console leaves the file at 0 bytes **and** prints 12 `[boot]` lines
on the console, so `ogrebattle64.exe > run.log 2>&1` from a shell discards the
redirect. That is `main.cpp:192-196`, which freopens `CONOUT$` onto stdout and
stderr whenever `AttachConsole(ATTACH_PARENT_PROCESS)` succeeds. It cannot be
seen in isolation in the rest of the table because the missing capture produces
the same empty file.

## 2. Where in the code

`crash_log::install` (`app/src/crash_log.cpp:766`) calls `redirect_start` once per
fd. `redirect_start` (`:177`) makes a pipe, sets `forward = io_dup(target)`,
replaces `target` with the pipe's write end through `io_dup2`, and starts the
drain thread. The comment at `:174` already records that a Windows GUI process can
start with no valid handle on fd 1/2, and falls back to `io_open_null()` (`"NUL"`)
"rather than skipping the capture".

With NULL handles `io_dup(target)` fails, so the fallback decides the forward
destination. Two candidate mechanisms remain:

* **(a) The capture is never installed.** The remaining `return -1` path is
  `io_dup2(fds[1], target) < 0`, so no drain thread would exist and the ring would
  stay empty. `_dup2`'s UCRT contract is not stated clearly enough to settle this
  from the documentation: the `fd2` parameter is "Any file descriptor", and
  `EBADF` is documented for "the file descriptor is invalid"
  ([`_dup`, `_dup2`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/dup-dup2?view=msvc-170)).
* **(b) The streams are unusable.** The UCRT may leave `stdout`/`stderr`
  unassociated when the process has no standard handles, so `fprintf` writes
  nothing for the pipe to carry. This one has a consequence beyond the report: a
  player's build would discard its whole log, crash or not.

`error.log` is still written correctly in both cases, so the report itself is
never lost — only its captured sections.

## 3. Proposed fix, for the Mac to land and for CI to verify

Not verified here: this machine has no compiler and cannot build the tree.

1. **Give fds 1/2 a real destination before installing the capture.** At the top
   of `main`, when `GetStdHandle(STD_OUTPUT_HANDLE)` or `STD_ERROR_HANDLE` is
   NULL, open the null device and assign it to that fd (`_dup2`, or
   `freopen("NUL", "w", stdout)` / `stderr`). That makes the streams usable and
   makes `io_dup` succeed, which closes (a) and (b) at once, and it must happen
   before `crash_log::install` so the capture's forward target is that fallback.
2. **Do not freopen over an inherited redirection.** In the `AttachConsole`
   branch, skip the `CONOUT$` freopen when the corresponding standard handle is
   already a disk or pipe (`GetFileType(...)` is `FILE_TYPE_DISK`/`FILE_TYPE_PIPE`).
   A caller who redirected wants the file.

Verification, on a Windows machine with the package folder excluded from Defender:

```powershell
# the player's launch state: Explorer, with no standard handles
setx OGRE_CRASH_TEST segv            # then log out/in, or set it in System Properties
# double-click ogrebattle64.exe, let it raise, then read the report
notepad error.log
```

Before the fix, both captured sections read `(empty)`. After it, the stderr
section must carry the `[boot] OGRE_CRASH_TEST=segv: raising signal 11` line.
Run the same binary from a `cmd` prompt with `> run.log 2>&1` to check fix 2
separately: `run.log` must stop being empty.

## 4. Verification was cut short, and Defender is the reason

Windows Defender quarantined `ogrebattle64.exe` from the package folder four
times while this session ran, as `Behavior:Win32/DefenseEvasion.A!ml`:

* 21:37:08 and 21:37:34 — a remote thread running a `mov dword ptr [0], 0` stub,
  used to fault a live process six seconds in so that the capture thread could not
  be racing the report. `CreateRemoteThread` plus `WriteProcessMemory` is the
  trigger.
* 21:39:07 and 21:39:27 — the throwaway reproducer script, whose launch style is
  handles cleared plus `DETACHED_PROCESS | CREATE_NO_WINDOW`.

The file is byte-identical to the release (`sha256
de69ff2ada416b66ca4d038bbc87ce67629c3ee9dbba963a26e2a6202c80aaa0`) and was
restored from the release archive each time. The ROM, `saves/`, the DLLs and
`mods/` were never touched, and the reproducer was deleted rather than landed.

**Do not run a NULL-handle launch or inject into this binary on a
Defender-protected machine without an exclusion for the folder.** A CI job that
adds the NULL-handle check must exclude the package path on the runner first, or
it will quarantine the artifact it is testing.

## 5. What was run

* Eleven launches of the packaged binary across the §1 modes: the immediate
  `OGRE_CRASH_TEST` runs take 0.1–0.3 s, the two late-fault runs 6 s before the
  fault.
* The v0.4.0 binary from `Downloads\ogre-battle-64-recomp-windows-x86_64 (4).zip`
  for the comparable rows.
* Two bounded normal boots (18 s and 9 s), both exit 0 with no new `error.log`.
* The console-buffer read used `CreateFile("CONOUT$")` plus
  `ReadConsoleOutputCharacterW`.
* `Get-MpThreat`, `Get-MpThreatDetection`, PE header and byte reads.

Probes: none in the source tree. The reproducer script lived at
`tools/win-report-capture-check.py` for part of the session and was deleted;
`docs/` is the only thing this session changed.

## 6. Follow-ups this session cannot do

* Land §3's two fixes and rebuild.
* Separate (a) from (b) in §2. The cheapest instrument is a temporary line in
  `redirect_start` that writes its own return value and `errno` to `error.log`
  through `write_report`'s raw-fd path, built by the Windows CI job.
* Fold the `(empty)` check into `tools/smoke-dist.sh` as a NULL-handle launch of
  an excluded package copy, and keep the current check 4 (which starts the
  process with inherited handles and therefore cannot see this).
