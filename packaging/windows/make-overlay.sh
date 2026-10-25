#!/usr/bin/env bash
#
# The Windows build's source, made on macOS or Linux: KiCad 10.0.7 with this repository's
# patches and sources applied, as one patch, plus this repository's public part as a
# tarball. Copy both to the build machine; vm/prepare-tree.ps1 applies them.
#
#   packaging/windows/make-overlay.sh OUTDIR [KICAD_REPO]
#
# Writes OUTDIR/overlay.patch and OUTDIR/inductive.tar. KICAD_REPO defaults to KiCad's
# GitLab; a local clone that has the 10.0.7 tag is faster.
#
# From the private repo, only the public export goes in (tools/export-public.sh), so the
# Windows build never sees private code; from a public checkout, the checkout itself.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:?usage: make-overlay.sh OUTDIR [KICAD_REPO]}"
KICAD_REPO="${2:-https://gitlab.com/kicad/code/kicad.git}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -x "$REPO/tools/export-public.sh" ]; then
  "$REPO/tools/export-public.sh" "$WORK/export" > /dev/null
  SRC="$WORK/export"
else
  SRC="$REPO"
fi

case "$KICAD_REPO" in /*) KICAD_REPO="file://$KICAD_REPO" ;; esac
git clone --quiet --depth 1 --branch 10.0.7 "$KICAD_REPO" "$WORK/kicad" 2> /dev/null
bash "$SRC/tools/install-into-kicad.sh" "$WORK/kicad" > /dev/null

# Windows checkouts make links into text files: ours become copies of what they point
# at. KiCad's own tracked links (qa/tests/resources) stay as they are.
cd "$WORK/kicad"
tracked="$(git ls-files -s | awk '$1=="120000" { print $4 }')"
find . -type l -not -path "./.git/*" | sed 's#^\./##' | while read -r link; do
  grep -qxF "$link" <<< "$tracked" && continue
  target="$(cd "$(dirname "$link")" && cd "$(dirname "$(readlink "$(basename "$link")")")" && pwd)/$(basename "$(readlink "$link")")"
  rm "$link"
  cp -R "$target" "$link"
done
git add -A
git diff --cached --binary > "$OUT/overlay.patch"
tar -C "$SRC" -cf "$OUT/inductive.tar" .
echo "overlay: $(grep -c '^diff --git' "$OUT/overlay.patch") files ($(du -h "$OUT/overlay.patch" | cut -f1)), $OUT/inductive.tar ($(du -h "$OUT/inductive.tar" | cut -f1))"
