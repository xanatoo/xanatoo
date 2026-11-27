#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

set -eu

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf '%s\n' 'Usage: bash tools/export-cookie.sh | xanatoo --import-cookie HTTPS-URL' \
        'Paste one cookie name and value from your browser’s developer tools.'
    exit 0
fi

if (( $# != 0 )) || [[ -t 1 ]]; then
    printf '%s\n' 'Pipe this tool to xanatoo --import-cookie HTTPS-URL.' >&2
    exit 1
fi

if ! exec {terminal}<>/dev/tty; then
    printf '%s\n' 'A terminal is required to paste the cookie fields.' >&2
    exit 1
fi

printf 'Cookie name: ' >&"$terminal"
IFS= read -r name <&"$terminal" || exit 1
if [[ -z $name || $name == *'='* ]]; then
    printf '%s\n' 'Enter a single cookie name.' >&2
    exit 1
fi

printf 'Cookie value (hidden): ' >&"$terminal"
if ! IFS= read -rs value <&"$terminal"; then
    printf '\n' >&"$terminal"
    exit 1
fi
printf '\n' >&"$terminal"
printf '%s=%s\n' "$name" "$value"
