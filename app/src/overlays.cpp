#include <cstdio>
#include <cstdint>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/overlays.hpp"

// Includes the generated section/reloc/function tables for the recompiled ELF:
// defines `section_table`, `overlay_sections_by_index` and `num_sections`.
#include "recomp_overlays.inl"

namespace ogre {

// Registers the base code sections (entry + main) with the runtime overlay
// system. Must run before the game starts (init_overlays clears its state).
void register_base_overlays() {
    recomp::overlays::register_overlays(
        recomp::overlays::overlay_section_table_data_t{
            .code_sections = section_table,
            .num_code_sections = ARRLEN(section_table),
            .total_num_sections = num_sections,
        },
        recomp::overlays::overlays_by_index_t{
            .table = overlay_sections_by_index,
            .len = ARRLEN(overlay_sections_by_index),
        });
    printf("[overlays] registered %zu code sections\n", ARRLEN(section_table));
}

// -----------------------------------------------------------------------------
// Streamed-code stubs.
//
// The main segment references functions that live in streamed/overlay data
// (battle engine, cinematics, ...). The ELF has absolute placeholders for them,
// so the recompiled code calls them via `LOOKUP_FUNC` -> `get_function`, which
// hard-fails when the address is unknown. Until real overlay loading lands
// (Phase 4), register log-and-return stubs so the boot path survives.
// -----------------------------------------------------------------------------

static void streamed_stub_impl(uint8_t* rdram, recomp_context* ctx, uint32_t address) {
    printf("[overlays] streamed function stub called @ 0x%08X (not yet loaded)\n", address);
}

#define STREAMED_STUB(address)                                    \
    static void streamed_stub_##address(uint8_t* rdram, recomp_context* ctx) { \
        streamed_stub_impl(rdram, ctx, address);                  \
    }

STREAMED_STUB(0x800E9C20)
STREAMED_STUB(0x800E9CEC)
STREAMED_STUB(0x800E9E34)
STREAMED_STUB(0x800EA714)
STREAMED_STUB(0x800EA8E0)
STREAMED_STUB(0x800EAC24)
STREAMED_STUB(0x800EAF1C)
STREAMED_STUB(0x8016C900)
STREAMED_STUB(0x8016CB44)
STREAMED_STUB(0x8016CD30)
STREAMED_STUB(0x8016CD3C)
STREAMED_STUB(0x8016CD50)
STREAMED_STUB(0x8016CD90)
STREAMED_STUB(0x8016CDCC)
STREAMED_STUB(0x8016CDF4)
STREAMED_STUB(0x80173630)
STREAMED_STUB(0x80173B80)
STREAMED_STUB(0x80173BC0)
STREAMED_STUB(0x80173D34)
STREAMED_STUB(0x80173D6C)
STREAMED_STUB(0x80173DA4)
STREAMED_STUB(0x80173DDC)
STREAMED_STUB(0x80179080)
STREAMED_STUB(0x8017BDE0)
STREAMED_STUB(0x8017C2BC)
STREAMED_STUB(0x8017F4B0)
STREAMED_STUB(0x80180BFC)
STREAMED_STUB(0x80184214)
STREAMED_STUB(0x80184D90)
STREAMED_STUB(0x801AB740)
STREAMED_STUB(0x801AB76C)
STREAMED_STUB(0x840010BC)
STREAMED_STUB(0x84001120)
STREAMED_STUB(0x8400114C)

#undef STREAMED_STUB

// Registers stubs for every function the recompiled code looks up dynamically.
// Must run after init_overlays() (which clears the function map), so it is
// invoked from the GameEntry on_init callback.
void register_streamed_stubs() {
    using recomp::overlays::add_loaded_function;

    add_loaded_function(0x800E9C20, streamed_stub_0x800E9C20);
    add_loaded_function(0x800E9CEC, streamed_stub_0x800E9CEC);
    add_loaded_function(0x800E9E34, streamed_stub_0x800E9E34);
    add_loaded_function(0x800EA714, streamed_stub_0x800EA714);
    add_loaded_function(0x800EA8E0, streamed_stub_0x800EA8E0);
    add_loaded_function(0x800EAC24, streamed_stub_0x800EAC24);
    add_loaded_function(0x800EAF1C, streamed_stub_0x800EAF1C);
    add_loaded_function(0x8016C900, streamed_stub_0x8016C900);
    add_loaded_function(0x8016CB44, streamed_stub_0x8016CB44);
    add_loaded_function(0x8016CD30, streamed_stub_0x8016CD30);
    add_loaded_function(0x8016CD3C, streamed_stub_0x8016CD3C);
    add_loaded_function(0x8016CD50, streamed_stub_0x8016CD50);
    add_loaded_function(0x8016CD90, streamed_stub_0x8016CD90);
    add_loaded_function(0x8016CDCC, streamed_stub_0x8016CDCC);
    add_loaded_function(0x8016CDF4, streamed_stub_0x8016CDF4);
    add_loaded_function(0x80173630, streamed_stub_0x80173630);
    add_loaded_function(0x80173B80, streamed_stub_0x80173B80);
    add_loaded_function(0x80173BC0, streamed_stub_0x80173BC0);
    add_loaded_function(0x80173D34, streamed_stub_0x80173D34);
    add_loaded_function(0x80173D6C, streamed_stub_0x80173D6C);
    add_loaded_function(0x80173DA4, streamed_stub_0x80173DA4);
    add_loaded_function(0x80173DDC, streamed_stub_0x80173DDC);
    add_loaded_function(0x80179080, streamed_stub_0x80179080);
    add_loaded_function(0x8017BDE0, streamed_stub_0x8017BDE0);
    add_loaded_function(0x8017C2BC, streamed_stub_0x8017C2BC);
    add_loaded_function(0x8017F4B0, streamed_stub_0x8017F4B0);
    add_loaded_function(0x80180BFC, streamed_stub_0x80180BFC);
    add_loaded_function(0x80184214, streamed_stub_0x80184214);
    add_loaded_function(0x80184D90, streamed_stub_0x80184D90);
    add_loaded_function(0x801AB740, streamed_stub_0x801AB740);
    add_loaded_function(0x801AB76C, streamed_stub_0x801AB76C);
    add_loaded_function(0x840010BC, streamed_stub_0x840010BC);
    add_loaded_function(0x84001120, streamed_stub_0x84001120);
    add_loaded_function(0x8400114C, streamed_stub_0x8400114C);

    printf("[overlays] registered streamed-code stubs\n");
}

}  // namespace ogre
