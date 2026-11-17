#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#
# publish-build.sh TAG TARGET DIR      (nix run .#publish-build -- …)
#
# Replace TARGET's assets on the rolling GitHub release TAG with the files
# in DIR, the output of `nix build .#release-TARGET`: the download(s) plus
# TARGET.json (version, rev, revCount, lastModified). Every other target is
# left alone; a build that fails never reaches this script, so its previous
# assets stay, and the website (fetch-builds.sh) serves whatever is there.
#
# Each file is uploaded under a temporary name first, then the old asset
# is deleted and the new one renamed, so a failed upload keeps the old
# file. TARGET.json goes last: it marks the target as complete. A build of
# an older commit (lower revCount) than the published one is skipped, so
# overlapping runs cannot roll a target back. Asset names carry the
# version ("file" in TARGET.json), so the previous build's file is deleted
# once the new TARGET.json is up.
#
# Needs GH_TOKEN (contents: write) and GITHUB_REPOSITORY.
set -euo pipefail

tag=$1 target=$2 dir=$3
repo=${GITHUB_REPOSITORY:?}
meta="$dir/$target.json"
test -s "$meta" || { echo "publish-build: no $meta" >&2; exit 1; }
cat "$meta"
rev_count=$(jq -r '.revCount // 0' "$meta")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Create the release on first use. Parallel jobs may race on this.
if ! gh release view "$tag" -R "$repo" >/dev/null 2>&1; then
  gh release create "$tag" -R "$repo" --prerelease --target "${GITHUB_SHA:-master}" \
    --title "Latest builds" \
    --notes "Rolling builds from master, one target at a time: each asset is \
replaced only when its own build succeeds, so targets can come from \
different commits. <target>.json names the version of each." \
    || gh release view "$tag" -R "$repo" >/dev/null
fi

assets() { gh api "repos/$repo/releases/tags/$tag" --jq '.assets[] | "\(.id) \(.name)"'; }
asset_id() { assets | awk -v n="$1" '$2 == n { print $1; exit }'; }

# The previously published file: "file" in the old TARGET.json, or the
# fixed name used before file names carried the version.
legacy_file() {
  case $1 in
    android) echo xanatoo.apk ;;
    wasm) echo xanatoo-wasm.tar.gz ;;
    *) echo "xanatoo-$1.zip" ;;
  esac
}
old_file=
if [ -n "$(asset_id "$target.json")" ]; then
  old_meta=$(curl -fsSL "https://github.com/$repo/releases/download/$tag/$target.json") \
    || old_meta='{}'
  old_count=$(jq -r '.revCount // 0' <<<"$old_meta")
  if [ "$old_count" -gt "$rev_count" ]; then
    echo "Release has $target from revCount $old_count, newer than $rev_count: skipping."
    exit 0
  fi
  old_file=$(jq -r --arg f "$(legacy_file "$target")" '.file // $f' <<<"$old_meta")
fi

replace() {
  local file=$1 name tmp new_id old
  name=$(basename "$file")
  tmp="$work/next-${GITHUB_RUN_ID:-local}-$name"
  cp -L "$file" "$tmp"
  gh release upload "$tag" -R "$repo" --clobber "$tmp"
  new_id=$(asset_id "$(basename "$tmp")")
  old=$(asset_id "$name")
  if [ -n "$old" ]; then
    gh api -X DELETE "repos/$repo/releases/assets/$old"
  fi
  gh api -X PATCH "repos/$repo/releases/assets/$new_id" -f name="$name" >/dev/null
  rm -f "$tmp"
  echo "Published $name"
}

for f in "$dir"/*; do
  [ "$(basename "$f")" = "$target.json" ] || replace "$f"
done
replace "$meta"

new_file=$(jq -r '.file // empty' "$meta")
if [ -n "$old_file" ] && [ "$old_file" != "$new_file" ]; then
  old=$(asset_id "$old_file")
  if [ -n "$old" ]; then
    gh api -X DELETE "repos/$repo/releases/assets/$old"
    echo "Removed $old_file"
  fi
fi
