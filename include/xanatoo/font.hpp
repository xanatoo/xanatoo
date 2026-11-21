// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include "xanatoo/sdl.hpp"
#include <memory>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace xanatoo {

// A font file plus the member to open when it is a collection (.ttc /
// .otc): FreeType's face index, 0 for plain .ttf / .otf files.
struct FontFace {
  std::string path;
  int index = 0;
  bool empty() const { return path.empty(); }
  bool operator==(const FontFace&) const = default;
};

struct Glyph {
  SDL_Texture* texture = nullptr;
  int width = 0;
  int height = 0;
  int bearing_x = 0;
  int bearing_y = 0;
  int advance = 0;  // in 1/64th pixels, unhinted (fractional)
  // A colour bitmap (emoji): draw it untinted, only the alpha applies.
  bool color = false;
  float advance_px() const { return static_cast<float>(advance) / 64.f; }
};

// Glyph bitmaps are rasterised at this many horizontal sub-pixel phases,
// so text keeps the fractional pen positions of its unhinted advances.
constexpr int kSubpixelSteps = 4;

class Font {
public:
  Font() = default;
  ~Font();

  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  bool load(const FontFace& face, int pixel_size, SDL_Renderer* renderer);
  bool load(const std::string& path, int pixel_size, SDL_Renderer* renderer) {
    return load(FontFace{path, 0}, pixel_size, renderer);
  }
  void set_size(int pixel_size);

  // Metrics (and the phase-0 bitmap for dark text) of `codepoint`.
  const Glyph& glyph(uint32_t codepoint);
  // The bitmap to draw `codepoint` with its pen (origin, before the
  // bearing) at `pen_x`: the nearest quarter-pixel phase, with coverage
  // gamma-corrected for dark or light text. `*x` gets the bitmap's left.
  const Glyph& raster(uint32_t codepoint, float pen_x, bool light_text,
                      int* x);
  // True when the face maps `codepoint` (no .notdef fallback). glyph()
  // still draws unmapped codepoints from a fallback face when one has it.
  bool has_glyph(uint32_t codepoint) const {
    return face_ && FT_Get_Char_Index(face_, codepoint) != 0;
  }
  int line_height() const { return line_height_; }
  int ascender() const { return ascender_; }
  int descender() const { return descender_; }
  int pixel_size() const { return pixel_size_; }
  bool loaded() const { return face_ != nullptr; }
  const std::string& path() const { return path_; }
  int face_index() const { return face_index_; }
  FontFace face() const { return {path_, face_index_}; }

  // Measure a UTF-8 string width in pixels
  int measure(const std::string& text);

private:
  FT_Library library_ = nullptr;
  FT_Face face_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::string path_;
  int face_index_ = 0;
  int pixel_size_ = 0;
  int line_height_ = 0;
  int ascender_ = 0;
  int descender_ = 0;
  // Keyed by codepoint * kVariants + variant (phase, light text).
  std::unordered_map<uint64_t, Glyph> cache_;
  // Direct lookup for Latin-1 (most text): layout measures every character
  // of a block, so a hash lookup each is the hot path. Points into
  // `cache_` (node-based, so stable); cleared with it.
  static constexpr int kVariants = kSubpixelSteps * 2;
  std::array<const Glyph*, 256 * kVariants> latin1_{};
  // Faces opened for codepoints the primary face lacks, same pixel size.
  // Colour bitmap faces (CBDT emoji) come in fixed strikes: `scale`
  // maps the chosen strike onto the pixel size.
  struct FallbackFace {
    FontFace file;
    FT_Face face = nullptr;
    float scale = 1.f;
    // Colour only as OpenType-SVG (no renderer here): its monochrome
    // outlines are drawn, and other faces covering a codepoint win.
    bool svg_only = false;
  };
  std::vector<FallbackFace> fallbacks_;

  Glyph load_glyph(uint32_t codepoint, int variant);
  Glyph color_glyph(FT_GlyphSlot slot, float scale);
  Glyph missing_glyph(uint32_t codepoint);
  SDL_Texture* make_texture(const std::vector<uint8_t>& rgba, int w, int h);
  const Glyph& variant(uint32_t codepoint, int variant);
  const FallbackFace* fallback_face(uint32_t codepoint);
  void drop_cache();
};

// Faces that may cover `codepoint` when a face of `family` lacks it,
// best first (FontConfig: sorted by similarity, filtered by coverage;
// without FontConfig: the usual broad-coverage system faces, unfiltered).
std::vector<FontFace> fallback_font_faces(uint32_t codepoint,
                                          const std::string& family,
                                          int weight, int slant);

