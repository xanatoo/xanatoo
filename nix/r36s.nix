# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The R36S handheld (RK3326, 640x480) running ArkOS, whose userland is
# Ubuntu 19.10 (eoan), with glibc 2.30. Build GCC and its target
# runtimes against the eoan arm64 sysroot. The app links libstdc++ and
# libgcc statically while keeping the device's glibc and system libraries
# dynamic. SDL3 is built against the same sysroot and shipped with the port.
#
# Same approach as kurvenrausch's nix/r36s.nix; FreeType/Fontconfig debs
# added for xanatoo's text stack. The SDL3 build follows the
# KMS/DRM setup of Grumbel/sdl-jstest (nix/r36s.nix): SDL3 is built for the
# eoan sysroot and shipped alongside the PortMaster executable.
# Historical SDL2 fallback: ImGui's SDL2 renderer backend is patched
# for SDL 2.0.10 headers (no RenderGeometry): it looks up
# SDL_RenderGeometryRaw in the device SDL at runtime, else draws per triangle
# (ArkOS has 2.0.10) into a texture that is only redrawn when the UI changes.
#
#   nix build .#xanatoo-r36s
#   nix build .#xanatoo-r36s-portmaster
#   nix build .#xanatoo-r36s-portmaster-zip
#   nix build .#xanatoo-r36s-sdl3-portmaster-zip

{ pkgs
, version
, date
}:

