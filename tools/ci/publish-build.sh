#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#
# publish-build.sh TAG TARGET FILE...
#
# Replace TARGET's assets on the rolling GitHub release TAG with FILE...
# plus TARGET.json (version metadata), leaving every other target alone.
# A build that fails never reaches this script, so its previous assets
# stay; the website (xanatoo.github.io) assembles whatever is there.
#
# Each file is uploaded under a temporary name first, then the old asset
# is deleted and the new one renamed, so a failed upload keeps the old
# file. TARGET.json goes last: it marks the target as complete. A build of
# an older commit (lower revCount) than the published one is skipped, so
# overlapping runs cannot roll a target back.
#
# Needs: gh (GH_TOKEN with contents: write), curl, jq, nix, GITHUB_REPOSITORY.
set -euo pipefail

tag=$1 target=$2
shift 2
repo=${GITHUB_REPOSITORY:?}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

meta=$(nix flake metadata --json)
rev_count=$(jq -r '.revCount // 0' <<<"$meta")
jq -n \
  --arg target "$target" \
  --arg version "$(nix eval --raw .#xanatoo-unwrapped.version)" \
  --arg rev "$(jq -r '.revision // ""' <<<"$meta")" \
  --argjson revCount "$rev_count" \
  --argjson lastModified "$(jq -r '.lastModified // 0' <<<"$meta")" \
  --arg built "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  '{$target, $version, $rev, $revCount, $lastModified, $built}' \
  > "$work/$target.json"
cat "$work/$target.json"

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

old_id=$(asset_id "$target.json")
if [ -n "$old_id" ]; then
  old_count=$(curl -fsSL "https://github.com/$repo/releases/download/$tag/$target.json" \
    | jq -r '.revCount // 0') || old_count=0
  if [ "$old_count" -gt "$rev_count" ]; then
    echo "Release has $target from revCount $old_count, newer than $rev_count: skipping."
    exit 0
  fi
fi

replace() {
  local file=$1 name
  name=$(basename "$file")
  local tmp="$work/upload/next-${GITHUB_RUN_ID:-local}-$name"
  mkdir -p "$work/upload"
  cp -L "$file" "$tmp"
  gh release upload "$tag" -R "$repo" --clobber "$tmp"
  local new_id old
  new_id=$(asset_id "$(basename "$tmp")")
  old=$(asset_id "$name")
  if [ -n "$old" ]; then
    gh api -X DELETE "repos/$repo/releases/assets/$old"
  fi
  gh api -X PATCH "repos/$repo/releases/assets/$new_id" -f name="$name" >/dev/null
  rm -f "$tmp"
  echo "Published $name"
}

for f in "$@"; do replace "$f"; done
replace "$work/$target.json"
