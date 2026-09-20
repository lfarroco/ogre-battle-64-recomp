// Crash reporting for a distributed build.
//
// The distributed build has no console window: on macOS it is a .app bundle, so
// Finder does not open Terminal, and on Windows the .exe is linked as a GUI
// subsystem binary, so no console is allocated. The [boot]/[bank]/[crash] lines
// that used to reach a terminal therefore have nowhere to go, and a crash left
// a player with nothing to report.
//
// This module tees stdout and stderr into two fixed-size in-memory rings while
// forwarding every byte to the real descriptors, so a run from a terminal or a
// scripted run (`... > run.log 2>&1`) is unchanged. On a fatal signal (or an
// unhandled exception on Windows) it writes `error.log` into the config
// directory, which is the executable's own directory unless that one is
// read-only:
//
//   * a header: the reason, the fault address, the N64 thread and the host pc;
//   * the last lines of the captured stderr and stdout, which carry the boot
//     log, any `[overlays] streamed function stub called @ ...` line and any
//     `[bank] UNCOMPILED streamed record ...` line;
//   * the runtime's per-thread guest call chains.
//
// The player attaches that one file to a GitHub issue.
//
// The signal handler runs on the faulting thread with no guarantees. Its own
// output uses only open/write/close (POSIX) or _open/_write/_close (Windows),
// and it reads the rings through an atomic published length. The runtime's dump
// helpers are fprintf-based, so they run afterwards with fd 2 temporarily
// redirected to error.log.

#include "crash_log.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#define _XOPEN_SOURCE 700
#include <dlfcn.h>
#include <ucontext.h>
#elif defined(__linux__)
#include <sys/ucontext.h>
#include <ucontext.h>
#endif
#endif

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

