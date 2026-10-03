// The recomp mod API's section macros.
//
// N64Recomp finds a mod's extra behaviour by the section a function is placed
// in. The names below are the API: `.recomp_hook.<function>` is a hook that runs
// at the entry (or at the return, for `.recomp_hook_return.`) of a function in
// the base game, `.recomp_patch` replaces it, and `.recomp_export` makes it
// callable from another mod. See `tools/N64Recomp/RecompModTool/main.cpp` and
// `tools/N64ModernRuntime/librecomp/include/librecomp/mods.hpp`.
//
// The same macros ship with the upstream mod template; this copy is what the
// example mods in this repository build against.
#ifndef OGRE_MODDING_H
#define OGRE_MODDING_H

#define RECOMP_IMPORT(mod, func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    _Pragma("GCC diagnostic ignored \"-Wreturn-type\"") \
    __attribute__((noinline, weak, used, section(".recomp_import." mod))) func {} \
    _Pragma("GCC diagnostic pop")

#define RECOMP_EXPORT __attribute__((section(".recomp_export")))

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

#define RECOMP_FORCE_PATCH __attribute__((section(".recomp_force_patch")))

#define RECOMP_DECLARE_EVENT(func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    __attribute__((noinline, weak, used, section(".recomp_event"))) void func {} \
    _Pragma("GCC diagnostic pop")

#define RECOMP_CALLBACK(mod, event) __attribute__((section(".recomp_callback." mod ":" #event)))

#define RECOMP_HOOK(func) __attribute__((section(".recomp_hook." func)))

#define RECOMP_HOOK_RETURN(func) __attribute__((section(".recomp_hook_return." func)))

#endif
