<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com> -->

# Fallback fonts for portable builds

The four DejaVu TTF files here are used by the initial Windows and
WebAssembly builds when FontConfig is unavailable. Their own license is
provided in `LICENSE-DejaVu.txt`; they are not covered by Xanatóo's GPL.

On native Linux, normal font discovery still uses FontConfig and the installed
system fonts. Other systems can supply a font with `--font`.

`latinmodern-math.otf` (Latin Modern Math 1.959, GUST e-foundry) is the TeX
math font (`find_math_font_path`, `docs/MATH.md`). It is installed by every
build and the Android / R36S packages, because Android and the minimal ports
have no FontConfig to find one. GUST Font License (LPPL 1.3c), see
`LICENSE-LatinModernMath.txt`; distributed unmodified.
