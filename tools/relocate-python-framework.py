#!/usr/bin/env python3
"""Make python.org's Python.framework relocatable, for bundling inside Inductive.

    tools/relocate-python-framework.py /path/to/Python.framework

python.org builds the framework for /Library/Frameworks: the library's own install
name, the interpreter and every module that links back to it name that absolute path,
so inside an app bundle on a Mac without python.org's Python they would not load.

  * The library's install name becomes @rpath/Versions/<x.y>/Python. KiCad 10 adds the
    framework itself as a build rpath, and @executable_path/../Frameworks/Python.framework
    as an install rpath, so this one name resolves in the build tree and in the app.
  * Every other reference into the framework becomes @loader_path-relative.

Every binary is thinned to arm64 first: Inductive is built for Apple Silicon only, and
python.org's universal binaries leave too little header room in their Intel half for
the two rpaths KiCad 10's install step adds to bin/python3 ("larger updated load
commands do not fit"). Binaries with no arm64 slice (the Intel-only python3-intel64
launcher) are removed. Halves the framework's size, too.

Each changed binary is re-signed ad hoc (install_name_tool invalidates a signature);
a release build signs the whole app again with its Developer ID. Standard library and
Apple's own tools only.
"""
import os
import subprocess
import sys

ROOT_PREFIX = "/Library/Frameworks/Python.framework/"
ARCH = "arm64"


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def is_macho(path):
    with open(path, "rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def install_id(path):
    out = run("otool", "-D", path).splitlines()
    return out[1].strip() if len(out) > 1 else None


def dependencies(path):
    # otool -L prints the file name, then one line per load command (its own id first
    # for a dylib); fat files repeat the list per architecture.
    deps = []
    for line in run("otool", "-L", path).splitlines()[1:]:
        line = line.strip()
        if line and not line.endswith(":"):
            deps.append(line.split(" (compatibility")[0])
    return sorted(set(deps))


def thin(framework):
    """Every Mach-O down to ARCH; ones without it removed. Returns how many changed."""
    changed = 0
    for top, dirs, files in os.walk(framework):
        for name in files:
            path = os.path.join(top, name)
            if os.path.islink(path) or not is_macho(path):
                continue
            archs = run("lipo", "-archs", path).split()
            if ARCH not in archs:
                os.remove(path)
                changed += 1
            elif len(archs) > 1:
                run("lipo", "-thin", ARCH, path, "-output", path + ".thin")
                os.chmod(path + ".thin", os.stat(path).st_mode)
                os.replace(path + ".thin", path)
                run("codesign", "--force", "--sign", "-", path)
                changed += 1
    # Symlinks left pointing at a removed binary (python3-intel64 -> python3.13-intel64).
    for top, dirs, files in os.walk(framework):
        for name in files:
            path = os.path.join(top, name)
            if os.path.islink(path) and not os.path.exists(path):
                os.remove(path)
    return changed


def main(framework):
    framework = os.path.realpath(framework)
    thinned = thin(framework)
    versions = [v for v in os.listdir(os.path.join(framework, "Versions")) if v != "Current"]
    if len(versions) != 1:
        sys.exit(f"expected one version in {framework}/Versions, found {versions}")
    version = versions[0]
    changed = 0

    for top, dirs, files in os.walk(framework):
        for name in files:
            path = os.path.join(top, name)
            if os.path.islink(path) or not is_macho(path):
                continue

            args = []
            own = install_id(path)
            if own and own.startswith(ROOT_PREFIX):
                rest = own[len(ROOT_PREFIX):]
                if rest == f"Versions/{version}/Python":
                    args += ["-id", f"@rpath/Versions/{version}/Python"]
                else:
                    target = os.path.join(framework, rest)
                    args += ["-id", "@loader_path/" + os.path.relpath(target, os.path.dirname(path))]

            for dep in dependencies(path):
                if dep.startswith(ROOT_PREFIX) and dep != own:
                    target = os.path.join(framework, dep[len(ROOT_PREFIX):])
                    args += ["-change", dep, "@loader_path/" + os.path.relpath(target, os.path.dirname(path))]

            if args:
                run("install_name_tool", *args, path)
                run("codesign", "--force", "--sign", "-", path)
                changed += 1

    # Nothing may still point at /Library/Frameworks.
    left = []
    for top, dirs, files in os.walk(framework):
        for name in files:
            path = os.path.join(top, name)
            if not os.path.islink(path) and is_macho(path):
                if any(d.startswith(ROOT_PREFIX) for d in dependencies(path)):
                    left.append(path)
    if left:
        sys.exit("still pointing at /Library/Frameworks:\n  " + "\n  ".join(left))

    print(f"thinned {thinned} binaries to {ARCH}, relocated {changed} in {framework} (Python {version})")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
