// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Native DOCX / ODT import: OpenXML / ODF ZIP packages → simplified HTML →
// parse_html_page → Markdown. Mirrors the EPUB native reader’s quality goal.

#include "xanatoo/office_import.hpp"

#include "xanatoo/html_min.hpp"
#include "xanatoo/html_page.hpp"
#include "xanatoo/inflate.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {
namespace {

struct ZipEntry {
  size_t local_offset = 0;
  size_t comp_size = 0;
  int method = 0;
};
using ZipDir = std::map<std::string, ZipEntry, std::less<>>;

uint32_t le(std::string_view s, size_t pos, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i)
    v |= static_cast<uint32_t>(static_cast<unsigned char>(s[pos + i]))
         << (8 * i);
  return v;
}

std::optional<ZipDir> zip_directory(std::string_view zip) {
  constexpr size_t kEocd = 22, kCentral = 46;
  if (zip.size() < kEocd) return std::nullopt;
  size_t eocd = zip.size() - kEocd;
  const size_t lowest =
      zip.size() > kEocd + 0xFFFF ? zip.size() - kEocd - 0xFFFF : 0;
  while (le(zip, eocd, 4) != 0x06054b50) {
    if (eocd == lowest) return std::nullopt;
    --eocd;
  }
  const uint32_t count = le(zip, eocd + 10, 2);
  size_t pos = le(zip, eocd + 16, 4);
  ZipDir dir;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos > zip.size() || zip.size() - pos < kCentral ||
        le(zip, pos, 4) != 0x02014b50)
      return std::nullopt;
    const size_t name_len = le(zip, pos + 28, 2);
    const size_t extra_len = le(zip, pos + 30, 2);
    const size_t comment_len = le(zip, pos + 32, 2);
    if (zip.size() - pos - kCentral < name_len) return std::nullopt;
    ZipEntry e;
    e.method = static_cast<int>(le(zip, pos + 10, 2));
    e.comp_size = le(zip, pos + 20, 4);
    e.local_offset = le(zip, pos + 42, 4);
    dir.emplace(std::string(zip.substr(pos + kCentral, name_len)), e);
    pos += kCentral + name_len + extra_len + comment_len;
  }
  return dir;
}

std::optional<std::string> zip_read(std::string_view zip, const ZipDir& dir,
                                    std::string_view name) {
  auto it = dir.find(name);
  if (it == dir.end()) return std::nullopt;
  const ZipEntry& e = it->second;
  constexpr size_t kLocal = 30;
  if (e.local_offset > zip.size() || zip.size() - e.local_offset < kLocal ||
      le(zip, e.local_offset, 4) != 0x04034b50)
    return std::nullopt;
  const size_t data = e.local_offset + kLocal +
                      le(zip, e.local_offset + 26, 2) +
                      le(zip, e.local_offset + 28, 2);
  if (data > zip.size() || zip.size() - data < e.comp_size) return std::nullopt;
  std::string_view body = zip.substr(data, e.comp_size);
  if (e.method == 0) return std::string(body);
  if (e.method == 8) return inflate_raw(body);
  return std::nullopt;
}

bool is_name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
         c == ':' || c == '.';
}

std::string_view local_name(std::string_view qname) {
  if (const size_t c = qname.rfind(':'); c != std::string_view::npos)
    return qname.substr(c + 1);
  return qname;
}

