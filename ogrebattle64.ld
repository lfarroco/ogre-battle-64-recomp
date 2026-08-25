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

    streamedA_ROM_START = __romPos;
    streamedA_VRAM = ADDR(.streamedA);
    .streamedA 0x800E9C20 : AT(streamedA_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        streamedA_TEXT_START = .;
        build/asm/3F1B0.o(.text);
        . = ALIGN(., 16);
        streamedA_TEXT_END = .;
        streamedA_TEXT_SIZE = ABSOLUTE(streamedA_TEXT_END - streamedA_TEXT_START);
        streamedA_DATA_START = .;
        build/asm/3F1B0.o(.data);
        build/asm/data/40640.data.o(.data);
        . = ALIGN(., 16);
        streamedA_DATA_END = .;
        streamedA_DATA_SIZE = ABSOLUTE(streamedA_DATA_END - streamedA_DATA_START);
        streamedA_RODATA_START = .;
        build/asm/3F1B0.o(.rodata);
        . = ALIGN(., 16);
        streamedA_RODATA_END = .;
        streamedA_RODATA_SIZE = ABSOLUTE(streamedA_RODATA_END - streamedA_RODATA_START);
    }
    streamedA_bss_VRAM = ADDR(.streamedA_bss);
    .streamedA_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        streamedA_BSS_START = .;
        build/asm/3F1B0.o(.bss);
        . = ALIGN(., 16);
        streamedA_BSS_END = .;
        streamedA_BSS_SIZE = ABSOLUTE(streamedA_BSS_END - streamedA_BSS_START);
    }
    __romPos += SIZEOF(.streamedA);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    streamedA_ROM_END = __romPos;
    streamedA_VRAM_END = .;

    streamedB_ROM_START = __romPos;
    streamedB_VRAM = ADDR(.streamedB);
    .streamedB 0x8016AF80 : AT(streamedB_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        streamedB_TEXT_START = .;
        build/asm/40E80.o(.text);
        . = ALIGN(., 16);
        streamedB_TEXT_END = .;
        streamedB_TEXT_SIZE = ABSOLUTE(streamedB_TEXT_END - streamedB_TEXT_START);
        streamedB_DATA_START = .;
        build/asm/40E80.o(.data);
        build/asm/data/5C230.data.o(.data);
        . = ALIGN(., 16);
        streamedB_DATA_END = .;
        streamedB_DATA_SIZE = ABSOLUTE(streamedB_DATA_END - streamedB_DATA_START);
        streamedB_RODATA_START = .;
        build/asm/40E80.o(.rodata);
        . = ALIGN(., 16);
        streamedB_RODATA_END = .;
        streamedB_RODATA_SIZE = ABSOLUTE(streamedB_RODATA_END - streamedB_RODATA_START);
    }
    streamedB_bss_VRAM = ADDR(.streamedB_bss);
    .streamedB_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        streamedB_BSS_START = .;
        build/asm/40E80.o(.bss);
        . = ALIGN(., 16);
        streamedB_BSS_END = .;
        streamedB_BSS_SIZE = ABSOLUTE(streamedB_BSS_END - streamedB_BSS_START);
    }
    __romPos += SIZEOF(.streamedB);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    streamedB_ROM_END = __romPos;
    streamedB_VRAM_END = .;

    _66E30_ROM_START = __romPos;
    _66E30_VRAM = ADDR(._66E30);
    ._66E30 0x66E30 : AT(_66E30_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        _66E30_DATA_START = .;
        build/assets/66E30.o(.data);
        _66E30_DATA_END = .;
        _66E30_DATA_SIZE = ABSOLUTE(_66E30_DATA_END - _66E30_DATA_START);
    }
    __romPos += SIZEOF(._66E30);
    _66E30_ROM_END = __romPos;
    _66E30_VRAM_END = .;

    streamedC_ROM_START = __romPos;
    streamedC_VRAM = ADDR(.streamedC);
    .streamedC 0x80197B90 : AT(streamedC_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        streamedC_TEXT_START = .;
        build/asm/1CE040.o(.text);
        . = ALIGN(., 16);
        streamedC_TEXT_END = .;
        streamedC_TEXT_SIZE = ABSOLUTE(streamedC_TEXT_END - streamedC_TEXT_START);
        streamedC_DATA_START = .;
        build/asm/1CE040.o(.data);
        build/asm/data/1EE540.data.o(.data);
        . = ALIGN(., 16);
        streamedC_DATA_END = .;
        streamedC_DATA_SIZE = ABSOLUTE(streamedC_DATA_END - streamedC_DATA_START);
        streamedC_RODATA_START = .;
        build/asm/1CE040.o(.rodata);
        . = ALIGN(., 16);
        streamedC_RODATA_END = .;
        streamedC_RODATA_SIZE = ABSOLUTE(streamedC_RODATA_END - streamedC_RODATA_START);
    }
    streamedC_bss_VRAM = ADDR(.streamedC_bss);
    .streamedC_bss (NOLOAD) : SUBALIGN(16)
    {
        FILL(0x00000000);
        streamedC_BSS_START = .;
        build/asm/1CE040.o(.bss);
        build/asm/data/1F0A00.bss.o(.bss);
        . = ALIGN(., 16);
        streamedC_BSS_END = .;
        streamedC_BSS_SIZE = ABSOLUTE(streamedC_BSS_END - streamedC_BSS_START);
    }
    __romPos += SIZEOF(.streamedC);
    __romPos = ALIGN(__romPos, 16);
    . = ALIGN(., 16);
    streamedC_ROM_END = __romPos;
    streamedC_VRAM_END = .;

    _1F0A00_ROM_START = __romPos;
    _1F0A00_VRAM = ADDR(._1F0A00);
    ._1F0A00 0x1F0A00 : AT(_1F0A00_ROM_START) SUBALIGN(16)
    {
        FILL(0x00000000);
        _1F0A00_DATA_START = .;
        build/assets/1F0A00.o(.data);
        _1F0A00_DATA_END = .;
        _1F0A00_DATA_SIZE = ABSOLUTE(_1F0A00_DATA_END - _1F0A00_DATA_START);
    }
    __romPos += SIZEOF(._1F0A00);
    _1F0A00_ROM_END = __romPos;
    _1F0A00_VRAM_END = .;

    /DISCARD/ :
    {
        *(*);
    }
}
