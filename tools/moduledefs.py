"""Writes the two .def files between the executable and the game's code module (ANALYSIS.md, Phase 5, The game's code
module), from the symbol tables of the executable's objects (llvm-nm), as the build makes them before linking either:

  <executable>.def      what the executable exports to the module: the runtime's functions the generated code calls and
                        the C runtime's (runtime/module/runtime_exports.def), every kernel and XAM import's
                        implementation (runtime/imports.inc), every replacement of a game function, a strong
                        definition of sub_XXXXXXXX in the runtime's objects, and the mid-asm hooks the XenonRecomp
                        config places in the generated code (its [[midasm_hook]] entries, by name). The module is linked
                        with the import library made from it as a whole (/WHOLEARCHIVE), so the generated code's calls
                        to a replaced function reach the executable's, and its own (a weak alias) is left unused.
  <module>.def          what the module exports to the executable: the generated functions the runtime calls by name,
                        sub_XXXXXXXX and the originals of the replaced ones, __imp__sub_XXXXXXXX.

A definition that looks like a replacement but has another signature (so it would replace nothing) is an error, and so
is an export of runtime_exports.def that the objects don't define, by its name alone (the C runtime's are name=internal),
and a mid-asm hook the objects don't define. Files are rewritten only when they change. Usage:
  python tools/moduledefs.py --nm <llvm-nm> --objects <file listing the objects> --imports runtime/imports.inc
      --base runtime/module/runtime_exports.def --executable doa4.exe --module doa4_ppc.dll
      --out-executable <dir>/doa4.def --out-module <dir>/doa4_ppc.def [--config config/DOA4.toml]
"""
import argparse
import os
import re
import subprocess
import sys
import tomllib

# A generated function's signature, void (PPCContext& __restrict, uint8_t*), in the Microsoft C++ ABI.
SIGNATURE = "@@YAXAEIAUPPCContext@@PEAE@Z"
REPLACEMENT = re.compile(r"^\?sub_[0-9A-F]{8}" + re.escape(SIGNATURE) + "$")
ORIGINAL = re.compile(r"^__imp__sub_[0-9A-F]{8}$")
# A function named as a game function with another signature or C linkage.
LOOKS_LIKE_GAME = re.compile(r"^(\?sub_[0-9A-F]{8}@@|sub_[0-9A-F]{8}$)")
IMPORT = re.compile(r"^\s*IMPORT_FUNC\(\s*\w+\s*,\s*\d+\s*,\s*(\w+)\s*,")


def symbols(nm, objects):
    """Defined (strong or weak) and undefined external symbols of the objects."""
    strong, weak, undefined = set(), set(), set()
    # llvm-nm takes many files at once; batches keep the command line short.
    for start in range(0, len(objects), 40):
        out = subprocess.run([nm, "--extern-only", *objects[start:start + 40]], capture_output=True, text=True,
                             encoding="utf-8", check=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                undefined.add(parts[1])
            elif len(parts) == 3:
                kind, name = parts[1], parts[2]
                (weak if kind in "wWvV" else strong).add(name)
    return strong, weak, undefined


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser()
    for name in ("nm", "objects", "imports", "base", "executable", "module", "out_executable", "out_module"):
        parser.add_argument("--" + name.replace("_", "-"), required=True)
    parser.add_argument("--config", help="XenonRecomp's config, whose mid-asm hooks the executable exports")
    args = parser.parse_args()

    with open(args.objects, encoding="utf-8") as f:
        objects = [line.strip() for line in f if line.strip()]
    strong, weak, undefined = symbols(args.nm, objects)
    defined = strong | weak

    replacements = sorted(name for name in strong if REPLACEMENT.match(name))
    odd = sorted(name for name in defined if LOOKS_LIKE_GAME.match(name) and not REPLACEMENT.match(name))
    if odd:
        sys.exit("moduledefs: definitions that look like replacements of game functions but aren't PPC_FUNC(sub_XXXXXXXX), "
                 "so they'd replace nothing:\n  " + "\n  ".join(odd))

    imports = []
    with open(args.imports, encoding="utf-8") as f:
        for line in f:
            m = IMPORT.match(line)
            if m:
                imports.append("?__imp__" + m.group(1) + SIGNATURE)
    missing = [name for name in imports if name not in defined]
    if missing:
        sys.exit("moduledefs: imports without an implementation or stub in the objects:\n  " + "\n  ".join(missing))

    with open(args.base, encoding="utf-8") as f:
        base = [line.split(";")[0].strip() for line in f]
    base = [name for name in base if name]
    # An export the objects don't define, the C runtime's, must name the runtime's symbol it exports (name=internal, a
    # jump to the C runtime's function, runtime/module/crt_exports.h): by its name alone lld-link binds it to a C++
    # function of that name if it finds one before the C runtime's, as the export sqrt was bound to the C++ library's
    # sqrt(long double), which called itself for negative inputs.
    unbound = [name for name in base if "=" not in name and name not in defined]
    if unbound:
        sys.exit("moduledefs: exports that the executable's objects don't define, by their names alone (lld-link could bind "
                 "them to C++ functions of those names; export them as name=ModuleCrt_name, a jump to the C runtime's "
                 "function, runtime/module/crt_exports.h):\n  " + "\n  ".join(unbound))
    unjumped = [name for name in base if "=" in name and name.split("=", 1)[1] not in defined]
    if unjumped:
        sys.exit("moduledefs: exports from symbols the executable's objects don't define:\n  " + "\n  ".join(unjumped))

    # The mid-asm hooks the config places in the generated code, which calls them by name (C++ linkage, their parameters
    # the registers the config lists): each name's definitions in the objects.
    hooks = []
    if args.config:
        with open(args.config, "rb") as f:
            names = sorted({hook["name"] for hook in tomllib.load(f).get("midasm_hook", [])})
        for name in names:
            found = sorted(symbol for symbol in strong if symbol == name or symbol.startswith(f"?{name}@@"))
            if not found:
                sys.exit(f"moduledefs: the mid-asm hook {name} ({args.config}) isn't defined in the executable's objects")
            hooks += found

    module_exports = sorted(name for name in undefined - defined if REPLACEMENT.match(name) or ORIGINAL.match(name))

    header = "; Generated by tools/moduledefs.py from the executable's objects; don't edit.\n"
    write_if_changed(args.out_executable, header + f"LIBRARY {args.executable}\nEXPORTS\n" +
                     "".join(f"    {name}\n" for name in base + sorted(imports) + replacements + hooks))
    write_if_changed(args.out_module, header + f"LIBRARY {args.module}\nEXPORTS\n" +
                     "".join(f"    {name}\n" for name in module_exports))
    print(f"moduledefs: {len(replacements)} replacements, {len(imports)} imports, {len(base)} runtime functions and "
          f"{len(hooks)} mid-asm hooks exported to the module; {len(module_exports)} of its functions called by the runtime")


if __name__ == "__main__":
    main()
