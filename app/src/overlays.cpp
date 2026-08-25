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
// Streamed overlays.
//
// The main segment references functions that live in streamed/overlay data
// (battle engine, cinematics, ...). The boot-resident streamed overlays are now
// recompiled into the ELF and registered here; functions in later, not-yet-
// recompiled overlays fall back to the runtime's generic log-and-return stub
// (see get_function in librecomp/src/overlays.cpp).
// -----------------------------------------------------------------------------

// Registers the boot streamed overlays. Must run after init_overlays() (which
// clears the function map), so it is invoked from the GameEntry on_init
// callback.
//
// Phase 4 (streamed overlays): the boot-resident streamed overlays
// (ROM 0x3F1B0 -> RAM 0x800E9C20, ROM 0x40E80 -> RAM 0x8016AF80, and the
// on-demand overlay C at ROM 0x1CE040 -> RAM 0x80197B90) are now recompiled
// into the ELF. They are registered here at their fixed load addresses (the
// game DMA's them before calling into them). Functions in later, not-yet-
// recompiled overlays fall back to the runtime's generic log-and-return stub.
void register_streamed_overlays() {
    load_overlays(0x3F1B0, (int32_t)0x800E9C20, 0x1CD0);   // overlay A
    load_overlays(0x40E80, (int32_t)0x8016AF80, 0x25FB0);  // overlay B
    load_overlays(0x1CE040, (int32_t)0x80197B90, 0x22A00); // overlay C
    printf("[overlays] registered boot streamed overlays A+B+C\n");
}

}  // namespace ogre
