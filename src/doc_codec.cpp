// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/doc_codec.hpp"

#include <cstring>

namespace xanatoo {
namespace {

constexpr std::string_view kMagic = "XDOC";
// Upper bounds against corrupt input (a snapshot is at most a few MB).
constexpr uint64_t kMaxNodes = 1u << 24;
constexpr uint64_t kMaxString = 1u << 28;

struct Writer {
  std::string out;
  void u(uint64_t v) {
    while (v >= 0x80) {
      out.push_back(static_cast<char>((v & 0x7f) | 0x80));
      v >>= 7;
    }
    out.push_back(static_cast<char>(v));
  }
  void i(int64_t v) {
    u((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
  }
  void b(uint8_t v) { out.push_back(static_cast<char>(v)); }
  void s(std::string_view v) {
    u(v.size());
    out.append(v);
  }
  void f(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    for (int k = 0; k < 4; ++k) b(static_cast<uint8_t>(bits >> (8 * k)));
  }
};

struct Reader {
  std::string_view in;
  size_t at = 0;
  bool ok = true;
  uint64_t u() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at >= in.size()) break;
      const auto c = static_cast<uint8_t>(in[at++]);
      v |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80)) return v;
    }
    ok = false;
    return 0;
  }
  int64_t i() {
    const uint64_t z = u();
    return static_cast<int64_t>((z >> 1) ^ (~(z & 1) + 1));
  }
  uint8_t b() {
    if (at >= in.size()) {
      ok = false;
      return 0;
    }
    return static_cast<uint8_t>(in[at++]);
  }
  std::string s() {
    const uint64_t n = u();
    if (!ok || n > kMaxString || n > in.size() - at) {
      ok = false;
      return {};
    }
    std::string v(in.substr(at, static_cast<size_t>(n)));
    at += static_cast<size_t>(n);
    return v;
  }
  float f() {
    uint32_t bits = 0;
    for (int k = 0; k < 4; ++k) bits |= static_cast<uint32_t>(b()) << (8 * k);
    float v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
  }
};

void put_span(Writer& w, const InlineSpan& sp) {
  w.b(static_cast<uint8_t>(sp.kind));
  w.b(static_cast<uint8_t>(sp.role));
  w.b(sp.marks);
  w.s(sp.text);
  w.s(sp.href);
  w.s(sp.rel);
  w.s(sp.datetime);
  w.u(sp.target);
  w.s(sp.src);
  w.s(sp.alt);
  w.u(sp.width);
  w.u(sp.height);
  w.b(sp.display ? 1 : 0);
}

InlineSpan get_span(Reader& r) {
  InlineSpan sp;
  sp.kind = static_cast<InlineKind>(r.b());
  sp.role = static_cast<InlineRole>(r.b());
  sp.marks = r.b();
  sp.text = r.s();
  sp.href = r.s();
  sp.rel = r.s();
  sp.datetime = r.s();
  sp.target = r.u();
  sp.src = r.s();
  sp.alt = r.s();
  sp.width = static_cast<uint16_t>(r.u());
  sp.height = static_cast<uint16_t>(r.u());
  sp.display = r.b() != 0;
  if (sp.kind > InlineKind::Math || sp.role > InlineRole::Metadata) r.ok = false;
  return sp;
}

}  // namespace

std::string encode_document(const Document& doc) {
  Writer w;
  w.out.append(kMagic);
  w.b(kDocCodecVersion);
  w.s(doc.meta.title);
  w.s(doc.meta.lang);
  w.s(doc.meta.author);
  w.s(doc.meta.front_matter);
  w.u(doc.root);
  w.u(doc.next_id);
  w.u(doc.nodes.size());
  for (const Node& n : doc.nodes) {
    w.u(n.id);
    w.b(static_cast<uint8_t>(n.kind));
    w.u(n.parent);
    w.u(n.children.size());
    for (NodeId c : n.children) w.u(c);
    w.u(n.spans.size());
    for (const InlineSpan& sp : n.spans) put_span(w, sp);
    w.i(n.level);
    w.i(n.col_span);
    w.i(n.row_span);
    w.s(n.alt);
    w.s(n.src);
    w.b(n.expanded ? 1 : 0);
    w.s(n.summary);
    w.s(n.float_side);
    w.s(n.align);
    w.s(n.valign);
    w.s(n.cols);
    w.f(n.display_scale);
    w.i(n.width);
    w.i(n.height);
    w.s(n.label);
    w.s(n.tags);
    w.s(n.style);
  }
  return std::move(w.out);
}

std::optional<Document> decode_document(std::string_view bytes) {
  if (bytes.size() < kMagic.size() + 1 || bytes.substr(0, kMagic.size()) != kMagic ||
      static_cast<uint8_t>(bytes[kMagic.size()]) != kDocCodecVersion)
    return std::nullopt;
  Reader r{bytes, kMagic.size() + 1};
  Document doc;
  doc.meta.title = r.s();
  doc.meta.lang = r.s();
  doc.meta.author = r.s();
  doc.meta.front_matter = r.s();
  doc.root = r.u();
  doc.next_id = r.u();
  const uint64_t count = r.u();
  if (!r.ok || count > kMaxNodes) return std::nullopt;
  doc.nodes.reserve(static_cast<size_t>(count));
  // Ids name nodes by position (Document::get), so they must stay in range.
  auto in_range = [&](NodeId id) { return id <= count; };
  for (uint64_t k = 0; k < count && r.ok; ++k) {
    Node n;
    n.id = r.u();
    n.kind = static_cast<NodeKind>(r.b());
    n.parent = r.u();
    const uint64_t kids = r.u();
    if (!r.ok || kids > count) return std::nullopt;
    n.children.reserve(static_cast<size_t>(kids));
    for (uint64_t c = 0; c < kids && r.ok; ++c) {
      const NodeId id = r.u();
      if (!in_range(id)) return std::nullopt;
      n.children.push_back(id);
    }
    const uint64_t spans = r.u();
    if (!r.ok || spans > bytes.size()) return std::nullopt;
    n.spans.reserve(static_cast<size_t>(spans));
    for (uint64_t sp = 0; sp < spans && r.ok; ++sp)
      n.spans.push_back(get_span(r));
    n.level = static_cast<int>(r.i());
    n.col_span = static_cast<int>(r.i());
    n.row_span = static_cast<int>(r.i());
    n.alt = r.s();
    n.src = r.s();
    n.expanded = r.b() != 0;
    n.summary = r.s();
    n.float_side = r.s();
    n.align = r.s();
    n.valign = r.s();
    n.cols = r.s();
    n.display_scale = r.f();
    n.width = static_cast<int>(r.i());
    n.height = static_cast<int>(r.i());
    n.label = r.s();
    n.tags = r.s();
    n.style = r.s();
    if (n.kind > NodeKind::Metadata || !in_range(n.id) || !in_range(n.parent))
      return std::nullopt;
    doc.nodes.push_back(std::move(n));
  }
  if (!r.ok || r.at != bytes.size() || !in_range(doc.root)) return std::nullopt;
  return doc;
}

}  // namespace xanatoo
