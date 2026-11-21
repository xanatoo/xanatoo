// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/font.hpp"
#include "xanatoo/math_font.hpp"
#include "xanatoo/paths.hpp"
#include <algorithm>
#include <cmath>
#include FT_OUTLINE_H
#include FT_DRIVER_H
#include FT_MODULE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#ifndef XANATOO_FONTCONFIG
#define XANATOO_FONTCONFIG 1
#endif
#if XANATOO_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif
#include <iostream>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <filesystem>
#include <unordered_set>
#include <mutex>
#include <vector>
#if XANATOO_FONTCONFIG
#include <strings.h>
#endif

namespace xanatoo {

Font::~Font() {
  drop_cache();
  for (FallbackFace& f : fallbacks_) FT_Done_Face(f.face);
  fallbacks_.clear();
  if (face_) {
    FT_Done_Face(face_);
    face_ = nullptr;
  }
  if (library_) {
    FT_Done_FreeType(library_);
    library_ = nullptr;
  }
}

bool Font::load(const FontFace& file, int pixel_size, SDL_Renderer* renderer) {
  renderer_ = renderer;
  path_ = file.path;
  face_index_ = file.index;
  // Clear previous face if reloading
  drop_cache();
  for (FallbackFace& f : fallbacks_) FT_Done_Face(f.face);
  fallbacks_.clear();
  if (face_) {
    FT_Done_Face(face_);
    face_ = nullptr;
  }
  if (!library_) {
    if (FT_Init_FreeType(&library_)) {
      std::cerr << "FreeType init failed\n";
      return false;
    }
    // Stem darkening (auto-hinter, which light hinting uses for TrueType,
    // and the CFF / Type 1 drivers) emboldens small sizes to make up for
    // the thinning of gamma-correct coverage (kTextGamma); it fades out
    // towards large sizes.
    FT_Bool no_darkening = 0;
    for (const char* module : {"autofitter", "cff", "type1", "t1cid"})
      FT_Property_Set(library_, module, "no-stem-darkening", &no_darkening);
  }
  if (file.index < 0 ||
      FT_New_Face(library_, file.path.c_str(), file.index, &face_)) {
    face_ = nullptr;
    std::cerr << "Failed to load font: " << file.path;
    if (file.index != 0) std::cerr << " (face " << file.index << ")";
    std::cerr << "\n";
    return false;
  }
  set_size(pixel_size);
  return true;
}

void Font::drop_cache() {
  for (auto& [cp, g] : cache_) {
    if (g.texture) SDL_DestroyTexture(g.texture);
  }
  cache_.clear();
  latin1_.fill(nullptr);
}

// Sizes a face for `px` and returns the factor from its glyphs to `px`:
// 1 for outline faces; fixed-strike faces (colour emoji) pick the smallest
// strike at least `px` (else the largest) and get scaled when drawn.
static float size_face(FT_Face face, int px) {
  if (FT_IS_SCALABLE(face)) {
    FT_Set_Pixel_Sizes(face, 0, px);
    return 1.f;
  }
  if (face->num_fixed_sizes <= 0 || px <= 0) return 1.f;
  int best = -1, largest = 0;
  for (int i = 0; i < face->num_fixed_sizes; ++i) {
    const FT_Pos ppem = face->available_sizes[i].y_ppem;
    if (ppem > face->available_sizes[largest].y_ppem) largest = i;
    if (ppem >= px * 64 &&
        (best < 0 || ppem < face->available_sizes[best].y_ppem))
      best = i;
  }
  if (best < 0) best = largest;
  FT_Select_Size(face, best);
  const FT_Pos ppem = face->available_sizes[best].y_ppem;
  return ppem > 0 ? static_cast<float>(px * 64) / static_cast<float>(ppem)
                  : 1.f;
}

void Font::set_size(int pixel_size) {
  // Drop cached textures — they are sized for the old pixel size
  drop_cache();

  pixel_size_ = pixel_size;
  FT_Set_Pixel_Sizes(face_, 0, pixel_size);
  for (FallbackFace& f : fallbacks_) f.scale = size_face(f.face, pixel_size);
  line_height_ = static_cast<int>(face_->size->metrics.height >> 6);
  ascender_ = static_cast<int>(face_->size->metrics.ascender >> 6);
  descender_ = static_cast<int>(face_->size->metrics.descender >> 6);
}

static bool has_sfnt_table(FT_Face face, FT_ULong tag) {
  FT_ULong length = 0;
  return FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) == 0 &&
         length > 0;
}

// Colour that FreeType cannot draw by itself: only an SVG table, no
// bitmap strikes (CBDT / sbix) or COLR layers.
static bool svg_only_color(FT_Face face) {
  return FT_HAS_COLOR(face) && has_sfnt_table(face, TTAG_SVG) &&
         !has_sfnt_table(face, TTAG_CBDT) && !has_sfnt_table(face, TTAG_sbix) &&
         !has_sfnt_table(face, TTAG_COLR);
}

