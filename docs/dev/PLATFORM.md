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
  The app and vendored ImGui still compile against SDL2 headers for Windows.
  WASM and Android currently retain SDL2. First-party render, window,
  surface, time, mouse, input events, gamepads, cursor and SDL diagnostic
  operations now use `xanatoo/sdl.hpp`
  (switch `XANATOO_SDL3`); `checks.sdl3-api` compiles this
  bridge with real SDL3 headers. `-DXANATOO_SDL3=ON` selects an experimental
  first-party ImGui platform/renderer backend (`src/imgui_sdl3.cpp`) and the
  flake's separate `xanatoo-sdl3-native` package. The default R36S
  PortMaster package ships SDL3 with KMS/DRM. The `xanatoo-r36s-sdl2*`
  packages retain the device's SDL2 ABI as a fallback.
- Both R36S app variants use `arkos-cross-gcc` from `nix/r36s.nix`. GCC
  and its target runtimes are built against the arm64 eoan sysroot so the
  app can use current C++ headers while keeping glibc 2.30 as the dynamic
  baseline. The driver links libstdc++ and libgcc statically.

## Android

- On Linux, `nix develop .#android` provides incremental configure, build
  and install commands over the live checkout. Staging and the NDK object
  tree persist in `XANATOO_ANDROID_BUILD_DIR` (default
  `/tmp/xanatoo-android-build`). Packaged builds share the recipe in
  `nix/android.nix` and `mk/android/`. Append an ABI suffix to
  `.#xanatoo-android` or `.#xanatoo-android-install` to build and package
  only that ABI. See `docs/ANDROID.md`.
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
- Video: `media_android.cpp` (AMediaExtractor + AMediaCodec, `-lmediandk`
  in `Android.mk`); the decoder's picture layout is logged
  ("video decoder output: …") for devices that show garbled colours.
  `AMediaExtractor_setDataSource(url)` goes through Java: the worker
  thread attaches via `SDL_AndroidGetJNIEnv()` first (Android 5 crashes
  on a NULL JNIEnv instead of failing). HLS (`.m3u8`) uses the shared
  FFmpeg backend in the Media panel. `nix/android-ffmpeg.nix` builds the
  static HLS libraries per ABI with the same NDK as the app. The APK's
  existing Mbed TLS module and bundled CA roots handle HTTPS.
- Soft keyboard: `platform_request_keyboard` (SDL_StartTextInput) when an
  ImGui text field gains focus (`Ui::prepare_render`) **or** a focused one
  is tapped again — the user may have hidden the keyboard while the field
  kept focus. Caret placement in an editable document requests it too.
  SDL 3.4 can cache the keyboard as shown based on the input connection
  accepting text. After SDL's request, the activity checks actual IME
  visibility on API 30+ and can request the keyboard again through SDL's
  existing editor. Older Android versions retry through the input method
  manager. To check recovery on a device, dismiss the keyboard and
  tap the same field again, then repeat after backgrounding and resuming
  the app. Verify that typing still reaches the focused field.
  The SDL3 ImGui backend takes the pointer position from touch button
  presses, including repeated taps at a position where SDL omits motion.
- Test on the device: `adb logcat -s xanatoo` shows std::cout / std::cerr.

## Website and downloads

The website <https://xanatoo.github.io/> is built from `website/`
(`index.html`, `images/`, scripts) by `nix/site.nix`; the
`xanatoo/xanatoo.github.io` repository only holds the deploy workflow.

- `.#release-<target>` (android, r36s, win64, win32, wasm): the exact
  release asset plus `<target>.json` (version, rev, revCount, lastModified
  from the flake's `self`, and `file`, the asset's name). Names carry the
  version (`+` → `-`) and platform: `xanatoo-<v>-android-universal.apk`,
  `-windows-x86_64.zip`, `-windows-i686.zip`, `-wasm32.tar.gz`; only R36S
  stays `xanatoo.zip` (the name PortMaster's `port.json` gives). The site
  serves `downloads/<file>` and links them through `@file-<target>@`;
  Android also publishes standalone APKs with the suffixes `-arm64-v8a`,
  `-armeabi-v7a`, and `-x86_64` before `.apk`. Its release metadata lists
  these under `additionalFiles` and uses the universal APK for `file`.
  `publish-build.sh` deletes superseded assets once the new json is up;
  `fetch-builds.sh` still reads older records (no `file`: the old fixed
  names). `website/404.html` (Pages serves it for missing paths) sends old
  or outdated `downloads/*.apk|zip` links to `/#download`.
- The browser edition on Pages relies on `wasm/coi-sw.js` for cross-origin
  isolation (`docs/PORTS.md`); `assemble.sh` fails if the page registers it
  but the build lacks the file.
- CI (`artifacts.yml`) builds each target as its own matrix job. On master a
  success runs `nix run .#publish-build -- continuous <target> dist`, which
  swaps that target's assets on the `continuous` prerelease (temporary
  upload, then delete + rename; skips if the release already holds a newer
  revCount), then triggers the site's `pages.yml` with the secret
  `SITE_DISPATCH_TOKEN` (fine-grained, xanatoo.github.io, Actions read and
  write). A failed target keeps its previous assets and download.
- `pages.yml` runs `nix run github:xanatoo/xanatoo#site-from-release -- site`:
  `website/fetch-builds.sh` takes each target from the release, or from the
  live site when the release lacks it, then `website/assemble.sh` fills in
  the page. Page-only pushes (`website/**`) skip check / artifacts and only
  trigger a deploy (`website.yml`); the site also redeploys nightly.
- The page shows every download's version; `android-update.json` (the
  app's update check, `XanatooActivity.java`) is derived from
  `android.json`, so its versionCode matches the APK actually served.
- Local: `nix build .#site` builds every target from this tree (slow, all
  cross builds); `nix run .#site-from-release -- out && nix run .#site-serve -- out`
  previews page edits against the published builds.
- Page text: feature claims follow `README.md` and `TODO.md`; do not
  describe planned passage or revision features as available. The page
  warns that the OpenSSL / Mbed TLS clients do not verify peer
  certificates yet; update it when they do.
