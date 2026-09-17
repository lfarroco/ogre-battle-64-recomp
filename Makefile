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
	@# N64Recomp only writes the files the new layout needs; it never deletes the
	@# previous run's. A symbol that moved (e.g. static_17_8021F470 -> a data
	@# label after a config change) therefore left its old definition behind and
	@# the build failed with "conflicting types" in a file the new run no longer
	@# emits (session 58, after moving bankRec10a out of unit C). Clear first.
	rm -rf RecompiledFuncs
	$(N64RECOMP) config.toml
	python3 tools/cross_bank.py dispatch --only 0x80198D28,0x801AFC2C,0x801980A0,0x801B00D0,0x8019A7C0,0x8019A884,0x8019AF0C,0x8019B060,0x8019B340,0x8019C4A8,0x8019C69C,0x801A103C

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
# Three targets are dispatched in the default build (the line in `recomp` above):
# 0x80198D28, scene 0x02's loader entry. The recompiler binds its single call
# site (func_80178920) to the containing overlay-C body func_801989AC — which
# dereferences $a0+3 with $a0 unset and dies — while bank unit E (record 0,
# resident in that scene) provides the real entry func_ovlE_80198D28. The
# dispatch also repairs the recompiler's tail-call emission at that site (an
# early `return` that abandons the caller's epilogue) back to call-and-continue.
# 0x801AFC2C, scene 0x0D's worker. The recompiler binds its two call sites
# (func_80177754 @0x80177964, func_80178568 @0x80178834 — the latter is scene
# 0x0D's update, reached via the dispatcher) to the containing overlay-C body
# func_801AFAF4, which runs stale overlay-C bytes while bank unit C's record 10
# (RAM 0x801AD5C0, resident in that scene) owns the address; the stale body
# faults on a wild halfword load. Bank unit C provides the real entry
# func_ovlC_801AFC2C. Same tail-call repair at both sites.
# 0x801980A0, scene 0x02's init worker. The recompiler binds its two call
# sites (func_801776EC @0x801776F4, func_80177720 @0x80177728) to the
# overlay-C fragment func_801980A0 (no prologue: it reads $s0/$t9 the callers
# never set), while bank unit E (record 0, resident in scene 0x02) provides
# the real entry func_ovlE_801980A0, whose contract takes the callers' $a0
# (-1/0x29). Scene 0x0D's init reads a flag this worker writes (0x8019F794);
# without the dispatch the flag stays zero and 0x0D walks the wrong init
# path. Both sites are already call-and-continue, so no tail repair.
# 0x801B00D0, scene 0x0D's teardown. Its two call sites (func_801779FC
# @0x801779FC, func_80178B7C @0x80178BB8 — the latter is 0x0D's leave callback)
# sit in .streamedB and target .streamedC, where the main ELF only has a 4-byte
# placeholder symbol, so N64Recomp redirected them into whichever size override
# contained 0x801B00D0 (func_801AFC2C's 0x4A8, or func_801AFAF4's 0x5E0) — the
# wrong function, so 0x0D's teardown never ran and the 0x02->0x0D re-entry
# crashed in func_800988A0 on a NULL destination (sessions 40/41). Bank unit C's
# record 10 provides the real entry func_ovlC_801B00D0, so the call is
# dispatched like the others; both sites were the redirect/early-return shape
# and both got the tail repair.
# A lookup that misses the map is a no-op stub, so the scenes that never load
# the owning record are unaffected. `make recomp` re-applies this after every
# regen; with no bank data at all the step warns and leaves the tree alone.
# 0x8019A7C0/0x8019A884/0x8019B060/0x8019B340/0x8019C4A8/0x8019C69C, the
# scene-0x07 form module (session 55). Scene 0x07's descriptor (0x8018FDAC,
# streamedB ROM 0x65CAC) enters through func_8017B794, which chunk-DMAs a
# 0x8600-byte code module (ROM 0x712A0 -> RAM 0x8019A7C0) and calls it. The
# module's RAM overlaps record 3 (unit A) and overlay C (the main ELF), so the
# recompiler bound those calls to overlay C's bodies at the same addresses —
# a different layout, which left the name-entry form black. Bank unit H is the
# module; these six are every call the resident code makes into it. See
# docs/HANDOFF-2026-09-16-session55.md.
# The remaining targets stay on the recompiler's bindings (session 33's
# behaviour); dispatching the other resolvable ones is still an opt-in
# experiment. See docs/DECISIONS.md (sessions 34, 37, 38, 41, 55).
# ---------------------------------------------------------------------------
cross-bank-report:
	python3 tools/cross_bank.py report

cross-bank-dispatch:
	python3 tools/cross_bank.py write-seeds
	python3 tools/cross_bank.py dispatch

# `make cross-bank-check`: the full audit, including the main unit's known
# backlog of calls into swappable RAM (informational; `--strict` fails on it).
cross-bank-check:
	python3 tools/cross_bank.py check

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
BANK_UNITS := A B C D E F G H I J K L M
BANK_ELFS  := $(addprefix build/bank,$(addsuffix .elf,$(BANK_UNITS)))
BANK_LDS   := $(addprefix build/bank,$(addsuffix .ld,$(BANK_UNITS)))

bank-split: $(BANK_LDS)

