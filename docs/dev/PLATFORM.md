# Platforms, ports and versioning

Developer notes split out of `AGENTS.md`: versioning, Windows / Emscripten
ports, the SDL3 transition and Android.

## Versioning

- gamepkgstd `VERSIONING.md` (`~/projects/gamepkgstd`): the top-level
  `VERSION` file is the only place the version is written; on master it
  always ends in `-dev`. Nix builds report `<VERSION>.<revCount>+g<rev>`
  through `-DPROJECT_VERSION_FULL` → `XANATOO_VERSION` (`--version`,
  `about:`, Android `versionName`). Never hard-code a version elsewhere.

## Ports and SDL3

- Windows and Emscripten default to `XANATOO_MINIMAL_PORT=ON` (offline
  SDL2/FreeType builds without FontConfig, SQLite, TLS clients or thumtoo).
  The web main loop uses `emscripten_set_main_loop_arg` and preloaded `/data`;
  Windows paths come from SDL. Maintain the feature guards and bundled font
  fallback. The authoritative cross-target packages are in `flake.nix` with
  `nix/windows.nix` and `nix/wasm.nix`. GitHub must build these exact flake
  outputs, never an independent CMake recipe. Build and limitations:
  `docs/PORTS.md`.
- SDL3 transition (in progress): Win32/Win64 flake packages use nixpkgs'
  SDL2-compatible API over an SDL3 runtime, explicitly packaging SDL3.dll.
  The app and vendored ImGui still compile against SDL2 headers; WASM,
  Android, and the default R36S port currently retain SDL2. Do not change
  their ABI until the SDL3 API/backend migration and platform tests are
  complete. First-party render, window, surface, time, mouse, input events,
  gamepads, cursor and SDL diagnostic operations now use `xanatoo/sdl.hpp`
  (switch `XANATOO_SDL3`); `checks.sdl3-api` compiles this
  bridge with real SDL3 headers. `-DXANATOO_SDL3=ON` selects an experimental
  first-party ImGui platform/renderer backend (`src/imgui_sdl3.cpp`) and the
  flake's separate `xanatoo-sdl3-native` package. `nix/r36s.nix` also has an
  experimental SDL3 KMS/DRM build and alternative PortMaster package that
  ships `libSDL3.so.0`; keep the SDL2 package as the default. Do not switch
  production packages until native SDL3 builds and runtime tests pass.

## Android

- `nix build .#xanatoo-android` (ndk-build pipeline from Kurvenrausch;
  `nix/android.nix`, `mk/android/`). See `docs/ANDROID.md`.
- Built with `XANATOO_FONTCONFIG=0`, SQLite (amalgamation) and the network
  clients on Mbed TLS; keep code compiling with `-DXANATOO_FONTCONFIG=OFF`
  and `-DXANATOO_TLS=mbedtls` (desktop CMake accepts both). New `src/*.cpp`
  files are picked up by `mk/android/app/jni/Android.mk` automatically;
  OpenSSL-only files must be excluded in `build-apk.sh` like
  `tls_openssl.cpp`.
- Images: thumtoo is an ndk-build module (`mk/android/thumtoo/Android.mk`,
  sources from `external/thumtoo`, no MuPDF / DjVu / unarr / curl) over
  prebuilt C libraries (libvips, glib, libarchive, codecs) that
  `nix/android-imagelibs.nix` cross-builds per ABI with nixpkgs' Android
  set (NDK 27, plain C, so they link into the NDK 29 build) and merges into
  one `libimagedeps.a`; `nix build .#imagedeps-android`.
- Test on the device: `adb logcat -s xanatoo` shows std::cout / std::cerr.
