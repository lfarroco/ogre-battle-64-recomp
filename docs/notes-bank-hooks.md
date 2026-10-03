# Hooks on streamed-overlay bank functions

Read-only investigation. No game run, no probe, no tracked file changed.

Goal: what has to change so that a code mod can write
`RECOMP_HOOK("func_ovlN_801DC740")` and have the hook resolve, where that
function lives in a streamed-overlay bank unit (`BankNFuncs/`, ROM `0x101D00` ->
RAM `0x801AD5C0`). That function is outside the base ELF's sections.

Every claim is marked **Proven** (read from a file in this tree at the line
given) or **Inference** (a conclusion drawn from those reads).

## 0. The concrete target

**Proven.** `BankNFuncs/recomp_overlays.inl:160` holds
`{ .func = func_ovlN_801DC740, .offset = 0x0002F180, .rom_size = 0x000001F0 }`.
`0x801AD5C0 + 0x2F180 = 0x801DC740`, so the function's ROM address is
`0x101D00 + 0x2F180 = 0x130E80`.

**Proven.** `BankNFuncs/recomp_overlays.inl:494` is the record's section entry:

```
{ .rom_addr = 0x00101D00, .ram_addr = 0x801AD5C0, .size = 0x00043530, .funcs = section_2_bankRec7_funcs, .num_funcs = ARRLEN(section_2_bankRec7_funcs), .relocs = nullptr, .num_relocs = 0, .index = 2 },
```

**Proven.** The base ELF's section table is 5 code sections and
`const size_t num_sections = 23` (`RecompiledFuncs/recomp_overlays.inl`, tail).
The main unit's code sections carry relocs; every bank unit's entries read
`.relocs = nullptr, .num_relocs = 0`.

**Proven.** `app/src/bank_funcs.inc` flattens 34 bank units into 44 records. It
is registered by `app/src/bank_overlays.cpp:848 register_bank_overlays()`, which
runs from the `on_init_callback` at `app/src/main.cpp:344-347`. That callback
runs after `init_overlays` (`tools/N64ModernRuntime/librecomp/src/recomp.cpp:540`)
and before `load_mods` (`recomp.cpp:843`).

## 1. What RecompModTool records in a `Hook`

**Proven.** `tools/N64Recomp/RecompModTool/main.cpp:736-770`:

```cpp
if (hook_section || hook_return_section) {
    // Get the name of the hooked function.
    size_t section_prefix_length = hook_section ? N64Recomp::HookSectionPrefix.size() : N64Recomp::HookReturnSectionPrefix.size();
    std::string hooked_function_name = cur_section.name.substr(section_prefix_length);

    // Find the corresponding symbol in the reference symbols.
    N64Recomp::SymbolReference cur_reference;
    bool original_func_exists = input_context.find_regular_reference_symbol(hooked_function_name, cur_reference);

    // Check that the function being patched exists in the original reference symbols.
    if (!original_func_exists) {
        fmt::print(stderr, "Function {} hooks a function ({}) that doesn't exist in the original ROM.\n", cur_func.name, hooked_function_name);
        return {};
    }

    // Check that the reference symbol is actually a function.
    const auto& reference_symbol = input_context.get_reference_symbol(cur_reference);
    if (!reference_symbol.is_function) {
        fmt::print(stderr, "Function {0} hooks {1}, but {1} was a variable in the original ROM.\n", cur_func.name, hooked_function_name);
        return {};
    }

    uint32_t reference_section_vram = input_context.get_reference_section_vram(reference_symbol.section_index);
    uint32_t reference_section_rom = input_context.get_reference_section_rom(reference_symbol.section_index);

    // Add a replacement for this function to the output context.
    ret.hooks.emplace_back(
        N64Recomp::FunctionHook {
            .func_index = (uint32_t)output_func_index,
            .original_section_vrom = reference_section_rom,
            .original_vram = reference_section_vram + reference_symbol.section_offset,
            .flags = hook_return_section ? N64Recomp::HookFlags::AtReturn : N64Recomp::HookFlags{}
        }
    );
}
```

**Proven.** `N64Recomp::FunctionHook` has exactly four fields
(`tools/N64Recomp/include/recompiler/context.h:211-216`): `func_index`,
`original_section_vrom`, `original_vram`, `flags`.

Field origins:

- `func_index` is the index of the mod's own recompiled hook function in the
  mod's output context.
- `original_section_vrom` is `get_reference_section_rom(reference_symbol.section_index)`
  (`context.h:580-590`), which returns
  `reference_sections[section_index].rom_addr`. That value is the `rom` key of a
  `[[section]]` table in the reference dump.
- `original_vram` is `get_reference_section_vram(...)` plus
  `reference_symbol.section_offset` (`context.h:568-578`). `section_offset` is
  `function.vram - section.ram_addr` (`config.cpp:468`), so the sum is the
  function's own `vram` key.
