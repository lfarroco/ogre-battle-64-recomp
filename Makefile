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
# Streamed-overlay bank unit (Phase 4). A *second*, independent splat + link +
# N64Recomp run for the streamed overlay records that are loaded into overlay
# C's RAM by a different bank (see config-bank.yaml). Kept separate because
# their VMAs overlap overlay C's and one ELF cannot hold both; the main unit is
# left untouched. Small enough to rebuild from scratch every time.
#
#   make bank        -> build/ogrebank.elf
#   make bank-recomp -> BankFuncs/ (registered by app/src/bank_overlays.cpp)
# ---------------------------------------------------------------------------
BANKCFG    := config-bank.yaml
BANKLD     := build/ogrebank.ld
BANKELF    := build/ogrebank.elf
BANKRECOMP := BankFuncs

bank-split: $(BANKLD)

$(BANKLD): $(BANKCFG)
	@mkdir -p build
	tools/venv/bin/splat split $(BANKCFG)

bank: $(BANKELF)

# The ld script names the object files next to their sources, so assemble in
# place (build/bank/asm/...).
$(BANKELF): $(BANKLD) $(wildcard build/bank/asm/*.s) $(wildcard build/bank/asm/data/*.s) \
            $(wildcard build/bank/assets/*.bin)
	@for f in build/bank/asm/*.s build/bank/asm/data/*.s; do \
	    $(AS) $(ASFLAGS) -o $${f%.s}.o $$f || exit 1; \
	done
	@for f in build/bank/assets/*.bin; do \
	    $(OBJCOPY) -I binary -O elf32-tradbigmips -B mips:3000 $$f $${f%.bin}.o || exit 1; \
	done
	$(LD) --emit-relocs -T $(BANKLD) -T build/bank/undefined_syms_auto.txt \
		-T build/bank/undefined_funcs_auto.txt -o $@ \
		build/bank/asm/*.o build/bank/asm/data/*.o build/bank/assets/*.o
	@echo "==> linked $(BANKELF)"

bank-recomp: $(BANKELF)
	$(N64RECOMP) config-bank.toml
	python3 tools/gen_bank_funcs.py

.PHONY: all clean recomp bank bank-split bank-recomp
