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

recomp: recomp-prep $(ELF)
	@# N64Recomp only writes the files the new layout needs; it never deletes the
	@# previous run's. A symbol that moved (e.g. static_17_8021F470 -> a data
	@# label after a config change) therefore left its old definition behind and
	@# the build failed with "conflicting types" in a file the new run no longer
	@# emits (session 58, after moving bankRec10a out of unit C). Clear first.
	rm -rf RecompiledFuncs
	$(N64RECOMP) config.toml
	python3 tools/cross_bank.py dispatch --only 0x80198D28,0x801AFC2C,0x801980A0,0x801B00D0,0x8019A7C0,0x8019A884,0x8019AF0C,0x8019B060,0x8019B340,0x8019C4A8,0x8019C69C,0x8019D67C,0x801A103C,0x801C19B0,0x801C214C,0x801B7FC0

# The cross-bank dispatch inside `recomp` can only see bank records that
# `make bank-recomp` has already written to app/src/bank_funcs.inc. Without it
# the dispatch is skipped, the build succeeds, and calls into swappable RAM are
# bound to the wrong bank — the failure class of sessions 41/45/55. Refuse to
# recompile in that state. (`app/src/bank_funcs.inc` is a prerequisite of the
# app build, not of this target, so make will not silently regenerate it here.)
.PHONY: recomp-prep
recomp-prep:
	@test -f app/src/bank_funcs.inc || { \
	  echo "recomp: app/src/bank_funcs.inc is missing, so the cross-bank dispatch would be skipped"; \
	  echo "        and calls into swappable RAM would be bound to the wrong bank."; \
	  echo "        Run 'make bank-recomp' first, or use 'make regenerate' for the whole"; \
	  echo "        order (resplit -> link -> bank-recomp -> recomp -> rsp-recomp)."; \
	  exit 1; }

# ---------------------------------------------------------------------------
# make regenerate -- everything a fresh clone needs, in the one order that
# works, from the ROM at assets/ogre64.z64:
#
#   splat split (asm/ + assets/ + the linker script)   -> make resplit
#   assemble + link the MIPS ELF                       -> make
#   the bank units' ELFs + Bank*Funcs/ + bank_funcs.inc-> make bank-recomp
#   the main unit's C (dispatches cross-bank calls
#     against the bank data the previous step wrote)   -> make recomp
#   the RSP microcode (njpeg + audio)                  -> make rsp-recomp
#
# The order is load-bearing: `make recomp` runs the cross-bank dispatch, which
# can only see bank records that `bank_funcs.inc` already lists. Running the
# two the other way round leaves 15 dispatch sites undispatched — the build
# succeeds and runs *wrong-bank code*, which is the failure class of sessions
# 41/45/55. `make recomp` now refuses to run in that state; this target is the
# supported way to regenerate a tree.
# ---------------------------------------------------------------------------
.PHONY: regenerate
regenerate:
	@echo "==> [1/5] splat split (asm/, assets/, linker script)"
	$(MAKE) resplit
	@echo "==> [2/5] assemble + link the main ELF"
	$(MAKE) $(ELF)
	@echo "==> [3/5] the streamed bank units"
	$(MAKE) bank-recomp
	@echo "==> [4/5] recompile the main unit (with the cross-bank dispatch)"
	$(MAKE) recomp
	@echo "==> [5/5] the RSP microcode"
	$(MAKE) rsp-recomp
	@echo "==> regenerated: RecompiledFuncs/ Bank*Funcs/ RspFuncs/ app/src/bank_funcs.inc"

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
# The default build dispatches a growing list of targets (the line in `recomp`
# above). Each entry is a call from resident code into RAM that N64Recomp bound
# into the wrong layout; the notes below record why that target and not another:
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
# 0x8019D67C, the same module's save-data menu entry, is the seventh (session
# 88). It is scene 0x18's `enter` (func_8017BA60 @0x8017BB04) that calls it,
# right after chunk-DMAing ROM 0x712A0 into RAM 0x8019A7C0 — and scene 0x18 is
# exactly the menu the game reaches by holding Start through boot
# (func_800721DC: `D_800E79B0 & 0x1000` -> scene 0x18, else 0x09). The call was
# bound to func_8019D568 because config.toml extends that symbol over
# 0x8019D67C (0xCB0, session 30's fall-through closure), so N64Recomp emitted
# the redirect-plus-early-`return` shape: the enter returned before the menu
# initialised, the scene sat on a black frame with no display lists, and unit
# H's func_ovlH_8019D67C never ran. Dispatched, the lookup hands the call to
# the resident module (unit H) and the tail repair restores call-and-continue.
# The `func_8019D568` override is left in place: 0x8019D67C is a genuine
# continuation of that body, and the one *call* to it is the site dispatched
# here.
# The remaining targets stay on the recompiler's bindings (session 33's
# behaviour); dispatching the other resolvable ones is still an opt-in
# experiment. See docs/DECISIONS.md (sessions 34, 37, 38, 41, 55, 88).
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

