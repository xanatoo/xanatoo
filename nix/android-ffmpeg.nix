# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

{ pkgs, androidSdk, packagePlatform, abi }:

let
  target = {
    "arm64-v8a" = { arch = "aarch64"; triple = "aarch64-linux-android"; };
    "armeabi-v7a" = { arch = "arm"; triple = "armv7a-linux-androideabi"; };
    "x86_64" = { arch = "x86_64"; triple = "x86_64-linux-android"; };
  }.${abi};
in pkgs.stdenvNoCC.mkDerivation {
  pname = "xanatoo-android-ffmpeg-${abi}";
  inherit (pkgs.ffmpeg-headless) src version;
  nativeBuildInputs = [ pkgs.stdenv.cc pkgs.cmake pkgs.gnumake pkgs.nasm ];
  dontUseCmakeConfigure = true;
  enableParallelBuilding = true;
  dontStrip = true;

  configurePhase = ''
    runHook preConfigure
    export ANDROID_HOME=${androidSdk}/libexec/android-sdk
    source ${../mk/android/scripts/ndk.sh}
    ndk="$(resolve_ndk)"
    toolchain=("$ndk"/toolchains/llvm/prebuilt/*)
    tools="''${toolchain[0]}/bin"
    # Build Mbed TLS for configure's link check. The APK already links its
    # own static Mbed TLS module from the same source.
    cmake -S ${pkgs.mbedtls.src} -B "$TMPDIR/mbedtls" \
      -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=${abi} -DANDROID_PLATFORM=android-${packagePlatform} \
      -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
    cmake --build "$TMPDIR/mbedtls" --parallel "$NIX_BUILD_CORES"

    ./configure --prefix="$out" --target-os=android --arch=${target.arch} \
      --enable-cross-compile --cc="$tools/${target.triple}${packagePlatform}-clang" \
      --ar="$tools/llvm-ar" --nm="$tools/llvm-nm" --ranlib="$tools/llvm-ranlib" \
      --extra-cflags="-I${pkgs.mbedtls.src}/include" \
      --extra-ldflags="-L$TMPDIR/mbedtls/library" \
      --disable-autodetect --disable-everything --disable-programs --disable-doc \
      --disable-avdevice --disable-avfilter --disable-symver \
      --enable-static --disable-shared --enable-pic --enable-mbedtls --enable-version3 \
      --enable-protocol=file,http,https,tcp,tls,crypto \
      --enable-demuxer=hls,aac,ac3,eac3,mov,mp3,mpegts \
      --enable-parser=aac,ac3,h264,hevc,mpegaudio,mpegvideo \
      --enable-decoder=aac,aac_latm,ac3,eac3,h264,hevc,mp3,mpeg2video,opus
    runHook postConfigure
  '';

  postInstall = ''
    {
      echo "create $out/lib/libhls.a"
      for lib in avformat avcodec avutil swresample swscale; do
        echo "addlib $out/lib/lib$lib.a"
      done
      echo save
      echo end
    } | "$tools/llvm-ar" -M
    rm "$out"/lib/libav*.a "$out"/lib/libsw*.a
    rm -r "$out/lib/pkgconfig"
  '';
}
