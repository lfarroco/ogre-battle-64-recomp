#!/usr/bin/env python3
"""List the DLLs a Windows PE binary imports, and check a package against them.

    tools/pe_imports.py <dir-or-file> [more...]

Every imported DLL must be either shipped next to the binaries (the package's
own `.exe`/`.dll` files) or a Windows system DLL (the allowlist below). This is
the check that catches "the package is missing a runtime DLL" on any machine,
including a CI runner that happens to have the Visual C++ redistributable
installed and where launching the program would therefore succeed anyway
(session 94: `dxcompiler.dll` imports `MSVCP140.dll`/`VCRUNTIME140.dll`, and the
package shipped neither).

Exit 0 when every import resolves, 1 when one does not, and 2 on a parse error.
"""

import os
import struct
import sys

# DLLs Windows itself provides. `api-ms-win-*` and `ext-ms-*` are API sets,
# resolved by the loader from the OS schema; everything else here is a documented
# system component. A DLL that is neither shipped nor in this list fails the
# check, so a new dependency has to be either bundled or added deliberately.
SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "cabinet.dll", "cfgmgr32.dll", "comctl32.dll",
    "comdlg32.dll", "crypt32.dll", "d2d1.dll", "d3d11.dll", "d3d12.dll",
    "d3dcompiler_47.dll", "dbghelp.dll", "dwmapi.dll", "dxgi.dll", "gdi32.dll",
    "gdiplus.dll", "hid.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    "mf.dll", "mfplat.dll", "mfreadwrite.dll", "mpr.dll", "msimg32.dll",
    "msvcrt.dll", "ncrypt.dll", "normaliz.dll", "ntdll.dll", "ole32.dll",
    "oleaut32.dll", "opengl32.dll", "powrprof.dll", "propsys.dll", "psapi.dll",
    "rpcrt4.dll", "sechost.dll", "setupapi.dll", "shcore.dll", "shell32.dll",
    "shlwapi.dll", "ucrtbase.dll", "user32.dll", "userenv.dll", "usp10.dll",
    "uxtheme.dll", "version.dll", "wer.dll", "winhttp.dll", "winmm.dll",
    "winspool.drv", "wintrust.dll", "ws2_32.dll", "wsock32.dll",
}
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-")


def _rva_to_offset(sections, rva):
    for vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None


def pe_imports(path):
    """Return the set of imported DLL names (lower case) for one PE file."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] != b"MZ":
        raise ValueError("not a PE image (no MZ header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image (no PE signature)")
    coff = pe + 4
    num_sections, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    if magic == 0x20B:      # PE32+
        data_dir = opt + 112
    elif magic == 0x10B:    # PE32
        data_dir = opt + 96
    else:
        raise ValueError("unknown optional-header magic 0x%X" % magic)
    import_rva, = struct.unpack_from("<I", data, data_dir + 8)  # directory 1

    sections = []
    sec_table = opt + opt_size
    for i in range(num_sections):
        off = sec_table + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, vsize, rawptr, rawsize))

    names = set()
    if import_rva == 0:
        return names
    desc = _rva_to_offset(sections, import_rva)
    while desc is not None:
        entry = data[desc:desc + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva, = struct.unpack_from("<I", entry, 12)
        name_off = _rva_to_offset(sections, name_rva)
        if name_off is None:
            break
        end = data.index(b"\0", name_off)
        names.add(data[name_off:end].decode("ascii", "replace").lower())
        desc += 20
    return names


def collect_paths(arguments):
    files = []
    for argument in arguments:
        if os.path.isdir(argument):
            for name in sorted(os.listdir(argument)):
                if name.lower().endswith((".exe", ".dll")):
                    files.append(os.path.join(argument, name))
        else:
            files.append(argument)
    return files


def main(argv):
    if not argv:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    files = collect_paths(argv)
    if not files:
        print("pe_imports: no .exe/.dll found", file=sys.stderr)
        return 2

    shipped = {os.path.basename(path).lower()
               for path in files if path.lower().endswith(".dll")}
    missing = {}
    for path in files:
        try:
            imports = pe_imports(path)
        except (ValueError, struct.error, OSError) as error:
            print("pe_imports: %s: %s" % (path, error), file=sys.stderr)
            return 2
        for dll in sorted(imports):
            if dll in shipped:
                continue
            if dll in SYSTEM_DLLS or dll.startswith(SYSTEM_PREFIXES):
                continue
            missing.setdefault(dll, []).append(os.path.basename(path))

    if missing:
        print("pe_imports: imports that the package does not provide:")
        for dll, users in sorted(missing.items()):
            print("  %-28s required by %s" % (dll, ", ".join(users)))
        return 1
    print("pe_imports: OK (%d binaries, every import is shipped or a system DLL)"
          % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