# `make stubmap`: which dispatched addresses the runtime's function map cannot
# resolve, and what points at them (see tools/stubmap.py). Offline, reads the
# generated C + the registration tables; `make stub-check` is the strict form
# for CI ("no dispatch is provably unresolved").
stubmap:
	python3 tools/stubmap.py report

stub-check:
	python3 tools/stubmap.py report --strict
.PHONY: stubmap stub-check

# `make recompcov`: how complete is the recompilation — code bytes, modules and
# dispatch offline; add `--log` for what a run executed (OGRE_COVER=1).
recompcov:
	python3 tools/recompcov.py
.PHONY: recompcov

# ---------------------------------------------------------------------------
# Example mods (mods/). A mod's code is compiled to big-endian MIPS and then
# recompiled against the base game's symbols, so it needs two derived inputs:
# the linked ELF (already a prerequisite of the recompilation) and the reference
# symbol dump made from it. Both are generated and gitignored, for the same
# reason RecompiledFuncs/ is: they need the ROM.
#
#   make mod-syms     -> mods/reference/dump.toml, data_dump.toml
#   make example-mods -> build/mods/<name>.nrm
#
# `tools/build-example-mods.sh` needs a MIPS cross gcc (or Docker); see its
# header. `make dist` ships whatever is already in build/mods/.
# ---------------------------------------------------------------------------
.PHONY: mod-syms
mod-syms: $(ELF)
	@mkdir -p mods/reference
	@cd mods/reference && ../../$(N64RECOMP) ../../config.toml --dump-context >/dev/null
	@echo "==> mods/reference/dump.toml, mods/reference/data_dump.toml"

.PHONY: example-mods
example-mods: mod-syms
	tools/build-example-mods.sh

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
BANK_UNITS := A B C D E F G H I J K L M N O P Q R S T U V W X Y Z AA AB AC AD AE AF AG AH
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
# RspFuncs/njpeg_ucode.cpp; rsp-audio.toml recompiles the audio microcode
# (M_AUDTASK, type 2) into RspFuncs/audio_ucode.cpp. app/src/rsp.cpp dispatches
# both by ucode address.
# ---------------------------------------------------------------------------
RSPRECOMP := tools/N64Recomp/build/RSPRecomp

.PHONY: rsp-recomp
rsp-recomp:
	$(RSPRECOMP) rsp-njpeg.toml
	$(RSPRECOMP) rsp-audio.toml

