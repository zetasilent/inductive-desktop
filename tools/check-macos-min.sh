#!/usr/bin/env bash
#
# Fail if any program or library in an app needs a newer macOS than it claims to support.
#
#   tools/check-macos-min.sh path/to/Inductive.app [MIN_MACOS]
#
# MIN_MACOS defaults to the app's LSMinimumSystemVersion. Every Mach-O file's minimum
# (LC_BUILD_VERSION) must be at or below it: one library built for a newer macOS, as a
# Homebrew library is for the macOS it was installed on, stops the app on older Macs.
set -euo pipefail

APP="${1:?usage: check-macos-min.sh Inductive.app [MIN_MACOS]}"
MIN="${2:-$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist" 2>/dev/null || true)}"
[ -n "$MIN" ] || { echo "no MIN_MACOS given and no LSMinimumSystemVersion in $APP" >&2; exit 2; }

newer=0 files=0
while IFS= read -r -d '' f; do
  file -b "$f" | grep -q '^Mach-O' || continue
  files=$((files + 1))
  need="$(otool -l "$f" 2>/dev/null | awk '/LC_BUILD_VERSION/{b=1} b && /minos/{print $2; exit}')"
  [ -n "$need" ] || continue
  if [ "$(printf '%s\n%s\n' "$MIN" "$need" | sort -V | tail -1)" != "$MIN" ]; then
    echo "needs macOS $need: ${f#"$APP"/}"
    newer=$((newer + 1))
  fi
done < <(find "$APP" -type f -print0)

if [ "$newer" -gt 0 ]; then
  echo "FAILED: $newer of $files code files need a newer macOS than $MIN" >&2
  exit 1
fi
echo "all $files code files run on macOS $MIN"