// A face that maps `codepoint`, opening fallback files on first need.
// Line metrics stay the primary face's; only the glyph comes from here.
// SVG-only colour faces are the last resort. The pointer is good until
// the next call (fallbacks_ may grow).
const Font::FallbackFace* Font::fallback_face(uint32_t codepoint) {
  int last_resort = -1;
  for (size_t i = 0; i < fallbacks_.size(); ++i) {
    if (FT_Get_Char_Index(fallbacks_[i].face, codepoint) == 0) continue;
    if (!fallbacks_[i].svg_only) return &fallbacks_[i];
    if (last_resort < 0) last_resort = static_cast<int>(i);
  }
  const std::string family = face_->family_name ? face_->family_name : "";
  const int weight = (face_->style_flags & FT_STYLE_FLAG_BOLD) ? 1 : 0;
  const int slant = (face_->style_flags & FT_STYLE_FLAG_ITALIC) ? 1 : 0;
  for (const FontFace& file :
       fallback_font_faces(codepoint, family, weight, slant)) {
    if (file == this->face()) continue;
    bool open = false;
    for (const FallbackFace& f : fallbacks_) open = open || f.file == file;
    if (open) continue;  // already checked above
    FT_Face face = nullptr;
    if (FT_New_Face(library_, file.path.c_str(), file.index, &face)) continue;
    // Fixed-strike faces only when they are colour (emoji); plain bitmap
    // fonts would look blocky once scaled.
    if (!FT_IS_SCALABLE(face) &&
        (!FT_HAS_COLOR(face) || face->num_fixed_sizes <= 0)) {
      FT_Done_Face(face);
      continue;
    }
    const float scale = size_face(face, pixel_size_);
    const bool svg_only = svg_only_color(face);
    fallbacks_.push_back({file, face, scale, svg_only});
    if (FT_Get_Char_Index(face, codepoint) == 0) continue;
    if (!svg_only) return &fallbacks_.back();
    if (last_resort < 0) last_resort = static_cast<int>(fallbacks_.size() - 1);
  }
  return last_resort >= 0 ? &fallbacks_[last_resort] : nullptr;
}

// Coverage curves for blending in sRGB: plain alpha blending makes dark
// text on a light page too heavy and light text on a dark page too thin.
// Blending as if in a space of gamma kTextGamma evens both out; the full
// sRGB 2.2 looks washed out, so this stays near what browsers show.
static constexpr double kTextGamma = 1.8; // 2.2 1.8 1.45;

static const std::array<uint8_t, 256>& coverage_curve(bool light_text) {
  static const auto curves = [] {
    std::array<std::array<uint8_t, 256>, 2> c{};
    for (int i = 0; i < 256; ++i) {
      const double a = i / 255.0;
      const double dark = 1.0 - std::pow(1.0 - a, 1.0 / kTextGamma);
      const double light = std::pow(a, 1.0 / kTextGamma);
      c[0][i] = static_cast<uint8_t>(std::lround(dark * 255.0));
      c[1][i] = static_cast<uint8_t>(std::lround(light * 255.0));
    }
    return c;
  }();
  return curves[light_text ? 1 : 0];
}

SDL_Texture* Font::make_texture(const std::vector<uint8_t>& rgba, int w,
                                int h) {
  SDL_Surface* surf = xanatoo::sdl::rgba_surface_from(
      const_cast<uint8_t*>(rgba.data()), w, h, w * 4);
  if (!surf) return nullptr;
  SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
  if (tex) {
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    xanatoo::sdl::set_nearest(tex);
  }
  xanatoo::sdl::destroy_surface(surf);
  return tex;
}

// A colour (BGRA, premultiplied) bitmap, scaled from its strike to the
// pixel size and handed to SDL as straight RGBA.
Glyph Font::color_glyph(FT_GlyphSlot slot, float scale) {
  Glyph g;
  g.color = true;
  const FT_Bitmap& bm = slot->bitmap;
  const int sw = static_cast<int>(bm.width), sh = static_cast<int>(bm.rows);
  g.width = std::max(1, static_cast<int>(std::lround(sw * scale)));
  g.height = std::max(1, static_cast<int>(std::lround(sh * scale)));
  g.bearing_x = static_cast<int>(std::lround(slot->bitmap_left * scale));
  g.bearing_y = static_cast<int>(std::lround(slot->bitmap_top * scale));
  g.advance = static_cast<int>(std::lround(slot->advance.x * scale));
  if (sw == 0 || sh == 0) {
    g.width = g.height = 0;
    return g;
  }
  if (!renderer_) return g;
  std::vector<uint8_t> px = resample_premultiplied(bm.buffer, sw, sh, bm.pitch,
                                                  g.width, g.height);
  for (size_t i = 0; i + 3 < px.size(); i += 4) {
    const int a = px[i + 3];
    const auto straight = [a](int c) {
      return static_cast<uint8_t>(a ? std::min(255, (c * 255 + a / 2) / a) : 0);
    };
    const uint8_t b = straight(px[i]), gr = straight(px[i + 1]),
                  r = straight(px[i + 2]);
    px[i] = r;
    px[i + 1] = gr;
    px[i + 2] = b;
  }
  g.texture = make_texture(px, g.width, g.height);
  return g;
}

// The hex box for a codepoint no face has, centred on the x-height band.
Glyph Font::missing_glyph(uint32_t codepoint) {
  Glyph g;
  const MissingGlyphBitmap box = missing_glyph_bitmap(codepoint, pixel_size_);
  const int margin = std::max(1, pixel_size_ / 16);
  g.width = box.width;
  g.height = box.height;
  g.bearing_x = margin;
  g.bearing_y = (ascender_ + descender_ + box.height) / 2;
  g.advance = (box.width + 2 * margin) * 64;
  if (!renderer_ || box.alpha.empty()) return g;
  std::vector<uint8_t> rgba(box.alpha.size() * 4);
  for (size_t i = 0; i < box.alpha.size(); ++i) {
    rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
    rgba[i * 4 + 3] = box.alpha[i];
  }
  g.texture = make_texture(rgba, g.width, g.height);
  return g;
}

