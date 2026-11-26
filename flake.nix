# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

{
  description = "xanatoo — interactive document composition engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;

      # gamepkgstd VER-5: VERSION is the only place the version is written.
      versionBase = nixpkgs.lib.fileContents ./VERSION;
      gitRev = self.shortRev or self.dirtyShortRev or "dirty";
      version =
        if nixpkgs.lib.hasInfix "-dev" versionBase
        then "${versionBase}.${toString (self.revCount or 0)}+g${gitRev}"
        else versionBase;

      # thumtoo's dependency list (libvips, libarchive, MuPDF pin, …) from the
      # vendored flake.nix: external/ is no flake input, so its outputs
      # function is evaluated directly (lib uses only nixpkgs), as galapix does.
      thumtooLib = ((import ./external/thumtoo/flake.nix).outputs {
        self = { };
        inherit nixpkgs;
        benchtoo = null;
      }).lib;
      thumtooBuildInputs = pkgs:
        thumtooLib.mkBuildInputs (pkgs // { mupdf = thumtooLib.pinMupdf pkgs; });

      # Android (Linux hosts): the SDK and NDK are unfree. API 22 is the
      # minimum supported by the SDL3 Android port; Java compiles against 33.
      androidFor = system:
        let
          androidPkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
            config.android_sdk.accept_license = true;
          };
        in import ./nix/android.nix {
          pkgs = androidPkgs;
          inherit nixpkgs;
          sdlSrc = androidPkgs.sdl3.src;
          sdlVersion = androidPkgs.sdl3.version;
          buildToolsVersion = "30.0.3";
          packagePlatform = "22";
          compilePlatform = "33";
          targetAbis = [ "armeabi-v7a" "arm64-v8a" "x86_64" ];
          androidSdk = (androidPkgs.androidenv.composeAndroidPackages {
            platformVersions = [ "22" "33" ];
            buildToolsVersions = [ "30.0.3" ];
            includeNDK = true;
            ndkVersion = "29.0.14206865";
            includeEmulator = false;
            includeSources = false;
          }).androidsdk;
          inherit version;
          # Last commit time: also set for dirty trees (revCount is not), so
          # local test builds install over committed ones; < 2^31 until 2036.
          versionCode = self.lastModified or 1;
        };

      # Website and release assets (x86_64-linux hosts the cross builds).
      siteFor = system: import ./nix/site.nix {
        pkgs = import nixpkgs { inherit system; };
        packages = self.packages.${system};
        inherit self version;
      };
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          xanatoo = pkgs.stdenv.mkDerivation {
            pname = "xanatoo";
            inherit version;
            src = ./.;

            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
              # CMake custom command runs scripts/rasterize_icons.py (SVG→PNG).
              python3
              librsvg # rsvg-convert; without it the icons are crude fallbacks
            ];

            buildInputs = with pkgs; [
              sdl3
              freetype
              fontconfig
              expat # fontconfig.pc Requires: expat
              openssl
              mbedtls # optional TLS backend: -DXANATOO_TLS=mbedtls
              secp256k1 # Nostr signing with -DXANATOO_TLS=mbedtls (as on Android)
              sqlite
              # Video / other audio formats in the Media panel (optional,
              # -DXANATOO_FFMPEG); headless: no X11 / SDL of its own.
              ffmpeg-headless
              # Prefer top-level libx11; xorg.* package set is deprecated.
              libx11
              libGL
            ] ++ thumtooBuildInputs pkgs;

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
              "-DPROJECT_VERSION_FULL=${version}"
              "-DXANATOO_SDL3=ON"
            ];
            # CTest unit tests (FLAKE-7); skipped automatically when cross.
            doCheck = true;

            meta = with pkgs.lib; {
              description = "Interactive document composition engine";
              license = licenses.gpl3Plus;
              platforms = platforms.linux ++ platforms.darwin;
              mainProgram = "xanatoo";
            };
          };

          # Minimal native SDL3 package for backend checks.
          xanatooSDL3 = pkgs.stdenv.mkDerivation {
            pname = "xanatoo-sdl3";
            inherit version;
            src = ./.;
            nativeBuildInputs = with pkgs; [ cmake pkg-config python3 ];
            buildInputs = with pkgs; [ sdl3 freetype ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
              "-DPROJECT_VERSION_FULL=${version}"
              "-DXANATOO_SDL3=ON"
              "-DXANATOO_MINIMAL_PORT=ON"
              "-DBUILD_TESTING=OFF"
            ];
            meta = xanatoo.meta;
          };

          # The binary wrapped with a font and pandoc; data is found next
          # to the binary (<exe>/../share/xanatoo).
          wrapped = pkgs.symlinkJoin {
            name = "xanatoo-${version}";
            paths = [ xanatoo ];
            nativeBuildInputs = [ pkgs.makeWrapper ];
            postBuild = ''
              wrapProgram $out/bin/xanatoo \
                --set-default XANATOO_FONT \
                  "${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf" \
                --prefix PATH : "${pkgs.pandoc}/bin"
            '';
            meta = xanatoo.meta;
          };
        in
        {
          # FLAKE-1: default and xanatoo are the same native build.
          default = wrapped;
          xanatoo = wrapped;

          # Unwrapped binary for packaging / debugging
          xanatoo-unwrapped = xanatoo;
          xanatoo-sdl3-native = xanatooSDL3;
        } // (
          # R36S / ArkOS PortMaster packages (Linux hosts only: cross to aarch64).
          if pkgs.stdenv.hostPlatform.isLinux then
            let
              date = "1970-01-01";
              r36s = import ./nix/r36s.nix { inherit pkgs version date; };
              android = androidFor system;
            in {
              xanatoo-android = android.apk;
              sdl3-android = android.sdlAndroidLibs;
              imagedeps-android = android.imageLibs;
              arkos-sysroot = r36s.sysroot;
              arkos-cross-gcc = r36s.toolchain;
              xanatoo-r36s = r36s.r36s;
              xanatoo-r36s-portmaster = r36s.portmaster;
              xanatoo-r36s-portmaster-zip = r36s.portmasterZip;
              # SDL3 backend; the unqualified package names above now select
              # this production path.
              sdl3-r36s = r36s.sdl3;
              xanatoo-r36s-sdl3 = r36s.r36sSDL3;
              xanatoo-r36s-sdl3-portmaster = r36s.portmasterSDL3;
              xanatoo-r36s-sdl3-portmaster-zip = r36s.portmasterSDL3Zip;
              # Retain the old device SDL2 ABI packages for rollback only.
              xanatoo-r36s-sdl2 = r36s.r36sSDL2;
              xanatoo-r36s-sdl2-portmaster = r36s.portmasterSDL2;
              xanatoo-r36s-sdl2-portmaster-zip = r36s.portmasterSDL2Zip;
            } // android.abiPackages
          else { }
        ) // (
          # Portable packages are built by the flake on a Linux build host.
          # The CI workflow uses these same outputs, not separate CMake rules.
          if system == "x86_64-linux" then
            let
              windows = import ./nix/windows.nix { inherit pkgs version; };
              wasm = import ./nix/wasm.nix {
                inherit pkgs version;
              };
            in {
              xanatoo-win32 = windows.win32;
              xanatoo-win32-zip = windows.win32Zip;
              xanatoo-win64 = windows.win64;
              xanatoo-win64-zip = windows.win64Zip;
              xanatoo-wasm = wasm.xanatooWasm;
              sdl3-wasm = wasm.sdl3Wasm;
              freetype-wasm = wasm.freetypeWasm;
              imagelibs-wasm = wasm.imageLibsWasm;
            } // (siteFor system).packages
          else { }
        ));

      # gamepkgstd FLAKE-9..11: `nix flake check` builds every package, checks
      # that every app's program exists, and runs the extra checks:
      # --version / --help (OPT-2, VER-3), man page, REUSE, and desktop
      # integration (INT-6..8: every launcher file is installed, the
      # .desktop / metainfo files validate).
      checks = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          pkg = self.packages.${system}.xanatoo-unwrapped;
          appChecks = nixpkgs.lib.mapAttrs' (name: app:
            nixpkgs.lib.nameValuePair "app-${name}"
              (pkgs.runCommand "xanatoo-check-app-${name}" { } ''
                test -x ${app.program}
                touch $out
              '')) self.apps.${system};
        in self.packages.${system} // appChecks // {
          # Compile the SDL-facing drawing, window, event, gamepad and input
          # bridge against SDL3 headers independently of the application build.
          sdl3-api = pkgs.runCommand "xanatoo-check-sdl3-api" {
            nativeBuildInputs = [ pkgs.pkg-config pkgs.stdenv.cc ];
            buildInputs = [ pkgs.sdl3 ];
          } ''
            c++ -std=c++20 -DXANATOO_SDL3=1 \
              $(pkg-config --cflags sdl3) -I${./include} \
              -fsyntax-only ${./tests/sdl_api_compile.cpp}
            touch $out
          '';
          version = pkgs.runCommand "xanatoo-check-version" { } ''
            out_v=$(${pkg}/bin/xanatoo --version)
            if [ "$out_v" != "xanatoo ${version}" ]; then
              echo "--version printed '$out_v', expected 'xanatoo ${version}'" >&2
              exit 1
            fi
            touch $out
          '';
          help = pkgs.runCommand "xanatoo-check-help" { } ''
            ${pkg}/bin/xanatoo --help > /dev/null
            touch $out
          '';
          reuse = pkgs.runCommand "xanatoo-check-reuse" {
            nativeBuildInputs = [ pkgs.reuse ];
          } ''
            cd ${self}
            HOME=$TMPDIR reuse lint
            touch $out
          '';
          man = pkgs.runCommand "xanatoo-check-man" {
            nativeBuildInputs = [ pkgs.mandoc ];
          } ''
            # stdenv fixup gzips man pages
            mandoc -T lint -W warning ${pkg}/share/man/man1/xanatoo.1.gz
            touch $out
          '';
          layout = pkgs.runCommand "xanatoo-check-layout" { } ''
            for f in \
              bin/xanatoo \
              share/man/man1/xanatoo.1.gz \
              share/applications/xanatoo.desktop \
              share/metainfo/xanatoo.metainfo.xml \
              share/icons/hicolor/scalable/apps/xanatoo.svg \
              share/icons/hicolor/{16x16,32x32,48x48,64x64,128x128,256x256}/apps/xanatoo.png
            do
              if [ ! -e ${pkg}/$f ]; then
                echo "missing: $f" >&2
                exit 1
              fi
            done
            touch $out
          '';
          integration = pkgs.runCommand "xanatoo-check-integration" {
            nativeBuildInputs = [ pkgs.desktop-file-utils pkgs.appstream ];
          } ''
            desktop-file-validate ${pkg}/share/applications/xanatoo.desktop
            # The app ID is the bare executable name on purpose (INT-1).
            appstreamcli validate --no-net \
              --override=cid-desktopapp-is-not-rdns=info \
              ${pkg}/share/metainfo/xanatoo.metainfo.xml
            touch $out
          '';
        });

      # FLAKE-5: run the native build; adb-install the APK (Linux hosts).
      apps = forAllSystems (system:
        let
          run = {
            type = "app";
            program = nixpkgs.lib.getExe self.packages.${system}.default;
            meta.description = "Run xanatoo";
          };
        in {
          default = run;
          xanatoo = run;
        } // nixpkgs.lib.optionalAttrs (nixpkgs.lib.hasSuffix "-linux" system)
          (androidFor system).installApps // nixpkgs.lib.optionalAttrs (system == "x86_64-linux") {
          xanatoo-wasm = (import ./nix/wasm.nix {
            pkgs = import nixpkgs { inherit system; };
            inherit version;
          }).serveApp;
        } // (siteFor system).apps);

      # Develop shell: deps + env + PATH scripts for out-of-tree CMake.
      # Helpers are writeShellScriptBin so `nix develop -c xanatoo-run` works.
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          # Main package for inputsFrom (pulls buildInputs into the shell).
          pkg = self.packages.${system}.xanatoo-unwrapped;

          printInvocation = ''
            _xanatoo_print_invocation() {
              printf '%s:' "''${0##*/}" >&2
              printf ' %q' "$@" >&2
              printf '\n' >&2
            }
          '';

          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          dejavuFont =
            "${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf";

          # Shared preamble: require shell entry, resolve build dir.
          xanatooDevPreamble = printInvocation + ''
            set -euo pipefail
            if [ -z "''${XANATOO_SOURCE:-}" ]; then
              echo "$0: XANATOO_SOURCE is not set (enter the shell with: nix develop)" >&2
              exit 1
            fi
            XANATOO_BUILD_DIR="''${XANATOO_BUILD_DIR:-/tmp/xanatoo-build}"
            printf '%s: source=%s build=%s\n' "''${0##*/}" "$XANATOO_SOURCE" "$XANATOO_BUILD_DIR" >&2

            _xanatoo_canon_path() {
              local p="$1"
              p="''${p%/}"
              if [ -d "$p" ]; then
                ( cd "$p" && pwd )
              else
                printf '%s\n' "$p"
              fi
            }

            # Mirror packaged wrapper env for the unwrapped out-of-tree binary.
            export XANATOO_FONT="''${XANATOO_FONT:-${dejavuFont}}"
            # Data directory of the build tree: generated icons, symlinks to
            # the live sources (styles/, examples/, FORMAT.md); DEV-6.
            XANATOO_DATA_ARGS=(--data-dir "$XANATOO_BUILD_DIR/share/xanatoo")

            export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath (with pkgs; [
              sdl3 libx11 libGL freetype fontconfig openssl sqlite
            ])}''${XANATOO_EXTRA_LD_LIBRARY_PATH:+:$XANATOO_EXTRA_LD_LIBRARY_PATH}"
          '';

          xanatooConfigure = pkgs.writeShellScriptBin "xanatoo-configure" (
            xanatooDevPreamble
            + ''
              mkdir -p "$XANATOO_BUILD_DIR"
              configure_args=(-S "$XANATOO_SOURCE" -B "$XANATOO_BUILD_DIR" -G Ninja
                -DCMAKE_BUILD_TYPE="''${CMAKE_BUILD_TYPE:-Debug}"
                -DCMAKE_CXX_FLAGS_DEBUG="''${CMAKE_CXX_FLAGS_DEBUG:--g3 -Og -fno-omit-frame-pointer}"
                "$@")
              _xanatoo_print_invocation cmake "''${configure_args[@]}"
              cmake "''${configure_args[@]}"
            ''
          );

          xanatooBuild = pkgs.writeShellScriptBin "xanatoo-build" (
            xanatooDevPreamble
            + ''
              # Reconfigure when cache is missing or source path changed.
              if [ ! -f "$XANATOO_BUILD_DIR/CMakeCache.txt" ]; then
                echo "xanatoo-build: no cache → running xanatoo-configure"
                xanatoo-configure || exit 1
              else
                cached=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
                  "$XANATOO_BUILD_DIR/CMakeCache.txt" | head -n1 || true)
                want=$(_xanatoo_canon_path "$XANATOO_SOURCE")
                cached_c=
                if [ -n "$cached" ]; then
                  cached_c=$(_xanatoo_canon_path "$cached")
                fi
                if [ -z "$cached" ] || [ ! -d "$cached" ]; then
                  echo "xanatoo-build: cached source missing ($cached) → reconfigure"
                  xanatoo-configure || exit 1
                elif [ -n "$cached_c" ] && [ "$cached_c" != "$want" ]; then
                  echo "xanatoo-build: CMAKE_HOME_DIRECTORY changed:" >&2
                  echo "  was: $cached_c" >&2
                  echo "  now: $want" >&2
                  echo "  → re-running xanatoo-configure" >&2
                  xanatoo-configure || exit 1
                fi
              fi
              build_type=$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$XANATOO_BUILD_DIR/CMakeCache.txt")
              printf 'xanatoo-build: CMAKE_BUILD_TYPE=%s (CMake cache)\n' "''${build_type:-<unset>}" >&2
              build_args=(--build "$XANATOO_BUILD_DIR" -j "''${XANATOO_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}" "$@")
              _xanatoo_print_invocation cmake "''${build_args[@]}"
              cmake "''${build_args[@]}"
            ''
          );

          xanatooRun = pkgs.writeShellScriptBin "xanatoo-run" (
            xanatooDevPreamble
            + ''
              printf 'xanatoo-run: executable=%s data=%s font=%s\n' \
                "$XANATOO_BUILD_DIR/xanatoo" "''${XANATOO_DATA_ARGS[1]}" "$XANATOO_FONT" >&2
              xanatoo-build || exit 1
              if [ ! -x "$XANATOO_BUILD_DIR/xanatoo" ]; then
                echo "xanatoo-run: $XANATOO_BUILD_DIR/xanatoo missing after build" >&2
                exit 1
              fi
              exec "$XANATOO_BUILD_DIR/xanatoo" "''${XANATOO_DATA_ARGS[@]}" "$@"
            ''
          );

          xanatooRunGdb = pkgs.writeShellScriptBin "xanatoo-run-gdb" (
            xanatooDevPreamble
            + ''
              printf 'xanatoo-run-gdb: executable=%s data=%s font=%s\n' \
                "$XANATOO_BUILD_DIR/xanatoo" "''${XANATOO_DATA_ARGS[1]}" "$XANATOO_FONT" >&2
              xanatoo-build || exit 1
              if [ ! -x "$XANATOO_BUILD_DIR/xanatoo" ]; then
                echo "xanatoo-run-gdb: $XANATOO_BUILD_DIR/xanatoo missing after build" >&2
                exit 1
              fi
              if ! command -v gdb >/dev/null 2>&1; then
                echo "xanatoo-run-gdb: gdb not found (should be in the nix develop shell)" >&2
                exit 1
              fi
              # -q: quiet banner. -ex run: start without typing "run".
              # Quit only on exit code 0; crashes leave an interactive prompt.
              gdb -q \
                -ex "set pagination off" \
                -ex "set confirm off" \
                -ex "set debuginfod enabled off" \
                -ex run \
                -ex 'python
try:
  ec = gdb.parse_and_eval("$_exitcode")
  if int(ec) == 0:
    gdb.execute("quit")
except Exception:
  pass
' \
                --args "$XANATOO_BUILD_DIR/xanatoo" "''${XANATOO_DATA_ARGS[@]}" "$@"
            ''
          );

          xanatooTest = pkgs.writeShellScriptBin "xanatoo-test" (
            xanatooDevPreamble
            + ''
              xanatoo-build || exit 1
              if [ -x "$XANATOO_BUILD_DIR/xanatoo_tests" ]; then
                _xanatoo_print_invocation "$XANATOO_BUILD_DIR/xanatoo_tests" "$@"
                "$XANATOO_BUILD_DIR/xanatoo_tests" "$@"
              elif [ -f "$XANATOO_BUILD_DIR/CTestTestfile.cmake" ]; then
                _xanatoo_print_invocation ctest --test-dir "$XANATOO_BUILD_DIR" --output-on-failure "$@"
                ctest --test-dir "$XANATOO_BUILD_DIR" --output-on-failure "$@"
              else
                echo "xanatoo-test: no xanatoo_tests binary or CTest files in $XANATOO_BUILD_DIR" >&2
                exit 1
              fi
            ''
          );
        in
        {
          default = pkgs.mkShell.override { stdenv = pkgs.ccacheStdenv; } {
            inputsFrom = [ pkg ];
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              python3
              gdb
              ccache
              clang-tools
              dejavu_fonts
              xanatooConfigure
              xanatooBuild
              xanatooRun
              xanatooRunGdb
              xanatooTest
              pandoc  # EPUB → Markdown import (optional at runtime)
            ];
            CMAKE_BUILD_TYPE = "Debug";
            shellHook = ''
              export XANATOO_SOURCE="$PWD"
              export XANATOO_BUILD_DIR="''${XANATOO_BUILD_DIR:-/tmp/xanatoo-build}"
              export XANATOO_FONT="''${XANATOO_FONT:-${dejavuFont}}"
              export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath (with pkgs; [
                sdl3 libx11 libGL freetype fontconfig openssl sqlite
              ])}''${XANATOO_EXTRA_LD_LIBRARY_PATH:+:$XANATOO_EXTRA_LD_LIBRARY_PATH}"
              # Prefer ccache when available (ccacheStdenv or explicit launcher).
              if command -v ccache >/dev/null 2>&1; then
                export CMAKE_CXX_COMPILER_LAUNCHER="''${CMAKE_CXX_COMPILER_LAUNCHER:-ccache}"
              fi
              # Full debug info for out-of-tree builds (override before configure).
              # -Og: optimise for debug; satisfies Nix _FORTIFY_SOURCE (needs -O).
              # C-only FLAGS_DEBUG omitted — project is CXX-only.
              export CMAKE_CXX_FLAGS_DEBUG="''${CMAKE_CXX_FLAGS_DEBUG:--g3 -Og -fno-omit-frame-pointer}"
              cat <<'XANATOO_DEV_BANNER'
xanatoo — Nix development shell

PC
  xanatoo-configure   configure the CMake build
  xanatoo-build       configure if needed, then build with Ninja
  xanatoo-run         build and run the app (forwards arguments)
  xanatoo-run-gdb     build and run under GDB (stays open on a crash)
  xanatoo-test        build and run the tests

  XANATOO_BUILD_DIR defaults to /tmp/xanatoo-build.
  From outside the shell: nix develop -c xanatoo-run examples/welcome.md
  See docs/ENVIRONMENT.md.
XANATOO_DEV_BANNER
              echo "  Build directory: $XANATOO_BUILD_DIR (''${CMAKE_BUILD_TYPE:-Debug})"
              ${pkgs.lib.optionalString isLinux ''
                cat <<'XANATOO_ANDROID_BANNER'

Android
  nix develop .#android                          open the Android shell
  nix develop .#android -c xanatoo-android-build  build the APK incrementally
XANATOO_ANDROID_BANNER
              ''}
            '';
          };
        } // pkgs.lib.optionalAttrs isLinux {
          android = (androidFor system).devShell;
        });
    };
}