- `flags` comes from the section name prefix: `.recomp_hook.` gives
  `HookFlags{}`, `.recomp_hook_return.` gives `AtReturn`
  (`context.h:95-96`, `main.cpp:738`).

**Proven.** The name lookup is `find_regular_reference_symbol`
(`context.h:395-409`), which reads `reference_symbols_by_name`. That map is
filled only by `Context::import_reference_context` (`config.cpp:718-741`), and
`import_reference_context` sets `is_function = true` only for entries of the
reference context's `functions` vector (`config.cpp:734-738`). Those entries come
from each `[[section]].functions[]` array (`config.cpp:604`, the
`cur_functions->for_each` lambda).

**Proven.** The hook is packaged only when all of these hold:

1. The name `func_ovlN_801DC740` appears in the reference dump's
   `[[section]].functions[]` list, with its true `vram`.
2. That same `[[section]]` carries `rom`, `vram`, `size`, and `name`.
3. The section's `vram` is the bank record's RAM base in the dump, so
   `function.vram - section.ram_addr` equals `0x2F180`.

A data reference dump cannot supply this. **Proven.**
`read_data_reference_syms` calls `add_reference_symbol(..., false)`
(`config.cpp:824`), so the symbol is not a function and `main.cpp:753` rejects
it. That function also maps a section only when the section's `rom` is already
in `reference_sections`, and otherwise uses `SectionAbsolute`
(`config.cpp:780-789`). **Proven.** `is_regular_reference_section` accepts
`SectionAbsolute` (`context.h:373-375`), so the name is found and then fails the
`is_function` test.

**One reference dump only.** **Proven.**
`tools/N64Recomp/RecompModTool/main.cpp:407-409` reads
`inputs_table["func_reference_syms_file"]` as one string, and
`main.cpp:1173-1184` calls `from_symbol_file` once and
`import_reference_context` once. `data_reference_syms_files` is an array
(`main.cpp:416-419`), and it covers data symbols only. **Proven.**
`import_reference_context` begins with
`reference_sections.resize(reference_context.sections.size())`
(`config.cpp:719`), which replaces the section list. A second call with another
ELF's context drops the first ELF's sections while `reference_symbols` keeps its
`reserve`d entries, and `reference_symbols_by_name` uses `emplace` so the first
name wins (`config.cpp:460`). **Proven.** `RecompModMerger/main.cpp:282` and
`OfflineModRecomp/main.cpp:74` also import one context.

**Merging several ELFs is possible outside the tool.** **Proven.**
`from_symbol_file` assigns each section the next positional index
(`config.cpp:578 uint16_t section_index = (uint16_t)ret.sections.size();`), and
`add_reference_symbol` stores the function's offset relative to its own section
(`config.cpp:445-471`). Concatenating the `[[section]]` blocks of two dumps into
one file therefore keeps both sets valid, and the section indices of the first
dump do not change. **Inference.** No code in this tree does that concatenation
today; `make mod-syms` (`Makefile:262-265`) runs
`N64Recomp ../../config/config.toml --dump-context` once in `mods/reference`,
and `dump_context` writes the fixed names `dump.toml`/`data_dump.toml` in the
current directory (`tools/N64Recomp/src/main.cpp:432`).

**Proven.** `mods/reference/dump.toml` has exactly 5 `[[section]]` entries
(`.entry`, `.main`, `.streamedA`, `.streamedB`, `.streamedC`) and no
`func_ovlN_*` name.

## 2. Runtime trace of `build_regen_list` for a bank hook

**Proven.** `load_mods` takes the section map at
`tools/N64ModernRuntime/librecomp/src/mods.cpp:1686`:

```cpp
const std::unordered_map<uint32_t, uint16_t>& section_vrom_map = recomp::overlays::get_vrom_to_section_map();
```

**Proven.** `init_overlays` fills that map
(`tools/N64ModernRuntime/librecomp/src/overlays.cpp:612-614`):

```cpp
section_addresses[sections_info.code_sections[section_index].index] = code_section->ram_addr;
code_sections_by_rom[code_section->rom_addr] = section_index;
```

Key = the section's ROM address. Value = the section's position in the sorted
`sections_info.code_sections` array. The array holds the 5 main code sections.
`0x101D00` is absent from the map.

**Proven.** The hook's `section_rom` is carried in the packaged mod's symbol
file and read back without a name lookup
(`tools/N64Recomp/src/mod_symbols.cpp:464-477`,
`original_section_vrom = hook_in.original_section_vrom`). `load_mod_code`
copies it into `HookDefinition.section_rom`
(`mods.cpp:2377-2383`). `HookDefinition` is
`{ uint32_t section_rom; uint32_t function_vram; bool at_return; }`
(`librecomp/mods.hpp:40-45`).

**Proven.** `build_regen_list` then fails at the first step,
`mods.cpp:1934-1946`:

```cpp
if (cur_hook_def.section_rom != cur_section_rom) {
    // Get the index of the section.
    auto find_section_it = section_vrom_map.find(cur_hook_def.section_rom);
    if (find_section_it == section_vrom_map.end()) {
        std::stringstream error_param_stream{};
        error_param_stream << std::hex <<
            "section: 0x" << cur_hook_def.section_rom <<
            " func: 0x" << std::setfill('0') << std::setw(8) << cur_hook_def.function_vram;
        ret.emplace_back(ModLoadErrorDetails{
            "", ModLoadError::FailedToLoadCode, error_to_string(CodeModLoadError::InvalidHook) + ":" + error_param_stream.str()
        });
        return ret;
    }
```

Result today: `FailedToLoadCode:InvalidHook:section: 0x101d00 func: 0x801dc740`.
`regenerate_with_hooks` returns that vector (`mods.cpp:2153-2157`),
`load_mods` calls `unload_mods()` (`mods.cpp:1891-1896`), and the mod is
reported as failed. No further accessor runs, so the code paths below are
hypothetical for this hook.

### Every accessor that would run next, and what each would return

Assume the map lookup succeeded and returned the bank record's own ELF section
number as the value. Bank unit N numbers its sections 2, 5 and 8; the record
ROM `0x101D00` is section 2.

1. `recomp::overlays::get_section_ram_addr(section_index)`
   (`mods.cpp:1956`, body `overlays.cpp:164-166`):
   `return sections_info.code_sections[code_section_index].ram_addr;`.
   There is no bounds check. **Proven.** `init_overlays` sorts
   `sections_info.code_sections` by ROM address before filling the map
   (`overlays.cpp:604-610`), so position 2 is `.streamedA`
   (`RecompiledFuncs/recomp_overlays.inl:26003`,
   `{ .rom_addr = 0x0003F1B0, .ram_addr = 0x800E9C20, .size = 0x00001CD0, ... .index = 9 }`).
   The call therefore returns `0x800E9C20`, and the hook's function offset becomes
   `0x801DC740 - 0x800E9C20 = 0x12B20`.
2. `recomp::overlays::get_section_relocs(section_index)`
   (`mods.cpp:1957`, body `overlays.cpp:168-175`):

   ```cpp
   std::span<const RelocEntry> recomp::overlays::get_section_relocs(uint16_t code_section_index) {
       if (code_section_index < sections_info.num_code_sections) {
           const auto& section = sections_info.code_sections[code_section_index];
           return std::span{ section.relocs, section.num_relocs };
       }
       assert(false);
       return {};
   }
   ```

   Index 2 is below `num_code_sections` (5), so it returns `.streamedA`'s reloc
   span, which is non-empty (`section_9_streamedA_relocs`,
   `RecompiledFuncs/recomp_overlays.inl:10234`). An index at or above 5 takes the
   `assert(false)` branch at `overlays.cpp:173`. **Inference.** The default app
   build is CMake `Release` (`app/CMakeLists.txt:8`), which defines `NDEBUG` and
   compiles the assert out, so that branch returns an empty span.
3. `recomp::overlays::get_func_entry_by_section_index_function_offset(section_index, function_offset, func_entry)`
   (`mods.cpp:1991`, body `overlays.cpp:621-643`):

   ```cpp
   if (code_section_index >= sections_info.num_code_sections) {
       return false;
   }

   SectionTableEntry* section = &sections_info.code_sections[code_section_index];
   if (function_offset >= section->size) {
       return false;
   }
   ```

   With index 2 and offset `0x12B20`, `0x12B20 >= 0x1CD0`, so it returns `false`.
   `mods.cpp:1994-2002` then reports `FailedToLoadCode:InvalidHook` again. With
   an index at or above 5 it returns `false` at the first check.

### Every place a `section_vrom_map` value indexes `sections_info.code_sections` or `section_addresses`

**Proven** list:

1. `mods.cpp:1956` -> `get_section_ram_addr` (`overlays.cpp:164-166`) ->
   `sections_info.code_sections[code_section_index]`.
2. `mods.cpp:1957` -> `get_section_relocs` (`overlays.cpp:168-175`) ->
   `sections_info.code_sections[code_section_index]`, with a bound against
   `num_code_sections` and an `assert` for larger values.
3. `mods.cpp:1991` -> `get_func_entry_by_section_index_function_offset`
   (`overlays.cpp:621-643`) -> `sections_info.code_sections[code_section_index]`,
   with a bound against `num_code_sections`.
4. `mods.cpp:1964 .original_index = section_index` -> `mods.cpp:2117-2121`
   (`original_section_indices[new_section_index] = ...original_index`) ->
   `LiveRecompilerCodeHandle` (`mods.cpp:2123-2124`) ->
   `live_generator.cpp:1619 reloc_section_index = inputs.original_section_indices[reloc_section_index]`
   for a jump table, then `live_generator.cpp:881`
   `int32_t* section_addr_ptr = (... ? inputs.reference_section_addresses : inputs.local_section_addresses) + ctx.reloc_section_index;`
   which indexes the global `section_addresses`.
   `handle_inputs.reference_section_addresses = section_addresses`
   (`mods.cpp:2114`), declared at `overlays.cpp:74` and allocated at
   `overlays.cpp:601`.