// `variant` = sub-pixel phase + kSubpixelSteps * light_text.
Glyph Font::load_glyph(uint32_t codepoint, int variant) {
  Glyph g;
  FT_Face face = face_;
  float scale = 1.f;
  bool svg_only = false;
  if (codepoint >= 0x20 && FT_Get_Char_Index(face_, codepoint) == 0) {
    if (is_default_ignorable(codepoint)) return g;  // zero width, no ink
    const FallbackFace* fb = fallback_face(codepoint);
    // Nothing covers it: a hex box (C1 controls keep the face's .notdef).
    if (!fb && (codepoint < 0x7f || codepoint >= 0xa0))
      return missing_glyph(codepoint);
    if (fb) {
      face = fb->face;
      scale = fb->scale;
      svg_only = fb->svg_only;
    }
  }
  // Light hinting snaps only vertical metrics and keeps stroke weights
  // fractional, as browsers / fontconfig "hintslight" do. Full native
  // hinting rounds strokes to whole pixels, so text visibly jumps in
  // weight between sizes (DejaVu Sans: 1 px bars at 16 px, 2 px at 17 px).
  // It leaves x alone, so outlines can shift by sub-pixel phases.
  FT_Int32 flags = FT_LOAD_TARGET_LIGHT;
  if (FT_HAS_COLOR(face) && !svg_only) flags |= FT_LOAD_COLOR;
  if (FT_Load_Char(face, codepoint, flags)) return g;
  FT_GlyphSlot slot = face->glyph;
  const int phase = variant % kSubpixelSteps;
  const bool light_text = variant >= kSubpixelSteps;
  if (slot->format == FT_GLYPH_FORMAT_OUTLINE && phase != 0)
    FT_Outline_Translate(&slot->outline, phase * 64 / kSubpixelSteps, 0);
  if (slot->format != FT_GLYPH_FORMAT_BITMAP &&
      FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT))
    return g;
  // CBDT strikes and COLR layers (FreeType renders those to BGRA).
  if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA)
    return color_glyph(slot, scale);
  g.width = slot->bitmap.width;
  g.height = slot->bitmap.rows;
  g.bearing_x = slot->bitmap_left;
  g.bearing_y = slot->bitmap_top;
  // Unhinted advance (16.16 → 26.6): pens keep fractional positions.
  g.advance = FT_IS_SCALABLE(face)
                  ? static_cast<int>((slot->linearHoriAdvance + 512) >> 10)
                  : static_cast<int>(slot->advance.x);

  if (g.width > 0 && g.height > 0 && renderer_) {
    // Convert the coverage bitmap to RGBA for SDL (skip when metrics-only).
    const FT_Bitmap& bm = slot->bitmap;
    const auto& curve = coverage_curve(light_text);
    std::vector<uint8_t> rgba(static_cast<size_t>(g.width) * g.height * 4);
    for (int y = 0; y < g.height; ++y) {
      const uint8_t* row = bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch;
      for (int x = 0; x < g.width; ++x) {
        uint8_t a = 0;
        if (bm.pixel_mode == FT_PIXEL_MODE_MONO)
          a = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
        else
          a = curve[row[x]];
        uint8_t* px = &rgba[(static_cast<size_t>(y) * g.width + x) * 4];
        px[0] = px[1] = px[2] = 255;
        px[3] = a;
      }
    }
    g.texture = make_texture(rgba, g.width, g.height);
  }
  return g;
}

const Glyph& Font::variant(uint32_t codepoint, int variant) {
  const size_t slot = static_cast<size_t>(codepoint) * kVariants + variant;
  if (slot < latin1_.size() && latin1_[slot]) return *latin1_[slot];
  auto it = cache_.find(slot);
  if (it == cache_.end())
    it = cache_.emplace(slot, load_glyph(codepoint, variant)).first;
  if (slot < latin1_.size()) latin1_[slot] = &it->second;
  return it->second;
}

const Glyph& Font::glyph(uint32_t codepoint) { return variant(codepoint, 0); }

const Glyph& Font::raster(uint32_t codepoint, float pen_x, bool light_text,
                          int* x) {
  const float fl = std::floor(pen_x);
  int ix = static_cast<int>(fl);
  int phase = static_cast<int>(std::lround((pen_x - fl) * kSubpixelSteps));
  if (phase >= kSubpixelSteps) {
    phase = 0;
    ++ix;
  }
  const Glyph& g =
      variant(codepoint, phase + (light_text ? kSubpixelSteps : 0));
  *x = ix + g.bearing_x;
  return g;
}

bool is_default_ignorable(uint32_t c) {
  return c == 0xad || c == 0x34f || c == 0x61c || c == 0x115f ||
         c == 0x1160 || c == 0x17b4 || c == 0x17b5 ||
         (c >= 0x180b && c <= 0x180f) || (c >= 0x200b && c <= 0x200f) ||
         (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f) ||
         c == 0x3164 || (c >= 0xfe00 && c <= 0xfe0f) || c == 0xfeff ||
         c == 0xffa0 || (c >= 0xfff0 && c <= 0xfff8) ||
         (c >= 0x1bca0 && c <= 0x1bca3) || (c >= 0x1d173 && c <= 0x1d17a) ||
         (c >= 0xe0000 && c <= 0xe0fff);
}

// 3 × 5 hex digits, one bit per pixel, rows top-down, MSB = left.
static constexpr uint16_t kHexDigits[16] = {
    0b111'101'101'101'111, 0b010'110'010'010'111, 0b111'001'111'100'111,
    0b111'001'111'001'111, 0b101'101'111'001'001, 0b111'100'111'001'111,
    0b111'100'111'101'111, 0b111'001'001'001'001, 0b111'101'111'101'111,
    0b111'101'111'001'111, 0b111'101'111'101'101, 0b110'101'110'101'110,
    0b111'100'100'100'111, 0b110'101'101'101'110, 0b111'100'111'100'111,
    0b111'100'111'100'100};

