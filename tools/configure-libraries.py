#!/usr/bin/env python3
"""
Point the Inductive build at a working set of KiCad libraries.

    tools/configure-libraries.py [--check] [--kicad-app /Applications/KiCad/KiCad.app]

WHY THIS EXISTS

Inductive is built on KiCad 10.0 with KICAD_CONFIG_DIR=inductive, so it keeps its
settings in ~/Library/Preferences/inductive/10.0/ -- a directory separate from a stock
KiCad 10 install's (kicad/10.0), which is the point: running Inductive cannot disturb
a working KiCad, and a working KiCad cannot disturb Inductive.

The cost is that this config starts empty and has to be pointed at some
libraries.  On this machine it was pointed at a path that no longer exists
(~/Desktop/kic/...), left behind when the project tree moved.  The symptom is
quiet and bad: every library chooser is empty, `place_footprint` can find
nothing, and the editor looks broken rather than unconfigured.

WHAT IT DOES

Two changes, both confined to Inductive's settings directory:

  1. The global symbol and footprint tables are single rows of type "Table" that
     nest another table by path.  Those paths are repointed at the stock KiCad
     install's own tables, which list all 153 footprint and symbol libraries.

  2. Those stock tables refer to ${KICAD<major>_FOOTPRINT_DIR} and friends, named
     for the stock install's own major version -- so the nested table would load
     and every URI in it would still be unresolved unless those same variables
     are defined for our build too.  The major version is read from the stock
     table itself (not assumed) and those names are defined in Inductive's
     kicad_common.json.  That also fixes the 3D models, whose paths live inside
     the .kicad_mod files themselves and cannot be rewritten by any change to a
     table.

Both files are backed up before being written, and the script is idempotent:
running it twice changes nothing the second time.  --check reports what it would
do and changes nothing.

The stock KiCad install is only read from, never modified.
"""

import argparse
import json
import os
import re
import shutil
import sys
from datetime import datetime
from pathlib import Path

CONFIG_DIR = Path.home() / "Library" / "Preferences" / "inductive" / "10.0"
DEFAULT_APP = Path("/Applications/KiCad/KiCad.app")


def stock_major(stock_table):
    """The major version the stock table's own directory variables are named
    for (e.g. 9 for ${KICAD9_SYMBOL_DIR}, 10 for ${KICAD10_SYMBOL_DIR}).

    This is a property of the stock install being borrowed from, not of our
    build, so it is read from the table rather than assumed.
    """
    match = re.search(r"KICAD(\d+)_", stock_table.read_text(encoding="utf-8"))
    if not match:
        fail(f"could not find a ${{KICAD<major>_...}} variable in {stock_table}")
    return int(match.group(1))


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def backup(path):
    """Copy a file aside before overwriting it, keeping every earlier copy."""
    if not path.exists():
        return None

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    dest = path.with_suffix(path.suffix + f".inductive-bak-{stamp}")
    shutil.copy2(path, dest)
    return dest


def nested_table(kind, target):
    """A one-row library table that defers to another table on disk."""
    root = "fp_lib_table" if kind == "fp" else "sym_lib_table"
    return (
        f"({root}\n"
        f"\t(version 7)\n"
        f'\t(lib (name "KiCad") (type "Table") (uri "{target}") (options "") '
        f'(descr "KiCad Default Libraries"))\n'
        f")\n"
    )


def current_target(path):
    """The path a nested table currently defers to, or None."""
    if not path.exists():
        return None

    match = re.search(r'\(uri\s+"([^"]+)"\)', path.read_text(encoding="utf-8"))
    return match.group(1) if match else None



