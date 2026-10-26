#!/usr/bin/env bash
#
# Install the built Inductive app over the previous one, safely.
#
#   tools/install-app.sh [/path/to/kicad/build/release]
#
# WHY THIS EXISTS -- read before replacing it with a bare `make install`.
#
# `make install` into this destination is NOT repeatable, and it fails
# DESTRUCTIVELY.  The macOS install step finishes by creating top-level symlinks
# beside the bundle:
#
#     kicad-dest/Inductive PCB.app -> Inductive.app/Contents/Applications/Inductive PCB.app
#
# On the next run, CMake's `file(INSTALL ...)` wants to create a real directory
# at that same path, finds the symlink, and aborts with
#
#     file INSTALL cannot make directory ".../Inductive Schematic.app": File exists.
#
# By then it has already emptied Inductive.app/Contents.  So the second install
# leaves you with no application at all -- not a failed upgrade, a deleted one.
# The first install of a session works; every one after it destroys the bundle.
#
# This script removes those symlinks first, so the install always sees the state
# it expects.  It also takes a copy of the previous bundle before touching
# anything, because a build failure halfway through is otherwise unrecoverable.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$(cd "$REPO/.." && pwd)/kicad/build/release}"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "No KiCad build at $BUILD" >&2
  exit 2
fi

DEST="$(grep -E '^KICAD_BIN:' "$BUILD/CMakeCache.txt" | cut -d= -f2-)"

if [ -z "$DEST" ]; then
  echo "Could not read KICAD_BIN from $BUILD/CMakeCache.txt" >&2
  exit 2
fi

echo "build:       $BUILD"
echo "destination: $DEST"
echo

# Editors hold the bundle's libraries open; installing over a running app gives
# you a half-replaced one and a crash on the next launch.
if pgrep -qx "Inductive" || pgrep -qx "Inductive PCB" || pgrep -qx "Inductive Schematic"; then
  echo "Inductive is running. Quit it first." >&2
  exit 1
fi