# ---------------------------------------------------------------------------
# make dist -- a self-contained folder a player can run
#
# The game is "bring your own ROM", so the package is the app and nothing else.
# On first launch the app shows its start screen ("Click to load your ROM"), and
# once a ROM has been chosen it stores it in its own folder, so from then on the
# same executable boots the game directly. The battery save lands in `saves/`
# beside the executable (app/src/launcher.cpp resolve_pref_dir; OGRE_PREF_DIR
# overrides it).
#
# On macOS the executable is wrapped in `Ogre Battle 64.app` so Finder launches
# it without opening a Terminal window; `SDL_FILESYSTEM_BASE_DIR_TYPE=parent` in
# packaging/macos-Info.plist keeps the ROM, saves/, mods/ and error.log in the
# folder that holds the .app. On Windows the executable is linked as a GUI
# subsystem binary (app/CMakeLists.txt), so no console window is allocated. A
# crash writes `error.log` beside the save file (app/src/crash_log.cpp).
#
#   make dist            # -> dist/ogre-battle-64-recomp/
#   make dist-zip        # -> dist/ogre-battle-64-recomp-<host>.zip
#
# The recompiled game code is *generated and gitignored* (RecompiledFuncs/,
# Bank*Funcs/, RspFuncs/, app/src/bank_funcs.inc), so this needs no ROM only if
# the tree has already been recompiled once on this machine. tools/release-build.sh
# checks that and says exactly what to run; a fresh clone cannot build at all
# until `make && make recomp && make bank-recomp` has run against a ROM.
#
# **SDL2 is linked statically, so the package is one file.** `make sdl2-static`
# fetches and builds a pinned SDL2 into tools/SDL2-static/ (gitignored), and
# `dist` builds the app against it. Homebrew's `sdl2` formula on macOS is
# `sdl2-compat`, a shim over libSDL3 with no static library, so a static link
# needs the real SDL2 sources -- hence the fetch. Set DIST_STATIC_SDL=0 to skip
# it and bundle the shared SDL2 instead (much faster, but two files).
# ---------------------------------------------------------------------------
DIST_NAME := ogre-battle-64-recomp
DIST_DIR  := dist/$(DIST_NAME)
# The appended `.exe` is what CMake produces on Windows; the copy step uses this
# name so the recipe is identical on every runner.
EXE_NAME  := ogrebattle64$(if $(filter windows,$(DIST_OS)),.exe,)

# The pinned real SDL2 (2.32.x is API/ABI-compatible with the 2.32.70
# sdl2-compat that Homebrew installs, so both headers and libraries work).
SDL2_VERSION := 2.32.10
SDL2_SRC     := tools/SDL2-static/SDL2-$(SDL2_VERSION)
SDL2_PREFIX  := tools/SDL2-static/prefix
SDL2_TARBALL := https://github.com/libsdl-org/SDL/releases/download/release-$(SDL2_VERSION)/SDL2-$(SDL2_VERSION).tar.gz

DIST_STATIC_SDL ?= 1

# Extra arguments for every CMake configure (SDL2 and the app). The Windows CI
# job sets this to `-G Ninja -DCMAKE_C_COMPILER=clang-cl
# -DCMAKE_CXX_COMPILER=clang-cl`, because the app and the runtime are written
# for Clang/GCC spellings of their flags; see docs/guides/app-build.md.
CMAKE_ARGS ?=

# `DIST_OS` selects the packaging rules. It defaults to the host, and CI passes
# it explicitly so one recipe serves every runner:
#
#   make dist DIST_OS=windows
#
DIST_OS ?= $(if $(filter Darwin,$(shell uname -s)),macos,$(if $(filter Linux,$(shell uname -s)),linux,windows))

# Where the executable lands inside the package. macOS ships a .app bundle so
# Finder launches it without opening Terminal; every other platform gets a bare
# file next to the README. `SDL_FILESYSTEM_BASE_DIR_TYPE=parent` in the bundle's
# Info.plist makes SDL_GetBasePath return the folder that contains the .app, so
# the ROM, saves/, mods/ and error.log stay in the package folder the README
# describes rather than inside the bundle.
ifeq ($(DIST_OS),macos)
DIST_APP     := $(DIST_DIR)/Ogre Battle 64.app
DIST_BIN_DIR := $(DIST_APP)/Contents/MacOS
else
DIST_BIN_DIR := $(DIST_DIR)
endif