std::string esc_html(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string xml_text_content(std::string_view xml) {
  // Concatenate character data; skip tags. Decode common entities at the end.
  std::string raw;
  for (size_t i = 0; i < xml.size();) {
    if (xml[i] == '<') {
      if (xml.substr(i, 9) == "<![CDATA[") {
        const size_t end = xml.find("]]>", i + 9);
        if (end == std::string_view::npos) break;
        raw.append(xml.substr(i + 9, end - (i + 9)));
        i = end + 3;
        continue;
      }
      const size_t gt = xml.find('>', i);
      if (gt == std::string_view::npos) break;
      i = gt + 1;
      continue;
    }
    raw.push_back(xml[i++]);
  }
  return html_decode_entities(raw);
}

// ---------- DOCX (WordprocessingML) ---------------------------------------

std::map<std::string, std::string> docx_rels(std::string_view rels_xml) {
  std::map<std::string, std::string> out;
  for (size_t lt = rels_xml.find('<'); lt != std::string_view::npos;
       lt = rels_xml.find('<', lt + 1)) {
    size_t end = lt + 1;
    while (end < rels_xml.size() && is_name_char(rels_xml[end])) ++end;
    if (local_name(rels_xml.substr(lt + 1, end - lt - 1)) != "Relationship")
      continue;
    const size_t gt = rels_xml.find('>', end);
    if (gt == std::string_view::npos) break;
    std::string_view attrs = rels_xml.substr(end, gt - end);
    const std::string id = html_attr(attrs, "Id");
    const std::string target = html_attr(attrs, "Target");
    if (!id.empty() && !target.empty()) out[id] = target;
  }
  return out;
}

int docx_heading_level(std::string_view style) {
  std::string s;
  for (char c : style) s.push_back(static_cast<char>(std::tolower(
                           static_cast<unsigned char>(c))));
  // Heading1 … Heading9, Title → 1, Subtitle → 2
  if (s == "title") return 1;
  if (s == "subtitle") return 2;
  if (s.rfind("heading", 0) == 0) {
    int n = 0;
    for (size_t i = 7; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]));
         ++i)
      n = n * 10 + (s[i] - '0');
    if (n >= 1 && n <= 6) return n;
  }
  return 0;
}

