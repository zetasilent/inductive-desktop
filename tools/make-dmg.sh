#!/usr/bin/env bash
#
# Package the installed Inductive.app as a DMG for testers.
#
#   tools/make-dmg.sh [output.dmg]
#
# Run tools/install-app.sh first: it builds the bundle this packages, with the
# cloud defaults and component libraries inside and a signature on it.
#
# Distribution outside your own Macs needs Apple's notarization, which needs a
# "Developer ID Application" certificate (Apple Developer Program). Set both of
#
#   INDUCTIVE_DIST_IDENTITY  "Developer ID Application: Name (TEAMID)"
#   INDUCTIVE_NOTARY_PROFILE a keychain profile made once with
#                            `xcrun notarytool store-credentials <profile>`
#
# and this script re-signs with the hardened runtime, notarizes and staples.
# Without them the DMG is still built, and says so: macOS will refuse to open it
# on another Mac until the tester approves it in Privacy & Security.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$(cd "$REPO/.." && pwd)/kicad-mac-builder/build/kicad-dest/Inductive.app"
# The release number, with the commit added unless this is the tagged release.
VERSION="$(sed -nE 's/^#define INDUCTIVE_VERSION "([^"]+)".*/\1/p' "$REPO/src/inductive_version.h")"
if ! git -C "$REPO" describe --exact-match --tags "v$VERSION" >/dev/null 2>&1 || [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ]; then
  VERSION="$VERSION-$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || date +%Y%m%d)"
fi
OUT="${1:-$REPO/dist/Inductive-$VERSION.dmg}"

if [ ! -d "$APP/Contents/MacOS" ]; then
  echo "No installed app at $APP. Run tools/install-app.sh first." >&2
  exit 1
fi

if ! codesign --verify --deep --strict "$APP" 2>/dev/null; then
  echo "The installed app's signature does not verify. Run tools/install-app.sh again." >&2
  exit 1
fi

[ -f "$APP/Contents/SharedSupport/inductive/cloud.env" ] \
  || echo "warning: no built-in server address; testers will see 'Not connected'." >&2
[ -d "$APP/Contents/SharedSupport/symbols" ] \
  || echo "warning: no bundled libraries; testers cannot place parts." >&2
grep -q '^INDUCTIVE_UPDATE_CHANNEL=' "$APP/Contents/SharedSupport/inductive/cloud.env" 2>/dev/null \
  || echo "warning: a development build (no update channel); it will not update itself. Build releases with INDUCTIVE_UPDATE_CHANNEL=stable tools/install-app.sh." >&2

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$(dirname "$OUT")"

echo "staging $(du -sh "$APP" | cut -f1) app..."
ditto "$APP" "$STAGE/Inductive.app"
ln -s /Applications "$STAGE/Applications"

NOTARIZE=0
if [ -n "${INDUCTIVE_DIST_IDENTITY:-}" ] && [ -n "${INDUCTIVE_NOTARY_PROFILE:-}" ]; then
  NOTARIZE=1
  echo "signing for distribution: $INDUCTIVE_DIST_IDENTITY"
  # Inner bundles first; the hardened runtime is required for notarization.
  for inner in "$STAGE"/Inductive.app/Contents/Applications/*.app; do
    [ -d "$inner" ] && ! [ -L "$inner" ] || continue
    codesign --force --deep --options runtime --timestamp --sign "$INDUCTIVE_DIST_IDENTITY" "$inner"
  done
  codesign --force --deep --options runtime --timestamp --sign "$INDUCTIVE_DIST_IDENTITY" "$STAGE/Inductive.app"
  codesign --verify --deep --strict "$STAGE/Inductive.app"
fi

# The update every older app installs at launch (tools/publish-update.sh), made
# from the same signed app as the DMG. Only for an exact release version: the
# updater compares x.y.z and accepts nothing else.
if [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  BUILD_ID="macos-$(uname -m)"
  ZIP="$(dirname "$OUT")/Inductive-$VERSION-$BUILD_ID.zip"
  rm -f "$ZIP"
  echo "building $(basename "$ZIP")..."
  ditto -c -k --sequesterRsrc --keepParent "$STAGE/Inductive.app" "$ZIP"
  printf '{"version":"%s","build":"%s","file":"%s","sha256":"%s","size":%s}\n' "$VERSION" "$BUILD_ID" \
    "$(basename "$ZIP")" "$(shasum -a 256 "$ZIP" | cut -d' ' -f1)" "$(stat -f%z "$ZIP")" > "${ZIP%.zip}.json"
  echo "update: $ZIP ($(du -h "$ZIP" | cut -f1)); publish it with tools/publish-update.sh ${ZIP%.zip}.json"
else
  echo "note: $VERSION is not a tagged release, so no update zip was made (tag v<version> on a clean tree)."
fi

rm -f "$OUT"
echo "building $(basename "$OUT")..."
hdiutil create -volname "Inductive" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 \
  -ov "$OUT" >/dev/null

if [ "$NOTARIZE" = 1 ]; then
  codesign --force --sign "$INDUCTIVE_DIST_IDENTITY" --timestamp "$OUT"
  echo "notarizing (this waits for Apple)..."
  xcrun notarytool submit "$OUT" --keychain-profile "$INDUCTIVE_NOTARY_PROFILE" --wait
  xcrun stapler staple "$OUT"
  spctl -a -t open --context context:primary-signature -v "$OUT"
  echo "PASS -- notarized: $OUT ($(du -h "$OUT" | cut -f1))"
else
  echo "built (not notarized): $OUT ($(du -h "$OUT" | cut -f1))"
  echo "On another Mac: open it once, then System Settings > Privacy & Security > Open Anyway."
  echo "Set INDUCTIVE_DIST_IDENTITY and INDUCTIVE_NOTARY_PROFILE to notarize instead."
fi
