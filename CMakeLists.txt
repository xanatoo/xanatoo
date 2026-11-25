# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

cmake_minimum_required(VERSION 3.16)
# Version (gamepkgstd VER-1, VER-6): VERSION is the only place it is
# written; Nix passes the full dev string (0.1.0-dev.<revCount>+g<rev>).
if(NOT DEFINED PROJECT_VERSION_FULL)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_FULL LIMIT_COUNT 1)
endif()
# project() only accepts numeric versions
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" _version_numeric "${PROJECT_VERSION_FULL}")
project(xanatoo VERSION ${_version_numeric} LANGUAGES CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(XANATOO_SDL3 "Use the native SDL3 window, input and renderer backend" ON)
option(XANATOO_R36S "Use compact default UI scale for 640x480 R36S" OFF)

# Catch common mistakes; ImGui is vendored and not under our control.
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# The Windows / browser ports deliberately ship a useful offline reader and
# editor. SDL3 is the common backend on desktop, Android and handheld builds.
set(_minimal_port_default OFF)
if(WIN32 OR EMSCRIPTEN)
  set(_minimal_port_default ON)
endif()
option(XANATOO_MINIMAL_PORT "Offline, dependency-light cross-platform build" ${_minimal_port_default})
# Browser build with fixed-layout documents: a prefix holding the Emscripten
# builds of thumtoo's libraries (libvips, glib, libarchive, SQLite, MuPDF,
# DjVuLibre, libunarr; nix/wasm-imagelibs.nix). thumtoo then needs pthreads,
# so the page must be cross-origin isolated (docs/PORTS.md).
set(XANATOO_WASM_DEPS_ROOT "" CACHE PATH "Emscripten prefix with thumtoo's libraries")
if(XANATOO_MINIMAL_PORT)
  set(_minimal_off FONTCONFIG SQLITE THUMTOO GEMINI NOSTR REDDIT LEMMY MATRIX HN YOUTUBE VIDEO
                   FORUMS EBOOKS FEEDS WEB WIKIPEDIA)
  # Gopher/Spartan/Nex/Finger use plain TCP only (no TLS library). Keep them
  # available under MINIMAL_PORT except on Emscripten (no raw sockets).
  # Browser fetch (src/https_wasm.cpp) replaces sockets + TLS. Synchronous
  # fetches must run on worker threads, so this needs the pthreads build.
  # Only services whose APIs are CORS-open can work: Gemini / Nostr / Reddit /
  # YouTube stay off. Ebooks use the native EPUB reader (no pandoc).
  set(XANATOO_WASM_FETCH OFF)
  if(EMSCRIPTEN AND XANATOO_WASM_DEPS_ROOT)
    set(XANATOO_WASM_FETCH ON)
    foreach(_f LEMMY MATRIX HN VIDEO EBOOKS FEEDS WEB WIKIPEDIA)
      list(REMOVE_ITEM _minimal_off ${_f})
    endforeach()
  endif()
  if(EMSCRIPTEN AND XANATOO_WASM_DEPS_ROOT)
    list(REMOVE_ITEM _minimal_off THUMTOO)
    set(XANATOO_THUMTOO ON CACHE BOOL "thumtoo from XANATOO_WASM_DEPS_ROOT" FORCE)
    list(APPEND CMAKE_PREFIX_PATH "${XANATOO_WASM_DEPS_ROOT}")
    set(ENV{PKG_CONFIG_PATH} "${XANATOO_WASM_DEPS_ROOT}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_LIBDIR} "${XANATOO_WASM_DEPS_ROOT}/lib/pkgconfig")
    add_compile_options(-pthread)
    add_link_options(-pthread -sPTHREAD_POOL_SIZE=8 "-L${XANATOO_WASM_DEPS_ROOT}/lib")
  endif()
  foreach(_feature ${_minimal_off})
    set(XANATOO_${_feature} OFF CACHE BOOL "Disabled by XANATOO_MINIMAL_PORT" FORCE)
  endforeach()
  # Plain TCP (BSD sockets) is available on Unix-like hosts. Windows
  # minimal builds stay offline; Emscripten has no raw TCP.
  if(EMSCRIPTEN OR WIN32)
    foreach(_feature GOPHER SPARTAN NEX FINGER NNTP)
      set(XANATOO_${_feature} OFF CACHE BOOL
          "Small-net TCP not enabled on this minimal target" FORCE)
    endforeach()
  endif()
endif()

if(EMSCRIPTEN)
  # Nix supplies offline-built SDL3 and FreeType libraries. SDL3_DIR may also
  # be set explicitly by standalone Emscripten builds.
  if(XANATOO_WASM_SDL3_ROOT)
    set(SDL3_DIR "${XANATOO_WASM_SDL3_ROOT}/lib/cmake/SDL3")
  endif()
  add_library(Freetype::Freetype INTERFACE IMPORTED)
  if(XANATOO_WASM_FREETYPE_ROOT)
    set_target_properties(Freetype::Freetype PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${XANATOO_WASM_FREETYPE_ROOT}/include/freetype2"
      INTERFACE_LINK_LIBRARIES "${XANATOO_WASM_FREETYPE_ROOT}/lib/libfreetype.a")
  else()
    add_compile_options(-sUSE_FREETYPE=1)
    add_link_options(-sUSE_FREETYPE=1)
  endif()
endif()

# Legacy SDL2 builds can still pass SDL2_INCLUDE_DIRS / SDL2_LIBRARIES.
if(XANATOO_SDL3)
  find_package(SDL3 REQUIRED CONFIG)
  set(SDL2_LIBRARIES SDL3::SDL3)
  set(SDL2_INCLUDE_DIRS "")
else()
  if(NOT SDL2_LIBRARIES)
    find_package(SDL2 REQUIRED)
  endif()
  if(TARGET SDL2::SDL2)
    set(SDL2_LIBRARIES SDL2::SDL2)
  endif()
endif()
if(NOT FREETYPE_LIBRARY AND NOT TARGET Freetype::Freetype)
  find_package(Freetype REQUIRED)
elseif(FREETYPE_LIBRARY AND NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype UNKNOWN IMPORTED)
  set_target_properties(Freetype::Freetype PROPERTIES
    IMPORTED_LOCATION "${FREETYPE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FREETYPE_INCLUDE_DIRS}")
endif()
# FontConfig resolves font families; without it (Android) family names map
# onto files in known font directories (see font.cpp).
option(XANATOO_FONTCONFIG "Resolve fonts with FontConfig" ON)
# SQLite side store (persisted section folds); without it folds are per-session.
option(XANATOO_SQLITE "Persist view state in SQLite" ON)
# Cross builds (R36S) may pass FONTCONFIG_* explicitly; otherwise pkg-config.
if(XANATOO_FONTCONFIG AND NOT FONTCONFIG_LIBRARIES)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
endif()
if(NOT XANATOO_FONTCONFIG)
  set(FONTCONFIG_LIBRARIES "")
  set(FONTCONFIG_INCLUDE_DIRS "")
endif()

# Gemini protocol (TLS client). Parser for .gmi is always built.
option(XANATOO_GEMINI "Enable gemini:// network client (TLS)" ON)
option(XANATOO_GOPHER "Enable gopher:// network client (plain TCP)" ON)
option(XANATOO_SPARTAN "Enable spartan:// network client (plain TCP)" ON)
option(XANATOO_NEX "Enable nex:// network client (plain TCP)" ON)
option(XANATOO_FINGER "Enable finger:// network client (plain TCP)" ON)
option(XANATOO_NNTP "Enable news: / nntp: Usenet reader (plain TCP, nntps: with TLS)" ON)
# Nostr read-only client (WSS + OpenSSL). Maps notes to Thread/Message nodes.
option(XANATOO_NOSTR "Enable Nostr note/thread fetch (TLS WebSocket)" ON)
option(XANATOO_REDDIT "Enable Reddit .json fetch (TLS HTTPS)" ON)
option(XANATOO_LEMMY "Enable Lemmy API v3 fetch (TLS HTTPS)" ON)
option(XANATOO_MATRIX "Enable read-only Matrix rooms / directories (Client-Server API, TLS HTTPS)" ON)
option(XANATOO_YOUTUBE "Enable YouTube channel / video / comment fetch (InnerTube, TLS HTTPS)" ON)
option(XANATOO_VIDEO "Enable PeerTube / Odysee / Dailymotion fetch (public JSON APIs, TLS HTTPS)" ON)
option(XANATOO_HN "Enable Hacker News fetch (Firebase + Algolia APIs, TLS HTTPS)" ON)
option(XANATOO_FORUMS "Enable Discourse / Lobsters / 4chan / Stack Exchange / Bluesky / Flarum / LessWrong / Digg fetch (public JSON / MCP, TLS HTTPS)" ON)
option(XANATOO_EBOOKS "Enable Standard Ebooks / remote EPUB downloads (TLS HTTPS)" ON)
option(XANATOO_FEEDS "Enable RSS / Atom feed fetch (TLS HTTPS)" ON)
option(XANATOO_WEB "Open other http(s) pages in-app as reader views (TLS HTTPS)" ON)
option(XANATOO_WIKIPEDIA "wikipedia: articles via the Wikimedia REST API (TLS HTTPS)" ON)

add_executable(xanatoo
  src/main.cpp
  src/fixed_layout.cpp
  src/app_pagetiles.cpp
  src/app_init.cpp
  src/app_buffers.cpp
  src/app_view.cpp
  src/app_session.cpp
  src/app_nav.cpp
  src/app_history.cpp
  src/app_softwin.cpp
  src/app_windows.cpp
  src/app_prompts.cpp
  src/app_pointer.cpp
  src/app_touch.cpp
  src/app_events.cpp
  src/app_frame.cpp
  src/app_chrome.cpp
  src/app_keymap.cpp
  src/app_fetch.cpp
  src/app_subscriptions.cpp
  src/app_downloads.cpp
  src/app_certs.cpp
  src/app_identity.cpp
  src/app_related.cpp
  src/app_split.cpp
  src/app_find.cpp
  src/app_footnotes.cpp
  src/app_sitemap.cpp
  src/app_search.cpp
  src/app_page.cpp
  src/app_pages.cpp
  src/paginate.cpp
  src/app_collection.cpp
  src/app_gallery.cpp
  src/app_hoverhelp.cpp
  src/app_drafts.cpp
  src/app_revisions.cpp
  src/app_nostr.cpp
  src/app_about.cpp
  src/app_passage.cpp
  src/app_quotes.cpp
  src/app_report.cpp
  src/about.cpp
  src/app_images.cpp
  src/theme.cpp
  src/font.cpp
  src/math_parse.cpp
  src/math_layout.cpp
  src/math_font.cpp
  src/document.cpp
  src/layout.cpp
  src/fold.cpp
  src/render.cpp
  src/parse.cpp
  src/xml.cpp
  src/source_view.cpp
  src/gallery.cpp
  src/org.cpp
  src/editor.cpp
  src/images.cpp
  src/url_cache.cpp
  src/object_store.cpp
  src/doc_codec.cpp
  src/snapshot.cpp
  src/ui.cpp
  src/ui_help.cpp
  src/util.cpp
  src/subscriptions.cpp
  src/minibuffer.cpp
  src/sidestore.cpp
  src/revstore.cpp
  src/sqlite_db.cpp
  src/statedb.cpp
  src/visits.cpp
  src/history_trail.cpp
  src/annotations.cpp
  src/epub_import.cpp
  src/epub_native.cpp
  src/office_import.cpp
  src/view_settings.cpp
  src/style.cpp
  src/platform.cpp
  src/cli.cpp
  src/paths.cpp
  src/http_url.cpp
  src/http_framing.cpp
  src/version.cpp
  src/gemini.cpp
  src/gopher.cpp
  src/spartan.cpp
  src/nex.cpp
  src/finger.cpp
  src/gemini_tofu.cpp
  src/gemini_identity.cpp
  src/nostr.cpp
  src/nostr_verify.cpp
  src/nostr_sign.cpp
  src/nostr_keys.cpp
  src/bech32.cpp
  src/nostr_store.cpp
  src/reddit.cpp
  src/reddit_credentials.cpp
  src/catalog_credentials.cpp
  src/lemmy.cpp
  src/lemmy_discovery.cpp
  src/lemmy_directory.cpp
  src/matrix.cpp
  src/matrix_directory.cpp
  src/hackernews.cpp
  src/forums.cpp
  src/forum_discourse.cpp
  src/forum_lobsters.cpp
  src/forum_4chan.cpp
  src/forum_stackexchange.cpp
  src/forum_bluesky.cpp
  src/forum_flarum.cpp
  src/forum_lesswrong.cpp
  src/forum_digg.cpp
  src/service_bar.cpp
  src/forum_fetch.cpp
  src/nntp.cpp
  src/nntp_fetch.cpp
  src/youtube.cpp
  src/video_sites.cpp
  src/archive.cpp
  src/forge.cpp
  src/wayback.cpp
  src/time.cpp
  src/ebooks.cpp
  src/feed.cpp
  src/page_links.cpp
  src/opensearch.cpp
  src/sitemap.cpp
  src/related_sites.cpp
  src/search.cpp
  src/search_results.cpp
  src/inflate.cpp
  src/html_min.cpp
  src/html_page.cpp
  src/site_hints.cpp
  src/html_footnotes.cpp
  src/download.cpp
  src/wikipedia.cpp
  src/locator.cpp
  src/quote_links.cpp
  src/markdown_body.cpp
  src/native.cpp
  src/audio.cpp
  src/byte_feed.cpp
  src/audio_codecs.cpp
  src/app_audio.cpp
  src/media.cpp
  src/audio_stream.cpp
  src/media_ffmpeg.cpp
  # Vendored Dear ImGui (MIT) — menu bar / toolbar shell
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp
)

# Vendored decoders (dr_mp3 / dr_flac / dr_wav / stb_vorbis): not our warnings.
set_source_files_properties(src/audio_codecs.cpp PROPERTIES COMPILE_OPTIONS "-w")

if(XANATOO_SDL3)
  target_sources(xanatoo PRIVATE src/imgui_sdl3.cpp)
else()
  target_sources(xanatoo PRIVATE
    external/imgui/backends/imgui_impl_sdl2.cpp
    external/imgui/backends/imgui_impl_sdlrenderer2.cpp)
endif()

target_include_directories(xanatoo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/external
  ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
  ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends
  ${SDL2_INCLUDE_DIRS}
  ${FREETYPE_INCLUDE_DIRS}
  ${FONTCONFIG_INCLUDE_DIRS}
)

if(NOT XANATOO_SQLITE)
  add_library(xanatoo_sqlite3 INTERFACE)
else()
find_package(SQLite3 REQUIRED)
# Imported target name differs by CMake version: SQLite::SQLite3 (some
# CMake versions) vs SQLite3::SQLite3. Normalise to xanatoo_sqlite3.
if(TARGET SQLite3::SQLite3)
  add_library(xanatoo_sqlite3 ALIAS SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  add_library(xanatoo_sqlite3 ALIAS SQLite::SQLite3)
else()
  add_library(xanatoo_sqlite3 UNKNOWN IMPORTED)
  set_target_properties(xanatoo_sqlite3 PROPERTIES
    IMPORTED_LOCATION "${SQLite3_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
endif()
endif()
target_compile_definitions(xanatoo PRIVATE
  "XANATOO_VERSION=\"${PROJECT_VERSION_FULL}\""
  "XANATOO_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
  XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>
  XANATOO_R36S=$<BOOL:${XANATOO_R36S}>
  XANATOO_SQLITE=$<BOOL:${XANATOO_SQLITE}>
  XANATOO_FONTCONFIG=$<BOOL:${XANATOO_FONTCONFIG}>)
# Dear ImGui's item hooks (ImGuiTestEngineHook_ItemInfo, defined in ui.cpp)
# tell the Help panel which widget the pointer is over; the vendored ImGui
# sources must see the same switch as ui.cpp.
target_compile_definitions(xanatoo PRIVATE IMGUI_ENABLE_TEST_ENGINE)

# nix/r36s.nix: newer GCC than the sysroot's libstdc++ 9 (__cxa_call_terminate).
if(XANATOO_R36S)
  target_sources(xanatoo PRIVATE src/r36s_abi.cpp)
endif()

if(NOT EMSCRIPTEN)
  find_package(Threads REQUIRED)
  set(XANATOO_THREADS Threads::Threads)
endif()
if(WIN32 AND TARGET SDL2::SDL2main)
  set(XANATOO_SDL2_MAIN SDL2::SDL2main)
endif()
target_link_libraries(xanatoo PRIVATE
  ${XANATOO_SDL2_MAIN}
  ${SDL2_LIBRARIES}
  ${XANATOO_THREADS}
  xanatoo_sqlite3
  Freetype::Freetype
  ${FONTCONFIG_LIBRARIES}
)
if(WIN32)
  set_target_properties(xanatoo PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MINGW)
    target_link_options(xanatoo PRIVATE -static-libgcc -static-libstdc++)
  endif()
endif()

# Images through thumtoo (vendored git subtree, shared with galapix /
# biltoo): libvips decoding and a tile cache for thumbnails. Without it
# images decode with stb_image (PNG / JPEG / GIF / BMP).
option(XANATOO_THUMTOO "Images through thumtoo (libvips, shared tile cache)" ON)
if(XANATOO_THUMTOO)
  set(THUMTOO_BUILD_TESTS OFF)
  set(THUMTOO_BUILD_TOOLS OFF)
  add_subdirectory(external/thumtoo ${CMAKE_BINARY_DIR}/thumtoo EXCLUDE_FROM_ALL)
  target_sources(xanatoo PRIVATE src/images_thumtoo.cpp
    src/fixed_layout_thumtoo.cpp)
  target_link_libraries(xanatoo PRIVATE thumtoo::thumtoo)
  if(EMSCRIPTEN AND XANATOO_WASM_DEPS_ROOT)
    # libwebp's static archives need it after themselves.
    target_link_libraries(xanatoo PRIVATE "${XANATOO_WASM_DEPS_ROOT}/lib/libsharpyuv.a")
  endif()
  target_compile_definitions(xanatoo PRIVATE XANATOO_THUMTOO=1)
  message(STATUS "Images: thumtoo (libvips)")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_THUMTOO=0)
  message(STATUS "Images: stb_image")
endif()

# Video and the audio formats the vendored decoders lack (media.hpp) through
# FFmpeg; without it those links go to an external player as before. Android
# uses the NDK's MediaCodec instead (mk/android, media_android.cpp).
set(_ffmpeg_default ON)
if(XANATOO_MINIMAL_PORT OR ANDROID)
  set(_ffmpeg_default OFF)
endif()
option(XANATOO_FFMPEG "Video / more audio formats through FFmpeg (libavformat, libavcodec)" ${_ffmpeg_default})
if(XANATOO_FFMPEG)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
      libavformat libavcodec libswscale libswresample libavutil)
  endif()
  if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found: building without video (-DXANATOO_FFMPEG=OFF)")
    set(XANATOO_FFMPEG OFF CACHE BOOL "" FORCE)
  endif()
endif()
if(XANATOO_FFMPEG)
  target_link_libraries(xanatoo PRIVATE PkgConfig::FFMPEG)
  target_compile_definitions(xanatoo PRIVATE XANATOO_FFMPEG=1)
  message(STATUS "Video: FFmpeg (libavformat ${FFMPEG_libavformat_VERSION})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_FFMPEG=0)
  message(STATUS "Video: none (external player)")
endif()

set(XANATOO_NEED_OPENSSL OFF)
if(XANATOO_GEMINI OR XANATOO_NOSTR OR XANATOO_REDDIT OR XANATOO_LEMMY OR XANATOO_MATRIX OR
   XANATOO_HN OR XANATOO_FORUMS OR XANATOO_YOUTUBE OR XANATOO_VIDEO OR XANATOO_EBOOKS OR XANATOO_FEEDS OR XANATOO_WEB OR
   XANATOO_WIKIPEDIA)
  set(XANATOO_NEED_OPENSSL ON)
endif()
set(XANATOO_NEED_TCP OFF)
if(XANATOO_NEED_OPENSSL OR XANATOO_GOPHER OR XANATOO_SPARTAN OR XANATOO_NEX OR
   XANATOO_FINGER OR XANATOO_NNTP)
  set(XANATOO_NEED_TCP ON)
endif()
if(XANATOO_NEED_TCP AND NOT EMSCRIPTEN)
  target_sources(xanatoo PRIVATE src/tcp.cpp)
endif()

# TLS backend for the network clients (tls.hpp): OpenSSL, or the lighter
# Mbed TLS (what the Android build uses).
set(XANATOO_TLS "openssl" CACHE STRING "TLS backend: openssl or mbedtls")
set_property(CACHE XANATOO_TLS PROPERTY STRINGS openssl mbedtls)
if(XANATOO_NEED_OPENSSL AND XANATOO_WASM_FETCH)
  target_sources(xanatoo PRIVATE src/https_wasm.cpp)
  target_compile_definitions(xanatoo PRIVATE XANATOO_HTTPS=1)
  target_link_options(xanatoo PRIVATE -sFETCH=1)
  message(STATUS "HTTP transport: browser fetch (CORS applies)")
elseif(XANATOO_NEED_OPENSSL)
  target_sources(xanatoo PRIVATE src/https.cpp)
  # https.hpp is available: remote images download (app_images.cpp).
  target_compile_definitions(xanatoo PRIVATE XANATOO_HTTPS=1)
  if(XANATOO_TLS STREQUAL "mbedtls")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MBEDTLS REQUIRED IMPORTED_TARGET mbedtls mbedx509 mbedcrypto)
    target_link_libraries(xanatoo PRIVATE PkgConfig::MBEDTLS)
    target_sources(xanatoo PRIVATE src/tls_mbedtls.cpp)
  elseif(XANATOO_TLS STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xanatoo PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_sources(xanatoo PRIVATE src/tls_openssl.cpp)
  else()
    message(FATAL_ERROR "XANATOO_TLS must be openssl or mbedtls")
  endif()
  message(STATUS "TLS backend: ${XANATOO_TLS}")
endif()

if(XANATOO_GEMINI)
  target_compile_definitions(xanatoo PRIVATE XANATOO_GEMINI=1)
  message(STATUS "Gemini protocol client: ON (${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_GEMINI=0)
  message(STATUS "Gemini protocol client: OFF")
endif()
if(XANATOO_GOPHER)
  target_compile_definitions(xanatoo PRIVATE XANATOO_GOPHER=1)
  message(STATUS "Gopher protocol client: ON")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_GOPHER=0)
  message(STATUS "Gopher protocol client: OFF")
endif()
if(XANATOO_SPARTAN)
  target_compile_definitions(xanatoo PRIVATE XANATOO_SPARTAN=1)
  message(STATUS "Spartan protocol client: ON")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_SPARTAN=0)
  message(STATUS "Spartan protocol client: OFF")
endif()
if(XANATOO_NEX)
  target_compile_definitions(xanatoo PRIVATE XANATOO_NEX=1)
  message(STATUS "Nex protocol client: ON")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_NEX=0)
  message(STATUS "Nex protocol client: OFF")
endif()
if(XANATOO_FINGER)
  target_compile_definitions(xanatoo PRIVATE XANATOO_FINGER=1)
  message(STATUS "Finger protocol client: ON")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_FINGER=0)
  message(STATUS "Finger protocol client: OFF")