// DOCX paragraph → HTML block with bold / italic / links. `inline_only`
// returns just the inline content (the inside of a hyperlink).
std::string docx_para_html(std::string_view p_xml,
                           const std::map<std::string, std::string>& rels,
                           bool inline_only = false) {
  std::string style;
  // pPr pStyle
  size_t ppr = p_xml.find(":pPr");
  if (ppr == std::string_view::npos) ppr = p_xml.find("<w:pPr");
  if (ppr != std::string_view::npos) {
    const size_t ppr_end = p_xml.find("</w:pPr", ppr);
    std::string_view pr =
        ppr_end == std::string_view::npos
            ? p_xml.substr(ppr)
            : p_xml.substr(ppr, ppr_end - ppr);
    size_t ps = pr.find("pStyle");
    if (ps != std::string_view::npos) {
      const size_t g = pr.find('>', ps);
      if (g != std::string_view::npos)
        style = html_attr(pr.substr(ps, g - ps), "val");
      if (style.empty()) style = html_attr(pr.substr(ps, g - ps), "w:val");
    }
  }

  std::string body;
  // Split into runs: <w:r>…</w:r> and hyperlinks
  size_t i = 0;
  while (i < p_xml.size()) {
    const size_t lt = p_xml.find('<', i);
    if (lt == std::string_view::npos) break;
    size_t ns = lt + 1;
    if (ns < p_xml.size() && p_xml[ns] == '/') {
      i = lt + 1;
      continue;
    }
    size_t ne = ns;
    while (ne < p_xml.size() && is_name_char(p_xml[ne])) ++ne;
    std::string_view name = local_name(p_xml.substr(ns, ne - ns));
    const size_t gt = p_xml.find('>', ne);
    if (gt == std::string_view::npos) break;
    const bool self = gt > 0 && p_xml[gt - 1] == '/';
    std::string_view attrs = p_xml.substr(ne, gt - ne - (self ? 1 : 0));

    if (name == "hyperlink" && !self) {
      std::string href;
      std::string rid = html_attr(attrs, "id");
      if (rid.empty()) rid = html_attr(attrs, "r:id");
      // OOXML uses r:id="rIdN"
      if (rid.empty()) {
        size_t idpos = attrs.find(":id=");
        if (idpos != std::string_view::npos) {
          idpos += 4;
          if (idpos < attrs.size() && (attrs[idpos] == '"' || attrs[idpos] == '\'')) {
            char q = attrs[idpos++];
            size_t e = attrs.find(q, idpos);
            if (e != std::string_view::npos)
              rid = std::string(attrs.substr(idpos, e - idpos));
          }
        }
      }
      auto it = rels.find(rid);
      if (it != rels.end()) href = it->second;
      if (href.empty()) {
        std::string anc = html_attr(attrs, "anchor");
        if (!anc.empty()) href = "#" + anc;
      }
      // Find matching close
      size_t depth = 1, j = gt + 1, start = j;
      while (j < p_xml.size() && depth) {
        size_t l2 = p_xml.find('<', j);
        if (l2 == std::string_view::npos) {
          j = p_xml.size();
          break;
        }
        bool c = l2 + 1 < p_xml.size() && p_xml[l2 + 1] == '/';
        size_t n0 = l2 + 1 + (c ? 1 : 0);
        size_t n1 = n0;
        while (n1 < p_xml.size() && is_name_char(p_xml[n1])) ++n1;
        std::string_view ln = local_name(p_xml.substr(n0, n1 - n0));
        size_t g2 = p_xml.find('>', n1);
        if (g2 == std::string_view::npos) {
          j = p_xml.size();
          break;
        }
        bool se = g2 > 0 && p_xml[g2 - 1] == '/';
        if (ln == "hyperlink") {
          if (c)
            --depth;
          else if (!se)
            ++depth;
        }
        j = g2 + 1;
        if (depth == 0) {
          body += "<a href=\"" + esc_html(href) + "\">";
          body += docx_para_html(p_xml.substr(start, l2 - start), rels,
                                 /*inline_only=*/true);
          body += "</a>";
          i = j;
          break;
        }
      }
      if (depth) i = j;
      continue;
    }

    if (name == "r" && !self) {
      size_t depth = 1, j = gt + 1, start = j;
      bool bold = false, italic = false;
      while (j < p_xml.size() && depth) {
        size_t l2 = p_xml.find('<', j);
        if (l2 == std::string_view::npos) {
          j = p_xml.size();
          break;
        }
        bool c = l2 + 1 < p_xml.size() && p_xml[l2 + 1] == '/';
        size_t n0 = l2 + 1 + (c ? 1 : 0);
        size_t n1 = n0;
        while (n1 < p_xml.size() && is_name_char(p_xml[n1])) ++n1;
        std::string_view ln = local_name(p_xml.substr(n0, n1 - n0));
        size_t g2 = p_xml.find('>', n1);
        if (g2 == std::string_view::npos) {
          j = p_xml.size();
          break;
        }
        bool se = g2 > 0 && p_xml[g2 - 1] == '/';
        if (ln == "r") {
          if (c)
            --depth;
          else if (!se)
            ++depth;
        }
        j = g2 + 1;
        if (depth == 0) {
          std::string_view run = p_xml.substr(start, l2 - start);
          // Walk the run's tags in order: rPr toggles (<w:b/>, <w:i/>; not
          // bCs / br / iCs), text (<w:t>), tabs and line breaks.
          std::string text;  // already HTML
          for (size_t k = run.find('<'); k != std::string_view::npos;) {
            const bool closing = k + 1 < run.size() && run[k + 1] == '/';
            size_t n0 = k + 1 + (closing ? 1 : 0), n1 = n0;
            while (n1 < run.size() && is_name_char(run[n1])) ++n1;
            const std::string_view tag = local_name(run.substr(n0, n1 - n0));
            const size_t tg = run.find('>', n1);
            if (tg == std::string_view::npos) break;
            const bool empty_el = tg > 0 && run[tg - 1] == '/';
            const std::string_view attrs =
                run.substr(n1, tg - n1 - (empty_el ? 1 : 0));
            size_t next = run.find('<', tg + 1);
            if (!closing) {
              if (tag == "b" || tag == "i") {
                std::string v = html_attr(attrs, "w:val");
                if (v.empty()) v = html_attr(attrs, "val");
                const bool on = v != "0" && v != "false" && v != "off";
                (tag == "b" ? bold : italic) = on;
              } else if (tag == "t" && !empty_el) {
                const size_t tc = run.find("</", tg + 1);
                if (tc == std::string_view::npos) break;
                text += esc_html(xml_text_content(run.substr(tg + 1, tc - tg - 1)));
                next = run.find('<', tc + 2);
              } else if (tag == "tab") {
                text.push_back('\t');
              } else if (tag == "br" || tag == "cr") {
                text += "<br/>";
              }
            }
            k = next;
          }
          if (!text.empty()) {
            std::string piece = std::move(text);
            if (italic) piece = "<em>" + piece + "</em>";
            if (bold) piece = "<strong>" + piece + "</strong>";
            body += piece;
          }
          i = j;
          break;
        }
      }
      if (depth) i = j;
      continue;
    }

    i = gt + 1;
  }

  if (body.empty()) body = esc_html(xml_text_content(p_xml));
  if (inline_only) return body;

  const int level = docx_heading_level(style);
  if (level > 0)
    return "<h" + std::to_string(level) + ">" + body + "</h" +
           std::to_string(level) + ">";
  if (style.find("Quote") != std::string::npos ||
      style.find("quote") != std::string::npos)
    return "<blockquote>" + body + "</blockquote>";
  return "<p>" + body + "</p>";
}

