#!/usr/bin/env bash
#
# The Python runtime KiCad 10 embeds, from python.org's own macOS installer.
#
#   tools/fetch-python-framework.sh /path/to/kicad-mac-builder
#
# Produces <kicad-mac-builder>/build/python-official/Library/Frameworks/Python.framework,
# relocatable, for configure-kicad.sh to pass as PYTHON_FRAMEWORK.
#
# WHY NOT kicad-mac-builder's own python step: it clones a third-party script at an
# unpinned branch and pip-installs unpinned packages into the bundle. This takes one
# file -- the installer python.org signs and Apple notarises -- checks its SHA-256 and
# signer, unpacks it with pkgutil (nothing in it runs), and makes it relocatable with
# Apple's own install_name_tool (tools/relocate-python-framework.py). No wxPython and
# no pip packages: KiCad's scripting and plugin API work; plugins that open their own
# wxPython windows, and the wxPython console, do not.
set -euo pipefail

KMB="${1:?usage: fetch-python-framework.sh /path/to/kicad-mac-builder}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VERSION=3.13.16
PKG="python-$VERSION-macos11.pkg"
URL="https://www.python.org/ftp/python/$VERSION/$PKG"
SHA256=30666509020b4da0dd8bc2e773255f34d76b7bb80b66960a928d5f6daa0192d7
SIGNER="Developer ID Installer: Python Software Foundation (BMM5U3QVKW)"

OUT="$KMB/build/python-official"
mkdir -p "$OUT/download"

if [ ! -f "$OUT/download/$PKG" ]; then
  echo "downloading $URL"
  curl -sSfL -o "$OUT/download/$PKG.part" "$URL"
  mv "$OUT/download/$PKG.part" "$OUT/download/$PKG"
fi

echo "$SHA256  $OUT/download/$PKG" | shasum -a 256 -c -
pkgutil --check-signature "$OUT/download/$PKG" | grep -qF "$SIGNER" \
  || { echo "$PKG is not signed by $SIGNER" >&2; exit 1; }
pkgutil --check-signature "$OUT/download/$PKG" | grep -q "trusted by the Apple notary service" \
  || { echo "$PKG is not notarised" >&2; exit 1; }

rm -rf "$OUT/expanded" "$OUT/Library"
pkgutil --expand-full "$OUT/download/$PKG" "$OUT/expanded"
mkdir -p "$OUT/Library/Frameworks"
cp -R "$OUT/expanded/Python_Framework.pkg/Payload" "$OUT/Library/Frameworks/Python.framework"
rm -rf "$OUT/expanded"

python3 -I "$REPO/tools/relocate-python-framework.py" "$OUT/Library/Frameworks/Python.framework"

PY="$OUT/Library/Frameworks/Python.framework/Versions/Current/bin/python3"
"$PY" -c "import sys, ssl; print('python', sys.version.split()[0], 'at', sys.prefix)"
echo "PYTHON_FRAMEWORK=$OUT/Library/Frameworks/Python.framework"