# Static SDL2, built once. Its license ships with the binary (see `dist`).
#
# The library name depends on the generator: a single-config generator (Unix
# Makefiles, Ninja, MinGW Makefiles) writes `libSDL2.a`, and a multi-config one
# (Visual Studio, Xcode -- the default on a windows runner, which has VS but no
# MinGW) writes `SDL2-static.lib`. `--config Release` is passed to the build and
# install because a multi-config generator otherwise builds and installs Debug,
# whose name carries the `d` postfix (`SDL2-staticd.lib`) and would not be found.
.PHONY: sdl2-static
sdl2-static:
	@if [ -f "$(SDL2_PREFIX)/lib/libSDL2.a" ] || [ -f "$(SDL2_PREFIX)/lib/SDL2-static.lib" ]; then \
	  echo "==> static SDL2 already built ($(SDL2_PREFIX))"; \
	else \
	  echo "==> fetching SDL2 $(SDL2_VERSION)"; \
	  mkdir -p tools/SDL2-static; \
	  curl -fsSL -o tools/SDL2-static/sdl2.tar.gz "$(SDL2_TARBALL)"; \
	  tar xzf tools/SDL2-static/sdl2.tar.gz -C tools/SDL2-static; \
	  echo "==> building static SDL2 (this takes a minute)"; \
	  { cmake -S "$(SDL2_SRC)" -B tools/SDL2-static/build $(CMAKE_ARGS) \
	      -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
	      -DSDL_TEST=OFF -DSDL_TESTS=OFF \
	    && cmake --build tools/SDL2-static/build --config Release -j \
	    && cmake --install tools/SDL2-static/build --config Release --prefix "$(SDL2_PREFIX)"; \
	  } > tools/SDL2-static/build.log 2>&1 || true; \
	  if [ -f "$(SDL2_PREFIX)/lib/libSDL2.a" ] || [ -f "$(SDL2_PREFIX)/lib/SDL2-static.lib" ]; then \
	    echo "==> static SDL2 ready"; \
	  else \
	    echo "static SDL2 build failed; last 40 lines of tools/SDL2-static/build.log:"; \
	    tail -n 40 tools/SDL2-static/build.log; \
	    exit 1; \
	  fi; \
	fi

# Build (or refresh) the app binary. CMake owns its own dependency tracking.
# The app build directory is separate for the static-SDL2 link so switching
# DIST_STATIC_SDL never leaves a stale library in the cache.
ifeq ($(DIST_STATIC_SDL),1)
APP_BUILD_DIR := build-dist
# app/CMakeLists.txt locates the package config itself: its directory is
# <prefix>/cmake under MSVC and <prefix>/lib/cmake/SDL2 elsewhere.
APP_CMAKE_SDL := -DOGRE_STATIC_SDL2=ON
app: sdl2-static
else
APP_BUILD_DIR := build-app
APP_CMAKE_SDL :=
endif

# Where the compiler wrote the executable. A single-config generator writes it
# directly in APP_BUILD_DIR; a multi-config one (Visual Studio, Xcode) writes it
# in a per-config subdirectory. Recursive on purpose: the wildcard is expanded
# when the `dist` recipe runs, after `app` has built the binary.
EXE_BUILT = $(firstword $(wildcard $(APP_BUILD_DIR)/$(EXE_NAME) $(APP_BUILD_DIR)/Release/$(EXE_NAME) $(APP_BUILD_DIR)/RelWithDebInfo/$(EXE_NAME)))

.PHONY: app
app:
	@test -f $(APP_BUILD_DIR)/CMakeCache.txt || cmake -S app -B $(APP_BUILD_DIR) $(CMAKE_ARGS) -DCMAKE_BUILD_TYPE=Release $(APP_CMAKE_SDL)
	@cmake --build $(APP_BUILD_DIR) --target ogrebattle64 --config Release -j