std::string docx_body_html(std::string_view document_xml,
                           const std::map<std::string, std::string>& rels) {
  std::string html = "<html><body>";
  // Iterate top-level w:p and w:tbl inside body
  size_t body = document_xml.find(":body");
  if (body == std::string_view::npos) body = 0;
  else body = document_xml.find('>', body);
  std::string_view xml = body == std::string_view::npos
                             ? document_xml
                             : document_xml.substr(body + 1);

  size_t i = 0;
  while (i < xml.size()) {
    size_t lt = xml.find('<', i);
    if (lt == std::string_view::npos) break;
    size_t ns = lt + 1;
    if (ns < xml.size() && xml[ns] == '/') {
      i = lt + 1;
      continue;
    }
    size_t ne = ns;
    while (ne < xml.size() && is_name_char(xml[ne])) ++ne;
    std::string_view name = local_name(xml.substr(ns, ne - ns));
    size_t gt = xml.find('>', ne);
    if (gt == std::string_view::npos) break;
    bool self = gt > 0 && xml[gt - 1] == '/';
    if ((name == "p" || name == "tbl") && !self) {
      size_t depth = 1, j = gt + 1, start = j;
      while (j < xml.size() && depth) {
        size_t l2 = xml.find('<', j);
        if (l2 == std::string_view::npos) {
          j = xml.size();
          break;
        }
        bool c = l2 + 1 < xml.size() && xml[l2 + 1] == '/';
        size_t n0 = l2 + 1 + (c ? 1 : 0);
        size_t n1 = n0;
        while (n1 < xml.size() && is_name_char(xml[n1])) ++n1;
        std::string_view ln = local_name(xml.substr(n0, n1 - n0));
        size_t g2 = xml.find('>', n1);
        if (g2 == std::string_view::npos) {
          j = xml.size();
          break;
        }
        bool se = g2 > 0 && xml[g2 - 1] == '/';
        if (ln == name) {
          if (c)
            --depth;
          else if (!se)
            ++depth;
        }
        j = g2 + 1;
        if (depth == 0) {
          std::string_view el = xml.substr(start, l2 - start);
          if (name == "p")
            html += docx_para_html(el, rels);
          else {
            // Basic table: each tr → row, tc → cell
            html += "<table>";
            size_t r = 0;
            while (r < el.size()) {
              size_t tr = el.find(":tr", r);
              if (tr == std::string_view::npos) break;
              // find start of tag
              size_t trs = tr;
              while (trs > 0 && el[trs] != '<') --trs;
              size_t tre = el.find('>', tr);
              if (tre == std::string_view::npos) break;
              size_t depth_tr = 1, k = tre + 1, tr_start = k;
              while (k < el.size() && depth_tr) {
                size_t l3 = el.find('<', k);
                if (l3 == std::string_view::npos) {
                  k = el.size();
                  break;
                }
                bool c = l3 + 1 < el.size() && el[l3 + 1] == '/';
                size_t a = l3 + 1 + (c ? 1 : 0);
                size_t b = a;
                while (b < el.size() && is_name_char(el[b])) ++b;
                std::string_view ln = local_name(el.substr(a, b - a));
                size_t g3 = el.find('>', b);
                if (g3 == std::string_view::npos) {
                  k = el.size();
                  break;
                }
                bool se = g3 > 0 && el[g3 - 1] == '/';
                if (ln == "tr") {
                  if (c)
                    --depth_tr;
                  else if (!se)
                    ++depth_tr;
                }
                k = g3 + 1;
                if (depth_tr == 0) {
                  std::string_view row = el.substr(tr_start, l3 - tr_start);
                  html += "<tr>";
                  size_t cpos = 0;
                  while (cpos < row.size()) {
                    size_t tc = row.find(":tc", cpos);
                    if (tc == std::string_view::npos) break;
                    size_t tcs = tc;
                    while (tcs > 0 && row[tcs] != '<') --tcs;
                    size_t tce = row.find('>', tc);
                    if (tce == std::string_view::npos) break;
                    size_t dtc = 1, m = tce + 1, tc_start = m;
                    while (m < row.size() && dtc) {
                      size_t l4 = row.find('<', m);
                      if (l4 == std::string_view::npos) {
                        m = row.size();
                        break;
                      }
                      bool cc = l4 + 1 < row.size() && row[l4 + 1] == '/';
                      size_t a2 = l4 + 1 + (cc ? 1 : 0);
                      size_t b2 = a2;
                      while (b2 < row.size() && is_name_char(row[b2])) ++b2;
                      std::string_view ln2 = local_name(row.substr(a2, b2 - a2));
                      size_t g4 = row.find('>', b2);
                      if (g4 == std::string_view::npos) {
                        m = row.size();
                        break;
                      }
                      bool se2 = g4 > 0 && row[g4 - 1] == '/';
                      if (ln2 == "tc") {
                        if (cc)
                          --dtc;
                        else if (!se2)
                          ++dtc;
                      }
                      m = g4 + 1;
                      if (dtc == 0) {
                        html += "<td>";
                        html += esc_html(xml_text_content(
                            row.substr(tc_start, l4 - tc_start)));
                        html += "</td>";
                        cpos = m;
                        break;
                      }
                    }
                    if (dtc) cpos = m;
                    else {
                      /* advanced */
                    }
                  }
                  html += "</tr>";
                  r = k;
                  break;
                }
              }
              if (depth_tr) r = k;
            }
            html += "</table>";
          }
          i = j;
          break;
        }
      }
      if (depth) i = j;
      continue;
    }
    i = gt + 1;
  }
  html += "</body></html>";
  return html;
}