def verify(tables, env):
    """Resolve every library URI the way KiCad will, and report what does not exist.

    Repointing the tables is only half the job: the nested table's URIs are
    written against ${KICAD9_*_DIR}, so a table that loads perfectly can still
    yield 153 libraries that all resolve to nothing.  This is the check that the
    two halves agree, and it is cheap enough to run every time.
    """
    print()

    for kind, (live, _stock) in tables.items():
        target = current_target(live)

        if not target or not Path(target).exists():
            print(f"  {kind}: nested table missing at {target}")
            continue

        text = Path(target).read_text(encoding="utf-8")
        ok, bad, examples = 0, 0, []

        for raw in re.findall(r"\(uri\s+([^)]+)\)", text):
            uri = raw.strip().strip('"')
            expanded = re.sub(r"\$\{([A-Z0-9_]+)\}",
                              lambda m: env.get(m.group(1), "\0"), uri)

            if "\0" in expanded:
                bad += 1
                examples.append(f"unresolved variable in {uri}")
            elif Path(expanded).exists():
                ok += 1
            else:
                bad += 1
                examples.append(f"missing {expanded}")

        status = "ok" if bad == 0 else "FAIL"
        print(f"  {status:6s}{kind}-lib-table: {ok} librar(ies) resolve, {bad} do not")

        for example in examples[:3]:
            print(f"            {example}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="report what would change; write nothing")
    parser.add_argument("--kicad-app", type=Path, default=DEFAULT_APP,
                        help="the stock KiCad.app to borrow libraries from")
    args = parser.parse_args()

    shared = args.kicad_app / "Contents" / "SharedSupport"

    if not shared.is_dir():
        fail(f"no KiCad install at {args.kicad_app}\n"
             f"       Install KiCad, or pass --kicad-app.")

    # Everything is checked before anything is written, so a half-configured
    # state is not reachable.
    templates = shared / "template"

    tables = {
        "sym": (CONFIG_DIR / "sym-lib-table", templates / "sym-lib-table"),
        "fp": (CONFIG_DIR / "fp-lib-table", templates / "fp-lib-table"),
    }

    for kind, (_, stock) in tables.items():
        if not stock.is_file():
            fail(f"the stock {kind} table is missing at {stock}")

    major = stock_major(tables["sym"][1])

    wanted = {
        f"KICAD{major}_SYMBOL_DIR": shared / "symbols",
        f"KICAD{major}_FOOTPRINT_DIR": shared / "footprints",
        f"KICAD{major}_3DMODEL_DIR": shared / "3dmodels",
        f"KICAD{major}_TEMPLATE_DIR": templates,
    }

    for name, path in wanted.items():
        if not path.is_dir():
            fail(f"{name} would point at {path}, which does not exist")

    if not CONFIG_DIR.is_dir():
        fail(f"no Inductive settings directory at {CONFIG_DIR}\n"
             f"       Launch Inductive once so it creates one, then re-run this.")

    common = CONFIG_DIR / "kicad_common.json"

    if not common.is_file():
        fail(f"{common} is missing; launch Inductive once to create it")

    print(f"stock libraries: {shared}")
    print(f"settings:        {CONFIG_DIR}")
    print()

    changes = []

    # ── the two library tables ───────────────────────────────────────
    for kind, (live, stock) in tables.items():
        target = str(stock)
        now = current_target(live)

        if now == target:
            print(f"  ok      {live.name} already points at the stock table")
            continue

        broken = now is not None and not Path(now).exists()
        why = "broken path" if broken else ("absent" if now is None else "different path")
        print(f"  change  {live.name}  ({why})")

        if now:
            print(f"            was: {now}")

        print(f"            now: {target}")
        changes.append(("table", kind, live, nested_table(kind, target)))

    # ── the versioned directory variables ────────────────────────────
    settings = json.loads(common.read_text(encoding="utf-8"))
    env = settings.get("environment") or {}
    vars_now = env.get("vars") or {}

    missing = {n: str(p) for n, p in wanted.items() if vars_now.get(n) != str(p)}

    if missing:
        print(f"  change  kicad_common.json  ({len(missing)} directory variable(s))")

        for name, value in missing.items():
            print(f"            {name} = {value}")

        merged = dict(vars_now)
        merged.update(missing)
        env["vars"] = merged
        settings["environment"] = env
        changes.append(("json", None, common,
                        json.dumps(settings, indent=2) + "\n"))
    else:
        print("  ok      kicad_common.json already defines the directory variables")

    print()

    if not changes:
        print("Nothing to do -- the libraries are already configured.")
        verify(tables, {n: str(p) for n, p in wanted.items()})
        return 0

    if args.check:
        print(f"{len(changes)} change(s) needed. Re-run without --check to apply.")
        return 1

    # KiCad rewrites its settings on exit, so a running editor would undo this.
    if os.system("pgrep -qx Inductive || pgrep -qx 'Inductive PCB' "
                 "|| pgrep -qx 'Inductive Schematic'") == 0:
        fail("Inductive is running. Quit it first -- it rewrites these files on exit "
             "and would discard the change.")

    for kind, _sub, path, content in changes:
        saved = backup(path)
        path.write_text(content, encoding="utf-8")
        print(f"  wrote   {path.name}" + (f"   (backup: {saved.name})" if saved else ""))

    verify(tables, {n: str(p) for n, p in wanted.items()})

    print()
    print("Done. Restart Inductive and the libraries will be available.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
