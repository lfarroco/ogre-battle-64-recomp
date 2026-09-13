# Ogre Battle 64 - recompilation project build
#
# Stages:
#   1. splat split   -> asm/*.s + assets/*.bin + ld script
#   2. make          -> assemble/objcopy to .o, link to build/ogrebattle64.elf
#   3. N64Recomp     -> generate C from the ELF + config.toml

ROM      := assets/ogre64.z64
BASENAME := ogrebattle64
LDSCRIPT := $(BASENAME).ld
ELF      := build/$(BASENAME).elf

AS       := mips-linux-gnu-as
LD       := mips-linux-gnu-ld
OBJCOPY  := mips-linux-gnu-objcopy
N64RECOMP := tools/N64Recomp/build/N64Recomp

ASFLAGS  := -mips3 -mabi=32 -O0 -I include
LDFLAGS  := --emit-relocs -Map build/$(BASENAME).map

ASM_FILES  := $(wildcard asm/*.s)
DATA_FILES := $(wildcard asm/data/*.s)
OBJS       := $(patsubst asm/%.s,build/asm/%.o,$(ASM_FILES)) \
              $(patsubst asm/data/%.s,build/asm/data/%.o,$(DATA_FILES))
BIN_FILES  := $(patsubst assets/%.bin,build/assets/%.o,$(wildcard assets/*.bin))

all: $(ELF)

.PHONY: fix-labels
# Re-applies manual cross-overlay label fixes to splat-generated asm. Must run
# BEFORE the .s files are assembled (a phony prerequisite of the object rules
# below), because splat re-split regenerates the unfixed .s files.
fix-labels:
	@bash tools/fix_cross_overlay_labels.sh

clean:
	rm -rf build

$(ELF): $(OBJS) $(BIN_FILES) $(LDSCRIPT) undefined_syms_auto.txt undefined_funcs_auto.txt
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LDSCRIPT) -T undefined_syms_auto.txt \
		-T undefined_funcs_auto.txt -T extra_syms.txt -o $@ $(OBJS) $(BIN_FILES)
	@echo "==> linked $(ELF)"
	@$(OBJCOPY) --dump-section .entry=$@.entry.bin $@ 2>/dev/null || true

build/asm/%.o: asm/%.s fix-labels
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

build/asm/data/%.o: asm/data/%.s fix-labels
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

build/assets/%.o: assets/%.bin
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf32-tradbigmips -B mips:3000 $< $@

recomp: $(ELF)
	$(N64RECOMP) config.toml

# ---------------------------------------------------------------------------
# Streamed-overlay bank units (Phase 4). Independent splat + link + N64Recomp
# runs for the streamed overlay records the game loads into RAM that overlay C
# also uses (see config-bankA.yaml / config-bankC.yaml). Kept separate because
# their VMAs overlap overlay C's and one ELF cannot hold both; the main unit is
# left untouched.
#
# A unit can only hold records whose RAM ranges are mutually disjoint: records
# that overlap are different banks of the same RAM. The runtime does not care
# which unit a record came from — app/src/bank_overlays.cpp registers a record's
# functions when the game DMA's it — so new records can be added to whichever
# unit has a free RAM range.
#
#   make bank        -> build/bank<U>.elf
#   make bank-recomp -> Bank<U>Funcs/ + app/src/bank_funcs.inc
# ---------------------------------------------------------------------------
BANK_UNITS := A C
BANK_ELFS  := $(addprefix build/bank,$(addsuffix .elf,$(BANK_UNITS)))
BANK_LDS   := $(addprefix build/bank,$(addsuffix .ld,$(BANK_UNITS)))

bank-split: $(BANK_LDS)

build/bank%.ld: config-bank%.yaml
	@mkdir -p build
	tools/venv/bin/splat split $<

bank: $(BANK_ELFS)

# The ld script names the object files next to their sources, so assemble in
# place (build/bank<U>/asm/...).
build/bank%.elf: build/bank%.ld
	@for f in build/bank$*/asm/*.s build/bank$*/asm/data/*.s; do \
	    $(AS) $(ASFLAGS) -o $${f%.s}.o $$f || exit 1; \
	done
	@for f in build/bank$*/assets/*.bin; do \
	    $(OBJCOPY) -I binary -O elf32-tradbigmips -B mips:3000 $$f $${f%.bin}.o || exit 1; \
	done
	@python3 tools/gen_bank_syms.py $*
	$(LD) --emit-relocs -T $< -T build/bank$*/undefined_syms_auto.txt \
		-T build/bank$*/undefined_funcs_auto.txt -T build/bank$*/extra_syms.txt -o $@ \
		build/bank$*/asm/*.o build/bank$*/asm/data/*.o build/bank$*/assets/*.o
	@echo "==> linked $@"

bank-recomp: $(BANK_ELFS)
	@for u in $(BANK_UNITS); do $(N64RECOMP) config-bank$$u.toml || exit 1; done
	python3 tools/gen_bank_funcs.py

.PHONY: all clean recomp bank bank-split bank-recomp
