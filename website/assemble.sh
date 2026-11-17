#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#
# assemble.sh SRC OUT
#
# Build the site in OUT from the page sources next to this script
# (index.html, images/) and the builds in SRC (see fetch-builds.sh):
#   versions/{android,r36s,win32,win64,wasm}.json, each naming its
#   download in "file", those files, and wasm/ (the browser edition).
# Used by `nix run .#site-from-release` (CI: builds from the release) and
# by `nix build .#site` (every build from this checkout), see nix/site.nix.
set -euo pipefail

src=$1 out=$2
site=$(dirname "$(readlink -f "$0")")
targets=(android r36s win64 win32 wasm)

file() { jq -r '.file // empty' "$src/versions/$1.json"; }
for t in "${targets[@]}"; do
  test -s "$src/versions/$t.json" || { echo "assemble: missing $src/versions/$t.json" >&2; exit 1; }
  test -n "$(file "$t")" || { echo "assemble: $t.json names no file" >&2; exit 1; }
done
for f in "$(file android)" "$(file r36s)" "$(file win32)" "$(file win64)" \
         wasm/index.html wasm/xanatoo.js wasm/xanatoo.wasm wasm/xanatoo.data; do
  test -s "$src/$f" || { echo "assemble: missing $src/$f" >&2; exit 1; }
done

mkdir -p "$out/downloads/r36s" "$out/downloads/versions" "$out/wasm"
cp -r "$site/images" "$out/images"
# Versioned names (from <target>.json), so downloads never collide; the
# page links them through @file-<target>@.
for t in android win64 win32; do
  cp "$src/$(file "$t")" "$out/downloads/$(file "$t")"
done
# PortMaster expects the zip under the name its port.json gives.
cp "$src/$(file r36s)" "$out/downloads/r36s/xanatoo.zip"
# The Emscripten index.html loads relative .js / .wasm / .data files.
cp -r "$src/wasm/." "$out/wasm/"

ver() { jq -r '.version // "unknown"' "$src/versions/$1.json"; }
for t in "${targets[@]}"; do
  cp "$src/versions/$t.json" "$out/downloads/versions/$t.json"
done

# The app's update check compares versionCode with its own (the commit
# time of the APK's build), so it must come from the APK's own build.
jq '{versionCode: (.lastModified // 1), versionName: .version}' \
  "$src/versions/android.json" > "$out/downloads/android-update.json"

# Headline version: the newest build (highest revCount) of any target.
newest=$(jq -rs 'max_by(.revCount // 0) | .version // "unknown"' "$src"/versions/*.json)
same=$(jq -rs 'map(.version) | unique | length' "$src"/versions/*.json)
if [ "$same" = 1 ]; then
  note="every file below is built from the same revision."
else
  note="each file below shows the revision it was built from; a build that failed keeps its previous version."
fi

sed -e "s|@version@|$newest|g" \
    -e "s|@version-note@|$note|g" \
    -e "s|@version-android@|$(ver android)|g" \
    -e "s|@version-r36s@|$(ver r36s)|g" \
    -e "s|@version-win64@|$(ver win64)|g" \
    -e "s|@version-win32@|$(ver win32)|g" \
    -e "s|@version-wasm@|$(ver wasm)|g" \
    -e "s|@file-android@|$(file android)|g" \
    -e "s|@file-win64@|$(file win64)|g" \
    -e "s|@file-win32@|$(file win32)|g" \
    "$site/index.html" > "$out/index.html"
if grep -q -E '@(version|file)' "$out/index.html"; then
  echo "assemble: unreplaced placeholder in index.html" >&2; exit 1
fi
chmod -R u+w "$out"