namespace ogre {
namespace crash_log {
namespace {

// Two rings: stdout and stderr keep separate rings so that forwarding each byte
// to the descriptor it came from preserves `> run.log 2>&1` and `2> run.log`
// alike. Each ring is single-writer (its capture thread) and read lock-free by
// the crash handler, which only ever sees whole-prefix data because the length
// is published with release ordering after the bytes.
constexpr size_t kRingSize = 128 * 1024;
constexpr size_t kStderrLines = 200;
constexpr size_t kStdoutLines = 100;

char g_stderr_ring[kRingSize];
char g_stdout_ring[kRingSize];
std::atomic<size_t> g_stderr_written{0};
std::atomic<size_t> g_stdout_written{0};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_reported{false};

char g_error_path[4096] = {};

int g_saved_stdout = -1;
int g_saved_stderr = -1;
std::thread g_stdout_thread;
std::thread g_stderr_thread;

// --- low-level I/O ----------------------------------------------------------

#if defined(_WIN32)
using io_ssize = int;
io_ssize io_read(int fd, void* buf, unsigned count) { return _read(fd, buf, count); }
io_ssize io_write(int fd, const void* buf, unsigned count) { return _write(fd, buf, count); }
int io_dup(int fd) { return _dup(fd); }
int io_dup2(int from, int to) { return _dup2(from, to); }
int io_close(int fd) { return _close(fd); }
int io_open_trunc(const char* path) {
    return _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
int io_open_null() {
    return _open("NUL", _O_WRONLY | _O_BINARY);
}
#else
using io_ssize = ssize_t;
io_ssize io_read(int fd, void* buf, size_t count) { return ::read(fd, buf, count); }
io_ssize io_write(int fd, const void* buf, size_t count) { return ::write(fd, buf, count); }
int io_dup(int fd) { return ::dup(fd); }
int io_dup2(int from, int to) { return ::dup2(from, to); }
int io_close(int fd) { return ::close(fd); }
int io_open_trunc(const char* path) {
    return ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
int io_open_null() {
    return ::open("/dev/null", O_WRONLY);
}
#endif

void write_all(int fd, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        // Chunks are at most a few KiB, so the unsigned count is exact.
        const io_ssize written = io_write(fd, bytes, static_cast<unsigned>(size));
        if (written > 0) {
            bytes += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

// --- capture ----------------------------------------------------------------

void ring_append(char* ring, std::atomic<size_t>& written, const char* data, size_t size) {
    size_t offset = written.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size; i++) {
        ring[(offset + i) % kRingSize] = data[i];
    }
    written.store(offset + size, std::memory_order_release);
}

void capture_loop(int read_fd, int forward_fd, char* ring, std::atomic<size_t>& written) {
    char buffer[8192];
    for (;;) {
        const io_ssize got = io_read(read_fd, buffer, sizeof(buffer));
        if (got > 0) {
            ring_append(ring, written, buffer, static_cast<size_t>(got));
            write_all(forward_fd, buffer, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;  // 0 = every write end closed, <0 = the pipe is gone
    }
    io_close(read_fd);
}

// Replaces fd `target` (1 or 2) with the write end of a fresh pipe. Returns the
// descriptor the capture thread forwards to, or -1 when the pipe could not be
// created. A GUI-subsystem process on Windows can start with no valid handle on
// fd 1/2, so a failed `dup` falls back to the null device rather than skipping
// the capture.
int redirect_start(int target, std::thread& thread, char* ring, std::atomic<size_t>& written) {
    int fds[2] = {-1, -1};
#if defined(_WIN32)
    if (_pipe(fds, 64 * 1024, _O_BINARY) != 0) {
        return -1;
    }
#else
    if (pipe(fds) != 0) {
        return -1;
    }
#endif
    int forward = io_dup(target);
    if (forward < 0) {
        forward = io_open_null();
    }
    if (forward < 0 || io_dup2(fds[1], target) < 0) {
        io_close(fds[0]);
        io_close(fds[1]);
        if (forward >= 0) {
            io_close(forward);
        }
        return -1;
    }
    io_close(fds[1]);
    thread = std::thread(capture_loop, fds[0], forward, ring, std::ref(written));
    return forward;
}

// Drops the pipe's write end from `target`, which makes the capture thread read
// EOF, then joins it and closes the forwarded descriptor. `forward` stays open
// until the join so the thread never writes to a closed (or reused) fd.
void redirect_stop(int& saved, int target, std::thread& thread) {
    if (saved < 0) {
        return;
    }
    const int forward = saved;
    saved = -1;
    io_dup2(forward, target);
    if (thread.joinable()) {
        thread.join();
    }
    io_close(forward);
}

// --- report formatting ------------------------------------------------------

struct Report {
    int fd = -1;

    void put(const void* data, size_t size) { write_all(fd, data, size); }
    void str(const char* text) { put(text, std::strlen(text)); }

    void hex(uint64_t value, int min_digits) {
        static const char* table = "0123456789ABCDEF";
        char digits[16];
        int count = 0;
        do {
            digits[count++] = table[value & 0xF];
            value >>= 4;
        } while (value != 0 || count < min_digits);
        char out[20];
        int n = 0;
        out[n++] = '0';
        out[n++] = 'x';
        while (count > 0) {
            out[n++] = digits[--count];
        }
        put(out, static_cast<size_t>(n));
    }

    void dec(uint64_t value) {
        char digits[20];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        char out[20];
        int n = 0;
        while (count > 0) {
            out[n++] = digits[--count];
        }
        put(out, static_cast<size_t>(n));
    }

    // Civil date from the Unix time, all integer arithmetic, so the handler can
    // call it.
    void date() {
        const int64_t seconds = static_cast<int64_t>(time(nullptr));
        int64_t days = seconds / 86400;
        int64_t remainder = seconds % 86400;
        if (remainder < 0) {
            remainder += 86400;
            days -= 1;
        }
        // Days since 1970-01-01 to civil date (Howard Hinnant's algorithm).
        const int64_t z = days + 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
        const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t year = static_cast<int64_t>(yoe) + era * 400;
        const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const uint64_t mp = (5 * doy + 2) / 153;
        const uint64_t day = doy - (153 * mp + 2) / 5 + 1;
        const uint64_t month = mp + (mp < 10 ? 3 : -9);
        const int64_t full_year = year + (month <= 2);

        char buf[32];
        int n = 0;
        auto number = [&buf, &n](uint64_t value, int width) {
            char digits[8];
            int count = 0;
            do {
                digits[count++] = static_cast<char>('0' + (value % 10));
                value /= 10;
            } while (value != 0);
            for (int i = count; i < width; i++) {
                buf[n++] = '0';
            }
            while (count > 0) {
                buf[n++] = digits[--count];
            }
        };
        number(static_cast<uint64_t>(full_year), 4);
        buf[n++] = '-';
        number(month, 2);
        buf[n++] = '-';
        number(day, 2);
        buf[n++] = 'T';
        number(static_cast<uint64_t>(remainder / 3600), 2);
        buf[n++] = ':';
        number(static_cast<uint64_t>((remainder / 60) % 60), 2);
        buf[n++] = ':';
        number(static_cast<uint64_t>(remainder % 60), 2);
        buf[n++] = 'Z';
        put(buf, static_cast<size_t>(n));
    }
};

const char* os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

const char* arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
#if defined(SIGBUS)
        case SIGBUS: return "SIGBUS";
#endif
        case SIGABRT: return "SIGABRT";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        default: return "signal";
    }
}

// Writes the tail of one ring, at most `max_lines` lines. Returns whether it
// wrote anything.
bool write_ring_tail(Report& report, char* ring, std::atomic<size_t>& written, size_t max_lines) {
    const size_t total = written.load(std::memory_order_acquire);
    const size_t available = total < kRingSize ? total : kRingSize;
    if (available == 0) {
        return false;
    }
    const size_t first = total - available;
    size_t start = first;
    size_t lines = 0;
    for (size_t i = total; i > first;) {
        --i;
        if (ring[i % kRingSize] == '\n') {
            lines++;
            if (lines >= max_lines) {
                start = i + 1;
                break;
            }
        }
    }
    if (start > first) {
        report.str("... (earlier lines dropped)\n");
    } else if (first > 0) {
        report.str("... (ring buffer wrapped; earlier lines dropped)\n");
    }
    char buffer[4096];
    size_t used = 0;
    for (size_t i = start; i < total; i++) {
        buffer[used++] = ring[i % kRingSize];
        if (used == sizeof(buffer)) {
            report.put(buffer, used);
            used = 0;
        }
    }
    if (used > 0) {
        report.put(buffer, used);
    }
    if (total > start && ring[(total - 1) % kRingSize] != '\n') {
        report.str("\n");
    }
    return true;
}

void write_prelude(Report& report, const char* reason) {
    report.str("Ogre Battle 64: Recomp crash report\n");
    report.str("time: ");
    report.date();
    report.str("\nos: ");
    report.str(os_name());
    report.str("/");
    report.str(arch_name());
    report.str("\npid: ");
#if defined(_WIN32)
    report.dec(static_cast<uint64_t>(_getpid()));
#else
    report.dec(static_cast<uint64_t>(getpid()));
#endif
    report.str("\nreason: ");
    report.str(reason);
    report.str("\nlog: ");
    report.str(g_error_path);
    report.str("\n\n--- stderr (last 200 lines) ---\n");
    if (!write_ring_tail(report, g_stderr_ring, g_stderr_written, kStderrLines)) {
        report.str("(empty)\n");
    }
    report.str("\n--- stdout (last 100 lines) ---\n");
    if (!write_ring_tail(report, g_stdout_ring, g_stdout_written, kStdoutLines)) {
        report.str("(empty)\n");
    }
}

#if defined(_WIN32)
// Module name and offset for a host address, written with raw writes. The module
// (our exe, a GPU driver, the CRT) is the first thing to look at and needs no
// symbols.
void write_host_symbol(Report& report, const void* address) {
    if (address == nullptr) {
        return;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr) {
        return;
    }
    wchar_t wide[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, wide, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    char narrow[MAX_PATH * 4];
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                            narrow, sizeof(narrow) - 1, nullptr, nullptr);
    if (written <= 0) {
        return;
    }
    narrow[written] = '\0';
    report.str(" (");
    report.str(narrow);
    report.str("+");
    report.hex(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module), 1);
    report.str(")");
}
#endif

// The runtime's dump helpers write to stdout and stderr (`printf` for the
// per-thread chain, `fprintf(stderr, ...)` elsewhere) and are stdio-based. A
// fault can arrive while the faulting thread holds a stream lock (inside
// printf): re-entering that lock hangs the handler instead of finishing the
// report. On POSIX the dumps therefore run only behind a non-blocking
// ftrylockfile probe; Windows has no such probe, so there the handler uses raw
// writes only. Everything below is a raw write and cannot block.
void write_guest_diagnostics(Report& report) {
    const uint8_t* rdram = ultramodern::get_rdram_base();
    const int tid = (rdram != nullptr && ultramodern::this_thread() != 0)
                        ? static_cast<int>(TO_PTR(OSThread, ultramodern::this_thread())->id)
                        : -1;
    report.str("\n--- guest diagnostics ---\n");
    report.str("n64 thread: ");
    if (tid < 0) {
        report.str("-1");
    } else {
        report.dec(static_cast<uint64_t>(tid));
    }
    report.str("\n");
    for (int t = 1; t < 32; t++) {
        const uint32_t func = ultramodern::debug_last_func_vram(t);
        if (func != 0) {
            report.str("  t");
            report.dec(static_cast<uint64_t>(t));
            report.str(" last func ");
            report.hex(func, 8);
            report.str("\n");
        }
    }

#if !defined(_WIN32)
    // The full call chain and the recorded history need the runtime's
    // printf-based dumpers, so they run only when both stream locks are free.
    bool stdio_free = true;
    if (ftrylockfile(stdout) != 0) {
        stdio_free = false;
    } else {
        funlockfile(stdout);
    }
    if (stdio_free) {
        if (ftrylockfile(stderr) != 0) {
            stdio_free = false;
        } else {
            funlockfile(stderr);
        }
    }
    if (!stdio_free) {
        report.str("  (call chain skipped: a stdio stream lock is held)\n");
        return;
    }
    const int saved_stdout = io_dup(1);
    const int saved_stderr = io_dup(2);
    if (saved_stdout < 0 && saved_stderr < 0) {
        return;
    }
    if (saved_stdout >= 0) {
        io_dup2(report.fd, 1);
    }
    if (saved_stderr >= 0) {
        io_dup2(report.fd, 2);
    }
    ultramodern::debug_dump_call_chain(tid, "crash");
    ultramodern::debug_dump_chain_history();
    std::fflush(stderr);
    if (saved_stdout >= 0) {
        io_dup2(saved_stdout, 1);
        io_close(saved_stdout);
    }
    if (saved_stderr >= 0) {
        io_dup2(saved_stderr, 2);
        io_close(saved_stderr);
    }
#endif
}

// Writes the report. `signal_number` is -1 for a non-signal fatal condition.
// `instr` is the faulting instruction, `data_addr` the address it touched (the
// access-violation address on Windows, `si_addr` on POSIX) and `access` the
// Windows access kind (0 read, 1 write, 8 execute; -1 when unknown). All are
// nullable/optional. `once` suppresses a second report from the same fatal
// event (a std::terminate that then aborts).
void write_report(int signal_number, const void* instr, const void* data_addr, int access,
                  const char* reason, bool once) {
    if (once && g_reported.exchange(true)) {
        return;
    }
    if (g_error_path[0] == '\0') {
        return;
    }
    const int fd = io_open_trunc(g_error_path);
    if (fd < 0) {
        return;
    }
    Report report{fd};
    write_prelude(report, reason);

    if (signal_number >= 0) {
        report.str("\nsignal: ");
        report.str(signal_name(signal_number));
        report.str(" (");
        report.dec(static_cast<uint64_t>(signal_number));
        report.str(")\n");
    }
    uint8_t* rdram = ultramodern::get_rdram_base();
    report.str("rdram base: ");
    report.hex(reinterpret_cast<uintptr_t>(rdram), 1);
    report.str("\n");
    const uint8_t* instr_bytes = static_cast<const uint8_t*>(instr);
    if (instr_bytes != nullptr) {
        report.str("fault instruction: ");
        report.hex(reinterpret_cast<uintptr_t>(instr_bytes), 1);
#if defined(__APPLE__)
        Dl_info info{};
        if (dladdr(instr, &info) != 0 && info.dli_fname != nullptr) {
            report.str(" (");
            report.str(info.dli_fname);
            report.str("+");
            report.hex(reinterpret_cast<uintptr_t>(instr) -
                           reinterpret_cast<uintptr_t>(info.dli_fbase),
                       1);
            report.str(")");
        }
#elif defined(_WIN32)
        write_host_symbol(report, instr);
#endif
        report.str("\n");
    }
    const uint8_t* data_bytes = static_cast<const uint8_t*>(data_addr);
    if (data_bytes != nullptr) {
        report.str("fault address: ");
        report.hex(reinterpret_cast<uintptr_t>(data_bytes), 1);
#if defined(_WIN32)
        write_host_symbol(report, data_addr);
#endif
        report.str("\n");
        if (rdram != nullptr && data_bytes >= rdram && data_bytes < rdram + 0x800000) {
            report.str("fault offset from rdram: ");
            report.hex(static_cast<uint64_t>(data_bytes - rdram), 1);
            report.str("\n");
        }
    }
    if (access >= 0) {
        report.str("access: ");
        switch (access) {
            case 0: report.str("read"); break;
            case 1: report.str("write"); break;
            case 8: report.str("execute"); break;
            default: report.dec(static_cast<uint64_t>(access)); break;
        }
        report.str("\n");
    }

    write_guest_diagnostics(report);

    // OGRE_DUMP_RDRAM=<path>: the whole 8 MiB image alongside error.log, the
    // same dump the exit path writes, so a crash's data pointers can be
    // followed offline instead of guessed.
    if (const char* dump_path = std::getenv("OGRE_DUMP_RDRAM")) {
        uint8_t* dump_rdram = ultramodern::get_rdram_base();
        if (dump_rdram != nullptr) {
            FILE* file = std::fopen(dump_path, "wb");
            if (file != nullptr) {
                const size_t written = std::fwrite(dump_rdram, 1, 0x800000, file);
                std::fclose(file);
                report.str("\nrdram dump: ");
                report.str(dump_path);
                report.str(" (");
                report.dec(static_cast<uint64_t>(written));
                report.str(" bytes)\n");
            } else {
                report.str("\nrdram dump: could not open ");
                report.str(dump_path);
                report.str("\n");
            }
        }
    }

    io_close(fd);
}

// --- fatal handlers ---------------------------------------------------------

void report_signal(int signal_number, const void* instr, const void* data_addr, int access) {
    char reason[64];
    const char* name = signal_name(signal_number);
    size_t used = 0;
    while (name[used] != '\0' && used < sizeof(reason) - 8) {
        reason[used] = name[used];
        used++;
    }
    reason[used++] = ' ';
    reason[used++] = '(';
    reason[used++] = static_cast<char>('0' + (signal_number / 10) % 10);
    reason[used++] = static_cast<char>('0' + signal_number % 10);
    reason[used++] = ')';
    reason[used] = '\0';
    write_report(signal_number, instr, data_addr, access, reason, true);
}

#if defined(_WIN32)
void win_signal_handler(int signal_number) {
    report_signal(signal_number, nullptr, nullptr, -1);
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record =
        (info != nullptr) ? info->ExceptionRecord : nullptr;
    const DWORD code = (record != nullptr) ? record->ExceptionCode : 0;
    static const char* table = "0123456789ABCDEF";
    char reason[40] = "unhandled exception 0x";
    for (int i = 0; i < 8; i++) {
        reason[22 + i] = table[(code >> ((7 - i) * 4)) & 0xF];
    }
    reason[30] = '\0';
    const void* instr = (record != nullptr) ? record->ExceptionAddress : nullptr;
#if defined(_M_X64) || defined(__x86_64__)
    if (info != nullptr && info->ContextRecord != nullptr) {
        instr = reinterpret_cast<const void*>(info->ContextRecord->Rip);
    }
#endif
    // An access violation carries the access kind and the address it touched;
    // the instruction address alone cannot say whether a load or a store faulted
    // or which pointer was bad.
    int access = -1;
    const void* data_addr = nullptr;
    if (code == 0xC0000005 && record != nullptr && record->NumberParameters >= 2) {
        access = static_cast<int>(record->ExceptionInformation[0]);
        data_addr = reinterpret_cast<const void*>(record->ExceptionInformation[1]);
    }
    write_report(-1, instr, data_addr, access, reason, true);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void fatal_signal_handler(int signal_number, siginfo_t* info, void* ucontext) {
    const void* data_addr = info != nullptr ? info->si_addr : nullptr;
    const void* instr = nullptr;
#if defined(__APPLE__) && defined(__x86_64__)
    if (ucontext != nullptr) {
        instr = reinterpret_cast<const void*>(
            static_cast<const ucontext_t*>(ucontext)->uc_mcontext->__ss.__rip);
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    if (ucontext != nullptr) {
        instr = reinterpret_cast<const void*>(
            static_cast<const ucontext_t*>(ucontext)->uc_mcontext->__ss.__pc);
    }
#elif defined(__linux__) && defined(__x86_64__)
    if (ucontext != nullptr) {
        instr = reinterpret_cast<const void*>(
            static_cast<const ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_RIP]);
    }
#elif defined(__linux__) && defined(__aarch64__)
    if (ucontext != nullptr) {
        instr = reinterpret_cast<const void*>(
            static_cast<const ucontext_t*>(ucontext)->uc_mcontext.pc);
    }
#else
    (void)ucontext;
#endif
    report_signal(signal_number, instr, data_addr, -1);
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}
#endif

void terminate_handler() {
    write_report(-1, nullptr, nullptr, -1, "std::terminate (uncaught C++ exception)", true);
    std::_Exit(EXIT_FAILURE);
}

void install_handlers() {
#if defined(_WIN32)
    std::signal(SIGABRT, win_signal_handler);
    std::signal(SIGFPE, win_signal_handler);
    std::signal(SIGILL, win_signal_handler);
    std::signal(SIGSEGV, win_signal_handler);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
#else
    struct sigaction action = {};
    action.sa_sigaction = fatal_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &action, nullptr);
#ifdef SIGBUS
    sigaction(SIGBUS, &action, nullptr);
#endif
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
#endif
    std::set_terminate(terminate_handler);
}

}  // namespace

void set_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "error.log").string();
    const size_t length =
        path.size() < sizeof(g_error_path) - 1 ? path.size() : sizeof(g_error_path) - 1;
    std::memcpy(g_error_path, path.c_str(), length);
    g_error_path[length] = '\0';
}

void install(const std::filesystem::path& dir) {
    set_directory(dir);
    if (g_installed.exchange(true)) {
        return;
    }
    // Arm the handlers before anything else can fail: a fault in the capture
    // setup then still reaches write_report.
    install_handlers();
    g_saved_stdout = redirect_start(1, g_stdout_thread, g_stdout_ring, g_stdout_written);
    g_saved_stderr = redirect_start(2, g_stderr_thread, g_stderr_ring, g_stderr_written);
    // stdout is line-buffered while it is a terminal and becomes block-buffered
    // when it becomes a pipe. Keep the terminal behaviour so an interactive run
    // still prints as it goes.
    //
    // A static buffer, and not `nullptr` with size 0: the MSVC CRT rejects a
    // null buffer with a zero size as an invalid parameter, and its
    // invalid-parameter handler fast-fails the process (0xC0000409) before any
    // handler can run. That was the whole "the Windows build does nothing"
    // report of session 94 — the call is at the top of main, so the process died
    // silently on every launch and wrote no error.log. glibc and macOS accept a
    // zero size, so only Windows was affected.
    static char stdout_buffer[4096];
    std::setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer));
    std::atexit(flush);
}

void write_error_log(const char* reason) {
    // The capture threads drain the pipe asynchronously; give them a moment so
    // the message the runtime just printed is in the ring.
    std::fflush(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_report(-1, nullptr, nullptr, -1, reason != nullptr ? reason : "runtime error", false);
}

void flush() {
    if (!g_installed.exchange(false)) {
        return;
    }
    std::fflush(nullptr);
    redirect_stop(g_saved_stdout, 1, g_stdout_thread);
    redirect_stop(g_saved_stderr, 2, g_stderr_thread);
}

}  // namespace crash_log
}  // namespace ogre
