SECTIONS
{
    HIDDEN(__romPos = 0);
    header_ROM_START = __romPos;
    header_VRAM = ADDR(.header);
    .header : AT(header_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        header_DATA_START = .;
        build/asm/header.o(.data);
        header_DATA_END = .;
        header_DATA_SIZE = ABSOLUTE(header_DATA_END - header_DATA_START);
    }
    __romPos += SIZEOF(.header);
    header_ROM_END = __romPos;
    header_VRAM_END = .;

    ipl3_ROM_START = __romPos;
    ipl3_VRAM = ADDR(.ipl3);
    .ipl3 : AT(ipl3_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        ipl3_DATA_START = .;
        build/assets/ipl3.o(.data);
        ipl3_DATA_END = .;
        ipl3_DATA_SIZE = ABSOLUTE(ipl3_DATA_END - ipl3_DATA_START);
    }
    __romPos += SIZEOF(.ipl3);
    ipl3_ROM_END = __romPos;
    ipl3_VRAM_END = .;

    entry_ROM_START = __romPos;
    entry_VRAM = ADDR(.entry);
    .entry 0x80070C00 : AT(entry_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        entry_TEXT_START = .;
        build/asm/1000.o(.text);
        . = ALIGN(., 16);
        entry_TEXT_END = .;
        entry_TEXT_SIZE = ABSOLUTE(entry_TEXT_END - entry_TEXT_START);
        entry_DATA_START = .;
        build/asm/1000.o(.data);
        . = ALIGN(., 16);
        entry_DATA_END = .;
        entry_DATA_SIZE = ABSOLUTE(entry_DATA_END - entry_DATA_START);
        entry_RODATA_START = .;
        build/asm/1000.o(.rodata);
        . = ALIGN(., 16);
        entry_RODATA_END = .;
        entry_RODATA_SIZE = ABSOLUTE(entry_RODATA_END - entry_RODATA_START);
    }
    entry_bss_VRAM = ADDR(.entry_bss);
    .entry_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        entry_BSS_START = .;
        build/asm/1000.o(.bss);
        . = ALIGN(., 16);
        entry_BSS_END = .;
        entry_BSS_SIZE = ABSOLUTE(entry_BSS_END - entry_BSS_START);
    }
    __romPos += SIZEOF(.entry);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    entry_ROM_END = __romPos;
    entry_VRAM_END = .;

    main_ROM_START = __romPos;
    main_VRAM = ADDR(.main);
    .main entry_VRAM_END : AT(main_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        main_TEXT_START = .;
        build/asm/1060.o(.text);
        . = ALIGN(., 16);
        main_TEXT_END = .;
        main_TEXT_SIZE = ABSOLUTE(main_TEXT_END - main_TEXT_START);
        main_DATA_START = .;
        build/asm/1060.o(.data);
        build/asm/data/2E570.data.o(.data);
        . = ALIGN(., 16);
        main_DATA_END = .;
        main_DATA_SIZE = ABSOLUTE(main_DATA_END - main_DATA_START);
        main_RODATA_START = .;
        build/asm/1060.o(.rodata);
        . = ALIGN(., 16);
        main_RODATA_END = .;
        main_RODATA_SIZE = ABSOLUTE(main_RODATA_END - main_RODATA_START);
    }
    main_bss_VRAM = ADDR(.main_bss);
    .main_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        main_BSS_START = .;
        build/asm/1060.o(.bss);
        build/asm/data/3F1B0.bss.o(.bss);
        . = ALIGN(., 16);
        main_BSS_END = .;
        main_BSS_SIZE = ABSOLUTE(main_BSS_END - main_BSS_START);
    }
    __romPos += SIZEOF(.main);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    main_ROM_END = __romPos;
    main_VRAM_END = .;

    _3F1B0_ROM_START = __romPos;
    _3F1B0_VRAM = ADDR(._3F1B0);
    ._3F1B0 : AT(_3F1B0_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        _3F1B0_DATA_START = .;
        build/assets/3F1B0.o(.data);
        _3F1B0_DATA_END = .;
        _3F1B0_DATA_SIZE = ABSOLUTE(_3F1B0_DATA_END - _3F1B0_DATA_START);
    }
    __romPos += SIZEOF(._3F1B0);
    _3F1B0_ROM_END = __romPos;
    _3F1B0_VRAM_END = .;

    /DISCARD/ :
    {
        *(*);
    }
}
