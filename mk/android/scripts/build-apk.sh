#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the APK: xanatoo's sources as libmain.so with ndk-build against the
# prebuilt SDL3 and a static FreeType, packaged with SDL's Java side,
# aligned and signed (after Kurvenrausch / Pingus). Data files (examples,
# FORMAT.md, style sheets, toolbar icons) go to assets/xanatoo/ with a
# MANIFEST that platform.cpp unpacks into internal storage.
#
# Environment comes from nix/android.nix. ANDROID_PROJECT_DIR points to
# mk/android, and APP_SRC_DIR points to the application sources and assets.
# Run in a dedicated build directory. --configure stages the NDK project.
set -euo pipefail

# shellcheck source=ndk.sh
. "$(dirname "$0")/ndk.sh"
NDK="$(resolve_ndk)"
jobs=${NIX_BUILD_CORES:-$(nproc)}
printf 'Android SDK: %s (build-tools %s, compile API %s, minimum API %s)\n' \
  "$ANDROID_HOME" "$BUILD_TOOLS_VERSION" "$COMPILE_PLATFORM" "$PACKAGE_PLATFORM"
printf 'Android NDK: %s (ABIs: %s, jobs: %s)\n' "$NDK" "$TARGET_ABIS" "$jobs"
printf 'Android APK: %s/out/%s.apk (version %s, code %s)\n' \
  "$PWD" "$APP_NAME" "$APP_VERSION" "$VERSION_CODE"
APP_DIR="$ANDROID_PROJECT_DIR/app"
BT="$ANDROID_HOME/build-tools/$BUILD_TOOLS_VERSION"
PACKAGE_JAR="$ANDROID_HOME/platforms/android-$PACKAGE_PLATFORM/android.jar"
COMPILE_JAR="$ANDROID_HOME/platforms/android-$COMPILE_PLATFORM/android.jar"

# Preserve the timestamps of unchanged inputs for ndk-build. Compare
# contents because unrelated Nix store inputs can share the same timestamps.
sync_inputs() {
  rsync -rlpc --chmod=Du+rwx,Fu+rw "$@"
}

# Removing a wildcard source drops a Make prerequisite without creating a
# newer input. Invalidate its module so the old object leaves the library.
sync_sources() {
  local makefile=$1 changes
  shift
  changes=$(sync_inputs --itemize-changes "$@")
  if grep -Eq '^\*deleting .*\.(c|cpp)$' <<< "$changes"; then
    touch "$makefile"
  fi
}

mkdir -p src/jni/{src/src,SDL/include,freetype,mbedtls,secp256k1,sqlite,thumtoo,ffmpeg/deps}
sync_inputs "$APPLICATION_MK" src/jni/Application.mk
sync_inputs "$TOP_ANDROID_MK" src/jni/Android.mk
sync_inputs "$APP_DIR/jni/Android.mk" src/jni/src/Android.mk

# Stage the native sources and private headers together for quoted includes.
sync_sources src/jni/src/Android.mk --delete-excluded --exclude=tls_openssl.cpp --exclude=https_wasm.cpp \
  --include='/*.cpp' --include='/*.hpp' --exclude='*' \
  "$APP_SRC_DIR/src/" src/jni/src/src/
sync_inputs --delete "$APP_SRC_DIR/include/" src/jni/src/include/
sync_inputs --delete "$APP_SRC_DIR/external/" src/jni/src/external/

# FreeType ships as a tarball. Extract it again only when its input changes.
if [ "$(cat src/freetype-source 2>/dev/null || true)" != "$FREETYPE_SRC" ]; then
  unpacked="$(mktemp -d)"
  trap 'rm -rf -- "$unpacked"' EXIT
  tar xf "$FREETYPE_SRC" -C "$unpacked" --strip-components=1
  sync_inputs --delete "$unpacked/" src/jni/freetype/
  printf '%s\n' "$FREETYPE_SRC" > src/freetype-source
  rm -rf -- "$unpacked"
  trap - EXIT
fi
sync_inputs "$ANDROID_PROJECT_DIR/freetype/Android.mk" src/jni/freetype/Android.mk