MissingGlyphBitmap missing_glyph_bitmap(uint32_t codepoint, int pixel_size) {
  MissingGlyphBitmap out;
  const int s = std::max(1, (pixel_size + 4) / 16);  // dot size
  const int pad = pixel_size >= 14 ? s : 0;          // frame to digits
  const int ndigits = codepoint > 0xffff ? 6 : 4;
  const int cols = ndigits / 2;
  const int inset = s + pad;
  out.width = 2 * inset + cols * 3 * s + (cols - 1) * s;
  out.height = 2 * inset + 2 * 5 * s + s;
  out.alpha.assign(static_cast<size_t>(out.width) * out.height, 0);
  auto fill = [&](int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; ++y)
      for (int x = x0; x < x0 + w; ++x)
        out.alpha[static_cast<size_t>(y) * out.width + x] = 255;
  };
  fill(0, 0, out.width, s);
  fill(0, out.height - s, out.width, s);
  fill(0, 0, s, out.height);
  fill(out.width - s, 0, s, out.height);
  for (int i = 0; i < ndigits; ++i) {
    const int digit = (codepoint >> (4 * (ndigits - 1 - i))) & 0xf;
    const int x0 = inset + (i % cols) * 4 * s;
    const int y0 = inset + (i / cols) * 6 * s;
    for (int r = 0; r < 5; ++r)
      for (int c = 0; c < 3; ++c)
        if (kHexDigits[digit] & (1 << (14 - (r * 3 + c))))
          fill(x0 + c * s, y0 + r * s, s, s);
  }
  return out;
}

std::vector<uint8_t> resample_premultiplied(const uint8_t* src, int sw, int sh,
                                            int pitch, int dw, int dh) {
  std::vector<uint8_t> out(static_cast<size_t>(std::max(dw, 0)) *
                           std::max(dh, 0) * 4);
  if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return out;
  const double fx = static_cast<double>(sw) / dw;
  const double fy = static_cast<double>(sh) / dh;
  for (int y = 0; y < dh; ++y) {
    const double y0 = y * fy, y1 = y0 + fy;
    for (int x = 0; x < dw; ++x) {
      const double x0 = x * fx, x1 = x0 + fx;
      double acc[4] = {0, 0, 0, 0}, area = 0;
      for (int sy = static_cast<int>(y0); sy < sh && sy < y1; ++sy) {
        const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
        if (wy <= 0) continue;
        const uint8_t* row = src + static_cast<ptrdiff_t>(sy) * pitch;
        for (int sx = static_cast<int>(x0); sx < sw && sx < x1; ++sx) {
          const double wx =
              std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
          if (wx <= 0) continue;
          const double w = wx * wy;
          for (int k = 0; k < 4; ++k) acc[k] += w * row[sx * 4 + k];
          area += w;
        }
      }
      uint8_t* px = &out[(static_cast<size_t>(y) * dw + x) * 4];
      for (int k = 0; k < 4; ++k)
        px[k] = static_cast<uint8_t>(
            area > 0 ? std::min(255L, std::lround(acc[k] / area)) : 0);
    }
  }
  return out;
}

int Font::measure(const std::string& text) {
  int width = 0;
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = 0;
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      cp = c;
      ++i;
    } else if ((c & 0xe0) == 0xc0 && i + 1 < text.size()) {
      cp = ((c & 0x1f) << 6) | (text[i + 1] & 0x3f);
      i += 2;
    } else if ((c & 0xf0) == 0xe0 && i + 2 < text.size()) {
      cp = ((c & 0x0f) << 12) | ((text[i + 1] & 0x3f) << 6) | (text[i + 2] & 0x3f);
      i += 3;
    } else if ((c & 0xf8) == 0xf0 && i + 3 < text.size()) {
      cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3f) << 12) |
           ((text[i + 2] & 0x3f) << 6) | (text[i + 3] & 0x3f);
      i += 4;
    } else {
      ++i;
      continue;
    }
    const Glyph& g = glyph(cp);
    width += g.advance;
  }
  return (width + 63) >> 6;
}

#if XANATOO_FONTCONFIG
// The file and collection index of a FontConfig match.
static FontFace fc_face_of(FcPattern* p) {
  FontFace out;
  FcChar8* file = nullptr;
  if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch || !file)
    return out;
  out.path = reinterpret_cast<const char*>(file);
  int index = 0;
  // Variable fonts put the named instance in the upper 16 bits; FreeType
  // takes the same encoding.
  if (FcPatternGetInteger(p, FC_INDEX, 0, &index) == FcResultMatch)
    out.index = index;
  return out;
}