// ---------- ODT (OpenDocument text) ---------------------------------------

std::string odt_inline(std::string_view xml);

std::string odt_children_html(std::string_view xml) {
  std::string html;
  size_t i = 0;
  while (i < xml.size()) {
    if (xml[i] != '<') {
      size_t lt = xml.find('<', i);
      if (lt == std::string_view::npos) {
        html += esc_html(html_decode_entities(std::string(xml.substr(i))));
        break;
      }
      html += esc_html(html_decode_entities(std::string(xml.substr(i, lt - i))));
      i = lt;
      continue;
    }
    bool close = i + 1 < xml.size() && xml[i + 1] == '/';
    size_t ns = i + 1 + (close ? 1 : 0);
    size_t ne = ns;
    while (ne < xml.size() && is_name_char(xml[ne])) ++ne;
    std::string_view name = local_name(xml.substr(ns, ne - ns));
    size_t gt = xml.find('>', ne);
    if (gt == std::string_view::npos) break;
    bool self = gt > 0 && xml[gt - 1] == '/';
    std::string_view attrs = xml.substr(ne, gt - ne - (self ? 1 : 0));
    if (close || self) {
      if (name == "line-break" || name == "line_break") html += "<br/>";
      if (name == "tab") html.push_back('\t');
      if (name == "s") {
        int n = 1;
        std::string c = html_attr(attrs, "c");
        if (!c.empty()) n = std::max(1, std::atoi(c.c_str()));
        html.append(static_cast<size_t>(n), ' ');
      }
      i = gt + 1;
      continue;
    }
    // open element with children
    size_t depth = 1, j = gt + 1, start = j;
    while (j < xml.size() && depth) {
      size_t l2 = xml.find('<', j);
      if (l2 == std::string_view::npos) {
        j = xml.size();
        break;
      }
      bool c = l2 + 1 < xml.size() && xml[l2 + 1] == '/';
      size_t a = l2 + 1 + (c ? 1 : 0);
      size_t b = a;
      while (b < xml.size() && is_name_char(xml[b])) ++b;
      std::string_view ln = local_name(xml.substr(a, b - a));
      size_t g2 = xml.find('>', b);
      if (g2 == std::string_view::npos) {
        j = xml.size();
        break;
      }
      bool se = g2 > 0 && xml[g2 - 1] == '/';
      if (ln == name) {
        if (c)
          --depth;
        else if (!se)
          ++depth;
      }
      j = g2 + 1;
      if (depth == 0) {
        std::string_view inner = xml.substr(start, l2 - start);
        if (name == "h") {
          int level = 1;
          std::string ol = html_attr(attrs, "outline-level");
          if (ol.empty()) ol = html_attr(attrs, "text:outline-level");
          if (!ol.empty()) level = std::clamp(std::atoi(ol.c_str()), 1, 6);
          html += "<h" + std::to_string(level) + ">";
          html += odt_children_html(inner);
          html += "</h" + std::to_string(level) + ">";
        } else if (name == "p") {
          html += "<p>";
          html += odt_children_html(inner);
          html += "</p>";
        } else if (name == "list") {
          html += "<ul>";
          html += odt_children_html(inner);
          html += "</ul>";
        } else if (name == "list-item") {
          html += "<li>";
          html += odt_children_html(inner);
          html += "</li>";
        } else if (name == "a") {
          std::string href = html_attr(attrs, "href");
          if (href.empty()) href = html_attr(attrs, "xlink:href");
          html += "<a href=\"" + esc_html(href) + "\">";
          html += odt_children_html(inner);
          html += "</a>";
        } else if (name == "span") {
          // style name may imply emphasis; keep text
          html += odt_children_html(inner);
        } else if (name == "table") {
          html += "<table>";
          html += odt_children_html(inner);
          html += "</table>";
        } else if (name == "table-row") {
          html += "<tr>";
          html += odt_children_html(inner);
          html += "</tr>";
        } else if (name == "table-cell") {
          html += "<td>";
          html += odt_children_html(inner);
          html += "</td>";
        } else if (name == "note" || name == "annotation") {
          // skip
        } else {
          html += odt_children_html(inner);
        }
        i = j;
        break;
      }
    }
    if (depth) i = j;
  }
  return html;
}