.PHONY: dist
dist: app
	rm -rf "$(DIST_DIR)"
	mkdir -p "$(DIST_DIR)"
	@test -n "$(EXE_BUILT)" || { echo "no $(EXE_NAME) under $(APP_BUILD_DIR) after the build"; exit 1; }
	@if [ "$(DIST_OS)" = "macos" ]; then mkdir -p "$(DIST_BIN_DIR)"; fi
	cp "$(EXE_BUILT)" "$(DIST_BIN_DIR)/"
	@if [ "$(DIST_OS)" = "macos" ]; then \
	  cp "$(CURDIR)/packaging/macos-Info.plist" "$(DIST_APP)/Contents/Info.plist"; \
	  echo "==> macOS bundle: $(DIST_APP)"; \
	fi
	# RT64 loads dxcompiler.dll/dxil.dll at runtime on Windows; its CMake copies
	# them next to the library, i.e. into $(APP_BUILD_DIR)/RT64. They are not
	# optional: without them the exe fails before the first frame.
	@if [ "$(DIST_OS)" = "windows" ]; then \
	  for dll in dxcompiler.dll dxil.dll; do \
	    src=$$(ls "$(APP_BUILD_DIR)/RT64/$$dll" "$(APP_BUILD_DIR)/$$dll" "$(APP_BUILD_DIR)/Release/RT64/$$dll" 2>/dev/null | head -1); \
	    test -n "$$src" || { echo "$$dll is missing from $(APP_BUILD_DIR); RT64 copies it next to the library on Windows"; exit 1; }; \
	    cp "$$src" "$(DIST_DIR)/"; \
	  done; \
	  echo "==> bundled the RT64 DXC runtime (dxcompiler.dll, dxil.dll)"; \
	fi
	@if [ "$(DIST_OS)" = "macos" ] && [ "$(DIST_STATIC_SDL)" = "1" ]; then \
	  printf '%s\n' \
	    'This executable includes SDL2 (https://libsdl.org), linked statically,' \
	    'Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>, under the zlib' \
	    'license reproduced below.' \
	    '' > "$(DIST_DIR)/SDL2-LICENSE.txt"; \
	  cat "$(SDL2_PREFIX)/share/licenses/SDL2/LICENSE.txt" 2>/dev/null \
	    >> "$(DIST_DIR)/SDL2-LICENSE.txt" || cat "$(SDL2_SRC)/LICENSE.txt" >> "$(DIST_DIR)/SDL2-LICENSE.txt"; \
	  echo "==> static SDL2: no shared library to bundle"; \
	elif [ "$(DIST_OS)" = "macos" ]; then \
	  echo "==> bundling shared SDL2"; \
	  sdl=$$(otool -L "$(DIST_BIN_DIR)/$(EXE_NAME)" | awk '/libSDL2-2[^ ]*\.dylib/ {print $$1; exit}'); \
	  if [ -n "$$sdl" ] && [ -f "$$sdl" ]; then \
	    cp "$$sdl" "$(DIST_BIN_DIR)/"; \
	    base=$$(basename "$$sdl"); \
	    install_name_tool -change "$$sdl" "@executable_path/$$base" "$(DIST_BIN_DIR)/$(EXE_NAME)"; \
	    install_name_tool -id "@executable_path/$$base" "$(DIST_BIN_DIR)/$$base" 2>/dev/null || true; \
	    install_name_tool -add_rpath "@executable_path" "$(DIST_BIN_DIR)/$(EXE_NAME)" 2>/dev/null || true; \
	    codesign --force --sign - "$(DIST_BIN_DIR)/$$base" 2>/dev/null || true; \
	  else \
	    echo "    no SDL2 dylib found to bundle (was it linked statically?)"; \
	  fi; \
	fi
	# Ad-hoc sign the bundle last, after any nested dylib is in place. Without a
	# signature a .app downloaded from a release may refuse to launch on macOS.
	@if [ "$(DIST_OS)" = "macos" ]; then \
	  if codesign --force --sign - "$(DIST_APP)" 2>/dev/null; then \
	    echo "==> ad-hoc signed $(DIST_APP)"; \
	  else \
	    echo "    (codesign unavailable; the bundle is unsigned)"; \
	  fi; \
	fi
	@if [ "$(DIST_OS)" = "linux" ]; then \
	  if ldd "$(DIST_DIR)/$(EXE_NAME)" | grep -q libSDL2; then \
	    if command -v patchelf >/dev/null 2>&1; then \
	      echo "==> bundling SDL2"; \
	      sdl=$$(ldd "$(DIST_DIR)/$(EXE_NAME)" | awk '/libSDL2/ {print $$3; exit}'); \
	      cp "$$sdl" "$(DIST_DIR)/"; \
	      patchelf --set-rpath '$$ORIGIN' "$(DIST_DIR)/$(EXE_NAME)"; \
	    else \
	      echo "    (install patchelf to bundle SDL2, or build with DIST_STATIC_SDL=1)"; \
	    fi; \
	  else \
	    echo "==> static SDL2: no shared library to bundle"; \
	  fi; \
	fi
	@echo "==> dependencies carried in the package:"
	@if [ "$(DIST_OS)" = "macos" ]; then \
	  otool -L "$(DIST_BIN_DIR)/$(EXE_NAME)" | tail -n +2 | grep -vE "System/Library|/usr/lib" || echo "    (none: only system frameworks)"; \
	elif [ "$(DIST_OS)" = "windows" ]; then \
	  echo "    dxcompiler.dll, dxil.dll (RT64's shader compiler); SDL2 is linked statically"; \
	else \
	  ldd "$(DIST_BIN_DIR)/$(EXE_NAME)" 2>/dev/null | grep -vE "linux-vdso|libc\.so|libm\.so|libstdc\+\+|libgcc|ld-linux|libpthread|libdl|librt" || echo "    (none beyond libc)"; \
	fi
	cp "$(CURDIR)/packaging/README-dist.txt" "$(DIST_DIR)/README.txt"
	# Example mods. `make example-mods` builds them (it needs a MIPS cross
	# compiler); this target only ships what is already there, so a package
	# built without that toolchain still succeeds and says what is missing.
	@mkdir -p "$(DIST_DIR)/mods" "$(DIST_DIR)/mod_config"
	@if ls build/mods/*.nrm >/dev/null 2>&1; then \
	  cp build/mods/*.nrm "$(DIST_DIR)/mods/"; \
	  echo "==> bundled example mods:"; ls -1 "$(DIST_DIR)/mods"; \
	else \
	  echo "    no example mods in build/mods (run 'make example-mods' first)"; \
	fi
	@echo "==> $(DIST_DIR)"
	@ls -lh "$(DIST_DIR)"

# Both archive flavours: `zip` is what a Windows player expects, `tar.gz`
# always exists on Linux/macOS runners. CI uploads whichever each runner makes.
# Git Bash for Windows ships no `zip`, so fall back to CMake's zip writer, which
# is present on every runner that builds this.
.PHONY: dist-zip
dist-zip: dist
	@cd dist && rm -f "$(DIST_NAME)-$(DIST_OS).zip" && \
	  if command -v zip >/dev/null 2>&1; then \
	    zip -q -r "$(DIST_NAME)-$(DIST_OS).zip" "$(DIST_NAME)"; \
	  else \
	    cmake -E tar cf "$(DIST_NAME)-$(DIST_OS).zip" --format=zip "$(DIST_NAME)"; \
	  fi && \
	  echo "==> dist/$(DIST_NAME)-$(DIST_OS).zip"

.PHONY: dist-tar
dist-tar: dist
	@cd dist && tar czf "$(DIST_NAME)-$(DIST_OS).tar.gz" "$(DIST_NAME)" && \
	  echo "==> dist/$(DIST_NAME)-$(DIST_OS).tar.gz"

.PHONY: all clean recomp recomp-prep regenerate cross-bank-report cross-bank-dispatch cross-bank-check bank-split bank-recomp handoffs midfunc rsp-recomp stubmap stub-check app dist dist-zip dist-tar sdl2-static