5. `mods.cpp:2153 build_regen_list<false>(..., section_vrom_map, ...)`, and
   `mods.cpp:1789 init_mod_code(rdram, section_vrom_map, ...)` ->
   `mods.cpp:2275 parse_mod_symbols(syms_data, binary_span, section_vrom_map, *mod.recompiler_context)`
   -> `mod_symbols.cpp:267-276`, which resolves a mod's own relocation target
   section by ROM address and stores the map value as
   `cur_reloc.target_section`. That value is later passed to
   `get_func_by_section_index_function_offset` (`mods.cpp:502` in
   `DynamicLibraryCodeHandle::populate_reference_symbols`, and `mods.cpp:579` in
   the live handle).

No other reader of `section_vrom_map` exists in the tree. **Proven** by
`grep -rn section_vrom_map tools/N64ModernRuntime`.

## 3. What the regenerated code needs besides the function's ROM bytes

### The regenerated context's reloc content

**Proven.** `context_from_regenerated_list` (`mods.cpp:1379-1500`) builds one
`N64Recomp::Section` per `RegeneratedSection`. Its relocs come only from the
regenlist, which `build_regen_list` filled from the section table
(`mods.cpp:2066-2071`):

```cpp
regenlist.relocs.emplace_back(RegeneratedReloc {
    .section_offset = reloc_in.offset,
    .target_section = reloc_in.target_section,
    .target_section_offset = reloc_in.target_section_offset,
    .type = reloc_in.type
});
```

**Proven.** For a bank record `cur_section_relocs` is empty
(`BankNFuncs/recomp_overlays.inl:494 .relocs = nullptr, .num_relocs = 0`), so
this loop adds nothing, and
`mods.cpp:1968 .relocatable = !patched_regenlist && !cur_section_relocs.empty()`
is `false`. `mods.cpp:1409 section_out.relocs.resize(cur_num_relocs)` then
resizes to 0, and `mods.cpp:1413` copies `relocatable = false` into the
regenerated context.

**Proven.** The function's words are read straight from the ROM
(`mods.cpp:1428`, `mods.cpp:1438-1439`):

```cpp
function_out.rom = section_out.rom_addr + function_in.section_offset;
...
const uint32_t* func_words = reinterpret_cast<const uint32_t*>(rom.data() + function_out.rom);
function_out.words.assign(func_words, func_words + function_in.size / sizeof(uint32_t));
```

`rom` is the decompressed ROM span passed from `load_mods`
(`mods.cpp:1660-1677`, `mods.cpp:1891`). For this function the address is
`0x101D00 + 0x2F180 = 0x130E80`, inside the ROM. No accessor fails here.

### Reloc use in the live recompiler

**Proven.** `recompilation.cpp:206-208` tests the reloc list before indexing it:

```cpp
// Check if this instruction has a reloc.
if (section.relocs.size() > 0 && section.relocs[reloc_index].address == instr_vram) {
```

An empty list short-circuits, so `has_reloc` stays false and the instruction is
emitted from its own immediate.

**Proven.** The two places that read `section_addresses` are the reloc path
(`live_generator.cpp:878-891 load_relocated_address`, reached from
`instruction_context.reloc_section_index = reloc_section` at
`recompilation.cpp:889`) and the jump-table path
(`live_generator.cpp:1609-1626`):

```cpp
const auto& jtbl_section = recompiler_context.sections[jtbl.section_index];
if (jtbl_section.relocatable) {
    ...
    load_relocated_address(dummy_context, Registers::arithmetic_temp2);
    ...
}
else {
    sljit_emit_op2(compiler, SLJIT_SUB, Registers::arithmetic_temp1, 0, SLJIT_MEM1(...), SLJIT_IMM, (sljit_sw)((int32_t)jtbl.vram));
}
```

With no relocs and `relocatable = false`, the reloc path is never entered and
the jump-table path uses the absolute `jtbl.vram`. `section_addresses` is not
indexed for a bank section in either path.

**Proven.** Jump-table detection is instruction-based. `analysis.cpp:222-247`
records a table when a `jr` reads a register with a loaded base, and
`analysis.cpp:306-347` computes entries from `func.rom - func.vram`. For a bank
record this delta is `0x101D00 - 0x801AD5C0`, the record's own ROM-to-RAM
delta. No reloc is consulted.

**Proven.** Every call in the regenerated code becomes a runtime lookup.
`apply_regenlist` sets `hook_context.use_lookup_for_all_function_calls = true`
(`mods.cpp:2103`). `resolve_jal` returns `Ambiguous` immediately in that mode
(`recompilation.cpp:54-56`), and the emission is
`generator.emit_function_call_lookup(target_func_vram)`
(`recompilation.cpp:418-420`). The live generator loads the absolute vram and
calls `get_function` (`live_generator.cpp:1371-1386`), which reads the runtime
`func_map` (`overlays.cpp:710-714`). `load_function_bank` fills that map with
the bank unit's function pointers (`overlays.cpp:375-381`).

