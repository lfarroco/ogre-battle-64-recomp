/* Link script for one example mod.
 *
 * A mod's ELF is relocatable: RecompModTool recompiles its sections and the
 * runtime loads them into the mod's own RAM. The link address is therefore only
 * a base that the runtime relocates, and the only thing that matters here is
 * that every section is kept and that the compiler's metadata sections are
 * dropped (the recompiler does not know them). */
RAMBASE = 0x81000000;

MEMORY {
    extram(ARWX) : ORIGIN = RAMBASE, LENGTH = 64M
}

SECTIONS {
    /DISCARD/ : {
        *(.got)
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.pdr)
        *(.comment)
        *(.note)
    }
}