// Codepoints drawn as nothing when no face maps them (variation selectors,
// joiners, bidi marks, tags …) instead of a missing-glyph box.
bool is_default_ignorable(uint32_t codepoint);

// The box drawn for a codepoint that no installed face covers: a frame
// around its hex digits in two rows (4 digits, or 6 past U+FFFF), as
// coverage bytes (0 / 255), row-major, `width` × `height`.
struct MissingGlyphBitmap {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> alpha;
};
MissingGlyphBitmap missing_glyph_bitmap(uint32_t codepoint, int pixel_size);

// Area-average resample of a 4-channel premultiplied image (FreeType's
// BGRA colour bitmaps) to dw × dh; the channel order is kept.
std::vector<uint8_t> resample_premultiplied(const uint8_t* src, int sw, int sh,
                                            int pitch, int dw, int dh);

// Which face to use when laying out / drawing a glyph.
enum class FontRole : uint8_t {
  Body = 0,
  BodyBold,
  BodyItalic,
  BodyBoldItalic,
  Heading,
  Code,
  // Margin text (asides, side bars): the body faces at kSmallScale.
  Small,
  SmallBold,
  SmallItalic,
  SmallBoldItalic,
  SmallCode,
};

// Size of the Small* faces relative to the body size.
constexpr float kSmallScale = 0.85f;
inline int small_px(int body_px) {
  return static_cast<int>(static_cast<float>(body_px) * kSmallScale + 0.5f);
}

// The small face for a body face (Heading → SmallBold, Code → SmallCode).
FontRole small_role(FontRole role);
inline bool is_small_role(FontRole role) {
  return role == FontRole::Small || role == FontRole::SmallBold ||
         role == FontRole::SmallItalic || role == FontRole::SmallBoldItalic ||
         role == FontRole::SmallCode;
}

// Resolve a face via FontConfig, or the font-directory scan without it
// (family + weight + slant). weight: 0=regular, 1=bold; slant: 0=roman,
// 1=italic. The index picks the matching member of a collection.
FontFace fontconfig_find_face(const std::string& family, int weight,
                              int slant);
// Just the file of fontconfig_find_face (callers that open face 0 only).
inline std::string fontconfig_find(const std::string& family, int weight,
                                   int slant) {
  return fontconfig_find_face(family, weight, slant).path;
}

// Infer a family name from a user-supplied TTF/OTF path (best-effort).
std::string fontconfig_family_from_file(const std::string& path);

// Preferred families per face, first installed one wins (generic names such
// as "serif" / "monospace" always resolve). Empty = built-in candidates.
struct FontFamilies {
  std::vector<std::string> body;
  std::vector<std::string> heading;  // empty = body family
  std::vector<std::string> code;
  bool heading_bold = true;
  bool operator==(const FontFamilies&) const = default;
};

// Face for UI chrome (menus, status line, panels): a GNOME 2–era sans
// (DejaVu Sans / Vera / Liberation …), independent of document style sheets.
FontFace ui_font_face(bool bold = false);

// First family of `names` that FontConfig has installed, else "".
std::string fontconfig_pick_family(const std::vector<std::string>& names);

// Enumerate actual installed font families for the style editor; on systems
// without FontConfig, return the three generic families instead.
const std::vector<std::string>& fontconfig_list_families();

class MathFont;

// Body regular/bold/italic + heading + monospace for code.
class FontSet {
public:
  FontSet();
  ~FontSet();
  FontSet(const FontSet&) = delete;
  FontSet& operator=(const FontSet&) = delete;

  Font regular;
  Font bold;
  Font italic;
  Font bold_italic;
  Font heading;
  Font mono;
  Font small;
  Font small_bold;
  Font small_italic;
  Font small_bold_italic;
  Font small_mono;

  // preferred_path may be empty (FontConfig defaults) or a concrete .ttf;
  // it wins over families.body (reader choice beats the style sheet).
  bool load(SDL_Renderer* renderer, const std::string& preferred_path,
            int body_px, int heading_px, const FontFamilies& families = {});

  void set_sizes(int body_px, int heading_px);

  Font& pick(FontRole role);
  const Font& pick(FontRole role) const;

  // The math font (math_font.hpp), found and opened on first use; null when
  // the system has none (math then shows its TeX source).
  MathFont* math_font();

private:
  SDL_Renderer* renderer_ = nullptr;
  std::unique_ptr<MathFont> math_;
  bool math_tried_ = false;
};

}  // namespace xanatoo
