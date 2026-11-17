#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#
# fetch-builds.sh DIR
#
# Fill DIR with the newest build of every target, in the layout
# assemble.sh reads: versions/<target>.json, which names the target's
# download in "file" (e.g. xanatoo-<version>-android-universal.apk), that
# file in DIR, and wasm/ (the unpacked browser edition).
#
# Builds come from the rolling `continuous` release of xanatoo/xanatoo
# (publish-build.sh), where each target is replaced only when its own
# build succeeds. A target
# the release lacks (or that fails to download) is copied from the live
# site instead, so one missing build never takes a download offline.
set -euo pipefail

dir=$1
release=${XANATOO_RELEASE:-https://github.com/xanatoo/xanatoo/releases/download/continuous}
site=${XANATOO_SITE:-https://xanatoo.github.io}
mkdir -p "$dir/versions"

get() { curl -fsSL --retry 3 -o "$2" "$1"; }

# target  file name before <target>.json had "file"  its live-site path
targets=(
  "android xanatoo.apk         downloads/xanatoo.apk"
  "r36s    xanatoo-r36s.zip    downloads/r36s/xanatoo.zip"
  "win64   xanatoo-win64.zip   downloads/xanatoo-win64.zip"
  "win32   xanatoo-win32.zip   downloads/xanatoo-win32.zip"
  "wasm    xanatoo-wasm.tar.gz wasm/"
)

# Write the file name into a version record that predates "file".
with_file() { jq --arg f "$2" '.file //= $f' "$1" > "$1.tmp" && mv "$1.tmp" "$1"; }

from_release() {
  local target=$1 legacy=$2 tmp asset
  tmp=$(mktemp -d)
  get "$release/$target.json" "$tmp/$target.json" || { rm -rf "$tmp"; return 1; }
  with_file "$tmp/$target.json" "$legacy"
  asset=$(jq -r .file "$tmp/$target.json")
  get "$release/$asset" "$tmp/$asset" || { rm -rf "$tmp"; return 1; }
  if [ "$target" = wasm ]; then
    mkdir "$tmp/wasm"
    # Older tarballs carry the Nix store's read-only modes, and a read-only
    # directory can neither be moved to another parent nor removed.
    if ! tar -C "$tmp/wasm" -xzf "$tmp/$asset"; then
      chmod -R u+w "$tmp"; rm -rf "$tmp"; return 1
    fi
    chmod -R u+w "$tmp/wasm"
    if [ -d "$dir/wasm" ]; then chmod -R u+w "$dir/wasm"; rm -rf "$dir/wasm"; fi
    mv "$tmp/wasm" "$dir/wasm"
  else
    mv "$tmp/$asset" "$dir/$asset"
  fi
  mv "$tmp/$target.json" "$dir/versions/$target.json"
  rm -rf "$tmp"
}

# The live site serves downloads/<file> (r36s: downloads/r36s/xanatoo.zip);
# sites from before "file" serve the legacy path.
from_site() {
  local target=$1 legacy=$2 path=$3 json=$dir/versions/$1.json file
  get "$site/downloads/versions/$target.json" "$json" \
    || legacy_version "$target" > "$json"
  with_file "$json" "$legacy"
  if [ "$target" = wasm ]; then
    mkdir -p "$dir/wasm"
    for f in index.html xanatoo.js xanatoo.wasm xanatoo.data; do
      get "$site/wasm/$f" "$dir/wasm/$f"
    done
    # Builds from before the isolation shim (mk/wasm/coi-sw.js) have none.
    get "$site/wasm/coi-sw.js" "$dir/wasm/coi-sw.js" || rm -f "$dir/wasm/coi-sw.js"
    return
  fi
  file=$(jq -r .file "$json")
  if [ "$target" != r36s ] && [ "$file" != "$legacy" ]; then
    path=downloads/$file
  fi
  get "$site/$path" "$dir/$file"
}

# Sites deployed before versions/ existed built every file from one
# revision, the one their Android update manifest names.
legacy_version() {
  curl -fsSL --retry 3 "$site/downloads/android-update.json" \
    | jq --arg t "$1" '{target: $t, version: .versionName, lastModified: .versionCode}' \
    || jq -n --arg t "$1" '{target: $t, version: "unknown"}'
}

failed=0
for t in "${targets[@]}"; do
  read -r target legacy path <<<"$t"
  if from_release "$target" "$legacy"; then
    echo "$target: release, $(jq -r .version "$dir/versions/$target.json")"
  elif from_site "$target" "$legacy" "$path"; then
    echo "::warning::$target: not on the release, kept the live site's copy"
  else
    echo "::error::$target: neither the release nor the live site has it"
    failed=1
  fi
done
exit $failed