FontFace fontconfig_find_face(const std::string& family, int weight,
                              int slant) {
  if (!FcInit()) return {};

  FcPattern* pat = FcPatternCreate();
  if (!pat) return {};
  if (!family.empty())
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
  FcPatternAddInteger(pat, FC_WEIGHT,
                      weight ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
  FcPatternAddInteger(pat, FC_SLANT,
                      slant ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
  FcPatternAddString(pat, FC_STYLE, reinterpret_cast<const FcChar8*>(""));
  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);

  FcResult result = FcResultNoMatch;
  FcPattern* match = FcFontMatch(nullptr, pat, &result);
  FcPatternDestroy(pat);
  if (!match) return {};
  FontFace face = fc_face_of(match);
  FcPatternDestroy(match);
  return face;
}

std::vector<FontFace> fallback_font_faces(uint32_t codepoint,
                                          const std::string& family,
                                          int weight, int slant) {
  // One FcFontSort per (family, style, codepoint); the answer never changes.
  static std::mutex memo_mutex;
  static std::unordered_map<std::string, std::vector<FontFace>> memo;
  std::lock_guard<std::mutex> lock(memo_mutex);
  std::string key = family + '\n' + std::to_string(weight) +
                    std::to_string(slant) + '\n' + std::to_string(codepoint);
  if (auto it = memo.find(key); it != memo.end()) return it->second;

  std::vector<FontFace> paths;
  if (FcInit()) {
    FcPattern* pat = FcPatternCreate();
    FcCharSet* want = FcCharSetCreate();
    if (pat && want) {
      FcCharSetAddChar(want, codepoint);
      if (!family.empty())
        FcPatternAddString(pat, FC_FAMILY,
                           reinterpret_cast<const FcChar8*>(family.c_str()));
      FcPatternAddCharSet(pat, FC_CHARSET, want);
      FcPatternAddInteger(pat, FC_WEIGHT,
                          weight ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
      FcPatternAddInteger(pat, FC_SLANT,
                          slant ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
      FcConfigSubstitute(nullptr, pat, FcMatchPattern);
      FcDefaultSubstitute(pat);
      FcResult result = FcResultNoMatch;
      if (FcFontSet* set = FcFontSort(nullptr, pat, FcFalse, nullptr, &result)) {
        for (int i = 0; i < set->nfont && paths.size() < 3; ++i) {
          FcPattern* f = set->fonts[i];
          FcCharSet* cs = nullptr;
          // Colour faces (emoji) count: Font draws their BGRA bitmaps.
          FcBool scalable = FcTrue;
          FcPatternGetBool(f, FC_SCALABLE, 0, &scalable);
          if (!scalable) continue;
          if (FcPatternGetCharSet(f, FC_CHARSET, 0, &cs) != FcResultMatch ||
              !FcCharSetHasChar(cs, codepoint))
            continue;
          if (FontFace face = fc_face_of(f); !face.empty())
            paths.push_back(std::move(face));
        }
        FcFontSetDestroy(set);
      }
    }
    if (want) FcCharSetDestroy(want);
    if (pat) FcPatternDestroy(pat);
  }
  memo.emplace(std::move(key), paths);
  return paths;
}

std::string fontconfig_family_from_file(const std::string& path) {
  if (path.empty() || !FcInit()) return {};
  FcPattern* pat = FcPatternCreate();
  if (!pat) return {};
  FcPatternAddString(pat, FC_FILE, reinterpret_cast<const FcChar8*>(path.c_str()));
  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result = FcResultNoMatch;
  FcPattern* match = FcFontMatch(nullptr, pat, &result);
  FcPatternDestroy(pat);
  if (!match) return {};
  FcChar8* family = nullptr;
  std::string name;
  if (FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch && family) {
    name = reinterpret_cast<const char*>(family);
  }
  FcPatternDestroy(match);
  return name;
}

#else  // !XANATOO_FONTCONFIG

// Without FontConfig (Android and other ports): scan the usual font
// directories with FreeType and expose real family names. Style sheets can
// then pick "Roboto", "Noto Serif", … instead of only the three CSS generics.

namespace {

struct ScannedFace {
  std::string path;
  int face_index = 0;
  std::string family;
  bool bold = false;
  bool italic = false;
};

std::vector<std::filesystem::path> font_dirs() {
  std::vector<std::filesystem::path> dirs;
  if (const std::string data = data_dir(); !data.empty())
    dirs.push_back(std::filesystem::path(data) / "fonts");
  // Android product partitions + the classic system tree.
  dirs.emplace_back("/system/fonts");
  dirs.emplace_back("/system/product/fonts");
  dirs.emplace_back("/product/fonts");
  dirs.emplace_back("/system/system_ext/fonts");
  // Desktop fallbacks when FontConfig is compiled out.
  dirs.emplace_back("/usr/share/fonts/truetype/dejavu");
  dirs.emplace_back("/usr/share/fonts/TTF");
  dirs.emplace_back("/usr/share/fonts");
  return dirs;
}

bool is_font_file(const std::filesystem::path& p) {
  const std::string e = p.extension().string();
  std::string lower;
  for (char c : e) lower.push_back(static_cast<char>(std::tolower(
                       static_cast<unsigned char>(c))));
  return lower == ".ttf" || lower == ".otf" || lower == ".ttc" ||
         lower == ".otc";
}

const std::vector<ScannedFace>& scanned_faces() {
  static const std::vector<ScannedFace> faces = [] {
    std::vector<ScannedFace> out;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib)) return out;
    std::error_code ec;
    for (const auto& dir : font_dirs()) {
      if (!std::filesystem::is_directory(dir, ec)) continue;
      for (const auto& entry :
           std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const auto path = entry.path();
        if (!is_font_file(path)) continue;
        const std::string file = path.string();
        // Face 0 also reports how many members a collection has.
        long nfaces = 1;
        for (long i = 0; i < nfaces; ++i) {
          FT_Face face = nullptr;
          if (FT_New_Face(lib, file.c_str(), i, &face)) {
            if (i == 0) break;
            continue;
          }
          if (i == 0 && face->num_faces > 1) nfaces = face->num_faces;
          if (!face->family_name || !face->family_name[0] ||
              !FT_IS_SCALABLE(face)) {
            FT_Done_Face(face);
            continue;
          }
          ScannedFace s;
          s.path = file;
          s.face_index = static_cast<int>(i);
          s.family = face->family_name;
          s.bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
          s.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
          // The style name tells collection members apart ("Bold Italic").
          if ((!s.bold || !s.italic) && face->style_name) {
            std::string style = face->style_name;
            for (char& c : style)
              c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (style.find("bold") != std::string::npos) s.bold = true;
            if (style.find("italic") != std::string::npos ||
                style.find("oblique") != std::string::npos)
              s.italic = true;
          }
          // Filename hints when the face flags are blank (common on
          // Android); a collection's file name describes no single member.
          if ((!s.bold || !s.italic) && nfaces == 1) {
            std::string stem = path.stem().string();
            for (char& c : stem)
              c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!s.bold &&
                (stem.find("bold") != std::string::npos ||
                 stem.find("-b.") != std::string::npos ||
                 stem.ends_with("-b") || stem.ends_with("_bold")))
              s.bold = true;
            if (!s.italic &&
                (stem.find("italic") != std::string::npos ||
                 stem.find("oblique") != std::string::npos ||
                 stem.find("-i.") != std::string::npos ||
                 stem.ends_with("-i") || stem.ends_with("_italic")))
              s.italic = true;
          }
          out.push_back(std::move(s));
          FT_Done_Face(face);
        }
      }
    }
    FT_Done_FreeType(lib);
    return out;
  }();
  return faces;
}

bool family_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto ca = static_cast<unsigned char>(a[i]);
    const auto cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

// Map CSS generics onto families actually present on the device.
const char* generic_prefer(std::string_view generic) {
  static const char* sans[] = {"Roboto", "Noto Sans", "Droid Sans",
                               "DejaVu Sans", "Liberation Sans", nullptr};
  static const char* serif[] = {"Noto Serif", "Droid Serif", "DejaVu Serif",
                                "Liberation Serif", "NotoSerif", nullptr};
  static const char* mono[] = {"Droid Sans Mono", "Cutive Mono",
                               "Roboto Mono", "DejaVu Sans Mono",
                               "Noto Sans Mono", nullptr};
  const char** list = sans;
  if (generic == "serif") list = serif;
  else if (generic == "monospace" || generic == "mono") list = mono;
  for (const char** p = list; *p; ++p) {
    for (const ScannedFace& f : scanned_faces())
      if (family_equal(f.family, *p)) return *p;
  }
  return nullptr;
}

}  // namespace

