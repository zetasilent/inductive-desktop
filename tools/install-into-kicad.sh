#!/usr/bin/env bash
# Wire this repo into a KiCad source tree.
#
#   ./tools/install-into-kicad.sh /path/to/kicad
#
# /path/to/kicad is a KiCad 10.0.7 checkout (git checkout 10.0.7): the patches
# in patches/ are made against it, so the files Inductive saves open in stock
# KiCad 10.
# Symlinks rather than copies, so the repo stays the single source of truth
# and edits here are picked up by the next build.
set -euo pipefail

KICAD="${1:?usage: install-into-kicad.sh /path/to/kicad-source}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[ -f "$KICAD/CMakeLists.txt" ] || { echo "not a KiCad source tree: $KICAD" >&2; exit 1; }

echo "repo:  $REPO"
echo "kicad: $KICAD"

# relative link targets so the pair stays portable if both move together
rel() { python3 -c 'import os,sys;print(os.path.relpath(sys.argv[1],sys.argv[2]))' "$1" "$2"; }

ln -sfn "$(rel "$REPO" "$KICAD")"                          "$KICAD/inductive"

# The panel itself lives in kicommon, which both editors link.
ln -sfn "$(rel "$REPO/src/inductive_panel.h"   "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_panel.h"
ln -sfn "$(rel "$REPO/src/inductive_panel.cpp" "$KICAD/common/widgets")"  "$KICAD/common/widgets/inductive_panel.cpp"
ln -sfn "$(rel "$REPO/src/inductive_cloud_auth.h"   "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_cloud_auth.h"
ln -sfn "$(rel "$REPO/src/inductive_cloud_auth.cpp" "$KICAD/common/widgets")"  "$KICAD/common/widgets/inductive_cloud_auth.cpp"
ln -sfn "$(rel "$REPO/src/inductive_nav_panel.h"   "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_nav_panel.h"
ln -sfn "$(rel "$REPO/src/inductive_nav_panel.cpp" "$KICAD/common/widgets")"  "$KICAD/common/widgets/inductive_nav_panel.cpp"

ln -sfn "$(rel "$REPO/src/inductive_attachments.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_attachments.h"
ln -sfn "$(rel "$REPO/src/inductive_parts.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_parts.h"
ln -sfn "$(rel "$REPO/src/inductive_datasheets.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_datasheets.h"
ln -sfn "$(rel "$REPO/src/inductive_review.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_review.h"
ln -sfn "$(rel "$REPO/src/inductive_review_store.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_review_store.h"
ln -sfn "$(rel "$REPO/src/inductive_version.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_version.h"
ln -sfn "$(rel "$REPO/src/inductive_reports.h" "$KICAD/include/widgets")" "$KICAD/include/widgets/inductive_reports.h"

for f in inductive_start_panel.h inductive_projects.h inductive_updates.h; do
  ln -sfn "$(rel "$REPO/src/$f" "$KICAD/include/widgets")" "$KICAD/include/widgets/$f"
done
ln -sfn "$(rel "$REPO/src/inductive_start_panel.cpp" "$KICAD/common/widgets")" "$KICAD/common/widgets/inductive_start_panel.cpp"

# The tool surfaces live in the editors, because kicommon cannot link against
# pcbnew or eeschema.  Each editor gets its own tools plus the two shared headers;
# symlinked rather than copied so the repo stays the single source of truth.
for f in inductive_board_tools.cpp inductive_board_tools.h inductive_tool_args.h \
         inductive_board_rules.h inductive_board_apply.h \
         inductive_commit_guard.h inductive_jlcpcb.h inductive_properties.h \
         inductive_board_items.h; do
  ln -sfn "$(rel "$REPO/src/$f" "$KICAD/pcbnew")" "$KICAD/pcbnew/$f"
done

for f in inductive_sch_tools.cpp inductive_sch_tools.h inductive_tool_args.h \
         inductive_commit_guard.h inductive_properties.h inductive_sch_items.h \
         inductive_design_checks.h; do
  ln -sfn "$(rel "$REPO/src/$f" "$KICAD/eeschema")" "$KICAD/eeschema/$f"
done

# app icon -- each bundle's CMakeLists expects it in its own source dir
for app in kicad eeschema pcbnew; do
  ln -sfn "$(rel "$REPO/brand/inductive.icns" "$KICAD/$app")" "$KICAD/$app/inductive.icns"
done

