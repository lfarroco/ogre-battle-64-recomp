build/ogrebattle64.elf: \
    build/asm/header.o \
    build/assets/ipl3.o \
    build/asm/1000.o \
    build/asm/1060.o \
    build/asm/data/2E570.data.o \
    build/asm/data/3F1B0.bss.o \
    build/assets/3F1B0.o
build/asm/header.o:
build/assets/ipl3.o:
build/asm/1000.o:
build/asm/1060.o:
build/asm/data/2E570.data.o:
build/asm/data/3F1B0.bss.o:
build/assets/3F1B0.o:
-include build/asm/header.d build/assets/ipl3.d build/asm/1000.d build/asm/1060.d build/asm/data/2E570.data.d build/asm/data/3F1B0.bss.d build/assets/3F1B0.d
