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
#   INDUCTIVE_DIST_IDENTITY  the Developer ID Application certificate's SHA-1
#                            (security find-identity -v -p codesigning)
#   INDUCTIVE_NOTARY_PROFILE a keychain profile made once with
#                            `xcrun notarytool store-credentials <profile>`
#
# and this script re-signs with the hardened runtime, notarizes and staples.
# Without them the DMG is still built, and says so: macOS will refuse to open it
# on another Mac until the tester approves it in Privacy & Security.
#
# INDUCTIVE_REPACKAGE_DMG=dist/Inductive-X.Y.Z.dmg makes a new DMG around the app in
# that one, as it is (already signed and notarized): for a change to the DMG itself.
# No update zip is made, as the app has not changed.
#
# The DMG opens on a window that says what to do: Inductive on the left, an arrow, and
# Applications on the right (packaging/macos/dmg-background.swift, laid out by
# tools/dmg-window.py without Finder).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The app to package: this Mac's build, or INDUCTIVE_APP (tools/adopt-macos-build.sh's).
APP="${INDUCTIVE_APP:-$(cd "$REPO/.." && pwd)/kicad-mac-builder/build/kicad-dest/Inductive.app}"
# The release number, with the commit added unless this is the tagged release.
VERSION="$(sed -nE 's/^#define INDUCTIVE_VERSION "([^"]+)".*/\1/p' "$REPO/src/inductive_version.h")"
if ! git -C "$REPO" describe --exact-match --tags "v$VERSION" >/dev/null 2>&1 || [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ]; then
  VERSION="$VERSION-$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || date +%Y%m%d)"
fi
OUT="${1:-$REPO/dist/Inductive-$VERSION.dmg}"

STAGE="$(mktemp -d)"
WORK="$(mktemp -d)"
trap 'rm -rf "$STAGE" "$WORK"' EXIT
mkdir -p "$(dirname "$OUT")"

REPACKAGE="${INDUCTIVE_REPACKAGE_DMG:-}"
if [ -n "$REPACKAGE" ]; then
  MOUNTED="$(hdiutil attach -nobrowse -readonly -noautoopen "$REPACKAGE" | awk -F'\t' '/\/Volumes\//{ print $NF }' | tail -1)"
  APP="$WORK/from-dmg/Inductive.app"
  ditto "$MOUNTED/Inductive.app" "$APP"
  hdiutil detach -quiet "$MOUNTED"
  syspolicy_check distribution "$APP" > /dev/null || { echo "The app in $REPACKAGE is not notarized." >&2; exit 1; }
fi

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

chmod 755 "$STAGE"   # the DMG's top folder, as people see it
echo "staging $(du -sh "$APP" | cut -f1) app..."
ditto "$APP" "$STAGE/Inductive.app"
ln -s /Applications "$STAGE/Applications"

NOTARIZE=0
if [ -n "${INDUCTIVE_DIST_IDENTITY:-}" ] && [ -n "${INDUCTIVE_NOTARY_PROFILE:-}" ]; then
  NOTARIZE=1
fi
if [ "$NOTARIZE" = 1 ] && [ -z "$REPACKAGE" ]; then
  echo "signing for distribution: $INDUCTIVE_DIST_IDENTITY"
  "$REPO/tools/sign-for-distribution.sh" "$STAGE/Inductive.app" "$INDUCTIVE_DIST_IDENTITY"
  # The app gets its own notarization ticket, so a first launch after dragging it out
  # of the DMG passes Gatekeeper even offline (the DMG's ticket covers only the DMG).
  echo "notarizing the app (this waits for Apple)..."
  ditto -c -k --keepParent "$STAGE/Inductive.app" "$STAGE/app-notarize.zip"
  xcrun notarytool submit "$STAGE/app-notarize.zip" --keychain-profile "$INDUCTIVE_NOTARY_PROFILE" --wait
  rm -f "$STAGE/app-notarize.zip"
  xcrun stapler staple "$STAGE/Inductive.app"
  # Apple's current distribution check. spctl's execute assessment rejects this app
  # (its editors are apps nested inside it) although Gatekeeper opens it normally.
  syspolicy_check distribution "$STAGE/Inductive.app"
fi

# The update every older app installs at launch (tools/publish-update.sh), made
# from the same signed app as the DMG. Only for an exact release version: the
# updater compares x.y.z and accepts nothing else.
if [ -n "$REPACKAGE" ]; then
  echo "note: repackaging the app from $(basename "$REPACKAGE"), so no update zip was made."
elif [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
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

# The window the DMG opens on. The background is drawn at 1x and 2x and joined into one
# TIFF, so it is sharp on Retina screens; the DMG wears the app's icon.
mkdir "$STAGE/.background"
swift "$REPO/packaging/macos/dmg-background.swift" "$WORK/background.png" 1
swift "$REPO/packaging/macos/dmg-background.swift" "$WORK/background@2x.png" 2
tiffutil -cathidpicheck "$WORK/background.png" "$WORK/background@2x.png" -out "$STAGE/.background/background.tiff" > /dev/null 2>&1
cp "$STAGE/Inductive.app/Contents/Resources/inductive.icns" "$STAGE/.VolumeIcon.icns"

# Laid out on a writable copy: tools/dmg-window.py writes the .DS_Store Finder reads
# (no Finder automation). Icon positions are the centres the background's arrow joins.
echo "laying out the DMG window..."
hdiutil create -volname "Inductive" -srcfolder "$STAGE" -fs HFS+ -format UDRW -ov "$WORK/layout.dmg" > /dev/null
ATTACHED="$(hdiutil attach -readwrite -noverify -noautoopen -nobrowse "$WORK/layout.dmg")"
DEVICE="$(awk '/Apple_HFS/{ print $1 }' <<< "$ATTACHED")"
VOLUME="$(awk -F'\t' '/\/Volumes\//{ print $NF }' <<< "$ATTACHED" | tail -1)"
trap 'hdiutil detach -quiet "$DEVICE" 2>/dev/null; rm -rf "$STAGE" "$WORK"' EXIT
SetFile -a C "$VOLUME"
python3 "$REPO/tools/dmg-window.py" "$VOLUME" --background .background/background.tiff \
  --window 660x428 --icon-size 128 --text-size 13 \
  --item "Inductive.app=180,200" --item "Applications=480,200"
sync
hdiutil detach -quiet "$DEVICE"
trap 'rm -rf "$STAGE" "$WORK"' EXIT

rm -f "$OUT"
echo "building $(basename "$OUT")..."
hdiutil convert "$WORK/layout.dmg" -format UDZO -imagekey zlib-level=9 -ov -o "$OUT" > /dev/null

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
