# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The Android build, after Kurvenrausch / Pingus's ndk-build pipeline
# (mk/android/): SDL3's native library and Java side are built once from its
# nixpkgs source; xanatoo (plus a static FreeType built from nixpkgs'
# source) into libmain.so against it; both packaged into a signed APK.
#
# Network clients (Gemini / Nostr / Reddit / Lemmy) use Mbed TLS, Nostr
# signatures libsecp256k1 (both from nixpkgs' sources); SQLite is
# built from its amalgamation; images go through thumtoo over prebuilt
# libvips / glib / libarchive (nix/android-imagelibs.nix); no FontConfig (fonts come from the bundled
# DejaVu and /system/fonts). Data files ship as APK assets (see
# mk/android/scripts/build-apk.sh).
#
#   nix build .#xanatoo-android        the APK
#   nix run .#xanatoo-android-install  install it with adb

{ pkgs
, nixpkgs           # for the cross-built image libraries
, sdlSrc
, sdlVersion
, androidSdk
, buildToolsVersion
, packagePlatform   # minSdk, linked against
, compilePlatform   # the Java side's classpath
, targetAbis
, version
, versionCode
}:

let
  abis = pkgs.lib.concatStringsSep " " targetAbis;

  # ndk-build entry point: every subdirectory's Android.mk (SDL, FreeType,
  # the app).
  topAndroidMk = pkgs.writeText "Android.mk" "include $(call all-subdir-makefiles)\n";

  applicationMk = pkgs.writeText "Application.mk" ''
    APP_STL := c++_shared
    APP_ABI := ${abis}
    APP_PLATFORM := android-${packagePlatform}
  '';

  sdlBuildScripts = pkgs.lib.fileset.toSource {
    root = ../mk/android/scripts;
    fileset = pkgs.lib.fileset.unions [
      ../mk/android/scripts/build-sdl-libs.sh
      ../mk/android/scripts/ndk.sh
    ];
  };

  sdlAndroidLibs = pkgs.stdenvNoCC.mkDerivation {
    pname = "sdl3-android-libs";
    version = sdlVersion;
    dontUnpack = true;
    nativeBuildInputs = [ androidSdk pkgs.jdk17 pkgs.gnumake ];
    env = {
      BUILD_TOOLS_VERSION = buildToolsVersion;
      COMPILE_PLATFORM = compilePlatform;
      PACKAGE_PLATFORM = packagePlatform;
      SDL_SRC = "${sdlSrc}";
      APPLICATION_MK = applicationMk;
      TOP_ANDROID_MK = topAndroidMk;
    };
    buildPhase = ''
      runHook preBuild
      export ANDROID_HOME=${androidSdk}/libexec/android-sdk
      bash ${sdlBuildScripts}/build-sdl-libs.sh
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      TARGET_ABIS=${pkgs.lib.escapeShellArg abis} bash ${../mk/android/scripts/install-sdl-libs.sh}
      runHook postInstall
    '';
    # Android ELF: the host's patchelf and strip must keep off.
    dontPatchELF = true;
    dontStrip = true;
  };

  # sqlite3.c + sqlite3.h from nixpkgs' full source tree (needs tclsh).
  sqliteAmalgamation = pkgs.stdenv.mkDerivation {
    pname = "sqlite-amalgamation";
    inherit (pkgs.sqlite) version src;
    nativeBuildInputs = [ pkgs.unzip pkgs.tcl ];
    configureFlags = [ "--disable-tcl" ];
    buildPhase = "make sqlite3.c";
    installPhase = ''
      mkdir -p $out
      cp sqlite3.c sqlite3.h $out/
    '';
  };

  # Prebuilt C libraries for thumtoo, one directory per ABI.
  imageLibs = pkgs.linkFarm "xanatoo-android-imagelibs" (map (abi: {
    name = abi;
    path = import ./android-imagelibs.nix { inherit nixpkgs abi; system = pkgs.stdenv.buildPlatform.system; };
  }) targetAbis);

  sdlPrebuiltMk = pkgs.writeText "SDL3-prebuilt-Android.mk" ''
    LOCAL_PATH := $(call my-dir)
    include $(CLEAR_VARS)
    LOCAL_MODULE := SDL3
    LOCAL_SRC_FILES := ${sdlAndroidLibs}/lib/$(TARGET_ARCH_ABI)/libSDL3.so
    include $(PREBUILT_SHARED_LIBRARY)
  '';

  # The sources the APK is built from: no build directories, no .git.
  appSrc = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ../src ../include ../external
      ../examples ../styles ../sites ../icons/svg ../scripts/rasterize_icons.py
      ../FORMAT.md
      ../data/fonts/latinmodern-math.otf ../data/fonts/LICENSE-LatinModernMath.txt
    ];
  };

  apkName = "xanatoo-${version}.apk";

  buildTools = [
    androidSdk pkgs.jdk17 pkgs.zip pkgs.gnumake pkgs.xz pkgs.rsync
    pkgs.python3 pkgs.librsvg
  ];

  buildEnv = {
    ANDROID_HOME = "${androidSdk}/libexec/android-sdk";
    BUILD_TOOLS_VERSION = buildToolsVersion;
    PACKAGE_PLATFORM = packagePlatform;
    COMPILE_PLATFORM = compilePlatform;
    TARGET_ABIS = abis;
    APP_NAME = "xanatoo";
    FREETYPE_SRC = "${pkgs.freetype.src}";
    DEJAVU_DIR = "${pkgs.dejavu_fonts}/share/fonts/truetype";
    # Mozilla's roots (follows nixpkgs); old Android stores lack ISRG Root X1.
    CA_BUNDLE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
    SQLITE_SRC = sqliteAmalgamation;
    MBEDTLS_SRC = "${pkgs.mbedtls.src}";
    SECP256K1_SRC = "${pkgs.secp256k1.src}";
    IMAGELIBS = imageLibs;
    APPLICATION_MK = applicationMk;
    TOP_ANDROID_MK = topAndroidMk;
    SDL_PREBUILT_MK = sdlPrebuiltMk;
    SDL_ANDROID_LIBS = sdlAndroidLibs;
    APP_VERSION = version;
    VERSION_CODE = toString versionCode;
  };

  apk = pkgs.stdenvNoCC.mkDerivation {
    pname = "xanatoo-android";
    inherit version;
    dontUnpack = true;
    nativeBuildInputs = buildTools;
    env = buildEnv // {
      APP_SRC_DIR = "${appSrc}";
      ANDROID_PROJECT_DIR = "${../mk/android}";
    };
    buildPhase = ''
      runHook preBuild
      bash "$ANDROID_PROJECT_DIR/scripts/build-apk.sh"
      runHook postBuild
    '';
    installPhase = ''
      mkdir -p $out
      cp out/xanatoo.apk $out/${apkName}
    '';
    dontPatchELF = true;
    dontStrip = true;
  };

  devCommand = name: action: pkgs.writeShellScriptBin name ''
    set -euo pipefail
    source_dir="''${XANATOO_SOURCE:?Enter the Android shell with nix develop .#android}"
    export APP_SRC_DIR="$(cd "$source_dir" && pwd -P)"
    export ANDROID_PROJECT_DIR="$APP_SRC_DIR/mk/android"
    build_dir="''${XANATOO_ANDROID_BUILD_DIR:-/tmp/xanatoo-android-build}"
    mkdir -p "$build_dir"
    cd -P "$build_dir"
    if [ "$PWD" = "$APP_SRC_DIR" ]; then
      echo "${name}: choose a build directory separate from the checkout" >&2
      exit 1
    fi
    printf '%s: source=%s build=%s\n' "${name}" "$APP_SRC_DIR" "$PWD" >&2
    # Keep staging, native builds and APK packaging serial in this directory.
    {
      flock 9
      ${action}
    } 9>.xanatoo-build.lock
  '';

  installApp = {
    type = "app";
    program = toString (pkgs.writeShellScript "install-xanatoo-apk" ''
      exec ${pkgs.android-tools}/bin/adb install -r ${apk}/${apkName}
    '');
    meta.description = "Install the xanatoo APK on a connected Android device with adb";
  };

  devShell = pkgs.mkShell (buildEnv // {
    packages = buildTools ++ [
      pkgs.android-tools
      pkgs.util-linux
      (devCommand "xanatoo-android-configure" ''
        bash "$ANDROID_PROJECT_DIR/scripts/build-apk.sh" --configure
      '')
      (devCommand "xanatoo-android-build" ''
        bash "$ANDROID_PROJECT_DIR/scripts/build-apk.sh"
      '')
      (devCommand "xanatoo-android-install" ''
        bash "$ANDROID_PROJECT_DIR/scripts/build-apk.sh"
        adb install -r "$@" "$PWD/out/$APP_NAME.apk" 9>&-
      '')
    ];
    shellHook = ''
      export XANATOO_SOURCE="$PWD"
      export XANATOO_ANDROID_BUILD_DIR="''${XANATOO_ANDROID_BUILD_DIR:-/tmp/xanatoo-android-build}"
      cat <<'XANATOO_ANDROID_BANNER'
xanatoo — Android development shell

  xanatoo-android-configure   stage the NDK project
  xanatoo-android-build       build incrementally and package the APK
  xanatoo-android-install     build and install the APK on a connected device

  XANATOO_ANDROID_BUILD_DIR defaults to /tmp/xanatoo-android-build.
  See docs/ANDROID.md.
XANATOO_ANDROID_BANNER
    '';
  });

in {
  inherit sdlAndroidLibs sqliteAmalgamation imageLibs apk installApp devShell;
}
