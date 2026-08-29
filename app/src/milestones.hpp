#pragma once

// Milestone + instrumentation helpers for the Ogre Battle 64 browser port
// (see docs/WEB-PORT.md §13-14).
//
// Every milestone is printed to stderr. On web builds (__EMSCRIPTEN__) each
// line is also appended to a small shared buffer that the page reads via
// `ogre_poll_milestones()` (exported from main_web.cpp), so the browser can
// show how far the runtime got (ROM -> runtime -> threads -> RSP -> DL).
//
// The buffer is best-effort diagnostics: a torn read while a pthread appends
// can garble at most one line.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ogre::milestones {

inline std::mutex g_mutex;
inline char g_buffer[32 * 1024];
inline size_t g_len = 0;

// Logs a single milestone line: `[tag] message`.
inline void log(const char* tag, const char* message) {
    char line[512];
    int n = snprintf(line, sizeof(line), "[%s] %s\n", tag, message);
    if (n < 0) {
        return;
    }
    fprintf(stderr, "%s", line);
#ifdef __EMSCRIPTEN__
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_len + static_cast<size_t>(n) < sizeof(g_buffer)) {
        memcpy(g_buffer + g_len, line, static_cast<size_t>(n));
        g_len += static_cast<size_t>(n);
    }
#endif
}

// Pointer to the accumulated milestone text (web builds only; JS diffs it).
inline const char* poll_buffer() {
    return g_buffer;
}

}  // namespace ogre::milestones

// Logs `[tag] <printf-style args>`.
#define OGRE_MILESTONE(tag, ...)                                                      \
    do {                                                                              \
        char _ogre_milestone_msg[384];                                                \
        snprintf(_ogre_milestone_msg, sizeof(_ogre_milestone_msg), __VA_ARGS__);      \
        ::ogre::milestones::log((tag), _ogre_milestone_msg);                          \
    } while (0)