std::string odt_body_html(std::string_view content_xml) {
  // Prefer office:text
  size_t text = content_xml.find(":text");
  std::string_view xml = content_xml;
  if (text != std::string_view::npos) {
    size_t gt = content_xml.find('>', text);
    size_t end = content_xml.rfind("</");
    if (gt != std::string_view::npos && end != std::string_view::npos &&
        end > gt)
      xml = content_xml.substr(gt + 1, end - gt - 1);
  }
  return "<html><body>" + odt_children_html(xml) + "</body></html>";
}

std::string html_to_md(std::string_view html) {
  Document doc = parse_html_page(html, {}, /*keep_chrome=*/false);
  return serialize_markup(doc);
}

}  // namespace

bool is_docx_path(std::string_view path) {
  return path_ends_with_ci(path, ".docx");
}
bool is_odt_path(std::string_view path) {
  return path_ends_with_ci(path, ".odt");
}
bool is_office_path(std::string_view path) {
  return is_docx_path(path) || is_odt_path(path);
}

std::optional<std::string> docx_to_markdown(std::string_view zip,
                                            std::string* err) {
  auto fail = [&](const char* m) -> std::optional<std::string> {
    if (err) *err = m;
    return std::nullopt;
  };
  auto dir = zip_directory(zip);
  if (!dir) return fail("not a zip package");
  auto doc = zip_read(zip, *dir, "word/document.xml");
  if (!doc) return fail("missing word/document.xml");
  std::map<std::string, std::string> rels;
  if (auto r = zip_read(zip, *dir, "word/_rels/document.xml.rels"))
    rels = docx_rels(*r);
  std::string title;
  if (auto core = zip_read(zip, *dir, "docProps/core.xml")) {
    size_t t = core->find("dc:title");
    if (t != std::string::npos) {
      size_t gt = core->find('>', t);
      size_t end = core->find("</", gt);
      if (gt != std::string::npos && end != std::string::npos)
        title = html_to_plain(std::string_view(*core).substr(gt + 1, end - gt - 1));
    }
  }
  std::string md;
  if (!title.empty()) {
    md += "---\ntitle: \"";
    for (char c : title) {
      if (c == '"')
        md += "\\\"";
      else
        md.push_back(c);
    }
    md += "\"\n---\n\n";
  }
  md += html_to_md(docx_body_html(*doc, rels));
  return md;
}