**Proven.** `context_from_regenerated_list` sets `section_out.size = 0`
(`mods.cpp:1406`). Nothing in this path depends on that size: calls are lookups,
jump tables use `func.rom - func.vram` and `func.words.size()`, and data
references use the instruction immediates.

### Why the bank unit's build-time recompilation works with no relocs

**Proven.** The bank code's immediates already hold the final absolute
addresses. `BankNFuncs/funcs_2.c:27297-27300`:

```c
// 0x801DC760: lui         $v1, 0x8019
ctx->r3 = S32(0X8019 << 16);
// 0x801DC764: addiu       $v1, $v1, 0x6B23
ctx->r3 = ADD32(ctx->r3, 0X6B23);
```

The main unit's generated C carries the relocation macros, for example
`RELOC_HI16` in `RecompiledFuncs/funcs_0.c`.

**Proven.** `config/banks/config-bankA.toml:5-8` states the design:

```
# Their sections are linked at their true RAM addresses and are NOT relocatable
# (the game pre-links them with absolute pointers), so no
# relocatable_sections_path is given: every reference is emitted as an absolute
# address, which is exactly what the game expects.
```

`config/banks/config-bankN.toml` has no `relocatable_sections_path` either.

**Proven.** The splat option `reloc_addrs_path` is identical in both configs
(`config/config.yaml:30-31`, `config/banks/config-bankN.yaml:61-62`). It only
controls which addresses splat emits as relocations in the disassembly. The
N64Recomp option that decides whether those relocations are read is
`relocatable_sections_path` (`tools/N64Recomp/src/config.cpp:339-344`), and only
the main unit's `.toml` sets it.

**Proven.** The bank ELF does carry relocation sections. `readelf -S
build/bankN.elf` lists `.rel.bankRec7` (offset `0x1cebb4`, size `0x1e1e8`).
N64Recomp never reads them, because `relocatable_sections` is empty and
`context.has_reference_symbols()` is false. `elf.cpp:378`:

```cpp
bool is_relocatable = section_out.relocatable || context.has_reference_symbols();
```

`section_out.relocatable` is false (`elf.cpp:303` tests
`elf_config.all_sections_relocatable || elf_config.relocatable_sections.contains(section_name)`,
both false), and `has_reference_symbols()` is false because
`config/config.toml` and every `config/banks/config-bank*.toml` omit
`func_reference_syms_file` (read at `tools/N64Recomp/src/main.cpp:367-386`).
The `.rel.*` sections are therefore skipped at `elf.cpp:379-386`.

The build works because the game DMA's each record to the same RAM base the
linker used, so every immediate stays valid.

**Conclusion. Proven.** The regenerated bank function needs the ROM bytes and
its section's RAM base. It needs no relocs. `.num_relocs = 0` is the correct
input, and `apply_regenlist` regenerates the function from the raw ROM words.

**Proven.** A bank reloc's `target_section` uses the bank unit's own
ELF numbering (`BankNFuncs` numbers its code sections 2, 5 and 8).
`get_code_section_index_from_section_index` (`mods.cpp:1470-1477`) scans only
`sections_info.code_sections`, so a bank-section target finds no main code
section. **Proven.** A HI16/LO16 pair in that state is turned into `R_MIPS_NONE`
(`mods.cpp:1490-1493`), and an `R_MIPS_26` becomes a reference-symbol jump whose
target section reaches `get_func_by_section_index_function_offset` in
`populate_reference_symbols` (`mods.cpp:579`). **Proven.** That lookup returns
false for an index at or above `sections_info.num_code_sections`
(`overlays.cpp:621-624`), which produces `InvalidReferenceSymbol`. **Inference.**
Adding relocs to a bank section would therefore not help.

