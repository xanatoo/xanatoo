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

void Font::set_size(int pixel_size) {
  // Drop cached textures — they are sized for the old pixel size
  drop_cache();

  pixel_size_ = pixel_size;
  FT_Set_Pixel_Sizes(face_, 0, pixel_size);
  for (FallbackFace& f : fallbacks_) FT_Set_Pixel_Sizes(f.face, 0, pixel_size);
  line_height_ = static_cast<int>(face_->size->metrics.height >> 6);
  ascender_ = static_cast<int>(face_->size->metrics.ascender >> 6);
  descender_ = static_cast<int>(face_->size->metrics.descender >> 6);
}

// A face that maps `codepoint`, opening fallback files on first need.
// Line metrics stay the primary face's; only the glyph comes from here.
FT_Face Font::fallback_face(uint32_t codepoint) {
  for (const FallbackFace& f : fallbacks_)
    if (FT_Get_Char_Index(f.face, codepoint) != 0) return f.face;
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
    if (!FT_IS_SCALABLE(face)) {  // bitmap-only (colour emoji) faces
      FT_Done_Face(face);
      continue;
    }
    FT_Set_Pixel_Sizes(face, 0, pixel_size_);
    fallbacks_.push_back({file, face});
    if (FT_Get_Char_Index(face, codepoint) != 0) return face;
  }
  return nullptr;
}

// Coverage curves for blending in sRGB: plain alpha blending makes dark
// text on a light page too heavy and light text on a dark page too thin.
// Blending as if in a space of gamma kTextGamma evens both out; the full
// sRGB 2.2 looks washed out, so this stays near what browsers show.
static constexpr double kTextGamma = 1.45;

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

// `variant` = sub-pixel phase + kSubpixelSteps * light_text.
Glyph Font::load_glyph(uint32_t codepoint, int variant) {
  Glyph g;
  FT_Face face = face_;
  if (codepoint >= 0x20 && FT_Get_Char_Index(face_, codepoint) == 0) {
    if (FT_Face fb = fallback_face(codepoint)) face = fb;
  }
  // Light hinting snaps only vertical metrics and keeps stroke weights
  // fractional, as browsers / fontconfig "hintslight" do. Full native
  // hinting rounds strokes to whole pixels, so text visibly jumps in
  // weight between sizes (DejaVu Sans: 1 px bars at 16 px, 2 px at 17 px).
  // It leaves x alone, so outlines can shift by sub-pixel phases.
  if (FT_Load_Char(face, codepoint, FT_LOAD_TARGET_LIGHT)) return g;
  FT_GlyphSlot slot = face->glyph;
  const int phase = variant % kSubpixelSteps;
  const bool light_text = variant >= kSubpixelSteps;
  if (slot->format == FT_GLYPH_FORMAT_OUTLINE && phase != 0)
    FT_Outline_Translate(&slot->outline, phase * 64 / kSubpixelSteps, 0);
  if (slot->format != FT_GLYPH_FORMAT_BITMAP &&
      FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT))
    return g;
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
    SDL_Surface* surf = xanatoo::sdl::rgba_surface_from(
        rgba.data(), g.width, g.height, g.width * 4);
    if (surf) {
      g.texture = SDL_CreateTextureFromSurface(renderer_, surf);
      if (g.texture) {
        SDL_SetTextureBlendMode(g.texture, SDL_BLENDMODE_BLEND);
        xanatoo::sdl::set_nearest(g.texture);
      }
      xanatoo::sdl::destroy_surface(surf);
    }
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
          FcBool scalable = FcTrue, color = FcFalse;
          FcPatternGetBool(f, FC_SCALABLE, 0, &scalable);
          FcPatternGetBool(f, FC_COLOR, 0, &color);
          if (!scalable || color) continue;
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
                          "DroidSansFallbackFull.ttf"}) {
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