std::optional<std::string> odt_to_markdown(std::string_view zip,
                                           std::string* err) {
  auto fail = [&](const char* m) -> std::optional<std::string> {
    if (err) *err = m;
    return std::nullopt;
  };
  auto dir = zip_directory(zip);
  if (!dir) return fail("not a zip package");
  auto content = zip_read(zip, *dir, "content.xml");
  if (!content) return fail("missing content.xml");
  std::string title;
  if (auto meta = zip_read(zip, *dir, "meta.xml")) {
    size_t t = meta->find("dc:title");
    if (t != std::string::npos) {
      size_t gt = meta->find('>', t);
      size_t end = meta->find("</", gt);
      if (gt != std::string::npos && end != std::string::npos)
        title = html_to_plain(std::string_view(*meta).substr(gt + 1, end - gt - 1));
    }
  }
  std::string md;
  if (!title.empty()) {
    md += "---\ntitle: \"";
    for (char c : title) {
      if (c == '"')
        md += "\\\"";
      else
        md.push_back(c);
    }
    md += "\"\n---\n\n";
  }
  md += html_to_md(odt_body_html(*content));
  return md;
}

std::optional<std::string> office_to_markdown(std::string_view path_hint,
                                              std::string_view zip,
                                              std::string* err) {
  if (is_docx_path(path_hint) ||
      zip.find("word/document.xml") != std::string_view::npos)
    return docx_to_markdown(zip, err);
  if (is_odt_path(path_hint) ||
      zip.find("content.xml") != std::string_view::npos)
    return odt_to_markdown(zip, err);
  if (err) *err = "unsupported office package";
  return std::nullopt;
}

}  // namespace xanatoo
