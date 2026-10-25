#!/usr/bin/env bash
#
# Configure a KiCad build tree against the dependencies kicad-mac-builder produced.
#
#   ./tools/configure-kicad.sh /path/to/kicad /path/to/kicad-mac-builder
#
# Run this AFTER kicad-mac-builder has built wxWidgets and ngspice. Creates
# <kicad>/build/release, which is where you then run `make eeschema pcbnew`.
#
# <kicad> is KiCad 10.0 (tag 10.0.7), so the files Inductive saves open in stock
# KiCad 10. KiCad 10 embeds Python and builds its scripting module with SWIG:
# tools/fetch-python-framework.sh supplies python.org's signed Python, and SWIG 4
# comes from Homebrew (brew install swig). Settings live in
# ~/Library/Preferences/inductive/10.0 (KICAD_CONFIG_DIR), apart from a stock
# KiCad 10's kicad/10.0. QA tests are built: tools/test-*.py link against them.
#
# INDUCTIVE_MACOS_MIN=14.0 builds for that macOS and later (the default is this Mac's
# version); the libraries must have been built for it too (tools/build-macos.sh).
# INDUCTIVE_QA_TESTS=OFF skips the QA programs, as a release build does.
set -euo pipefail

KICAD="${1:?usage: configure-kicad.sh /path/to/kicad /path/to/kicad-mac-builder}"
KMB="${2:?usage: configure-kicad.sh /path/to/kicad /path/to/kicad-mac-builder}"
KICAD="$(cd "$KICAD" && pwd)"
KMB="$(cd "$KMB" && pwd)"

WXCONFIG="$KMB/build/wxwidgets-dest/bin/wx-config"
NGSPICE_INC="$KMB/build/ngspice-dest/include"
NGSPICE_LIB="$KMB/build/ngspice-dest/lib/libngspice.dylib"
DEST="$KMB/build/kicad-dest"

[ -x "$WXCONFIG" ] || { echo "wx-config not found at $WXCONFIG -- run kicad-mac-builder's build.py first" >&2; exit 1; }

# Inductive's panel needs a wxWidgets built with a web view.
SETUP="$KMB/build/wxwidgets-dest/lib/wx/include/osx_cocoa-unicode-3.2/wx/setup.h"
if [ -f "$SETUP" ]; then
  grep -qE '^#define wxUSE_WEBVIEW +1' "$SETUP" \
    || { echo "wxWidgets was built without wxUSE_WEBVIEW -- Inductive's panel will not compile" >&2; exit 1; }
  echo "wxUSE_WEBVIEW: ok"
fi

OCC_PREFIX="$(brew --prefix opencascade 2>/dev/null || echo /opt/homebrew/opt/opencascade)"

command -v swig >/dev/null || { echo "SWIG 4 is needed to build KiCad 10: brew install swig" >&2; exit 1; }

PYFW="$KMB/build/python-official/Library/Frameworks/Python.framework"
if [ ! -x "$PYFW/Versions/Current/bin/python3" ]; then
  "$(dirname "${BASH_SOURCE[0]}")/fetch-python-framework.sh" "$KMB"
fi
PYXY="$(basename "$(readlink "$PYFW/Versions/Current")")"

# On Apple, KiCad's own CMakeLists.txt FORCE-derives KICAD_BIN/KICAD_LIB/KICAD_DATA
# from CMAKE_INSTALL_PREFIX (see the `else()` after `if(NOT APPLE)`) -- passing
# them directly as their own -D flags is silently discarded.
cmake -S "$KICAD" -B "$KICAD/build/release" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DwxWidgets_CONFIG_EXECUTABLE="$WXCONFIG" \
  -DOCC_INCLUDE_DIR="$OCC_PREFIX/include/opencascade" \
  -DOCC_LIBRARY_DIR="$OCC_PREFIX/lib" \
  -DNGSPICE_INCLUDE_DIR="$NGSPICE_INC" \
  -DNGSPICE_LIBRARY="$NGSPICE_LIB" \
  -DCMAKE_INSTALL_PREFIX="$DEST" \
  -DKICAD_APPLE_MAKE_RELOCATEABLE_BUNDLE=ON \
  -DKICAD_OSX_SIGNING_ID=- \
  -DKICAD_UPDATE_CHECK=OFF \
  -DKICAD_BUILD_QA_TESTS="${INDUCTIVE_QA_TESTS:-ON}" \
  ${INDUCTIVE_MACOS_MIN:+-DCMAKE_OSX_DEPLOYMENT_TARGET="$INDUCTIVE_MACOS_MIN"} \
  -DKICAD_CONFIG_DIR=inductive \
  -DKICAD_SCRIPTING_WXPYTHON=OFF \
  -DPYTHON_EXECUTABLE="$PYFW/Versions/Current/bin/python3" \
  -DPYTHON_INCLUDE_DIR="$PYFW/Versions/Current/include/python$PYXY/" \
  -DPYTHON_LIBRARY="$PYFW/Versions/Current/lib/libpython$PYXY.dylib" \
  -DPYTHON_SITE_PACKAGE_PATH="$PYFW/Versions/Current/lib/python$PYXY/site-packages" \
  -DPYTHON_FRAMEWORK="$PYFW"

echo
echo "configured. build with:"
echo "    cd $KICAD/build/release && make eeschema pcbnew -j\$(sysctl -n hw.ncpu)"
