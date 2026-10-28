#!/usr/bin/env python3
"""Reject reloadable modules that break the module contract (design doc §3.3).

    python3 tools/module_verify.py build/gameplay/libgameplay.so

Checks, on the ELF symbol tables (Linux; uses `readelf` from binutils):

  1. Exports: only EngineModuleManifest and System_* entry points
     (weak inline std:: functions a compiler emits are tolerated).
  2. No writable data: no object symbols in .data, .bss, .tdata or .tbss
     (globals, statics, function-local statics and their guard variables).
     Constant data in .rodata / .data.rel.ro is fine.
  3. No vtables (virtual functions on objects that could outlive a reload).

Exit status 0 = conforms, 1 = violations (listed), 2 = could not inspect.
"""
import re
import shutil
import subprocess
import sys

ALLOWED_EXPORT = re.compile(r"^(EngineModuleManifest|System_[A-Za-z0-9_]+)$")
WRITABLE_SECTIONS = (".data", ".bss", ".tdata", ".tbss")
# Emitted by the C runtime / toolchain into every shared object.
RUNTIME_SYMBOLS = {
    "__dso_handle", "__TMC_END__", "completed.0", "completed.1", "__bss_start", "_edata", "_end",
    "_GLOBAL_OFFSET_TABLE_", "_DYNAMIC", "DW.ref.__gxx_personality_v0", "__FRAME_END__",
}


def run(args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def demangle(names):
    if not names or not shutil.which("c++filt"):
        return {n: n for n in names}
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out))


def sections(path):
    """section index -> name"""
    result = {}
    for line in run(["readelf", "-SW", path]).splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", line)
        if m:
            result[m.group(1)] = m.group(2)
    return result


def symbols(path, dynamic):
    rows = []
    for line in run(["readelf", "--dyn-syms" if dynamic else "-sW", "-W", path]).splitlines():
        parts = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(parts) >= 8 and parts[0].endswith(":") and parts[0][:-1].isdigit():
            rows.append({"type": parts[3], "bind": parts[4], "vis": parts[5], "ndx": parts[6], "name": parts[7]})
    return rows


def verify(path):
    problems = []
    secs = sections(path)

    for s in symbols(path, dynamic=True):
        if s["ndx"] == "UND" or s["bind"] == "LOCAL" or s["type"] not in ("FUNC", "OBJECT", "IFUNC"):
            continue
        name = s["name"].split("@")[0]
        # Weak copies of inline standard-library functions (e.g. std::sin in
        # an unoptimised Clang build) carry default visibility from libstdc++'s
        # own pragmas. They are neither state nor API, so they are allowed.
        if s["bind"] == "WEAK" and s["type"] == "FUNC" and name.startswith(("_ZSt", "_ZNSt", "_ZNKSt")):
            continue
        if not ALLOWED_EXPORT.match(name):
            problems.append("unexpected export: %s (only EngineModuleManifest and System_* may be exported)" % name)

    offenders = []
    for s in symbols(path, dynamic=False):
        if s["type"] not in ("OBJECT", "TLS") or s["ndx"] in ("UND", "ABS", "COM"):
            continue
        name = s["name"].split("@")[0]
        if name in RUNTIME_SYMBOLS:
            continue
        sec = secs.get(s["ndx"], "")
        if name.startswith("_ZTV"):
            offenders.append((name, "vtable: virtual functions on module objects dangle after unload"))
        elif sec.startswith(WRITABLE_SECTIONS) and not sec.startswith(".data.rel.ro"):
            offenders.append((name, "writable data in %s: modules may not hold state" % sec))
    pretty = demangle([n for n, _ in offenders])
    for name, why in offenders:
        problems.append("%s -> %s" % (pretty.get(name, name), why))
    return problems


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    if not shutil.which("readelf"):
        print("module_verify: readelf not found (binutils); skipping", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        problems = verify(path)
    except (subprocess.CalledProcessError, OSError) as e:
        print("module_verify: cannot inspect %s: %s" % (path, e), file=sys.stderr)
        return 2
    if problems:
        print("module_verify: %s violates the module contract (1—Documentation/module-contract.md):" % path)
        for p in problems:
            print("  - " + p)
        return 1
    print("module_verify: %s OK" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
