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

# splat split is NOT part of the normal build: `make` only assembles and links.
# The generated `ogrebattle64.ld` therefore keeps describing the previous
# segment layout, and a `config.yaml` edit that moves a segment is silently
# ignored — that is how `streamedC` ended up linked at another record's ROM
# address, so overlay C was never loaded and its effects (the intro smoke, the
# logo's 3D characters) vanished. Run `make resplit` after any segment change,
# and check that every streamed segment's ROM start in the map matches
# config.yaml.
.PHONY: resplit
resplit:
	rm -f $(LDSCRIPT) assets/*.bin
	tools/venv/bin/splat split config.yaml

# `as` needs the fixed .s files, so re-apply the label patch before they change.
resplit: fix-labels

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
	python3 tools/cross_bank.py dispatch --only 0x80198D28

# ---------------------------------------------------------------------------
# Cross-bank call routing (Phase 4).
#
# N64Recomp binds a `jal` to a function it knows as a *direct C call*, so a call
# from resident code into RAM that a bank record can also occupy runs the main
# ELF's overlay C body even when a different bank is resident. tools/cross_bank.py
# analyses that (see `make cross-bank-report`) and can dispatch the calls, but
# its seeds are not usable yet: an address that is only a function in the main
# unit's layout is a body interior in the bank's, and forcing splat to split
# there makes the recompiler emit fragments with undefined `goto` labels.
#
# One target is dispatched in the default build (the line in `recomp` above):
# 0x80198D28, scene 0x02's loader entry. The recompiler binds its single call
# site (func_80178920) to the containing overlay-C body func_801989AC — which
# dereferences $a0+3 with $a0 unset and dies — while bank unit E (record 0,
# resident in that scene) provides the real entry func_ovlE_80198D28. The
# dispatch also repairs the recompiler's tail-call emission at that site (an
# early `return` that abandons the caller's epilogue) back to call-and-continue.
# A lookup that misses the map is a no-op stub, so the boot/attract scenes that
# never call it are unaffected. `make recomp` re-applies this after every
# regen; with no bank data at all the step warns and leaves the tree alone.
# The remaining 66 targets stay on the recompiler's bindings (session 33's
# behaviour); dispatching the other 8 resolvable ones is still an opt-in
# experiment. See docs/DECISIONS.md (sessions 34, 37).
# ---------------------------------------------------------------------------
cross-bank-report:
	python3 tools/cross_bank.py report

cross-bank-dispatch:
	python3 tools/cross_bank.py write-seeds
	python3 tools/cross_bank.py dispatch

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
BANK_UNITS := A C D E
BANK_ELFS  := $(addprefix build/bank,$(addsuffix .elf,$(BANK_UNITS)))
BANK_LDS   := $(addprefix build/bank,$(addsuffix .ld,$(BANK_UNITS)))

bank-split: $(BANK_LDS)

build/bank%.ld: config-bank%.yaml
	@mkdir -p build
	tools/venv/bin/splat split $<

# The bank ELF is `bank-force`-driven on purpose: its `.o` inputs are found by
# globbing (the ld script names them next to their sources), so `make` cannot
# see an `.s` change on its own. Depending on the *config* (not the generated
# `.ld`) makes every input change re-assemble and re-link; skipping that once
# silently linked stale objects when splat had rewritten an `.s` without
# touching the `.ld`.
build/bank%.elf: config-bank%.yaml | build/bank%.ld
	@for f in build/bank$*/asm/*.s build/bank$*/asm/data/*.s; do \
	    $(AS) $(ASFLAGS) -o $${f%.s}.o $$f || exit 1; \
	done
	@for f in build/bank$*/assets/*.bin; do \
	    $(OBJCOPY) -I binary -O elf32-tradbigmips -B mips:3000 $$f $${f%.bin}.o || exit 1; \
	done
	@python3 tools/gen_bank_syms.py $*
	$(LD) --emit-relocs -T build/bank$*.ld -T build/bank$*/undefined_syms_auto.txt \
		-T build/bank$*/undefined_funcs_auto.txt -T build/bank$*/extra_syms.txt -o $@ \
		build/bank$*/asm/*.o build/bank$*/asm/data/*.o build/bank$*/assets/*.o
	@echo "==> linked $@"

.PHONY: bank bank-force
bank: bank-force

bank-force: $(BANK_ELFS)

bank-recomp: bank
	@for u in $(BANK_UNITS); do $(N64RECOMP) config-bank$$u.toml || exit 1; done
	python3 tools/gen_bank_funcs.py
.PHONY: all clean recomp cross-bank-report cross-bank-dispatch bank-split bank-recomp