FontFace fontconfig_find_face(const std::string& family, int weight,
                              int slant) {
  if (family.empty()) return {};
  std::string want = family;
  if (want == "serif" || want == "sans-serif" || want == "sans" ||
      want == "monospace" || want == "mono") {
    if (const char* mapped = generic_prefer(want)) want = mapped;
  }
  const bool need_bold = weight != 0;
  const bool need_italic = slant != 0;
  const ScannedFace* best = nullptr;
  int best_score = -1;
  for (const ScannedFace& f : scanned_faces()) {
    if (!family_equal(f.family, want)) continue;
    // Prefer exact style; allow regular as a last resort for bold/italic.
    int score = 0;
    if (f.bold == need_bold) score += 2;
    else if (!f.bold && need_bold) score += 0;
    else if (f.bold && !need_bold) score -= 1;
    if (f.italic == need_italic) score += 2;
    else if (!f.italic && need_italic) score += 0;
    else if (f.italic && !need_italic) score -= 1;
    // Prefer face index 0 when scores tie (main face of a collection).
    if (score > best_score ||
        (score == best_score && best && f.face_index < best->face_index)) {
      best_score = score;
      best = &f;
    }
  }
  if (!best) return {};
  return {best->path, best->face_index};
}

std::vector<FontFace> fallback_font_faces(uint32_t, const std::string&,
                                          int, int) {
  static const std::vector<FontFace> paths = [] {
    std::vector<FontFace> found;
    std::unordered_set<std::string> seen;  // "path\nindex"
    auto add = [&](const std::string& path, int index) {
      if (seen.insert(path + '\n' + std::to_string(index)).second)
        found.push_back({path, index});
    };
    for (const char* f : {"DejaVuSans.ttf", "Roboto-Regular.ttf",
                          "NotoSans-Regular.ttf",
                          "NotoSansSymbols-Regular-Subsetted.ttf",
                          "NotoSansSymbols-Regular.ttf",
                          "NotoSansSymbols2-Regular.ttf",
                          "NotoSansMath-Regular.ttf", "DroidSansFallback.ttf",
                          "DroidSansFallbackFull.ttf", "NotoColorEmoji.ttf",
                          "NotoColorEmojiLegacy.ttf"}) {
      for (const auto& dir : font_dirs()) {
        std::error_code ec;
        const auto p = dir / f;
        if (std::filesystem::exists(p, ec)) add(p.string(), 0);
      }
    }
    // Any other scanned Regular face as late coverage.
    for (const ScannedFace& face : scanned_faces()) {
      if (face.bold || face.italic) continue;
      add(face.path, face.face_index);
    }
    return found;
  }();
  return paths;
}

std::string fontconfig_family_from_file(const std::string& path) {
  for (const ScannedFace& f : scanned_faces())
    if (f.path == path) return f.family;
  return {};
}

#endif  // XANATOO_FONTCONFIG


static bool try_load(Font& font, const FontFace& face, int px,
                     SDL_Renderer* r, const char* label) {
  if (face.empty()) return false;
  if (font.load(face, px, r)) {
    std::cerr << "font " << label << ": " << face.path;
    if (face.index != 0) std::cerr << " (face " << face.index << ")";
    std::cerr << " @ " << px << " px\n";
    return true;
  }
  return false;
}

// Restored pre-R36S UI font path (commit before d55b74d UI-scale work).
// GNOME 2–era faces: Bitstream Vera → DejaVu Sans.
FontFace ui_font_face(bool bold) {
  const char* families[] = {
      "DejaVu Sans",
      "Bitstream Vera Sans",
      "Liberation Sans",
      "FreeSans",
      "Nimbus Sans L",
      "Sans",
      nullptr,
  };
  for (const char** f = families; *f; ++f) {
    FontFace face = fontconfig_find_face(*f, bold ? 1 : 0, 0);
    if (!face.empty()) return face;
  }
  if (bold) return ui_font_face(false);
  const char* files[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/ttf-bitstream-vera/Vera.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      nullptr,
  };
  for (const char** f = files; *f; ++f) {
    if (std::filesystem::exists(*f)) return {*f, 0};
  }
  // PortMaster ships DejaVu in its data directory.
  if (std::string p = data_file("fonts/DejaVuSans.ttf"); !p.empty())
    return {p, 0};
  if (const char* body = std::getenv("XANATOO_FONT")) {
    if (body[0] != '\0' && std::filesystem::exists(body)) return {body, 0};
  }
  return {};
}

