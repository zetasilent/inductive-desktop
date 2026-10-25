#!/usr/bin/env bash
#
# Sign Inductive.app for distribution: Developer ID, hardened runtime, secure
# timestamp, KiCad's own entitlements (packaging/macos/entitlements.plist).
#
#   tools/sign-for-distribution.sh path/to/Inductive.app "IDENTITY"
#
# IDENTITY is the certificate's SHA-1 (`security find-identity -v -p codesigning`):
# a name can match a second certificate in another keychain.
#
# Inside out, as Apple asks (never --deep, which misses code outside the standard
# places, such as the bundled Python's modules), and in two codesign runs: every
# code file at once, then every bundle, innermost first. macOS asks for the signing
# key once per codesign run, so a release is a few "Allow"s, not one per file.
set -euo pipefail

APP="${1:?usage: sign-for-distribution.sh Inductive.app IDENTITY}"
IDENTITY="${2:?usage: sign-for-distribution.sh Inductive.app IDENTITY}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENTITLEMENTS="$REPO/packaging/macos/entitlements.plist"
SIGN=(codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" --sign "$IDENTITY")

LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT

# 0. KiCad's editors inside the app reach its shared libraries through a link,
#    Contents/Frameworks -> ../../../Frameworks, that points out of their own bundle;
#    Gatekeeper rejects such a bundle. Point their library search path at the app's
#    Frameworks instead and drop the link. KiCad itself finds the main bundle without
#    it (getOSXBundleRoot), so only the dynamic linker used it.
for nested in "$APP"/Contents/Applications/*.app; do
  [ -d "$nested" ] && [ ! -L "$nested" ] || continue
  exe="$nested/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$nested/Contents/Info.plist")"
  for rpath in "@executable_path/../../../../Frameworks" "@executable_path/../../../../Frameworks/Python.framework"; do
    otool -l "$exe" | grep -q "path $rpath " || install_name_tool -add_rpath "$rpath" "$exe" 2>/dev/null
  done
  [ -L "$nested/Contents/Frameworks" ] && rm "$nested/Contents/Frameworks"
done

# 1. Every Mach-O file (executables, dylibs, Python modules, kifaces). Shared
#    support is data only: symbols, footprints, scripts.
find "$APP" -path "$APP/Contents/SharedSupport" -prune -o -type f -print0 \
  | while IFS= read -r -d '' f; do
      if file -b "$f" | grep -q '^Mach-O'; then printf '%s\0' "$f"; fi
    done > "$LIST"
echo "signing $(tr -cd '\0' < "$LIST" | wc -c | tr -d ' ') code files..."
xargs -0 "${SIGN[@]}" < "$LIST"

# 2. Every bundle, innermost first: a nested bundle's path is longer than its parent's.
BUNDLES=()
while IFS= read -r bundle; do BUNDLES+=("$bundle"); done < <(
  find "$APP/Contents" -type d \( -name '*.app' -o -name '*.framework' -o -name '*.bundle' \) -print \
    | awk '{ print length($0) "\t" $0 }' | sort -rn | cut -f2-)
echo "signing ${#BUNDLES[@]} bundles and the app..."
"${SIGN[@]}" "${BUNDLES[@]}" "$APP"

codesign --verify --deep --strict "$APP"
echo "signed: $APP"