for module in mbedtls secp256k1; do
  case "$module" in
    mbedtls) module_src=$MBEDTLS_SRC; code_dir=library ;;
    secp256k1) module_src=$SECP256K1_SRC; code_dir=src ;;
  esac
  sync_inputs --delete "$module_src/include/" "src/jni/$module/include/"
  sync_inputs "$ANDROID_PROJECT_DIR/$module/Android.mk" "src/jni/$module/Android.mk"
  sync_sources "src/jni/$module/Android.mk" --delete "$module_src/$code_dir/" "src/jni/$module/$code_dir/"
done
sync_inputs "$SQLITE_SRC/sqlite3.c" "$SQLITE_SRC/sqlite3.h" src/jni/sqlite/
sync_inputs "$ANDROID_PROJECT_DIR/sqlite/Android.mk" src/jni/sqlite/Android.mk

T=src/jni/thumtoo
mkdir -p "$T/gen/thumtoo" "$T/deps"
sync_inputs "$ANDROID_PROJECT_DIR/thumtoo/Android.mk" "$T/Android.mk"
sync_sources "$T/Android.mk" --delete "$APP_SRC_DIR/external/thumtoo/src/" "$T/src/"
for dir in include external; do
  sync_inputs --delete "$APP_SRC_DIR/external/thumtoo/$dir/" "$T/$dir/"
done
sync_inputs "$ANDROID_PROJECT_DIR/ffmpeg.Android.mk" src/jni/ffmpeg/Android.mk
for abi in $TARGET_ABIS; do
  sync_inputs --delete "$FFMPEG_LIBS/$abi/" "src/jni/ffmpeg/deps/$abi/"
  sync_inputs --delete "$IMAGELIBS/$abi/" "$T/deps/$abi/"
done
tv="$(tr -d ' \n' < "$APP_SRC_DIR"/external/thumtoo/VERSION)"
IFS=. read -r tv_major tv_minor tv_patch <<< "${tv%%-*}"
sed -e "s/@THUMTOO_VERSION_MAJOR@/$tv_major/" -e "s/@THUMTOO_VERSION_MINOR@/$tv_minor/" \
  -e "s/@THUMTOO_VERSION_PATCH@/$tv_patch/" -e "s/@THUMTOO_VERSION_STRING@/$tv/" \
  -e "s/@THUMTOO_HAVE_[A-Z]*@/0/" \
  "$APP_SRC_DIR"/external/thumtoo/include/thumtoo/version.hpp.in > "$T/version.hpp.tmp"
sync_inputs "$T/version.hpp.tmp" "$T/gen/thumtoo/version.hpp"
rm "$T/version.hpp.tmp"

sync_inputs "$SDL_PREBUILT_MK" src/jni/SDL/Android.mk
sync_inputs --delete "$SDL_ANDROID_LIBS/include/" src/jni/SDL/include/

# ndk-build reads the manifest to choose debug or release objects on the
# first invocation as well as subsequent invocations.
sed -e "s/@VERSION_NAME@/$APP_VERSION/" -e "s/@VERSION_CODE@/$VERSION_CODE/" \
  -e "s/@MIN_SDK@/$PACKAGE_PLATFORM/" \
  "$APP_DIR/AndroidManifest.xml" > src/AndroidManifest.xml.tmp
# version.cpp embeds APP_VERSION via a compiler flag, which ndk-build
# does not track as a source dependency.
if ! cmp -s src/AndroidManifest.xml.tmp src/AndroidManifest.xml; then
  touch src/jni/src/src/version.cpp
fi
sync_inputs src/AndroidManifest.xml.tmp src/AndroidManifest.xml
rm src/AndroidManifest.xml.tmp

if [ "${1:-}" = --configure ]; then
  exit 0
fi

"$NDK/ndk-build" \
  NDK_PROJECT_PATH="$PWD/src" \
  APP_BUILD_SCRIPT="$PWD/src/jni/Android.mk" \
  NDK_APPLICATION_MK="$PWD/src/jni/Application.mk" \
  XANATOO_VERSION="$APP_VERSION" \
  -j"$jobs"

# Recreate packaging inputs so deleted files and old ABIs cannot leak into
# the next APK. Keep the native object tree and the last successfully signed APK.
rm -rf assets out/classes out/lib out/classes.dex
mkdir -p src/res
sync_inputs --delete "$APP_DIR/res/" src/res/