**Inference (separate observation about the existing mod path).** Two numberings
exist in the mod path. **Proven.** A mod relocation to a game section is keyed by
ROM address at load time and stored as `sections_by_vrom.find(vrom)->second`
(`mod_symbols.cpp:265-277`), which is the position in
`sections_info.code_sections` because that map is filled that way
(`overlays.cpp:614`). **Proven.** `section_addresses` is written at
`SectionTableEntry.index` instead (`overlays.cpp:613`, `overlays.cpp:190`, and
`overlays.cpp:292`/`overlays.cpp:346` in the unload paths), and
`RELOC_HI16(section_index, offset)` reads `section_addresses[section_index]`
(`tools/N64ModernRuntime/N64Recomp/include/recomp.h:502-506`). For this project
the positions are 0-4 and the indexes are 3/6/9/12/16, so the two numberings
agree nowhere except position 3, which holds `.entry`'s address.
**Proven.** The one hook that exists today,
`RECOMP_HOOK("func_80072944")` in `mods/item-randomizer/src/item_randomizer.c:520`,
has four relocs in its range (`.offset = 0x1CF0/0x1CF8/0x1CFC/0x1D00`), and its
HI16/LO16 pair targets section 8 (`RecompiledFuncs/recomp_overlays.inl`,
`section_6_main_relocs`). Section 8 holds no code in the main table, so
`get_code_section_index_from_section_index` returns false and
`mods.cpp:1490-1493` turns the pair into `R_MIPS_NONE`, which keeps the
instruction immediate. **Proven.** HI16/LO16 relocs that target main *code*
sections do exist in the table (section 6: 878 and 881, section 9: 134 and 134,
section 12: 443 and 443, section 16: 660 and 660, counted from
`RecompiledFuncs/recomp_overlays.inl`). **Inference.** A reloc of that kind whose
target is a main code section would reach `load_relocated_address` with the
position and read `section_addresses[position]`. I did not run the game, so I do
not know whether any shipped recipe reaches that case. It does not affect a bank
hook while the bank section's reloc count stays 0.

## 4. The smallest set of changes for an end-to-end bank hook

### (a) Build-time and tooling

**A1. Emit per-bank reference dumps and merge them.** File: `Makefile`
(target `mod-syms`, lines 262-265) plus a new tracked script
`tools/merge_mod_syms.py`. The target runs
`N64Recomp config/banks/config-bank<U>.toml --dump-context` for each unit in a
temporary directory, then concatenates those `[[section]]` blocks onto the main
dump's blocks. The merged file replaces `mods/reference/dump.toml`. The target
must depend on `build/bank<U>.elf`, which `make bank-recomp` writes
(`Makefile:380`). This needs no change inside a gitignored tree.

**A2. Alternative in the vendored tool (larger).** File:
`tools/N64Recomp/RecompModTool/main.cpp` and `tools/N64Recomp/src/config.cpp`.
Accept a `func_reference_syms_files` array next to the existing
`func_reference_syms_file` (`main.cpp:407-409`, `config.cpp:493-498`), and make
`Context::import_reference_context` (`config.cpp:718-741`) append to the section
list. The current body resizes that list on its first line. This lives in a gitignored tree, so `patches/n64recomp-ob64.patch`
must be regenerated (see the patch note below).

**A3.** Nothing else at build time. `RECOMP_HOOK` takes the name as a string, so
the mod's own ELF needs no symbol for `func_ovlN_801DC740`. **Proven.**
`main.cpp:739` takes the name from the section name
(`hooked_function_name = cur_section.name.substr(section_prefix_length)`), and
`main.cpp:743` resolves it in the reference context.

### (b) Offline reference symbols

**B1.** The merged dump must contain, for each bank record that a mod may hook,
a `[[section]]` whose `rom`, `vram` and `size` equal the fields in that unit's
`recomp_overlays.inl` section table, and a `functions[]` entry per hookable
function with the exact `vram`. For unit N that is
`.rom = 0x101D00, .vram = 0x801AD5C0, .size = 0x43530`, plus
`{ name = "func_ovlN_801DC740", vram = 0x801DC740, size = 0x1F0 }`
(`BankNFuncs/recomp_overlays.inl:160,494`). `N64Recomp --dump-context` writes
exactly this shape: `print_section` emits `rom`, `vram`, `size`
(`tools/N64Recomp/src/main.cpp:175-195`) and the functions array is emitted from
`section_functions` (`main.cpp:235-240`). Sections with no functions are not
dumped (`main.cpp:200 if (!section_funcs.empty())`), so a bank unit's bss
sections are absent, which is required because `from_symbol_file` demands `rom`
on every section (`config.cpp:578-585`).

**B2.** The dump must be regenerated whenever a bank record's size or a
function's address moves. The current `mods/reference/dump.toml` is gitignored
(`.gitignore:31 mods/reference/*.toml`) and is a prerequisite of
`make example-mods` (`Makefile:268`).

### (c) Runtime `librecomp`

The bank records are absent from `sections_info`, and `sections_info` also drives
the DMA-driven loader. The smallest change therefore keeps a second table for the
hook path.

**C1. Add a hook-section registry.** Files:
`tools/N64ModernRuntime/librecomp/include/librecomp/overlays.hpp` and
`tools/N64ModernRuntime/librecomp/src/overlays.cpp`. Add a struct holding
`{ rom_addr, ram_addr, size, funcs, num_funcs }` and a function such as
`register_bank_hook_sections(const BankSectionEntry* sections, size_t count)`,
with a `rom -> entry` map. Add lookups that return the record's RAM base and a
`FuncEntry` for a section offset. Keep this separate from
`sections_info.code_sections` so that `load_overlays` and `notify_rom_read` do
not see it.

