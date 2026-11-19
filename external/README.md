# external

Vendored third-party sources (not part of glyphtoo itself).

- `stb_image.h` — public domain image loader by Sean Barrett
  (https://github.com/nothings/stb)
- `dr_mp3.h` 0.7.5, `dr_flac.h` 0.13.5, `dr_wav.h` 0.14.7 — MP3 / FLAC / WAV
  decoders by David Reid, public domain or MIT-0
  (https://github.com/mackron/dr_libs, master 7f032b9)
- `stb_vorbis.c` 1.25 — Ogg Vorbis decoder by Sean Barrett, public domain or
  MIT (https://github.com/nothings/stb, master 033efa1); the version is also
  in `audio_decoder_versions()` (src/audio.cpp)