static bool is_generic_family(const std::string& name) {
  return name == "serif" || name == "sans-serif" || name == "sans" ||
         name == "monospace" || name == "mono";
}

std::string fontconfig_pick_family(const std::vector<std::string>& names) {
#if !XANATOO_FONTCONFIG
  // Prefer a name that is either a CSS generic or a family scanned from disk.
  for (const std::string& name : names) {
    if (name.empty()) continue;
    if (is_generic_family(name)) return name;
    for (const ScannedFace& f : scanned_faces())
      if (family_equal(f.family, name)) return f.family;
  }
  return {};
#else
  if (!FcInit()) return {};
  for (const std::string& name : names) {
    if (name.empty()) continue;
    if (is_generic_family(name)) return name;
    // FontConfig always returns *some* match; accept it only when the
    // matched family is the requested one.
    FcPattern* pat = FcPatternCreate();
    if (!pat) continue;
    FcPatternAddString(pat, FC_FAMILY,
                       reinterpret_cast<const FcChar8*>(name.c_str()));
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);
    if (!match) continue;
    FcChar8* fam = nullptr;
    bool same = false;
    if (FcPatternGetString(match, FC_FAMILY, 0, &fam) == FcResultMatch && fam)
      same = strcasecmp(reinterpret_cast<const char*>(fam), name.c_str()) == 0;
    FcPatternDestroy(match);
    if (same) return name;
  }
  return {};
#endif
}

const std::vector<std::string>& fontconfig_list_families() {
#if !XANATOO_FONTCONFIG
  static const std::vector<std::string> installed = [] {
    std::vector<std::string> out{"serif", "sans-serif", "monospace"};
    std::unordered_set<std::string> seen;
    for (const std::string& g : out) seen.insert(g);
    for (const ScannedFace& f : scanned_faces()) {
      if (seen.insert(f.family).second) out.push_back(f.family);
    }
    std::sort(out.begin() + 3, out.end(),
              [](const std::string& a, const std::string& b) {
                return std::lexicographical_compare(
                    a.begin(), a.end(), b.begin(), b.end(),
                    [](char ca, char cb) {
                      return std::tolower(static_cast<unsigned char>(ca)) <
                             std::tolower(static_cast<unsigned char>(cb));
                    });
              });
    return out;
  }();
  return installed;
#else
  static const std::vector<std::string> installed = [] {
    std::vector<std::string> out{"serif", "sans-serif", "monospace"};
    if (!FcInit()) return out;
    FcPattern* pattern = FcPatternCreate();
    FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, static_cast<const char*>(nullptr));
    if (!pattern || !objects) {
      if (pattern) FcPatternDestroy(pattern);
      if (objects) FcObjectSetDestroy(objects);
      return out;
    }
    FcFontSet* fonts = FcFontList(nullptr, pattern, objects);
    if (fonts) {
      for (int i = 0; i < fonts->nfont; ++i) {
        for (int j = 0;; ++j) {
          FcChar8* family = nullptr;
          if (FcPatternGetString(fonts->fonts[i], FC_FAMILY, j, &family) !=
              FcResultMatch) break;
          if (family && *family)
            out.emplace_back(reinterpret_cast<const char*>(family));
        }
      }
      FcFontSetDestroy(fonts);
    }
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }();
  return installed;
#endif
}

FontSet::FontSet() = default;
FontSet::~FontSet() = default;

MathFont* FontSet::math_font() {
  if (math_tried_) return math_ && math_->loaded() ? math_.get() : nullptr;
  math_tried_ = true;
  const std::string path = find_math_font_path();
  if (path.empty()) {
    std::cerr << "math font: none found (math shows its TeX source)\n";
    return nullptr;
  }
  math_ = std::make_unique<MathFont>();
  if (!math_->load(path, renderer_)) {
    std::cerr << "math font: cannot load " << path << "\n";
    math_.reset();
    return nullptr;
  }
  std::cerr << "math font: " << path
            << (math_->has_math_table() ? "" : " (no MATH table)") << "\n";
  return math_.get();
}