**C2. Consult the registry in `build_regen_list`.** File:
`tools/N64ModernRuntime/librecomp/src/mods.cpp`, function `build_regen_list`,
lines 1934-1992. When `section_vrom_map.find(cur_hook_def.section_rom)` misses,
look the ROM address up in the hook-section registry. Take `section_ram_addr`
from the record, leave `cur_section_relocs` empty, and resolve the `FuncEntry`
from the record's own table. Do not pass the bank section index to
`get_section_ram_addr`, `get_section_relocs` or
`get_func_entry_by_section_index_function_offset`. The rest of the function needs
only `func_entry.func` and `func_entry.rom_size` (`mods.cpp:2005` and
`mods.cpp:2066-2071`,
`mods.cpp:2129-2135`).

**C3. Carry `rom_size` on the bank function entries.** Files:
`tools/N64ModernRuntime/librecomp/include/librecomp/overlays.hpp`
(`BankFunctionEntry`, lines 71-74, gains a `rom_size` field) and
`tools/gen_bank_funcs.py` (tracked). `parse_func_arrays` (line 51) already
matches `rom_size` in `FUNC_ENTRY_RE` but discards it; the emitted table at
lines 171-175 must include it. `build_regen_list` needs that value at
`mods.cpp:2005 uint32_t function_rom_size = func_entry.rom_size;`.

**C4. Register the table at startup.** File: `app/src/bank_overlays.cpp`
(tracked), function `register_bank_overlays` (line 848). Call
`recomp::overlays::register_bank_hook_sections(...)` with the `kBankRecords`
array from `app/src/bank_funcs.inc`. The callback already runs after
`init_overlays` and before `load_mods` (`app/src/main.cpp:344-347`,
`recomp.cpp:540`, `recomp.cpp:843`), so the registry is populated in time.

**C5 (only if a mod must relocate against a bank section).** Extend the map
handed to `parse_mod_symbols` (`mods.cpp:2275`) with the bank records' ROM
addresses. The values must be indexes that
`get_func_by_section_index_function_offset` resolves. This is not needed for
`RECOMP_HOOK` alone.

### Patch and rebuild notes

**Proven.** `tools/N64ModernRuntime/` and `tools/N64Recomp/` are gitignored
(`.gitignore:36-37`). The tracked copies of local changes to them are
`patches/n64modernruntime-ob64.patch` and `patches/n64recomp-ob64.patch`
(`tools/patchcheck.py`, `TARGETS`). `librecomp/src/mods.cpp` and
`librecomp/src/overlays.cpp` appear in `patches/n64modernruntime-ob64.patch`;
`RecompModTool/main.cpp`, `src/config.cpp` and `src/elf.cpp` appear in
`patches/n64recomp-ob64.patch`. Changes C1, C2 and C3's header edit therefore
need `tools/patchcheck.py --fix` (`make patch-fix`) and a passing
`tools/patchcheck.py` (`make patch-check`), which `docs/guides/app-build.md`
around lines 560-600 describes as the hosted path's contract.

**Proven (tracked files).** `tools/gen_bank_funcs.py`,
`app/src/bank_overlays.cpp`, `app/src/bank_overlays.hpp`, `Makefile`, a new
`tools/merge_mod_syms.py`, and this note. `app/src/bank_funcs.inc`,
`Bank*Funcs/`, `RecompiledFuncs/` and `mods/reference/*.toml` are generated and
gitignored (`.gitignore:14,18,19,31`).

## 5. Risks of putting bank sections in the runtime section map

**R1. The DMA scan would pick bank sections up.** **Proven.**
`notify_rom_read` (`overlays.cpp:562-593`) loops over every
`sections_info.code_sections` entry, skips RAM bases below
`kFirstStreamedRamAddr = 0x800E0000` (`overlays.cpp:309`), matches the DMA's ROM
range and its constant ROM-to-RAM delta, then calls
`unload_overlapping_overlays` and `load_overlay` (`overlays.cpp:182-191`).
`load_overlay` writes `section_addresses[section.index] = ram` and appends to
`loaded_sections`. **Proven.** `notify_rom_read` is reached from every ROM PI DMA
through `recomp::do_rom_read` (`tools/N64ModernRuntime/librecomp/src/pi.cpp:240-250`).
So every bank record the game DMA's would be loaded through the main table in
addition to `on_streamed_dma`.

**Proven.** In this tree `load_overlays` (`overlays.cpp:216-240`) runs at boot
only: `recomp.cpp:550` for entry+main and `app/src/overlays.cpp:51-53` for the
three boot streamed overlays. The per-DMA entry point is `notify_rom_read`.

