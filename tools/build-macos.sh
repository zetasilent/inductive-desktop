#!/usr/bin/env bash
#
# Build Inductive.app from source for Apple Silicon Macs running MIN_MACOS or later.
#
#   tools/build-macos.sh WORKDIR [MIN_MACOS]
#
# Homebrew's libraries are built for the macOS they are installed on, and the app
# bundles them, so the oldest macOS the app runs on is the build machine's. Run it on
# a Mac running MIN_MACOS: the GitHub workflow (.github/workflows/macos.yml) uses a
# macOS 14 machine, so the app runs on macOS 14 and later, which every Apple Silicon
# Mac can run.
#
# Leaves WORKDIR/kicad-mac-builder/build/kicad-dest/Inductive.app, signed ad hoc and
# without server settings: tools/adopt-macos-build.sh adds those, and make-dmg.sh signs
# and notarizes it for distribution.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${1:?usage: build-macos.sh WORKDIR [MIN_MACOS]}"
MIN="${2:-14.0}"
KICAD_TAG=10.0.7
KMB_COMMIT=227de1489ae646b2723723bad99f27c10c07c322   # kicad-mac-builder, as the app was first built with
JOBS="$(sysctl -n hw.ncpu)"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
export MACOSX_DEPLOYMENT_TARGET="$MIN"
step() { echo; echo "=== $* ($(date +%H:%M:%S))"; }

step "Homebrew libraries and tools"
# kicad-mac-builder's list (ci/src/brew_deps.sh), less what a build here does not use
# (doxygen, wget, sentry-cli), plus SWIG for KiCad 10's Python module.
brew install --quiet glew bison opencascade glm boost harfbuzz cairo gettext libgit2 libtool \
  autoconf automake cmake openssl unixodbc ninja protobuf nng zstd libomp jpeg swig
export PATH="$(brew --prefix bison)/bin:$(brew --prefix gettext)/bin:$PATH"

step "sources: kicad-mac-builder, KiCad $KICAD_TAG and this repository's changes"
if [ ! -d "$WORK/kicad-mac-builder" ]; then
  git clone --quiet https://gitlab.com/kicad/packaging/kicad-mac-builder.git "$WORK/kicad-mac-builder"
fi
git -C "$WORK/kicad-mac-builder" checkout --quiet "$KMB_COMMIT"
if [ ! -d "$WORK/kicad" ]; then
  git clone --quiet --depth 1 --branch "$KICAD_TAG" https://gitlab.com/kicad/code/kicad.git "$WORK/kicad"
fi
bash "$REPO/tools/install-into-kicad.sh" "$WORK/kicad"

step "wxWidgets (KiCad's fork) and ngspice, for macOS $MIN"
( cd "$WORK/kicad-mac-builder" && ./build.py --arch arm64 --macos-min-version "$MIN" --build-type Release \
    --kicad-source-dir "$WORK/kicad" --symbols-ref "$KICAD_TAG" --footprints-ref "$KICAD_TAG" \
    --packages3d-ref "$KICAD_TAG" --templates-ref "$KICAD_TAG" --skip-docs-update --jobs "$JOBS" \
    --target wxwidgets ngspice )

step "KiCad $KICAD_TAG's symbol, footprint and template libraries"
LIBS="$WORK/libraries"
for lib in symbols footprints templates; do
  if [ ! -d "$LIBS/src/$lib" ]; then
    git clone --quiet --depth 1 --branch "$KICAD_TAG" "https://gitlab.com/kicad/libraries/kicad-$lib.git" "$LIBS/src/$lib"
  fi
  cmake -S "$LIBS/src/$lib" -B "$LIBS/build/$lib" -DCMAKE_INSTALL_PREFIX="$LIBS/out" > /dev/null
  cmake --build "$LIBS/build/$lib" --target install -j "$JOBS" > /dev/null
done
# On macOS the libraries install under the prefix itself; elsewhere under share/kicad.
LIB_SRC="$LIBS/out"
[ -d "$LIB_SRC/share/kicad/symbols" ] && LIB_SRC="$LIB_SRC/share/kicad"
echo "libraries: $(ls "$LIB_SRC/symbols" | grep -c '\.kicad_sym$') symbol, $(ls -d "$LIB_SRC/footprints"/*.pretty | wc -l | tr -d ' ') footprint"

step "KiCad and Inductive, for macOS $MIN"
INDUCTIVE_MACOS_MIN="$MIN" INDUCTIVE_QA_TESTS=OFF "$REPO/tools/configure-kicad.sh" "$WORK/kicad" "$WORK/kicad-mac-builder"
INDUCTIVE_LIBRARY_SOURCE="$LIB_SRC" "$REPO/tools/install-app.sh" "$WORK/kicad/build/release"

step "check: nothing in the app needs a newer macOS than $MIN"
"$REPO/tools/check-macos-min.sh" "$WORK/kicad-mac-builder/build/kicad-dest/Inductive.app" "$MIN"
