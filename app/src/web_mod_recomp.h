// The ABI a bundled mod's recompiled C is compiled against in the browser build.
//
// tools/gen_web_mods.py runs N64Recomp's OfflineModRecomp on each `.nrm`, which
// writes C that includes `mod_recomp.h`. The generated functions are then
// compiled into the wasm app, so they need the same register context, RDRAM
// accessors and relocation macros the recompiled game code uses.
//
// This header deliberately does not include N64Recomp's `recomp.h`. That header
// declares the game's single `section_addresses` pointer and the runtime's
// `get_function`, while a mod's generated C defines its own `section_addresses`
// array and its own `get_function` pointer, which the runtime fills in per mod
// (librecomp/src/mods.cpp, StaticModCodeHandle). The definitions below are the
// subset the mod code uses; the layout of `recomp_context` and the RDRAM byte
// rules match `recomp.h` exactly.
#ifndef OGRE_WEB_MOD_RECOMP_H
#define OGRE_WEB_MOD_RECOMP_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t gpr;

typedef union {
    double d;
    struct {
        float fl;
        float fh;
    };
    struct {
        uint32_t u32l;
        uint32_t u32h;
    };
    uint64_t u64;
} fpr;

typedef struct {
    gpr r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15,
        r16, r17, r18, r19, r20, r21, r22, r23,
        r24, r25, r26, r27, r28, r29, r30, r31;
    fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
        f8,  f9,  f10, f11, f12, f13, f14, f15,
        f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31;
    uint64_t hi, lo;
    uint32_t* f_odd;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
    uint32_t cop0_regs[32];
} recomp_context;

typedef void (recomp_func_t)(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
extern "C" {
#endif

// The runtime's function-entry trace (librecomp/src/function_trace.cpp). The
// generated code calls both; on a build without OGRE_TRACE_HOOKS they update a
// per-thread counter only.
void recomp_trace_entry(uint8_t* rdram, uint32_t func_vram, const char* func_name, uint32_t ra);
void recomp_trace_return(uint8_t* rdram, size_t func_index);

// The runtime's cooperative-scheduler yields. A mod's generated code emits
// these for `osSendMesg`-style calls and for the `pause` bridge; both live in
// librecomp and need no host code.
void yield_self(uint8_t* rdram);
void pause_self(uint8_t* rdram);

#ifdef __cplusplus
}
#endif

// The host-visible symbols the runtime fills in for each bundled mod. The names
// are the ones OfflineModRecomp emits; the static code handle in
// librecomp/src/mods.cpp reads them through recomp::mods::StaticModInfo.
#define RECOMP_EXPORT
#define RECOMP_FUNC __attribute__((noinline))

#define S32(val) ((int32_t)(val))
#define U32(val) ((uint32_t)(val))
#define S64(val) ((int64_t)(val))
#define U64(val) ((uint64_t)(val))

#define ADD32(a, b) ((gpr)(int32_t)((a) + (b)))
#define SUB32(a, b) ((gpr)(int32_t)((a) - (b)))

#define LO16(x) ((x) & 0xFFFF)
#define HI16(x) (((x) >> 16) + (((x) >> 15) & 1))

// N64 RDRAM addressing. This is the runtime's `recomp_mem_addr` in recomp.h:
// KSEG0 maps to the start of RDRAM, the KUSEG low 4 MiB mirrors it, and the
// MMIO window lands in the runtime's scratch page. Mod code normally touches
// only KSEG0 addresses, but keeping the rules identical means a mod that reads
// a low alias behaves the same in both builds.
#define OGRE_MOD_MMIO_SCRATCH_OFFSET 0x1F800000u
static inline uint64_t ogre_mod_mem_addr(gpr addr) {
    uint64_t a = (uint64_t)(uint32_t)addr;
    if ((a & 0xE0000000u) == 0xA0000000u) {
        return (a & 0x7FFFFFu) | OGRE_MOD_MMIO_SCRATCH_OFFSET;
    }
    if (a < 0x80000000u) {
        return a & 0x003FFFFFu;
    }
    return a - 0x80000000u;
}

#define MEM_W(offset, reg) \
    (*(int32_t*)(rdram + ogre_mod_mem_addr((gpr)(reg) + (offset))))
#define MEM_H(offset, reg) \
    (*(int16_t*)(rdram + (ogre_mod_mem_addr((gpr)(reg) + (offset)) ^ 2)))
#define MEM_B(offset, reg) \
    (*(int8_t*)(rdram + (ogre_mod_mem_addr((gpr)(reg) + (offset)) ^ 3)))
#define MEM_HU(offset, reg) \
    (*(uint16_t*)(rdram + (ogre_mod_mem_addr((gpr)(reg) + (offset)) ^ 2)))
#define MEM_BU(offset, reg) \
    (*(uint8_t*)(rdram + (ogre_mod_mem_addr((gpr)(reg) + (offset)) ^ 3)))

// `section_addresses` and `get_function` are the mod's own globals, defined by
// the generated C inside its namespace. A macro resolves them at the use site,
// so the same header works for every bundled mod.
#define RELOC_HI16(section_index, offset) HI16(section_addresses[section_index] + (offset))
#define RELOC_LO16(section_index, offset) LO16(section_addresses[section_index] + (offset))
#define LOOKUP_FUNC(val) get_function((int32_t)(val))

#endif  // OGRE_WEB_MOD_RECOMP_H