# Data files → assets/xanatoo/ + MANIFEST (stamp line, then one path per
# line; SDL can read assets but not list them).
A=assets/xanatoo
mkdir -p "$A/examples" "$A/styles" "$A/sites" "$A/icons" "$A/fonts" "$A/certs"
for f in DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSans-BoldOblique \
         DejaVuSerif DejaVuSerif-Bold DejaVuSerif-Italic DejaVuSerif-BoldItalic \
         DejaVuSansMono DejaVuSansMono-Bold DejaVuSansMono-Oblique \
         DejaVuSansMono-BoldOblique; do
  cp "$DEJAVU_DIR/$f.ttf" "$A/fonts/"
done
# TeX math font (find_math_font_path), GUST Font License.
cp "$APP_SRC_DIR"/data/fonts/latinmodern-math.otf \
   "$APP_SRC_DIR"/data/fonts/LICENSE-LatinModernMath.txt "$A/fonts/"
cp -r "$APP_SRC_DIR"/examples/. "$A/examples/"
cp "$APP_SRC_DIR"/styles/*.xss "$A/styles/"
cp "$APP_SRC_DIR"/sites/*.xsite "$A/sites/"
cp "$APP_SRC_DIR"/FORMAT.md "$A/"
cp "$CA_BUNDLE" "$A/certs/ca-bundle.crt"
python3 "$APP_SRC_DIR"/scripts/rasterize_icons.py "$APP_SRC_DIR"/icons/svg "$A/icons" 64
chmod -R u+rwX assets
files="$(cd "$A" && find . -type f ! -name MANIFEST | sed 's|^\./||' | LC_ALL=C sort)"
hash="$(cd "$A" && printf '%s\n' "$files" | xargs sha256sum | sha256sum | cut -c1-16)"
{ echo "xanatoo $APP_VERSION ($VERSION_CODE) $hash"; printf '%s\n' "$files"; } > "$A/MANIFEST"
echo "==> $(printf '%s\n' "$files" | wc -l) data files in assets/xanatoo"

mkdir -p out
"$BT/aapt" package -f -M src/AndroidManifest.xml -S src/res -A assets \
  -I "$PACKAGE_JAR" -F out/base.apk
mkdir -p out/classes
mapfile -d "" -t java_sources < <(find "$APP_DIR/java" -name '*.java' -print0)
javac -encoding UTF-8 --release 8 -classpath "$COMPILE_JAR:$SDL_ANDROID_LIBS/classes" \
  -d out/classes "${java_sources[@]}"
"$BT/d8" --output out --min-api "$PACKAGE_PLATFORM" --lib "$COMPILE_JAR" \
  $(find "$SDL_ANDROID_LIBS/classes" out/classes -name '*.class')
for abi in $TARGET_ABIS; do
  mkdir -p "out/lib/$abi"
  cp src/libs/"$abi"/*.so "out/lib/$abi/"
done
( cd out && "$BT/aapt" add base.apk classes.dex )

package_apk() {
  local name=$1
  shift
  cp out/base.apk out/unsigned.apk
  ( cd out && zip -r unsigned.apk "$@" )
  "$BT/zipalign" -f 4 out/unsigned.apk out/aligned.apk
  "$BT/apksigner" sign --ks "$ANDROID_PROJECT_DIR/keystore/debug.keystore" --ks-pass pass:android --key-pass pass:android \
    --v4-signing-enabled false --out "out/$name.new.apk" out/aligned.apk
}

package_apk "$APP_NAME" lib
read -r -a abis <<< "$TARGET_ABIS"
if (( ${#abis[@]} > 1 )); then
  for abi in "${abis[@]}"; do
    package_apk "$APP_NAME-$abi" "lib/$abi"
  done
  for abi in "${abis[@]}"; do
    mv "out/$APP_NAME-$abi.new.apk" "out/$APP_NAME-$abi.apk"
  done
fi
mv "out/$APP_NAME.new.apk" "out/$APP_NAME.apk"
"$BT/aapt" dump badging "out/$APP_NAME.apk" | sed -n '1,5p'