endif()
if(XANATOO_NNTP)
  target_compile_definitions(xanatoo PRIVATE XANATOO_NNTP=1)
  message(STATUS "NNTP (Usenet) client: ON")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_NNTP=0)
  message(STATUS "NNTP (Usenet) client: OFF")
endif()

if(XANATOO_NOSTR)
  target_compile_definitions(xanatoo PRIVATE XANATOO_NOSTR=1)
  # secp256k1 (signature checks, signing accounts): OpenSSL's libcrypto, or
  # with Mbed TLS libsecp256k1 (Mbed TLS has no Schnorr signatures).
  set(_nostr_crypto "none: hash checks only, no signing")
  if(XANATOO_TLS STREQUAL "openssl")
    target_compile_definitions(xanatoo PRIVATE XANATOO_NOSTR_VERIFY_OPENSSL=1)
    set(_nostr_crypto "OpenSSL")
  elseif(XANATOO_TLS STREQUAL "mbedtls")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SECP256K1 QUIET IMPORTED_TARGET libsecp256k1)
    if(SECP256K1_FOUND)
      target_compile_definitions(xanatoo PRIVATE XANATOO_NOSTR_SECP256K1=1)
      target_link_libraries(xanatoo PRIVATE PkgConfig::SECP256K1)
      set(_nostr_crypto "libsecp256k1 ${SECP256K1_VERSION} + Mbed TLS")
    endif()
  endif()
  message(STATUS "Nostr client: ON (WSS, ${XANATOO_TLS}; secp256k1: ${_nostr_crypto})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_NOSTR=0)
  message(STATUS "Nostr client: OFF")
endif()

if(XANATOO_REDDIT)
  target_compile_definitions(xanatoo PRIVATE XANATOO_REDDIT=1)
  message(STATUS "Reddit .json client: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_REDDIT=0)
  message(STATUS "Reddit .json client: OFF")
endif()

if(XANATOO_LEMMY)
  target_compile_definitions(xanatoo PRIVATE XANATOO_LEMMY=1)
  message(STATUS "Lemmy API client: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_LEMMY=0)
  message(STATUS "Lemmy API client: OFF")
endif()

if(XANATOO_MATRIX)
  target_compile_definitions(xanatoo PRIVATE XANATOO_MATRIX=1)
  message(STATUS "Matrix client (read-only): ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_MATRIX=0)
  message(STATUS "Matrix client (read-only): OFF")
endif()

if(XANATOO_VIDEO)
  target_compile_definitions(xanatoo PRIVATE XANATOO_VIDEO=1)
  message(STATUS "Video sites (PeerTube, Odysee, Dailymotion): ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_VIDEO=0)
  message(STATUS "Video sites: OFF")
endif()

if(XANATOO_YOUTUBE)
  target_compile_definitions(xanatoo PRIVATE XANATOO_YOUTUBE=1)
  message(STATUS "YouTube client: ON (InnerTube, HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_YOUTUBE=0)
  message(STATUS "YouTube client: OFF")
endif()

if(XANATOO_HN)
  target_compile_definitions(xanatoo PRIVATE XANATOO_HN=1)
  message(STATUS "Hacker News client: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_HN=0)
  message(STATUS "Hacker News client: OFF")
endif()

if(XANATOO_FORUMS)
  target_compile_definitions(xanatoo PRIVATE XANATOO_FORUMS=1)
  message(STATUS "Forum clients: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_FORUMS=0)
  message(STATUS "Forum clients: OFF")
endif()

if(XANATOO_EBOOKS)
  target_compile_definitions(xanatoo PRIVATE XANATOO_EBOOKS=1)
  message(STATUS "Ebook downloads: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_EBOOKS=0)
  message(STATUS "Ebook downloads: OFF")
endif()

if(XANATOO_FEEDS)
  target_compile_definitions(xanatoo PRIVATE XANATOO_FEEDS=1)
  message(STATUS "RSS / Atom feeds: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_FEEDS=0)
  message(STATUS "RSS / Atom feeds: OFF")
endif()

if(XANATOO_WEB)
  target_compile_definitions(xanatoo PRIVATE XANATOO_WEB=1)
  message(STATUS "Web pages: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_WEB=0)
  message(STATUS "Web pages: OFF")
endif()

if(XANATOO_WIKIPEDIA)
  target_compile_definitions(xanatoo PRIVATE XANATOO_WIKIPEDIA=1)
  message(STATUS "Wikipedia articles: ON (HTTPS, ${XANATOO_TLS})")
else()
  target_compile_definitions(xanatoo PRIVATE XANATOO_WIKIPEDIA=0)
  message(STATUS "Wikipedia articles: OFF")
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(xanatoo PRIVATE m)
endif()

if(NOT EMSCRIPTEN)
  install(TARGETS xanatoo DESTINATION bin)
endif()

# Data directory (gamepkgstd DIR-3, paths.hpp): --data-dir, else
# <exe>/../share/xanatoo, else the install path compiled in here. The build
# tree mirrors the installed share/ (generated icons, the rest symlinked to
# the sources so edits show at once); nix develop passes
# --data-dir $XANATOO_BUILD_DIR/share/xanatoo.
set(XANATOO_BUILD_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/share/xanatoo)
file(MAKE_DIRECTORY ${XANATOO_BUILD_DATA_DIR})
foreach(_entry styles sites examples FORMAT.md)
  if(WIN32)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/${_entry}
         DESTINATION ${XANATOO_BUILD_DATA_DIR})
  else()
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/${_entry}
         ${XANATOO_BUILD_DATA_DIR}/${_entry} SYMBOLIC)
  endif()
endforeach()
if(WIN32)
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data/icons
       DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/share)
else()
  file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/data/icons
       ${CMAKE_CURRENT_BINARY_DIR}/share/icons SYMBOLIC)
endif()
target_compile_definitions(xanatoo PRIVATE
  "XANATOO_DATA_INSTALL_DIR=\"${CMAKE_INSTALL_PREFIX}/share/xanatoo\"")

# Toolbar icons: SVG sources → PNG at build time (ImageMagick or pure-Python fallback)
set(XANATOO_SVG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/icons/svg)
set(XANATOO_ICON_DIR ${XANATOO_BUILD_DATA_DIR}/icons)
file(GLOB XANATOO_SVGS CONFIGURE_DEPENDS "${XANATOO_SVG_DIR}/*.svg")
add_custom_command(
  OUTPUT ${XANATOO_ICON_DIR}/.stamp
  COMMAND ${CMAKE_COMMAND} -E make_directory ${XANATOO_ICON_DIR}
  COMMAND ${CMAKE_COMMAND} -E env python3
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rasterize_icons.py
          ${XANATOO_SVG_DIR} ${XANATOO_ICON_DIR} 64
  COMMAND ${CMAKE_COMMAND} -E touch ${XANATOO_ICON_DIR}/.stamp
  DEPENDS ${XANATOO_SVGS} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rasterize_icons.py
  COMMENT "Rasterizing toolbar icons (SVG → PNG)"
  VERBATIM
)
add_custom_target(xanatoo_icons ALL DEPENDS ${XANATOO_ICON_DIR}/.stamp)
add_dependencies(xanatoo xanatoo_icons)

install(DIRECTORY ${XANATOO_ICON_DIR}/
        DESTINATION share/xanatoo/icons
        FILES_MATCHING PATTERN "*.png"
        PATTERN ".stamp" EXCLUDE)
install(DIRECTORY icons/svg/
        DESTINATION share/xanatoo/icons/svg
        FILES_MATCHING PATTERN "*.svg")
# Desktop integration: launcher, AppStream metainfo, hicolor app icons
# (generated from data/icons/xanatoo.svg by tools/make_icons.py).
# Section 1, not the 6 of gamepkgstd DIR-1: xanatoo is not a game.
install(FILES docs/xanatoo.1 DESTINATION share/man/man1)
install(FILES data/xanatoo.desktop DESTINATION share/applications)
install(FILES data/xanatoo.metainfo.xml DESTINATION share/metainfo)
install(DIRECTORY data/icons/hicolor/
        DESTINATION share/icons/hicolor
        FILES_MATCHING PATTERN "*.png")
install(FILES data/icons/xanatoo.svg
        DESTINATION share/icons/hicolor/scalable/apps)
install(FILES FORMAT.md DESTINATION share/xanatoo)
install(DIRECTORY styles/
        DESTINATION share/xanatoo/styles
        FILES_MATCHING PATTERN "*.xss")
install(DIRECTORY sites/
        DESTINATION share/xanatoo/sites
        FILES_MATCHING PATTERN "*.xsite")
# Everything under examples/ (pages, pictures, collection/): a test checks
# that the menu lists every page.
install(DIRECTORY examples/ DESTINATION share/xanatoo/examples)

# TeX math font (find_math_font_path looks here before FontConfig), on
# every build so formulas look the same everywhere.
install(FILES data/fonts/latinmodern-math.otf
              data/fonts/LICENSE-LatinModernMath.txt
        DESTINATION share/xanatoo/fonts)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data/fonts/latinmodern-math.otf
     DESTINATION ${XANATOO_BUILD_DATA_DIR}/fonts)

if(XANATOO_MINIMAL_PORT)
  install(DIRECTORY data/fonts/ DESTINATION share/xanatoo/fonts
          FILES_MATCHING PATTERN "*.ttf" PATTERN "LICENSE-DejaVu.txt"
                         PATTERN "README.md")
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data/fonts/
       DESTINATION ${XANATOO_BUILD_DATA_DIR}/fonts)
endif()

if(EMSCRIPTEN)
  # A standalone site: index.html + JavaScript + Wasm + preloaded /data.
  # Remote services, persistence and subprocess imports are intentionally off.
  set_target_properties(xanatoo PROPERTIES SUFFIX ".html")
  set(XANATOO_VERSION "${PROJECT_VERSION_FULL}")
  configure_file(mk/wasm/shell.html ${CMAKE_CURRENT_BINARY_DIR}/shell.html @ONLY)
  set_property(TARGET xanatoo APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/shell.html)
  target_link_options(xanatoo PRIVATE
    "SHELL:--shell-file ${CMAKE_CURRENT_BINARY_DIR}/shell.html"
    --profiling-funcs
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/examples@/data/examples"
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/styles@/data/styles"
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/sites@/data/sites"
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/data/fonts@/data/fonts"
    "SHELL:--preload-file ${XANATOO_ICON_DIR}@/data/icons"
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/FORMAT.md@/data/FORMAT.md"
    -sALLOW_MEMORY_GROWTH=1 -sFORCE_FILESYSTEM=1 -sENVIRONMENT=web
    -sEXPORTED_FUNCTIONS=_main,_xanatoo_web_open
    -sEXPORTED_RUNTIME_METHODS=ccall,FS)
  add_dependencies(xanatoo xanatoo_icons)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xanatoo.html
                ${CMAKE_CURRENT_BINARY_DIR}/xanatoo.js
                ${CMAKE_CURRENT_BINARY_DIR}/xanatoo.wasm
                ${CMAKE_CURRENT_BINARY_DIR}/xanatoo.data
                ${CMAKE_CURRENT_SOURCE_DIR}/mk/wasm/coi-sw.js
          DESTINATION .)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xanatoo.html
          DESTINATION . RENAME index.html)
endif()


# ---------------------------------------------------------------------------
# Unit tests (no network; URL/string/JSON helpers only)
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
add_executable(xanatoo_tests
  tests/unit_tests.cpp
  tests/forum_tests.cpp
  tests/nntp_tests.cpp
  tests/ui_help_tests.cpp
  tests/sqlite_db_tests.cpp
  tests/history_trail_tests.cpp
  tests/snapshot_tests.cpp
  tests/audio_tests.cpp
  tests/media_tests.cpp
  src/ui_help.cpp
  src/fixed_layout.cpp
  src/paginate.cpp
  src/about.cpp
  src/util.cpp
  src/paths.cpp
  src/version.cpp
  src/revstore.cpp
  src/sqlite_db.cpp
  src/statedb.cpp
  src/visits.cpp
  src/history_trail.cpp
  src/sidestore.cpp
  src/annotations.cpp
  src/minibuffer.cpp
  src/document.cpp
  src/parse.cpp
  src/xml.cpp
  src/source_view.cpp
  src/gallery.cpp
  src/org.cpp
  src/editor.cpp
  src/reddit.cpp
  src/reddit_credentials.cpp
  src/catalog_credentials.cpp
  src/lemmy.cpp
  src/lemmy_discovery.cpp
  src/lemmy_directory.cpp
  src/matrix.cpp
  src/matrix_directory.cpp
  src/hackernews.cpp
  src/forums.cpp
  src/forum_discourse.cpp
  src/forum_lobsters.cpp
  src/forum_4chan.cpp
  src/forum_stackexchange.cpp
  src/nntp.cpp
  src/forum_bluesky.cpp
  src/forum_flarum.cpp
  src/forum_lesswrong.cpp
  src/forum_digg.cpp
  src/service_bar.cpp
  src/youtube.cpp
  src/video_sites.cpp
  src/archive.cpp
  src/forge.cpp
  src/wayback.cpp
  src/time.cpp
  src/ebooks.cpp
  src/feed.cpp
  src/page_links.cpp
  src/opensearch.cpp
  src/sitemap.cpp
  src/related_sites.cpp
  src/search.cpp
  src/search_results.cpp
  src/inflate.cpp
  src/html_min.cpp
  src/html_page.cpp
  src/site_hints.cpp
  src/html_footnotes.cpp
  src/download.cpp
  src/wikipedia.cpp
  src/locator.cpp
  src/markdown_body.cpp
  src/quote_links.cpp
  src/native.cpp
  src/nostr.cpp
  src/nostr_verify.cpp
  src/nostr_sign.cpp
  src/nostr_keys.cpp
  src/bech32.cpp
  src/nostr_store.cpp
  src/gemini.cpp
  src/gemini_tofu.cpp
  src/gemini_identity.cpp
  src/epub_import.cpp
  src/epub_native.cpp
  src/office_import.cpp
  src/view_settings.cpp
  src/style.cpp
  src/layout.cpp
  src/fold.cpp
  src/font.cpp
  src/math_parse.cpp
  src/math_layout.cpp
  src/math_font.cpp
  src/images.cpp
  src/url_cache.cpp
  src/object_store.cpp
  src/doc_codec.cpp
  src/snapshot.cpp
  src/audio.cpp
  src/byte_feed.cpp
  src/audio_codecs.cpp
  src/media.cpp
  src/audio_stream.cpp
  src/media_ffmpeg.cpp
)
if(XANATOO_FFMPEG)
  target_link_libraries(xanatoo_tests PRIVATE PkgConfig::FFMPEG)
endif()
target_include_directories(xanatoo_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/external
  ${SDL2_INCLUDE_DIRS}
  ${FREETYPE_INCLUDE_DIRS}
  ${FONTCONFIG_INCLUDE_DIRS}
)
target_sources(xanatoo_tests PRIVATE tests/sdl_api_compile.cpp)
target_link_libraries(xanatoo_tests PRIVATE
  xanatoo_sqlite3
  ${SDL2_LIBRARIES}
  Freetype::Freetype
  ${FONTCONFIG_LIBRARIES}
)
# Detection/parse APIs compile without OpenSSL clients.
target_compile_definitions(xanatoo_tests PRIVATE
  XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>
  XANATOO_GEMINI=0
  XANATOO_NOSTR=0
  XANATOO_REDDIT=0
  XANATOO_LEMMY=0
  XANATOO_MATRIX=0
  XANATOO_HN=0
  XANATOO_FORUMS=0
  XANATOO_NNTP=0
  XANATOO_YOUTUBE=0
  XANATOO_VIDEO=0
  XANATOO_EBOOKS=0
  XANATOO_FEEDS=0
  XANATOO_WEB=0
  XANATOO_WIKIPEDIA=0
  XANATOO_HTTPS=0
  XANATOO_FFMPEG=$<BOOL:${XANATOO_FFMPEG}>
  XANATOO_FONTCONFIG=$<BOOL:${XANATOO_FONTCONFIG}>
  XANATOO_SQLITE=$<BOOL:${XANATOO_SQLITE}>
  "XANATOO_STYLE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/styles\""
  "XANATOO_SITES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/sites\""
  "XANATOO_TEST_DATA=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data\""
  "XANATOO_VERSION=\"${PROJECT_VERSION_FULL}\""
)
add_test(NAME unit_tests COMMAND xanatoo_tests)

# Group-discovery URL/hint parsing is dependency-free and also runnable with
# a standalone C++20 compiler when SDL/SQLite are unavailable.
add_executable(xanatoo_nostr_relay_tests tests/nostr_relay_tests.cpp)
target_include_directories(xanatoo_nostr_relay_tests PRIVATE include)
add_test(NAME nostr_relay_tests COMMAND xanatoo_nostr_relay_tests)

# Parser/integrity tests run independently of SDL. OpenSSL builds additionally
# exercise the BIP-340 secp256k1 verifier with upstream positive/negative vectors.
add_executable(xanatoo_nostr_verify_tests
  tests/nostr_verify_tests.cpp src/nostr_verify.cpp src/nostr_sign.cpp
  src/bech32.cpp src/util.cpp)
target_include_directories(xanatoo_nostr_verify_tests PRIVATE include)
# Keep this cryptography regression independent of the full SDL document app;
# util.cpp also contains unrelated document helpers that this target never uses.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xanatoo_nostr_verify_tests PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(xanatoo_nostr_verify_tests PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(xanatoo_nostr_verify_tests PRIVATE -Wl,--gc-sections)
  endif()
elseif(MSVC)
  target_compile_options(xanatoo_nostr_verify_tests PRIVATE /Gy)
  target_link_options(xanatoo_nostr_verify_tests PRIVATE /OPT:REF)
endif()
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(xanatoo_nostr_verify_tests PRIVATE XANATOO_NOSTR_VERIFY_OPENSSL=1)
  target_link_libraries(xanatoo_nostr_verify_tests PRIVATE OpenSSL::Crypto)
endif()
add_test(NAME nostr_verify_tests COMMAND xanatoo_nostr_verify_tests)

# The same vectors against the libsecp256k1 + Mbed TLS backend (the Android
# one), whenever both are installed, whatever TLS backend the app uses.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(NOSTR_SECP_TEST QUIET IMPORTED_TARGET libsecp256k1 mbedcrypto)
endif()
if(NOSTR_SECP_TEST_FOUND)
  add_executable(xanatoo_nostr_secp256k1_tests
    tests/nostr_verify_tests.cpp src/nostr_verify.cpp src/nostr_sign.cpp
    src/bech32.cpp src/util.cpp)
  target_include_directories(xanatoo_nostr_secp256k1_tests PRIVATE include)
  target_compile_definitions(xanatoo_nostr_secp256k1_tests PRIVATE XANATOO_NOSTR_SECP256K1=1)
  target_link_libraries(xanatoo_nostr_secp256k1_tests PRIVATE PkgConfig::NOSTR_SECP_TEST)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_compile_options(xanatoo_nostr_secp256k1_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(xanatoo_nostr_secp256k1_tests PRIVATE -Wl,--gc-sections)
  endif()
  add_test(NAME nostr_secp256k1_tests COMMAND xanatoo_nostr_secp256k1_tests)
endif()

# Exercise the real OpenSSL transport against a local, ephemeral TLS server:
# matching trusted identity succeeds, hostname/CA mismatches fail, and the
# explicit Gemini exception remains possible without external network access.
if(UNIX AND XANATOO_NEED_OPENSSL AND XANATOO_TLS STREQUAL "openssl")
  find_package(Threads REQUIRED)
  add_executable(xanatoo_tls_peer_tests
    tests/tls_peer_tests.cpp src/tls_openssl.cpp)
  target_include_directories(xanatoo_tls_peer_tests PRIVATE include)
  target_link_libraries(xanatoo_tls_peer_tests PRIVATE
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
  add_test(NAME tls_peer_tests COMMAND xanatoo_tls_peer_tests)
endif()

# SQLite trust-state migration and precedence need real signature verification,
# so exercise them independently of the SDL application when OpenSSL is used.
if(XANATOO_SQLITE AND XANATOO_NEED_OPENSSL AND XANATOO_TLS STREQUAL "openssl")
  add_executable(xanatoo_nostr_group_store_tests
    tests/nostr_group_store_tests.cpp
    src/nostr_store.cpp src/nostr_verify.cpp src/nostr.cpp src/bech32.cpp src/sqlite_db.cpp
    src/util.cpp src/paths.cpp)
  target_include_directories(xanatoo_nostr_group_store_tests PRIVATE
    include ${SDL2_INCLUDE_DIRS})
  # paths.cpp includes sdl.hpp for executable_dir() on non-Linux hosts.
  # Match the app's SDL ABI, and inherit the SDL3 imported target's include
  # directory (SDL2_INCLUDE_DIRS is intentionally empty in SDL3 builds).
  target_compile_definitions(xanatoo_nostr_group_store_tests PRIVATE
    XANATOO_SQLITE=1 XANATOO_NOSTR=0 XANATOO_NOSTR_VERIFY_OPENSSL=1
    XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>)
  target_link_libraries(xanatoo_nostr_group_store_tests PRIVATE
    xanatoo_sqlite3 OpenSSL::Crypto ${SDL2_LIBRARIES})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xanatoo_nostr_group_store_tests PRIVATE
      -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(xanatoo_nostr_group_store_tests PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(xanatoo_nostr_group_store_tests PRIVATE -Wl,--gc-sections)
    endif()
  elseif(MSVC)
    target_compile_options(xanatoo_nostr_group_store_tests PRIVATE /Gy)
    target_link_options(xanatoo_nostr_group_store_tests PRIVATE /OPT:REF)
  endif()
  add_test(NAME nostr_group_store_tests COMMAND xanatoo_nostr_group_store_tests)
endif()

# Parser-only regression tests (no SDL/network/font dependencies). Also useful
# for the experimental formats while the full application is cross-compiled.
add_executable(xanatoo_xml_tests
  tests/xml_tests.cpp
  src/xml.cpp
  src/document.cpp
)
target_include_directories(xanatoo_xml_tests PRIVATE include)
add_test(NAME xml_tests COMMAND xanatoo_xml_tests)

add_executable(xanatoo_http_status_tests tests/http_status_tests.cpp)
target_include_directories(xanatoo_http_status_tests PRIVATE include)
add_test(NAME http_status_tests COMMAND xanatoo_http_status_tests)

add_executable(xanatoo_http_url_tests
  tests/http_url_tests.cpp
  src/http_url.cpp
)
target_include_directories(xanatoo_http_url_tests PRIVATE include)
add_test(NAME http_url_tests COMMAND xanatoo_http_url_tests)
add_executable(xanatoo_http_framing_tests
  tests/http_framing_tests.cpp src/http_framing.cpp)
target_include_directories(xanatoo_http_framing_tests PRIVATE include)
add_test(NAME http_framing_tests COMMAND xanatoo_http_framing_tests)
add_executable(xanatoo_gemini_tofu_tests
  tests/gemini_tofu_tests.cpp src/gemini_tofu.cpp)
target_include_directories(xanatoo_gemini_tofu_tests PRIVATE include)
add_test(NAME gemini_tofu_tests COMMAND xanatoo_gemini_tofu_tests)
add_executable(xanatoo_nostr_keys_tests
  tests/nostr_keys_tests.cpp src/nostr_keys.cpp)
target_include_directories(xanatoo_nostr_keys_tests PRIVATE include)
add_test(NAME nostr_keys_tests COMMAND xanatoo_nostr_keys_tests)

add_executable(xanatoo_gemini_identity_tests
  tests/gemini_identity_tests.cpp src/gemini_identity.cpp src/gemini.cpp)
target_include_directories(xanatoo_gemini_identity_tests PRIVATE include)
add_test(NAME gemini_identity_tests COMMAND xanatoo_gemini_identity_tests)
add_executable(xanatoo_diff_overview_tests tests/diff_overview_tests.cpp)
target_include_directories(xanatoo_diff_overview_tests PRIVATE include)
add_test(NAME diff_overview_tests COMMAND xanatoo_diff_overview_tests)
add_executable(xanatoo_tile_tree_tests tests/tile_tree_tests.cpp)
target_include_directories(xanatoo_tile_tree_tests PRIVATE include)
add_test(NAME tile_tree_tests COMMAND xanatoo_tile_tree_tests)
add_executable(xanatoo_drop_url_tests tests/drop_url_tests.cpp)
target_include_directories(xanatoo_drop_url_tests PRIVATE include)
add_test(NAME drop_url_tests COMMAND xanatoo_drop_url_tests)
add_executable(xanatoo_file_url_tests tests/file_url_tests.cpp)
target_include_directories(xanatoo_file_url_tests PRIVATE include)
add_test(NAME file_url_tests COMMAND xanatoo_file_url_tests)
add_executable(xanatoo_smallnet_url_tests
  tests/smallnet_url_tests.cpp
  src/gopher.cpp
  src/spartan.cpp
  src/nex.cpp
  src/finger.cpp)
target_include_directories(xanatoo_smallnet_url_tests PRIVATE include)
# Network fetch is compiled out; only parsers + menu converters are tested.
target_compile_definitions(xanatoo_smallnet_url_tests PRIVATE
  XANATOO_GOPHER=0 XANATOO_SPARTAN=0 XANATOO_NEX=0 XANATOO_FINGER=0)
add_test(NAME smallnet_url_tests COMMAND xanatoo_smallnet_url_tests)
add_executable(xanatoo_pseudo_favicon_tests tests/pseudo_favicon_tests.cpp)
target_include_directories(xanatoo_pseudo_favicon_tests PRIVATE include)
add_test(NAME pseudo_favicon_tests COMMAND xanatoo_pseudo_favicon_tests)
add_executable(xanatoo_autolink_tests tests/autolink_tests.cpp)
target_include_directories(xanatoo_autolink_tests PRIVATE include)
add_test(NAME autolink_tests COMMAND xanatoo_autolink_tests)
add_executable(xanatoo_urlbar_suggestions_tests tests/urlbar_suggestions_tests.cpp)
target_include_directories(xanatoo_urlbar_suggestions_tests PRIVATE include)
add_test(NAME urlbar_suggestions_tests COMMAND xanatoo_urlbar_suggestions_tests)
add_executable(xanatoo_dock_slices_tests tests/dock_slices_tests.cpp)
target_include_directories(xanatoo_dock_slices_tests PRIVATE include)
add_test(NAME dock_slices_tests COMMAND xanatoo_dock_slices_tests)
add_executable(xanatoo_ui_panel_layers_tests tests/ui_panel_layers_tests.cpp)
target_include_directories(xanatoo_ui_panel_layers_tests PRIVATE include)
add_test(NAME ui_panel_layers_tests COMMAND xanatoo_ui_panel_layers_tests)
add_executable(xanatoo_edit_bar_tests tests/edit_bar_tests.cpp)
target_include_directories(xanatoo_edit_bar_tests PRIVATE include)
add_test(NAME edit_bar_tests COMMAND xanatoo_edit_bar_tests)
add_executable(xanatoo_subscription_tests
  tests/subscription_tests.cpp src/subscriptions.cpp src/document.cpp)
target_include_directories(xanatoo_subscription_tests PRIVATE include)
add_test(NAME subscription_tests COMMAND xanatoo_subscription_tests)
add_executable(xanatoo_quote_links_tests
  tests/quote_links_tests.cpp src/quote_links.cpp src/document.cpp)
target_include_directories(xanatoo_quote_links_tests PRIVATE include)
add_test(NAME quote_links_tests COMMAND xanatoo_quote_links_tests)
add_executable(xanatoo_scrollbar_geometry_tests tests/scrollbar_geometry_tests.cpp)
target_include_directories(xanatoo_scrollbar_geometry_tests PRIVATE include)
add_test(NAME scrollbar_geometry_tests COMMAND xanatoo_scrollbar_geometry_tests)

add_executable(xanatoo_lemmy_discovery_tests
  tests/lemmy_discovery_tests.cpp src/lemmy_discovery.cpp)
target_include_directories(xanatoo_lemmy_discovery_tests PRIVATE include)
add_test(NAME lemmy_discovery_tests COMMAND xanatoo_lemmy_discovery_tests)
add_executable(xanatoo_youtube_tests
  tests/youtube_tests.cpp src/youtube.cpp src/util.cpp src/paths.cpp
  src/version.cpp src/document.cpp src/search.cpp)
target_include_directories(xanatoo_youtube_tests PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(xanatoo_youtube_tests PRIVATE ${SDL2_LIBRARIES})
target_compile_definitions(xanatoo_youtube_tests PRIVATE XANATOO_YOUTUBE=0
  XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>
  "XANATOO_VERSION=\"${PROJECT_VERSION_FULL}\"")
add_test(NAME youtube_tests COMMAND xanatoo_youtube_tests)
add_executable(xanatoo_video_sites_tests
  tests/video_sites_tests.cpp src/video_sites.cpp src/util.cpp src/paths.cpp
  src/version.cpp src/document.cpp src/search.cpp src/html_min.cpp)
target_include_directories(xanatoo_video_sites_tests PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(xanatoo_video_sites_tests PRIVATE ${SDL2_LIBRARIES})
target_compile_definitions(xanatoo_video_sites_tests PRIVATE XANATOO_VIDEO=0
  XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>
  "XANATOO_VERSION=\"${PROJECT_VERSION_FULL}\"")
add_test(NAME video_sites_tests COMMAND xanatoo_video_sites_tests)
add_executable(xanatoo_archive_tests
  tests/archive_tests.cpp src/archive.cpp src/util.cpp src/paths.cpp
  src/version.cpp src/document.cpp src/html_min.cpp src/search.cpp)
target_include_directories(xanatoo_archive_tests PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(xanatoo_archive_tests PRIVATE ${SDL2_LIBRARIES})
target_compile_definitions(xanatoo_archive_tests PRIVATE XANATOO_HTTPS=0
  XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>
  "XANATOO_VERSION=\"${PROJECT_VERSION_FULL}\"")
add_test(NAME archive_tests COMMAND xanatoo_archive_tests)
add_executable(xanatoo_wayback_tests
  tests/wayback_tests.cpp src/wayback.cpp src/util.cpp src/paths.cpp
  src/version.cpp src/document.cpp)
target_include_directories(xanatoo_wayback_tests PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(xanatoo_wayback_tests PRIVATE ${SDL2_LIBRARIES})
target_compile_definitions(xanatoo_wayback_tests PRIVATE
  XANATOO_SDL3=$<BOOL:${XANATOO_SDL3}>
  "XANATOO_VERSION=\"${PROJECT_VERSION_FULL}\"")
add_test(NAME wayback_tests COMMAND xanatoo_wayback_tests)
add_executable(xanatoo_ui_panel_imgui_tests
  tests/ui_panel_imgui_tests.cpp
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp)
target_include_directories(xanatoo_ui_panel_imgui_tests PRIVATE include src external/imgui)
add_test(NAME ui_panel_imgui_tests COMMAND xanatoo_ui_panel_imgui_tests)
endif()