bool FontSet::load(SDL_Renderer* renderer, const std::string& preferred_path,
                   int body_px, int heading_px, const FontFamilies& families) {
  if (renderer != renderer_) {
    // A new renderer cannot use the old textures.
    math_.reset();
    math_tried_ = false;
  }
  renderer_ = renderer;
  std::string family;
  if (!preferred_path.empty()) {
    family = fontconfig_family_from_file(preferred_path);
    if (family.empty()) {
      // Fall back: use the path for regular; FontConfig for variants by guessing
      // common DejaVu/Liberation naming from the basename.
      family = "DejaVu Sans";
    }
  }
  if (family.empty()) family = fontconfig_pick_family(families.body);
  if (family.empty()) {
    // Prefer widely available families
    const char* candidates[] = {
        "DejaVu Sans", "Liberation Sans", "FreeSans", "Noto Sans",
        "Ubuntu", "Cantarell", "Arial", "Helvetica", nullptr};
    for (const char** p = candidates; *p; ++p) {
      std::string path = fontconfig_find(*p, 0, 0);
      if (!path.empty()) {
        family = *p;
        break;
      }
    }
  }
  if (family.empty()) family = "sans-serif";

  FontFace reg{preferred_path, 0};
  if (reg.empty() || !try_load(regular, reg, body_px, renderer, "regular")) {
    reg = fontconfig_find_face(family, 0, 0);
    if (!try_load(regular, reg, body_px, renderer, "regular")) {
      std::cerr << "Failed to load a regular body font (family=" << family << ")\n";
      return false;
    }
  }

  FontFace b = fontconfig_find_face(family, 1, 0);
  if (!try_load(bold, b, body_px, renderer, "bold")) {
    // Soft-fail: share regular face metrics by loading same file
    try_load(bold, regular.face(), body_px, renderer, "bold(fallback)");
  }
  FontFace it = fontconfig_find_face(family, 0, 1);
  if (!try_load(italic, it, body_px, renderer, "italic")) {
    try_load(italic, regular.face(), body_px, renderer, "italic(fallback)");
  }
  FontFace bi = fontconfig_find_face(family, 1, 1);
  if (!try_load(bold_italic, bi, body_px, renderer, "bold-italic")) {
    if (bold.loaded())
      try_load(bold_italic, bold.face(), body_px, renderer, "bold-italic(fallback)");
    else
      try_load(bold_italic, regular.face(), body_px, renderer, "bold-italic(fallback)");
  }

  // Heading: own family when the style names one, else the body family;
  // bold unless the style asks for a regular weight.
  FontFace h;
  const std::string heading_family = fontconfig_pick_family(families.heading);
  if (!heading_family.empty())
    h = fontconfig_find_face(heading_family, families.heading_bold ? 1 : 0, 0);
  else if (families.heading_bold)
    h = bold.loaded() ? bold.face() : regular.face();
  else
    h = regular.face();
  if (!try_load(heading, h, heading_px, renderer, "heading")) {
    try_load(heading, regular.face(), heading_px, renderer, "heading(fallback)");
  }

  // Monospace for code: the style's family first, then common ones.
  std::vector<std::string> mono_families;
  if (std::string f = fontconfig_pick_family(families.code); !f.empty())
    mono_families.push_back(f);
  for (const char* f : {"DejaVu Sans Mono", "Liberation Mono", "FreeMono",
                        "Noto Sans Mono", "Ubuntu Mono", "Courier New",
                        "monospace"})
    mono_families.emplace_back(f);
  bool mono_ok = false;
  for (const std::string& f : mono_families) {
    if (try_load(mono, fontconfig_find_face(f, 0, 0), body_px, renderer,
                 "mono")) {
      mono_ok = true;
      break;
    }
  }
  if (!mono_ok) {
    try_load(mono, regular.face(), body_px, renderer, "mono(fallback)");
  }

  // Margin text: the same faces, smaller.
  const int spx = small_px(body_px);
  auto face_of = [&](const Font& f) {
    return f.loaded() ? f.face() : regular.face();
  };
  try_load(small, regular.face(), spx, renderer, "small");
  try_load(small_bold, face_of(bold), spx, renderer, "small-bold");
  try_load(small_italic, face_of(italic), spx, renderer, "small-italic");
  try_load(small_bold_italic, face_of(bold_italic), spx, renderer,
           "small-bold-italic");
  try_load(small_mono, face_of(mono), spx, renderer, "small-mono");

  return regular.loaded();
}

void FontSet::set_sizes(int body_px, int heading_px) {
  if (regular.loaded()) regular.set_size(body_px);
  if (bold.loaded()) bold.set_size(body_px);
  if (italic.loaded()) italic.set_size(body_px);
  if (bold_italic.loaded()) bold_italic.set_size(body_px);
  if (heading.loaded()) heading.set_size(heading_px);
  if (mono.loaded()) mono.set_size(body_px);
  const int spx = small_px(body_px);
  for (Font* f :
       {&small, &small_bold, &small_italic, &small_bold_italic, &small_mono})
    if (f->loaded()) f->set_size(spx);
}

FontRole small_role(FontRole role) {
  switch (role) {
    case FontRole::Body: return FontRole::Small;
    case FontRole::BodyBold: return FontRole::SmallBold;
    case FontRole::Heading: return FontRole::SmallBold;
    case FontRole::BodyItalic: return FontRole::SmallItalic;
    case FontRole::BodyBoldItalic: return FontRole::SmallBoldItalic;
    case FontRole::Code: return FontRole::SmallCode;
    default: return role;
  }
}

Font& FontSet::pick(FontRole role) {
  switch (role) {
    case FontRole::BodyBold:
      return bold.loaded() ? bold : regular;
    case FontRole::BodyItalic:
      return italic.loaded() ? italic : regular;
    case FontRole::BodyBoldItalic:
      return bold_italic.loaded() ? bold_italic
             : (bold.loaded() ? bold : regular);
    case FontRole::Heading:
      return heading.loaded() ? heading : regular;
    case FontRole::Code:
      return mono.loaded() ? mono : regular;
    case FontRole::Small:
      return small.loaded() ? small : regular;
    case FontRole::SmallBold:
      return small_bold.loaded() ? small_bold : pick(FontRole::Small);
    case FontRole::SmallItalic:
      return small_italic.loaded() ? small_italic : pick(FontRole::Small);
    case FontRole::SmallBoldItalic:
      return small_bold_italic.loaded() ? small_bold_italic
                                        : pick(FontRole::SmallBold);
    case FontRole::SmallCode:
      return small_mono.loaded() ? small_mono : pick(FontRole::Code);
    case FontRole::Body:
    default:
      return regular;
  }
}

const Font& FontSet::pick(FontRole role) const {
  return const_cast<FontSet*>(this)->pick(role);
}

}  // namespace xanatoo