# ── back up whatever is there now ─────────────────────────────────────
# Until the new bundle is installed, verified and signed, any way out of this
# script -- a failed build, Ctrl-C, a closed terminal -- puts the backup back. A
# half-written bundle is worse than an old one: macOS kills every launch of it
# ("Code Signature Invalid") without a word. (kill -9 cannot be caught; restore
# by hand with the command printed below.)
INSTALLED=0
BACKUP=""
restore_backup() {
  [ "$INSTALLED" = 1 ] && return 0
  if [ -z "$BACKUP" ] || [ ! -d "$BACKUP/Inductive.app.backup" ]; then
    echo "no backup to restore; $DEST may be incomplete." >&2
    return 1
  fi
  echo "restoring the previous bundle from $(basename "$BACKUP")" >&2
  rm -rf "$DEST" && mv "$BACKUP/Inductive.app.backup" "$BACKUP/Inductive.app" && mv "$BACKUP" "$DEST" || {
    echo "RESTORE FAILED. By hand: rm -rf '$DEST' && mv '$BACKUP/Inductive.app.backup' '$BACKUP/Inductive.app' && mv '$BACKUP' '$DEST'" >&2
    return 1
  }
  # The backup dropped the editors' top-level links (see below); put them back.
  for app in "$DEST"/Inductive.app/Contents/Applications/*.app; do
    [ -d "$app" ] && ln -sfn "Inductive.app/Contents/Applications/$(basename "$app")" "$DEST/$(basename "$app")"
  done
  BACKUP=""
  echo "restored." >&2
}
trap 'echo >&2; echo "interrupted." >&2; restore_backup; exit 130' INT TERM HUP

if [ -d "$DEST/Inductive.app/Contents/MacOS" ]; then
  # .noindex: Spotlight skips the folder. Indexing a gigabyte of copied bundles
  # after every install kept the machine busy and slowed the next launch.
  BACKUP="$DEST-backup-$(date +%Y%m%d-%H%M%S).noindex"
  echo "backing up the current bundle to $(basename "$BACKUP")"
  cp -R "$DEST" "$BACKUP" || { echo "backup failed; not installing" >&2; exit 1; }
  # A copied bundle carries the same bundle id, and Spotlight registers any .app it
  # finds: opening "Inductive" by name then starts whichever copy macOS prefers,
  # which can be a weeks-old backup. Without the .app suffix it is not an app.
  for link in "$BACKUP"/*.app; do [ -L "$link" ] && rm "$link"; done
  mv "$BACKUP/Inductive.app" "$BACKUP/Inductive.app.backup"
else
  echo "no complete bundle to back up (nothing installed, or a previous install failed)"
fi

# ── clear the symlinks that would collide ─────────────────────────────
for app in "$DEST"/*.app; do
  if [ -L "$app" ]; then
    echo "clearing stale symlink $(basename "$app")"
    rm "$app"
  fi
done

# KiCad's own update check would tell Inductive's users about KiCad releases.
# A build tree configured before tools/configure-kicad.sh turned it off keeps it.
if grep -q '^KICAD_UPDATE_CHECK:BOOL=ON' "$BUILD/CMakeCache.txt"; then
  echo "turning off KiCad's update check in this build tree"
  cmake -DKICAD_UPDATE_CHECK=OFF "$BUILD" > /dev/null || { echo "could not reconfigure $BUILD" >&2; exit 1; }
fi

echo
echo "installing..."

BUILD_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 6)"
# KiCad's build copies _pcbnew.kiface to pcbnew/_pcbnew.so from two targets
# (pcbnew_python_module and ScriptingModulesPcbnewSoCopy) with one custom command;
# under -j both run it at once, and one deletes or truncates the file the other is
# copying ("No such file", or install_name_tool's "truncated or malformed object").
# Make that file first, alone, so the parallel install finds it up to date.
if ! { make -C "$BUILD" pcbnew_kiface -j"$BUILD_JOBS" && make -C "$BUILD" pcbnew_python_module -j1; } \
     > /tmp/inductive-install.$$.log 2>&1; then
  echo "BUILD FAILED. Last 20 lines:" >&2
  tail -20 /tmp/inductive-install.$$.log >&2
  echo >&2
  rm -f /tmp/inductive-install.$$.log
  restore_backup
  exit 1
fi
if ! make -C "$BUILD" install -j"$BUILD_JOBS" > /tmp/inductive-install.$$.log 2>&1; then
  echo "INSTALL FAILED. Last 20 lines:" >&2
  tail -20 /tmp/inductive-install.$$.log >&2
  echo >&2
  rm -f /tmp/inductive-install.$$.log
  restore_backup
  exit 1
fi

rm -f /tmp/inductive-install.$$.log

# ── verify what actually landed ───────────────────────────────────────
echo
KIFACE="$DEST/Inductive.app/Contents/PlugIns/_pcbnew.kiface"

if [ ! -f "$KIFACE" ]; then
  echo "FAILED: the board editor did not install." >&2
  restore_backup
  exit 1
fi

# The tool names are wxS() literals, so they are UTF-32 in the binary and
# invisible to a plain `strings`.  Checking for one proves this is a build that
# actually carries the agent, not just a KiCad that launches.
if python3 - "$KIFACE" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
probe = "".join(c + "\0\0\0" for c in "get_board_overview").encode("latin1")
sys.exit(0 if data.count(probe) else 1)
PY
then
  # Finder, Get Info and the server see Inductive's release, not KiCad's
  # development version. Must happen before signing seals the bundles.
  VERSION="$(sed -nE 's/^#define INDUCTIVE_VERSION "([^"]+)".*/\1/p' "$REPO/src/inductive_version.h")"
  for plist in "$DEST/Inductive.app/Contents/Info.plist" "$DEST"/Inductive.app/Contents/Applications/*.app/Contents/Info.plist; do
    [ -f "$plist" ] || continue
    /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" -c "Set :CFBundleVersion $VERSION" "$plist"
  done
  echo "version: $VERSION"

  # A build for an older macOS says so, and an even older Mac then explains instead of
  # failing to start (CMAKE_OSX_DEPLOYMENT_TARGET, set by INDUCTIVE_MACOS_MIN).
  # Given on the command line it is cached as UNINITIALIZED, not STRING.
  MIN_MACOS="$(sed -nE 's/^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]+=(.+)$/\1/p' "$BUILD/CMakeCache.txt")"
  if [ -n "$MIN_MACOS" ]; then
    for plist in "$DEST/Inductive.app/Contents/Info.plist" "$DEST"/Inductive.app/Contents/Applications/*.app/Contents/Info.plist; do
      [ -f "$plist" ] || continue
      /usr/libexec/PlistBuddy -c "Delete :LSMinimumSystemVersion" "$plist" 2> /dev/null || true
      /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $MIN_MACOS" "$plist"
    done
    echo "minimum macOS: $MIN_MACOS"
  fi

  # A downloaded app has no ~/.config/inductive/env, so it needs the server address
  # and sign-in settings in the bundle. Only these four public keys are copied --
  # never anything else in the file. Must happen before signing seals the bundle.
  CLOUD_SRC="${INDUCTIVE_CLOUD_ENV:-$HOME/.config/inductive/env}"
  CLOUD_DEST="$DEST/Inductive.app/Contents/SharedSupport/inductive/cloud.env"
  if [ -f "$CLOUD_SRC" ]; then
    grep -E '^(export )?INDUCTIVE_(API_BASE|GCP_PROJECT_ID|FIREBASE_API_KEY|FIREBASE_AUTH_DOMAIN)=' "$CLOUD_SRC" \
      | sed 's/^export //' > "$CLOUD_DEST"
    echo "cloud defaults: $(cut -d= -f1 "$CLOUD_DEST" | tr '\n' ' ')"
  else
    rm -f "$CLOUD_DEST"
    echo "note: no $CLOUD_SRC; this build has no built-in server address."
  fi

  # A release build updates itself from this channel at launch and requires the
  # newest release (src/inductive_updates.h). A development build has none, so it
  # never replaces itself:   INDUCTIVE_UPDATE_CHANNEL=stable tools/install-app.sh
  if [ -n "${INDUCTIVE_UPDATE_CHANNEL:-}" ]; then
    if ! [[ "$INDUCTIVE_UPDATE_CHANNEL" =~ ^[a-z][a-z0-9-]{0,31}$ ]]; then
      echo "INDUCTIVE_UPDATE_CHANNEL must be a short lowercase name such as stable." >&2
      exit 1
    fi
    mkdir -p "$(dirname "$CLOUD_DEST")"
    echo "INDUCTIVE_UPDATE_CHANNEL=$INDUCTIVE_UPDATE_CHANNEL" >> "$CLOUD_DEST"
    echo "update channel: $INDUCTIVE_UPDATE_CHANNEL (this build updates itself)"
  fi

  # A downloaded app has no KiCad beside it, so it carries the stock symbol and
  # footprint libraries itself (3D models, 4.6 GB, stay out). On first run KiCad
  # creates the user's global tables chained to template/{sym,fp}-lib-table, whose
  # ${KICAD<major>_*_DIR} variables default to this bundle's SharedSupport. The
  # stock tables name the stock app's major version; ours is renamed to match
  # this build's.
  LIB_SRC="${INDUCTIVE_LIBRARY_SOURCE:-/Applications/KiCad/KiCad.app/Contents/SharedSupport}"
  SHARED="$DEST/Inductive.app/Contents/SharedSupport"
  if [ -d "$LIB_SRC/symbols" ] && [ -d "$LIB_SRC/footprints" ] && [ -f "$LIB_SRC/template/sym-lib-table" ]; then
    rsync -a --delete "$LIB_SRC/symbols/" "$SHARED/symbols/"
    rsync -a --delete "$LIB_SRC/footprints/" "$SHARED/footprints/"
    OURS="$(sed -nE 's/.*define KICAD_MAJOR_VERSION[[:space:]]+"([0-9]+)".*/\1/p' "$BUILD/kicad_build_version.h" 2>/dev/null)"
    OURS="${OURS:-10}"
    for table in sym-lib-table fp-lib-table; do
      sed -E "s/\\\$\{KICAD[0-9]+_(SYMBOL|FOOTPRINT)_DIR\}/\${KICAD${OURS}_\1_DIR}/g" \
        "$LIB_SRC/template/$table" > "$SHARED/template/$table"
    done
    echo "libraries: $(ls "$SHARED/symbols" | grep -c '\.kicad_sym$') symbol and $(ls -d "$SHARED/footprints"/*.pretty | wc -l | tr -d ' ') footprint libraries bundled (KICAD${OURS}_*)"
  else
    echo "note: no KiCad libraries at $LIB_SRC; this build ships without symbols and footprints."
  fi

  # An ad-hoc signature is a hash of this exact build, so macOS forgets its privacy
  # approvals (Desktop, Documents) on every install and asks again. Signing with a
  # certificate gives every build the same identity. INDUCTIVE_SIGN_IDENTITY picks
  # one; otherwise the first Apple Development identity in the keychain is used.
  IDENTITY="${INDUCTIVE_SIGN_IDENTITY:-$(security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -1)}"
  if [ -n "$IDENTITY" ]; then
    SIGNLOG="$(mktemp)"
    SIGNED=1
    # --deep does not reach the editor apps under Contents/Applications. Sign them
    # first: re-signing anything inside after the outer bundle would break its seal.
    for app in "$DEST"/Inductive.app/Contents/Applications/*.app; do
      [ -d "$app" ] && ! [ -L "$app" ] || continue
      codesign --force --deep --sign "$IDENTITY" "$app" >>"$SIGNLOG" 2>&1 || SIGNED=0
    done
    if [ "$SIGNED" = 1 ] && codesign --force --deep --sign "$IDENTITY" "$DEST/Inductive.app" >>"$SIGNLOG" 2>&1 \
       && codesign --verify --deep --strict "$DEST/Inductive.app" >>"$SIGNLOG" 2>&1; then
      echo "signed with: $IDENTITY"
    else
      echo "WARNING: signing failed; macOS will ask for folder access again." >&2
      tail -5 "$SIGNLOG" >&2
      # A bundle left with a broken seal does not launch at all; ad hoc it does.
      codesign --force --deep --sign - "$DEST/Inductive.app" >/dev/null 2>&1 \
        && echo "signed ad hoc instead" >&2
    fi
    rm -f "$SIGNLOG"
  else
    echo "note: no code-signing certificate found; macOS will ask for folder access again."
  fi

  # Point Launch Services at this install, so Spotlight and `open -b` start it.
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f -R "$DEST/Inductive.app" >/dev/null 2>&1 || true
  INSTALLED=1
  trap - INT TERM HUP

  # Keep the newest few backups. Older ones go to the Trash (nothing is deleted
  # here): each is a full 1.1 GB bundle, and one per install added up to 95 GB.
  KEEP="${INDUCTIVE_KEEP_BACKUPS:-3}"
  ALL="$(ls -d "$DEST"-backup-* 2>/dev/null | sort)"
  COUNT="$(printf '%s' "$ALL" | grep -c . || true)"
  OLD=""
  [ "$COUNT" -gt "$KEEP" ] && OLD="$(printf '%s\n' "$ALL" | head -n $((COUNT - KEEP)))"
  if [ -n "$OLD" ]; then
    trashed=0
    while IFS= read -r old; do
      target="$HOME/.Trash/$(basename "$old")"
      [ -e "$target" ] && target="$target-$(date +%s)"
      mv "$old" "$target" 2>/dev/null && trashed=$((trashed + 1))
    done <<< "$OLD"
    echo "moved $trashed older backup(s) to the Trash (keeping the newest $KEEP)"
  fi

  echo "PASS -- installed, and the agent tools are present in the bundle."
  echo
  echo "Open Inductive.app for Home, project import and starter circuits."
  echo "The review and change sidebar is available in the editors:"
  echo
  echo "    open \"$DEST/Inductive PCB.app\""
  echo "    open \"$DEST/Inductive Schematic.app\""
else
  echo "FAILED: the installed bundle does not contain the Inductive tools." >&2
  restore_backup
  exit 1
fi
