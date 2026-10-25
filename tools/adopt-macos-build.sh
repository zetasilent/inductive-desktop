#!/usr/bin/env bash
#
# Make the GitHub workflow's app (built on macOS 14 for macOS 14 and later, unsigned)
# the app tools/make-dmg.sh signs: unpack it, add the release settings, check it.
#
#   tools/adopt-macos-build.sh Inductive-macos14-arm64.zip CLOUD_ENV [DEST]
#
# CLOUD_ENV holds the release settings: the four public INDUCTIVE_* keys a downloaded
# app needs to reach Inductive (as in an installed app's
# Contents/SharedSupport/inductive/cloud.env). Only those keys are copied, and the
# update channel is set to stable. DEST defaults to kicad-mac-builder/build/kicad-dest-ci
# beside this repository, and must not exist yet. Then sign and notarize it with
#
#   INDUCTIVE_APP=<DEST>/Inductive.app INDUCTIVE_DIST_IDENTITY=… INDUCTIVE_NOTARY_PROFILE=… tools/make-dmg.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZIP="${1:?usage: adopt-macos-build.sh ARTIFACT.zip CLOUD_ENV [DEST]}"
ENV_FILE="${2:?usage: adopt-macos-build.sh ARTIFACT.zip CLOUD_ENV [DEST]}"
DEST="${3:-$(cd "$REPO/.." && pwd)/kicad-mac-builder/build/kicad-dest-ci}"

[ -f "$ZIP" ] || { echo "no $ZIP" >&2; exit 1; }
[ -f "$ENV_FILE" ] || { echo "no $ENV_FILE" >&2; exit 1; }
[ -e "$DEST" ] && { echo "$DEST exists; move it aside first" >&2; exit 1; }

mkdir -p "$DEST"
ditto -x -k "$ZIP" "$DEST"
APP="$DEST/Inductive.app"
[ -d "$APP/Contents/MacOS" ] || { echo "$ZIP has no Inductive.app" >&2; exit 1; }
xattr -cr "$APP"   # the download's quarantine mark: this copy is signed here, not run as downloaded

SETTINGS="$APP/Contents/SharedSupport/inductive/cloud.env"
mkdir -p "$(dirname "$SETTINGS")"
grep -E '^(export )?INDUCTIVE_(API_BASE|GCP_PROJECT_ID|FIREBASE_API_KEY|FIREBASE_AUTH_DOMAIN)=' "$ENV_FILE" \
  | sed 's/^export //' > "$SETTINGS"
[ "$(wc -l < "$SETTINGS" | tr -d ' ')" = 4 ] || { echo "$ENV_FILE does not hold the four INDUCTIVE_* settings" >&2; exit 1; }
echo "INDUCTIVE_UPDATE_CHANNEL=stable" >> "$SETTINGS"

# The settings changed the bundle: seal it again (ad hoc; make-dmg.sh signs it properly),
# the editors inside first.
for app in "$APP"/Contents/Applications/*.app; do
  [ -d "$app" ] && ! [ -L "$app" ] || continue
  codesign --force --deep --sign - "$app"
done
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

"$REPO/tools/check-macos-min.sh" "$APP"
echo "adopted: $APP, version $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")," \
     "macOS $(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist") and later"