let
  lib = pkgs.lib;

  # eoan arm64 packages: glibc, kernel headers, GCC 9 shared runtimes for device libraries,
  # SDL2 2.0.10, FreeType, Fontconfig (+ link deps).
  debs = [
    { file = "pool/main/g/glibc/libc6_2.30-0ubuntu2_arm64.deb"; sha256 = "4853da1e3c9eae109708b30dc5df00b7ced888c09f0e77bf6731015a97fb859a"; }
    { file = "pool/main/g/glibc/libc6-dev_2.30-0ubuntu2_arm64.deb"; sha256 = "a0b1860c16c50934a5fe3206ac386267d24b015b0ab677005d38e60ac1d27823"; }
    { file = "pool/main/l/linux/linux-libc-dev_5.3.0-18.19_arm64.deb"; sha256 = "7575a37b1abf722f950fa8202c49f24b9c887dcf8e1f97bad15b555cb8c483de"; }
    { file = "pool/main/g/gcc-9/libgcc1_9.2.1-9ubuntu2_arm64.deb"; sha256 = "81dfdee2c50f13dab98967b0062e51f1cebb967f407e3593c5a77ed2146ea008"; }
    { file = "pool/main/g/gcc-9/libstdc++6_9.2.1-9ubuntu2_arm64.deb"; sha256 = "5af692b4ecf237979411acf74b7319785e09dd3450a917a94f3ea65cd260436e"; }
    { file = "pool/universe/libs/libsdl2/libsdl2-2.0-0_2.0.10+dfsg1-1ubuntu1_arm64.deb"; sha256 = "34f06694f13bbee317b89be868768e8fd5b030287431a630881f45ff99dc9264"; }
    { file = "pool/universe/libs/libsdl2/libsdl2-dev_2.0.10+dfsg1-1ubuntu1_arm64.deb"; sha256 = "9eb97797c392b68449045d814f8763ca984853abefff94ababdc72270b7d65ea"; }
    { file = "pool/main/f/freetype/libfreetype6_2.9.1-4_arm64.deb"; sha256 = "e1a18bdec319ab92e02912b6721846907ea13e420123c7225834180ba94360f1"; }
    { file = "pool/main/f/freetype/libfreetype6-dev_2.9.1-4_arm64.deb"; sha256 = "8f4b3f018e6ef26aac15cf378856dde03b8d2a7fd69e3c41c9b8b0467f483c74"; }
    { file = "pool/main/f/fontconfig/libfontconfig1_2.13.1-2ubuntu3_arm64.deb"; sha256 = "7abc3c68569ebd52e7e27ba1cd1e97004bf5a1641212acd821220a020b713c2f"; }
    { file = "pool/main/f/fontconfig/libfontconfig1-dev_2.13.1-2ubuntu3_arm64.deb"; sha256 = "a4c7b981a7003ab75932cfabedd6697474c2ec541517ea7850be9637f3b753c3"; }
    { file = "pool/main/e/expat/libexpat1_2.2.7-2_arm64.deb"; sha256 = "07b81f7223e65c52937fce24edc9d6dae79fa2fb66408bc68103362b555d6f3a"; }
    { file = "pool/main/e/expat/libexpat1-dev_2.2.7-2_arm64.deb"; sha256 = "5201f6619b3d6332e04cd5b48913ffbf2ea61a8b67d5e3bbce194f9a262ae75b"; }
    { file = "pool/main/z/zlib/zlib1g_1.2.11.dfsg-1ubuntu3_arm64.deb"; sha256 = "409aa5163b49fee4ece16744cc245c281d0e97c1114a6a199a5a9d48f93b6bf0"; }
    { file = "pool/main/z/zlib/zlib1g-dev_1.2.11.dfsg-1ubuntu3_arm64.deb"; sha256 = "b719c443866abb74bbe31010de9a92a5f889f2c7b3b13b95e8637ab79e9bd919"; }
    { file = "pool/main/libp/libpng1.6/libpng16-16_1.6.37-1_arm64.deb"; sha256 = "6dcde9dbcaa1c59fdf086989b44613bed1b97a8b160f53767d984e344d43d167"; }
    { file = "pool/main/libp/libpng1.6/libpng-dev_1.6.37-1_arm64.deb"; sha256 = "2f03c1b26d2835d9bd81e2fc38a9095e79e3e303bdedf52c8743c74de789c472"; }
    { file = "pool/main/u/util-linux/libuuid1_2.34-0.1ubuntu2_arm64.deb"; sha256 = "219dcf7b2bea4dd43c1685c651f61f63d903ac82cad0ee6a821b942283359b65"; }
    { file = "pool/main/u/util-linux/uuid-dev_2.34-0.1ubuntu2_arm64.deb"; sha256 = "d30984212486096a8f499354dfeae0a45d7754ccee6c4e1183de1997fc44bcd8"; }
    { file = "pool/main/b/brotli/libbrotli1_1.0.7-2_arm64.deb"; sha256 = "f180a18ae74016c5db89a1ed438e2fa45cd7d64c2abcec6a32e4316724b1b85a"; }
    { file = "pool/main/b/brotli/libbrotli-dev_1.0.7-2_arm64.deb"; sha256 = "861312067f81e664925243294070dcb31e4478c21f9a237a95d9cec7ebde7155"; }
    # SQLite for the revision store (drafts) and the side store.
    { file = "pool/main/s/sqlite3/libsqlite3-0_3.29.0-2_arm64.deb"; sha256 = "af04f3eb2390c433206b3a7d07b124fae097df302fcca9a47d76f76f806ee32c"; }
    { file = "pool/main/s/sqlite3/libsqlite3-dev_3.29.0-2_arm64.deb"; sha256 = "6f2fe12282ad58b54b5033cb3190ba1ee8846344ef51f243da25c0eaf78748ab"; }
    # OpenSSL 1.1 for gemini:// (ArkOS/device typically already has libssl1.1).
    { file = "pool/main/o/openssl/libssl1.1_1.1.1c-1ubuntu4_arm64.deb"; sha256 = "ec2aca230fa845c96c90afc5a4bb9b41ea10e0135a70eb5898d3d2597a3d0c3d"; }
    { file = "pool/main/o/openssl/libssl-dev_1.1.1c-1ubuntu4_arm64.deb"; sha256 = "275f730b1867066b841f3d66289bfd87d10d98c1358d611d687b1311c974fbab"; }
    # SDL3 KMS/DRM runtime loader needs the corresponding platform headers.
    { file = "pool/main/libd/libdrm/libdrm2_2.4.99-1ubuntu1_arm64.deb"; sha256 = "570407f42d8fac13c20be388fa45a1739ae0e900b30d9a924bd6599cd5e64d45"; }
    { file = "pool/main/libd/libdrm/libdrm-dev_2.4.99-1ubuntu1_arm64.deb"; sha256 = "69071f9360391bdf3ec9a3842aef851b05aac363e9c35b89a8c31f8227d31af5"; }
    { file = "pool/main/m/mesa/libgbm1_19.2.1-1ubuntu1_arm64.deb"; sha256 = "c982ba0bc2e00522e82bdde0b510fee50185b89670ff77853074d7b591c1f054"; }
    { file = "pool/main/m/mesa/libgbm-dev_19.2.1-1ubuntu1_arm64.deb"; sha256 = "7732f9ebe793b7ea5dd45b7c7c531446ea6d9a4f366229630e70a1bd9a58252a"; }
    { file = "pool/main/s/systemd/libudev1_242-7ubuntu3_arm64.deb"; sha256 = "af9cb069b6900a330d6ccf82f9e346ab1d74d5d8df76f4a50b76f63602904610"; }
    { file = "pool/main/s/systemd/libudev-dev_242-7ubuntu3_arm64.deb"; sha256 = "2785dd52d59b7a590f5471b474fd8f09fd2ef5c79d889510b740e5ce95fc69fa"; }
  ];
  fetchDeb = d: pkgs.fetchurl {
    url = "http://old-releases.ubuntu.com/ubuntu/${d.file}";
    inherit (d) sha256;
  };

  sysroot = pkgs.stdenvNoCC.mkDerivation {
    pname = "arkos-sysroot";
    version = "eoan-arm64";
    dontUnpack = true;
    nativeBuildInputs = [ pkgs.dpkg ];
    installPhase = ''
      mkdir -p $out
      ${lib.concatMapStrings (d: "dpkg-deb -x ${fetchDeb d} $out\n") debs}
      find $out -type l | while read -r link; do
        target=$(readlink "$link")
        case "$target" in
          /*) ln -sfn "$out$target" "$link" ;;
        esac
      done
    '';
    dontFixup = true;
  };

  cross = pkgs.pkgsCross.aarch64-multiplatform.stdenv.cc;
  binutils = cross.bintools.bintools;
  tp = lib.removeSuffix "-" cross.targetPrefix;
  multiarch = "aarch64-linux-gnu";

  toolchain = pkgs.stdenv.mkDerivation {
    pname = "arkos-cross-gcc";
    inherit (cross.cc) version src;
    nativeBuildInputs = [ binutils pkgs.bison pkgs.flex pkgs.texinfo ];
    buildInputs = [ pkgs.gmp pkgs.mpfr pkgs.libmpc pkgs.zlib ];
    strictDeps = true;
    enableParallelBuilding = true;
    hardeningDisable = [ "all" ];
    dontAddDisableDepTrack = true;
    configurePhase = ''
      runHook preConfigure
      mkdir build
      cd build
      ../configure \
        --prefix="$out" \
        --build=${pkgs.stdenv.buildPlatform.config} \
        --target=${tp} \
        --with-build-time-tools=${binutils}/${tp}/bin \
        --with-sysroot=${sysroot} \
        --with-as=${binutils}/bin/${tp}-as \
        --with-ld=${binutils}/bin/${tp}-ld \
        --with-arch=armv8-a --with-tune=cortex-a35 \
        --enable-multiarch --enable-languages=c,c++ \
        --disable-bootstrap --disable-multilib --disable-nls \
        --disable-libsanitizer --disable-libgomp --disable-libquadmath \
        --disable-libssp --disable-libvtv --disable-libatomic \
        --with-system-zlib
      runHook postConfigure
    '';
  };

  ldflags = lib.concatStringsSep " " [
    "-static-libgcc"
    "-Wl,-rpath-link,${sysroot}/usr/lib/${multiarch}:${sysroot}/lib/${multiarch}"
    "-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1"
  ];
  driver = compiler: linkFlags: pkgs.writeShellScript "arkos-${compiler}" ''
    for a in "$@"; do
      case "$a" in
        -c|-S|-E|-M|-MM) exec ${toolchain}/bin/${tp}-${compiler} "$@" ;;
      esac
    done
    exec ${toolchain}/bin/${tp}-${compiler} ${ldflags} ${linkFlags} "$@"
  '';
  cc = driver "gcc" "";
  cxx = driver "g++" "-static-libstdc++";

  # Link stub: same soname as device SDL2, empty symbols (kurvenrausch pattern).
  sdlStub = pkgs.runCommand "libSDL2-link-stub" { } ''
    mkdir -p $out/lib
    ${binutils}/bin/${tp}-nm -D --defined-only ${sysroot}/usr/lib/${multiarch}/libSDL2-2.0.so.0 \
      | awk '$2 == "T" { print "void " $3 "(void) {}" }' > stub.c
    ${cc} -shared -fPIC -o $out/lib/libSDL2.so -Wl,-soname,libSDL2-2.0.so.0 stub.c
  '';

  # Based on the tested R36S SDL3 build in Grumbel/sdl-jstest. SDL3 loads
  # the device's KMS/DRM, GBM, EGL and udev drivers at runtime; our build
  # uses the eoan headers and ships just libSDL3.so.0 in the port folder.
  sdl3 = pkgs.stdenv.mkDerivation {
    pname = "xanatoo-sdl3-r36s";
    inherit (pkgs.sdl3) version src;
    nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
    PKG_CONFIG_PATH = "${sysroot}/usr/lib/${multiarch}/pkgconfig";
    PKG_CONFIG_LIBDIR = "${sysroot}/usr/lib/${multiarch}/pkgconfig";
    PKG_CONFIG_SYSROOT_DIR = "${sysroot}";
    cmakeFlags = [
      "-DCMAKE_SYSTEM_NAME=Linux"
      "-DCMAKE_SYSTEM_PROCESSOR=aarch64"
      "-DCMAKE_SYSROOT=${sysroot}"
      "-DCMAKE_C_COMPILER=${cc}"
      "-DCMAKE_AR=${binutils}/bin/${tp}-ar"
      "-DCMAKE_RANLIB=${binutils}/bin/${tp}-ranlib"
      "-DCMAKE_STRIP=${binutils}/bin/${tp}-strip"
      "-DCMAKE_BUILD_TYPE=Release"
      "-DCMAKE_SKIP_RPATH=ON"
      "-DCMAKE_INSTALL_LIBDIR=lib"
      "-DCMAKE_INSTALL_INCLUDEDIR=include"
      "-DSDL_SHARED=ON"
      "-DSDL_STATIC=OFF"
      "-DSDL_TESTS=OFF"
      "-DSDL_EXAMPLES=OFF"
      "-DSDL_REVISION=SDL-${pkgs.sdl3.version}"
      "-DSDL_VENDOR_INFO=xanatoo R36S"
      "-DSDL_UNIX_CONSOLE_BUILD=ON"
      "-DSDL_KMSDRM=ON"
      "-DSDL_OPENGLES=ON"
      "-DSDL_OPENGL=OFF"
      "-DSDL_VULKAN=OFF"
      "-DSDL_RENDER_VULKAN=OFF"
      "-DSDL_RENDER_GPU=OFF"
      "-DSDL_X11=OFF"
      "-DSDL_WAYLAND=OFF"
      "-DSDL_ALSA=OFF"
      "-DSDL_PULSEAUDIO=OFF"
      "-DSDL_PIPEWIRE=OFF"
      "-DSDL_JACK=OFF"
      "-DSDL_SNDIO=OFF"
      "-DSDL_DBUS=OFF"
      "-DSDL_IBUS=OFF"
      "-DSDL_LIBURING=OFF"
      "-DSDL_HIDAPI_LIBUSB=OFF"
      "-DSDL_LIBUDEV=ON"
      "-DCMAKE_LIBRARY_PATH=/lib/${multiarch}"
    ];
    dontPatchELF = true;
    dontPatchShebangs = true;
    dontStrip = true;
  };

  mkApp = withSDL3: pkgs.stdenv.mkDerivation {
    pname = if withSDL3 then "xanatoo-r36s-sdl3" else "xanatoo-r36s";
    inherit version;
    src = lib.cleanSource ../.;
    nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.python3 pkgs.librsvg ];
    PKG_CONFIG_PATH = "${sysroot}/usr/lib/${multiarch}/pkgconfig";
    PKG_CONFIG_SYSROOT_DIR = "${sysroot}";
    cmakeFlags = [
      "-DCMAKE_SYSTEM_NAME=Linux"
      "-DCMAKE_SYSTEM_PROCESSOR=aarch64"
      "-DCMAKE_SYSROOT=${sysroot}"
      "-DCMAKE_C_COMPILER=${cc}"
      "-DCMAKE_CXX_COMPILER=${cxx}"
      "-DCMAKE_AR=${binutils}/bin/${tp}-ar"
      "-DCMAKE_RANLIB=${binutils}/bin/${tp}-ranlib"
      "-DCMAKE_STRIP=${binutils}/bin/${tp}-strip"
      "-DCMAKE_BUILD_TYPE=Release"
      "-DPROJECT_VERSION_FULL=${version}"
      "-DCMAKE_SKIP_RPATH=ON"
      "-DCMAKE_CXX_STANDARD=20"
      "-DXANATOO_GEMINI=ON"
      "-DXANATOO_GOPHER=ON"
      "-DXANATOO_SPARTAN=ON"
      "-DXANATOO_NEX=ON"
      "-DXANATOO_FINGER=ON"
      "-DXANATOO_NNTP=ON"
      "-DXANATOO_R36S=ON"
      # No libvips in the ArkOS sysroot: stb_image decodes, BMP thumbnails.
      "-DXANATOO_THUMTOO=OFF"
      "-DFREETYPE_INCLUDE_DIRS=${sysroot}/usr/include/freetype2"
      "-DFREETYPE_INCLUDE_DIR_ft2build=${sysroot}/usr/include/freetype2"
      "-DFREETYPE_INCLUDE_DIR_freetype2=${sysroot}/usr/include/freetype2"
      "-DFREETYPE_LIBRARY=${sysroot}/usr/lib/${multiarch}/libfreetype.so"
      "-DFONTCONFIG_INCLUDE_DIRS=${sysroot}/usr/include"
      "-DFONTCONFIG_LIBRARIES=${sysroot}/usr/lib/${multiarch}/libfontconfig.so"
      "-DOPENSSL_ROOT_DIR=${sysroot}/usr"
      "-DOPENSSL_INCLUDE_DIR=${sysroot}/usr/include"
      "-DOPENSSL_SSL_LIBRARY=${sysroot}/usr/lib/${multiarch}/libssl.so"
      "-DOPENSSL_CRYPTO_LIBRARY=${sysroot}/usr/lib/${multiarch}/libcrypto.so"
      "-DSQLite3_INCLUDE_DIR=${sysroot}/usr/include"
      "-DSQLite3_LIBRARY=${sysroot}/usr/lib/${multiarch}/libsqlite3.so"
    ] ++ (if withSDL3 then [
      "-DXANATOO_SDL3=ON"
      "-DSDL3_DIR=${sdl3}/lib/cmake/SDL3"
    ] else [
      "-DXANATOO_SDL3=OFF"
      "-DSDL2_INCLUDE_DIRS=${sysroot}/usr/include/SDL2"
      "-DSDL2_LIBRARIES=${sdlStub}/lib/libSDL2.so"
    ]);
    dontPatchELF = true;
    dontPatchShebangs = true;
    postInstall = ''
      binary="$out/bin/xanatoo"
      readelf=${binutils}/bin/${tp}-readelf
      if "$readelf" -d "$binary" | grep -E 'NEEDED.*lib(stdc\+\+|gcc_s)\.so'; then
        echo "Unexpected shared GCC runtime dependency" >&2
        exit 1
      fi
      glibc_versions=$("$readelf" --version-info "$binary" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu)
      if [ "$(printf '%s\n' "$glibc_versions" GLIBC_2.30 | sort -V | tail -n1)" != GLIBC_2.30 ]; then
        echo "Binary requires a newer glibc than ArkOS provides: $glibc_versions" >&2
        exit 1
      fi
      ${binutils}/bin/${tp}-strip "$binary"
    '';
    dontStrip = true;
  };

  r36s = mkApp true;
  r36sSDL3 = r36s;
  r36sSDL2 = mkApp false;

  mkPortmaster = { app, withSDL3 }: pkgs.stdenvNoCC.mkDerivation {
    pname = if withSDL3 then "xanatoo-r36s-sdl3-portmaster"
            else "xanatoo-r36s-portmaster";
    inherit version;
    dontUnpack = true;
    dontPatchShebangs = true;
    dontFixup = true;
    installPhase = ''
      mkdir -p $out/xanatoo/examples $out/xanatoo/styles $out/xanatoo/sites $out/xanatoo/icons $out/xanatoo/fonts
      install -m755 ${app}/bin/xanatoo $out/xanatoo/xanatoo
      cp -r ${../examples}/. $out/xanatoo/examples/
      cp ${../styles}/*.xss $out/xanatoo/styles/
      cp ${../sites}/*.xsite $out/xanatoo/sites/
      cp ${../FORMAT.md} $out/xanatoo/FORMAT.md
      if [ -d ${app}/share/xanatoo/icons ]; then
        cp -r ${app}/share/xanatoo/icons/. $out/xanatoo/icons/
      fi
      # Not every ArkOS/dArkOS image has SQLite or OpenSSL 1.1; ship the
      # sysroot copies (eoan). Newer CFWs often only provide libssl3.
      mkdir -p $out/xanatoo/libs
      cp -L ${sysroot}/usr/lib/${multiarch}/libsqlite3.so.0 $out/xanatoo/libs/
      cp -L ${sysroot}/usr/lib/${multiarch}/libssl.so.1.1 $out/xanatoo/libs/
      cp -L ${sysroot}/usr/lib/${multiarch}/libcrypto.so.1.1 $out/xanatoo/libs/
      cp ${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf \
         $out/xanatoo/fonts/DejaVuSans.ttf
      cp ${../data/fonts/latinmodern-math.otf} $out/xanatoo/fonts/latinmodern-math.otf
      cp ${../data/fonts/LICENSE-LatinModernMath.txt} $out/xanatoo/fonts/LICENSE-LatinModernMath.txt
      cp ${../LICENSE} $out/xanatoo/LICENSE.txt
      cp ${../data/icons/hicolor/256x256/apps/xanatoo.png} $out/xanatoo/cover.png
      cp ${../docs/screenshot.png} $out/screenshot.png
      cp ${../mk/r36s/Xanatoo.sh} $out/Xanatoo.sh
      chmod +x $out/Xanatoo.sh
      substitute ${../mk/r36s/port.json} $out/port.json --subst-var-by version ${version}
      cp ${../mk/r36s/README.md} $out/README.md
    '' + lib.optionalString withSDL3 ''
      cp -L ${sdl3}/lib/libSDL3.so.0 $out/xanatoo/libs/
    '';
  };

  portmaster = mkPortmaster { app = r36s; withSDL3 = true; };
  portmasterSDL3 = portmaster;
  portmasterSDL2 = mkPortmaster { app = r36sSDL2; withSDL3 = false; };

  mkPortmasterZip = { port, withSDL3 }: pkgs.runCommand
    "xanatoo-${version}-r36s${lib.optionalString withSDL3 "-sdl3"}-portmaster.zip" {
    nativeBuildInputs = [ pkgs.zip ];
  } ''
    mkdir -p $out
    cd ${port}
    zip -r -9 $out/xanatoo.zip .
  '';

  portmasterZip = mkPortmasterZip { port = portmaster; withSDL3 = true; };
  portmasterSDL3Zip = portmasterZip;
  portmasterSDL2Zip = mkPortmasterZip { port = portmasterSDL2; withSDL3 = false; };

in {
  inherit sysroot toolchain sdl3 r36s r36sSDL3 r36sSDL2 portmaster portmasterZip
    portmasterSDL3 portmasterSDL3Zip portmasterSDL2 portmasterSDL2Zip;
}
