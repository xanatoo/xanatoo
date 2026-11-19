// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Implementations of the vendored single-file audio decoders (public domain
// / MIT-0: external/dr_mp3.h, dr_flac.h, dr_wav.h, stb_vorbis.c), kept in
// their own translation unit: stb_vorbis defines short macros, and the
// libraries are compiled without our warnings (CMakeLists.txt).

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