# splat only rewrites the files the new config names; it does not delete the
# outputs of a segment that was moved to another unit. `build/bank<U>.elf`
# globs its inputs, so a stale `.o` from the removed segment is still linked and
# fails with "multiple definition" (or worse, links silently). Clear the unit's
# own asm/assets before each split: session 58 moved bankRec10a out of unit C
# into unit I and hit exactly this.
build/bank%.ld: config-bank%.yaml
	@mkdir -p build
	@rm -rf build/bank$*/asm build/bank$*/assets
	tools/venv/bin/splat split $<

# The bank ELF is `bank-force`-driven on purpose: its `.o` inputs are found by
# globbing (the ld script names them next to their sources), so `make` cannot
# see an `.s` change on its own. Depending on the *config* (not the generated
# `.ld`) makes every input change re-assemble and re-link; skipping that once
# silently linked stale objects when splat had rewritten an `.s` without
# touching the `.ld`.
build/bank%.elf: config-bank%.yaml | build/bank%.ld
	@for f in build/bank$*/asm/*.s build/bank$*/asm/data/*.s; do \
	    [ -f "$$f" ] || continue; \
	    $(AS) $(ASFLAGS) -o $${f%.s}.o $$f || exit 1; \
	done
	@for f in build/bank$*/assets/*.bin; do \
	    [ -f "$$f" ] || continue; \
	    $(OBJCOPY) -I binary -O elf32-tradbigmips -B mips:3000 $$f $${f%.bin}.o || exit 1; \
	done
	@python3 tools/gen_bank_syms.py $*
	$(LD) --emit-relocs -T build/bank$*.ld -T build/bank$*/undefined_syms_auto.txt \
		-T build/bank$*/undefined_funcs_auto.txt -T build/bank$*/extra_syms.txt -o $@ \
		$$(ls build/bank$*/asm/*.o build/bank$*/asm/data/*.o build/bank$*/assets/*.o 2>/dev/null)
	@echo "==> linked $@"

# --- session bookkeeping helpers ---------------------------------------------
# `make handoffs`: every docs/HANDOFF-*.md newest-first with its first heading,
# so the per-session record is one command away (and the newest one is obvious).
handoffs:
	python3 tools/handoffs.py
.PHONY: handoffs

# `make midfunc`: the fall-through / shared-tail report (see tools/midfunc.py).
# These are `jal` targets that are the tail of the function above them, so the
# callee inherits a frame and registers a direct call never sets up.
midfunc:
	python3 tools/midfunc.py
.PHONY: midfunc

.PHONY: bank bank-force
bank: bank-force

bank-force: $(BANK_ELFS)

# `make elf-rom-check`: every linked ELF must be byte-identical to the ROM at the
# same ROM offset, and every symbol whose name ends in eight hex digits must be
# defined at that address. Session 65: unit M's data subsegment was linked 8
# bytes high (the assembler pads `.text` to 16 bytes, while the ROM's code/data
# boundary is only 8-aligned), so every data label was +8 and the recompiled
# `lui/%lo` pairs read 8 bytes too high — the map's sprite table came out
# shifted by one entry. The tool names the section and the first differing
# offset, so the fix goes in the config, never in the generated `.s`.
# See tools/elfcheck.py.
elf-rom-check:
	python3 tools/elfcheck.py --syms
.PHONY: elf-rom-check

bank-recomp: bank
	@# A misplaced section is invisible in the generated C and only shows up as a
	@# scene misbehaving, so check the link before recompiling it (session 65).
	python3 tools/elfcheck.py --syms >/dev/null || { python3 tools/elfcheck.py --syms; exit 1; }
	@# Same stale-output trap as `recomp` above: clear each unit's generated tree
	@# so a symbol that changed section (or moved to another unit) cannot leave a
	@# definition behind. `Bank*Funcs/` is gitignored and regenerated here.
	@for u in $(BANK_UNITS); do rm -rf Bank$${u}Funcs; $(N64RECOMP) config-bank$$u.toml || exit 1; done
	python3 tools/gen_bank_funcs.py
	@# The njpeg stage-3 readback has to copy the buffer its own YUV draw landed
	@# in; the game's own pointer can be left at the framebuffer table's
	@# placeholder entry. Regenerated above, so re-apply here (session 48).
	python3 tools/njpeg_readback.py
	@# Assert the invariant session 45's wall broke: a unit must never define a
	@# RAM range another bank can own *and* call into it from another record (the
	@# call would be bound at build time to the wrong bank's layout). See
	@# `cross_bank.py check` and config-bankF.yaml.
	python3 tools/cross_bank.py check-banks

# ---------------------------------------------------------------------------
# RSP microcode (RSPRecomp). Only microcodes the runtime's task thread actually
# executes need this: gfx tasks go to RT64, which parses display lists itself.
# rsp-njpeg.toml recompiles the game's Nintendo-JPEG decoder (M_NJPEGTASK, type
# 4), which decodes the New Game opening's full-screen background images into
# RspFuncs/njpeg_ucode.cpp; app/src/rsp.cpp dispatches it by ucode address.
# ---------------------------------------------------------------------------
RSPRECOMP := tools/N64Recomp/build/RSPRecomp

.PHONY: rsp-recomp
rsp-recomp:
	$(RSPRECOMP) rsp-njpeg.toml

.PHONY: all clean recomp cross-bank-report cross-bank-dispatch cross-bank-check bank-split bank-recomp handoffs midfunc rsp-recomp