**R2. `load_overlays` would compute wrong load addresses.** **Proven.**
`load_overlays` loads each section it finds inside the requested ROM range at
`it->rom_addr - rom + ram_addr` (`overlays.cpp:235`). A bank record whose whole
ROM extent lies inside one of the boot ranges would be registered at that
computed RAM address, which is its RAM base only if the boot range is the DMA
that carries it. **Proven for the current tables.** No bank record satisfies the
inclusion test today: the candidate boundaries are unit E record 0 at ROM
`0x066E30`, which equals the end of the overlay B range (`0x40E80 + 0x25FB0`),
and unit C record 10 at ROM `0x1F0A00` inside the overlay C range
(`0x1CE040 + 0x22A00 = 0x1F0A40`). Both extend past the query end, so
`std::upper_bound` (`overlays.cpp:222-227`) stops at the first of them and
neither is iterated. **Inference.** A record that lies wholly inside a boot range
would be registered at the wrong address.

**R3. `section_addresses` is too small and its indexes collide.** **Proven.**
`section_addresses = (int32_t *)calloc(sections_info.total_num_sections, sizeof(int32_t))`
(`overlays.cpp:601`) with `total_num_sections = 23` for the main unit. Bank units
use their own ELF's section numbering: `BankCFuncs/recomp_overlays.inl:1041` is
`.index = 24` for bankRec14, and that unit's own
`const size_t num_sections = 31` (line 1043). Index 24 is outside a 23-entry
allocation. `BankCFuncs/recomp_overlays.inl:1036` is `.index = 6`, which equals
the main `.main` index 6, so `section_addresses[6]` would be overwritten with
`0x801AD5C0`. The main unit's recompiled code reads `section_addresses[6]`
through `RELOC_HI16(6, ...)`/`RELOC_LO16(6, ...)` (`RecompiledFuncs/funcs_*.c`).
`BankNFuncs/recomp_overlays.inl:494-496` uses indexes 2, 5 and 8, which are
unused by the main table and would be in range. **Proven.** Any merged table must
renumber `.index` to unique values and set `total_num_sections` above the
maximum.

**R4. `get_section_relocs` assert.** **Proven.** An index at or above
`num_code_sections` reaches `assert(false)` (`overlays.cpp:173`) and then returns
`{}`. A build with `NDEBUG` returns no relocs silently. **Inference.** That would
turn a bank hook into a hook on an unrelated main section and regenerate the
wrong bytes.

**R5. The two loader paths would overlap.** **Proven.** `load_function_bank`
starts with `unload_overlapping_overlays(ram_addr, size)` (`overlays.cpp:376`).
When the game's DMA of a bank record also matched a bank section in
`sections_info`, `notify_rom_read` would add the entry and then
`on_streamed_dma`/`load_function_bank` (`app/src/bank_overlays.cpp:753`) would
remove it and re-add the same `func_map` pointers from `record.funcs`. The final
function map is unchanged. `section_addresses[bank index]` is written twice and
`loaded_sections` churns.

**R6. The 89 dispatch sites are not affected at build time.** **Proven.**
`$N64RECOMP` is `tools/N64Recomp/build/N64Recomp` (`Makefile:16`), and
`make recomp` runs `tools/cross_bank.py dispatch --only <16 addresses>`
(`Makefile:81-82`), which rewrites generated main-unit C. **Proven.**
AGENTS.md records the full dispatch as 89 sites and 7 extra targets.
`cross_bank.py check` on this tree reports 951 direct calls into swappable RAM
across 236 targets in the main unit, 0 outside their own record in the bank
units, and 24 accepted pre-existing hazards. **Inference.** The regenerated hook code resolves its calls through
`get_function` (`live_generator.cpp:1371-1386`), so a wrong registration shows up
as a stub call at run time. The build stays green.

**R7. A wrong registration is silent.** **Proven.** `get_function`
(`overlays.cpp:710-735`) returns `streamed_stub_generic` for any address in
`0x8016A000..0x80400000` or `0x84000000..0x84200000` that is not in `func_map`.
Every bank record's RAM base is in the first range.

**R8. Reload and checkpoint state.** **Proven.**
`restore_overlay_state_blob` (`overlays.cpp:493-554`) rebuilds `func_map` from
the serialized `loaded_sections` list, bounded by
`sections_info.num_code_sections` (line 528), and from the
`loaded_function_banks` list. Bank sections added to `sections_info` would enter
the serialized section count as well. A checkpoint stays valid only within the
run that wrote it (comment at `overlays.cpp:400-415`).

## 6. Short answer

**Proven.** The hook fails at one place: `section_vrom_map.find(0x101D00)`
misses, because the map is filled from the main ELF's 5 code sections in
`init_overlays` (`overlays.cpp:612-614`). Three changes make it work: put the
bank record's section and functions into the reference dump that
`func_reference_syms_file` names (`main.cpp:407-409`, `config.cpp:734-738`); give
`build_regen_list` a way to resolve a hook ROM address to a bank record's RAM
base and function table (`mods.cpp:1936-1992`); and register the 44 bank records
from `app/src/bank_funcs.inc` with that resolver at startup
(`app/src/bank_overlays.cpp:848`). No relocs are needed, because the bank code's
immediates already hold the linked absolute addresses and every call in the
regenerated function becomes a `get_function` lookup.