echo "applying integration patches..."
# A completed launch patch changes context in the original pane patch. Check
# the newest patch first so this installer remains idempotent after upgrades.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0002-launch-workflows.patch" 2>/dev/null; then
  echo "  launch integration already applied"
else
  if ! git -C "$KICAD" apply --check --reverse "$REPO/patches/0001-kicad-inductive-panes.patch" 2>/dev/null; then
    git -C "$KICAD" apply "$REPO/patches/0001-kicad-inductive-panes.patch"
  fi
  git -C "$KICAD" apply "$REPO/patches/0002-launch-workflows.patch"
fi
# Independent of the two above (a different file), so checked on its own; 0004
# builds on 0003 in the same file, so the newer one is checked first.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0004-no-kicad-setup-wizard.patch" 2>/dev/null; then
  echo "  first-run defaults already applied"
else
  if ! git -C "$KICAD" apply --check --reverse "$REPO/patches/0003-relocatable-library-tables.patch" 2>/dev/null; then
    git -C "$KICAD" apply "$REPO/patches/0003-relocatable-library-tables.patch"
  fi
  git -C "$KICAD" apply "$REPO/patches/0004-no-kicad-setup-wizard.patch"
fi
# The macOS app menu says "About/Quit/Hide <app display name>"; made against the
# tree with the patches above applied.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0005-inductive-branding.patch" 2>/dev/null; then
  echo "  branding already applied"
else
  git -C "$KICAD" apply "$REPO/patches/0005-inductive-branding.patch"
fi
# Launch stall: with a document, wxWidgets' launch loop waited for user input
# before OnInit (patch 0006 wakes it). Made against the tree with 0005 applied.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0006-wake-launch-event-loop.patch" 2>/dev/null; then
  echo "  launch wake-up already applied"
else
  git -C "$KICAD" apply "$REPO/patches/0006-wake-launch-event-loop.patch"
fi
# The netlist updater without an editor window, which the headless QA needs and
# KiCad 10 lacks (KiCad 11 has it). The editor's own path is unchanged.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0007-netlist-updater-without-window.patch" 2>/dev/null; then
  echo "  headless netlist updater already applied"
else
  git -C "$KICAD" apply "$REPO/patches/0007-netlist-updater-without-window.patch"
fi
# The macOS install step: leave the bundled Python's own @loader_path references
# alone, add no rpaths a binary does not use, and give KiCad's Python modules an
# rpath of their own so a script run by the bundled python3 can import pcbnew.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0008-no-rpaths-where-unused.patch" 2>/dev/null; then
  echo "  bundle fix-up already applied"
else
  git -C "$KICAD" apply "$REPO/patches/0008-no-rpaths-where-unused.patch"
fi
# A copied footprint (every undo image, so every move) took over the board's UUID
# lookups for its pads and zones: DRC and the router then checked a module's antenna
# keep-out where the module used to be. Only the footprint on the board is indexed.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0009-footprint-copies-stay-out-of-the-id-cache.patch" 2>/dev/null; then
  echo "  footprint id-cache fix already applied"
else
  git -C "$KICAD" apply "$REPO/patches/0009-footprint-copies-stay-out-of-the-id-cache.patch"
fi
# The schematic and PCB editors are separate apps with a copy of the .kicad_pro each;
# the last to save erased the other's changes (a board's net classes and design rules
# after a schematic save). A save now keeps the sections it did not change.
if git -C "$KICAD" apply --check --reverse "$REPO/patches/0010-project-file-keeps-other-editors-changes.patch" 2>/dev/null; then
  echo "  project-file merge already applied"
else
  git -C "$KICAD" apply "$REPO/patches/0010-project-file-keeps-other-editors-changes.patch"
fi

echo "installing node deps..."
( cd "$REPO" && npm ci --ignore-scripts --no-fund --no-audit )

# Inductive's settings directory (inductive/10.0) starts empty, so the library choosers would be empty
# too and no part could be placed.  Reported rather than done: it edits the user's
# KiCad settings, which is not something an install script should do silently.
echo
if ! python3 "$REPO/tools/configure-libraries.py" --check > /dev/null 2>&1; then
  echo "note: the KiCad libraries are not configured for this build."
  echo "      run:  python3 tools/configure-libraries.py"
fi

echo
echo "done. build with:  cd $KICAD/build/release && make eeschema pcbnew -j\$(sysctl -n hw.ncpu)"
