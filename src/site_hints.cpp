// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/site_hints.hpp"

#include "xanatoo/html_min.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/sexpr.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace xanatoo {

namespace {

bool ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
         c == ':' || c == '*';
}

template <size_t N>
bool one_of(std::string_view s, const std::array<std::string_view, N>& set) {
  return std::find(set.begin(), set.end(), s) != set.end();
}

constexpr std::array<std::string_view, 14> kVoid = {
    "area", "base", "br", "col", "embed", "hr", "img",
    "input", "link", "meta", "param", "source", "track", "wbr"};
constexpr std::array<std::string_view, 4> kRawText = {"script", "style",
                                                      "textarea", "title"};
// Start tags that close an open <p> (HTML "closes a p element").
constexpr std::array<std::string_view, 30> kClosesP = {
    "address", "article", "aside", "blockquote", "details", "div",
    "dl", "fieldset", "figcaption", "figure", "footer", "form",
    "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "menu",
    "nav", "ol", "p", "pre", "section", "table", "ul", "li"};
// Elements an implied close does not reach across.
constexpr std::array<std::string_view, 8> kScope = {
    "table", "td", "th", "button", "html", "body", "object", "template"};

size_t find_ci(std::string_view h, std::string_view needle, size_t from) {
  for (size_t i = from; i + needle.size() <= h.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() &&
           std::tolower(static_cast<unsigned char>(h[i + k])) == needle[k])
      ++k;
    if (k == needle.size()) return i;
  }
  return std::string_view::npos;
}

bool has_class(std::string_view attrs, std::string_view cls) {
  const std::string v = html_attr(attrs, "class");
  std::string_view s = v;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t e = i;
    while (e < s.size() && !std::isspace(static_cast<unsigned char>(s[e]))) ++e;
    if (s.substr(i, e - i) == cls) return true;
    i = e;
  }
  return false;
}

bool compound_matches(const HintCompound& c, const HtmlElem& el) {
  if (!c.tag.empty() && c.tag != "*" && c.tag != el.tag) return false;
  if (!c.id.empty() && html_attr(el.attrs, "id") != c.id) return false;
  for (const std::string& cls : c.classes)
    if (!has_class(el.attrs, cls)) return false;
  for (const auto& [name, value] : c.attrs) {
    if (value.empty() ? !html_has_attr(el.attrs, name)
                      : html_attr(el.attrs, name) != value)
      return false;
  }
  return true;
}

// ---- Sheets -------------------------------------------------------------

struct SheetReader {
  std::string_view name;
  std::string* errors;

  void error(const std::string& msg) {
    if (errors) *errors += std::string(name) + ": " + msg + "\n";
  }

  bool selector(const SExpr& e, HintSelector& out) {
    if (e.kind != SExpr::Kind::String) {
      error("selector must be a \"string\"");
      return false;
    }
    std::string err;
    out = parse_hint_selector(e.text, &err);
    if (!err.empty()) error("selector \"" + e.text + "\": " + err);
    return err.empty() && !out.empty();
  }

  void field(const SExpr& f, HintField& out) {
    if (f.kids.size() < 2 || !selector(f.kids[1], out.sel)) return;
    if (f.kids.size() > 2) {
      out.regex = sexpr_text(f.kids[2]);
      try {
        std::regex re(out.regex);
      } catch (const std::regex_error&) {
        error("bad regex \"" + out.regex + "\"");
        out.regex.clear();
      }
    }
  }

  bool thread(const SExpr& f, HintRule& r) {
    for (size_t k = 1; k < f.kids.size(); ++k) {
      const SExpr& x = f.kids[k];
      if (x.kind == SExpr::Kind::String) {
        HintSelector s;
        if (selector(x, s)) r.sels.push_back(std::move(s));
      } else if (is_form(x, "heading") && x.kids.size() > 1) {
        r.thread.heading = sexpr_text(x.kids[1]);
      } else if (is_form(x, "message") && x.kids.size() > 1) {
        selector(x.kids[1], r.thread.message);
      } else if (is_form(x, "body") && x.kids.size() > 1) {
        selector(x.kids[1], r.thread.body);
      } else if (is_form(x, "title")) {
        field(x, r.thread.title);
      } else if (is_form(x, "author")) {
        field(x, r.thread.author);
      } else if (is_form(x, "date")) {
        field(x, r.thread.date);
      } else if (is_form(x, "score")) {
        field(x, r.thread.score);
      } else {
        error("unknown thread field");
      }
    }
    if (r.thread.message.empty()) {
      error("thread without (message \"selector\")");
      return false;
    }
    return true;
  }

  bool rule(const SExpr& f, HintRule& r) {
    const std::string head = f.kids.empty() ? "" : sexpr_text(f.kids[0]);
    if (head == "main") r.kind = HintKind::Main;
    else if (head == "chrome") r.kind = HintKind::Chrome;
    else if (head == "content") r.kind = HintKind::Content;
    else if (head == "drop") r.kind = HintKind::Drop;
    else if (head == "thread") {
      r.kind = HintKind::Thread;
      return thread(f, r) && !r.sels.empty();
    } else {
      error("unknown rule (" + head + " …)");
      return false;
    }
    for (size_t k = 1; k < f.kids.size(); ++k) {
      const SExpr& x = f.kids[k];
      // (chrome sidebar "#slashboxes"): the bare word is what it is.
      if (r.kind == HintKind::Chrome && x.kind == SExpr::Kind::Atom) {
        r.role = x.text;
        continue;
      }
      HintSelector s;
      if (selector(x, s)) r.sels.push_back(std::move(s));
    }
    if (r.sels.empty()) error("(" + head + ") without a selector");
    return !r.sels.empty();
  }
};

// ---- Registry -----------------------------------------------------------

struct Registry {
  std::mutex mutex;
  std::string signature;  // sheet paths, sizes and times last read
  std::vector<std::shared_ptr<const SiteHints>> sites;
};

std::vector<fs::path> sheet_files(std::string* signature) {
  std::vector<fs::path> out;
  for (const std::string& base : {data_dir(), user_data_dir()}) {
    if (base.empty()) continue;
    std::error_code ec;
    const fs::path dir = fs::path(base) / "sites";
    if (!fs::is_directory(dir, ec)) continue;
    std::vector<fs::path> files;
    for (const auto& ent : fs::directory_iterator(dir, ec))
      if (ent.path().extension() == ".xsite") files.push_back(ent.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
      const auto size = fs::file_size(p, ec);
      // The rep is __int128 on the Android NDK's libc++: no to_string.
      const auto time = static_cast<long long>(
          fs::last_write_time(p, ec).time_since_epoch().count());
      *signature += p.string() + ":" + std::to_string(static_cast<unsigned long long>(size)) + ":" +
                    std::to_string(time) + "\n";
      out.push_back(p);
    }
  }
  return out;
}

// ---- Threads ------------------------------------------------------------

std::string apply_regex(const std::string& text, const std::string& re) {
  if (re.empty()) return text;
  try {
    std::smatch m;
    if (!std::regex_search(text, m, std::regex(re))) return text;
    return m.size() > 1 && m[1].matched ? m[1].str() : m[0].str();
  } catch (const std::regex_error&) {
    return text;
  }
}

std::string elem_text(const PageHints& page, const HtmlElem& el) {
  return html_to_plain(page.html.substr(el.open, el.close - el.open));
}

// href of `e` when it is a link, else of the first link inside it.
std::string elem_href(const PageHints& page, int e) {
  const auto& els = page.els;
  for (int k = e; k < static_cast<int>(els.size()) && els[k].lt < els[e].end;
       ++k)
    if (els[k].tag == "a") {
      std::string h = html_attr(els[k].attrs, "href");
      if (!h.empty() && h != "#") return h;
    }
  return {};
}

void build_thread(const PageHints& page, const HintRule& rule, int te,
                  Document& doc, NodeId parent, const HtmlOptions& opt) {
  const auto& els = page.els;
  const HintThread& t = rule.thread;
  const size_t n = els.size();
  // Messages inside the thread, and for every element the message it
  // belongs to (the nearest message around it, -1 = none).
  std::vector<int> owner(n, -1);
  std::vector<char> is_msg(n, 0);
  std::vector<int> msgs;
  int last = te;
  for (int e = te + 1; e < static_cast<int>(n) && els[e].lt < els[te].end;
       ++e) {
    last = e;
    const int p = els[e].parent;
    if (p > te) owner[e] = is_msg[p] ? p : owner[p];
    if (hint_matches(t.message, els, e)) {
      is_msg[e] = 1;
      msgs.push_back(e);
    }
  }
  auto first_in = [&](int m, const HintSelector& sel) {
    if (sel.empty()) return -1;
    for (int e = m + 1; e <= last && els[e].lt < els[m].end; ++e)
      if (owner[e] == m && hint_matches(sel, els, e)) return e;
    return -1;
  };
  auto text_of = [&](int m, const HintField& f) {
    const int e = first_in(m, f.sel);
    std::string s = e < 0 ? std::string() : elem_text(page, els[e]);
    s = apply_regex(s, f.regex);
    std::string_view v = s;
    trim_ascii(v);
    return std::pair<std::string, int>(std::string(v), e);
  };
  const std::string& base = opt.base_url;
  auto link = [&](int e) {
    std::string h = e < 0 ? std::string() : elem_href(page, e);
    return h.empty() ? h : resolve_url(base, h);
  };

  NodeId thread = doc.create(NodeKind::Thread, parent);
  // No label: the band would show the element id as a thread id ("#…").
  if (!t.heading.empty()) set_thread_title(doc, thread, t.heading);
  std::vector<std::string> labels(n);
  // A message with nothing in it (a collapsed shell the page's script
  // fills in) is left out; its replies go under the message around it.
  std::vector<char> skip(n, 0);
  for (int m : msgs)
    skip[m] = text_of(m, t.author).first.empty() &&
              text_of(m, t.title).first.empty() && first_in(m, t.body) < 0;
  for (size_t k = 0; k < msgs.size(); ++k) {
    const int m = msgs[k];
    if (skip[m]) continue;
    labels[m] = html_attr(els[m].attrs, "id");
    if (labels[m].empty()) labels[m] = "msg-" + std::to_string(k + 1);
    int up = owner[m];
    while (up >= 0 && skip[up]) up = owner[up];
    auto [author, author_el] = text_of(m, t.author);
    auto [date, date_el] = text_of(m, t.date);
    auto [title, title_el] = text_of(m, t.title);
    const std::string score = text_of(m, t.score).first;

    NodeId msg = doc.create(NodeKind::Message, thread);
    Node& node = doc.get(msg);
    node.label = labels[m];
    node.src = author.empty() ? "[unknown]" : author;
    node.summary = date;
    if (up >= 0) node.alt = labels[up];  // indented under its parent
    // Header slots: [0] author, [1] date, [2] reply (types.hpp, Message).
    InlineSpan a;
    a.href = link(author_el);
    a.kind = a.href.empty() ? InlineKind::Strong : InlineKind::Link;
    a.text = node.src;
    node.spans.push_back(std::move(a));
    InlineSpan d;
    d.href = link(date_el);
    if (d.href.empty()) d.href = link(title_el);
    d.kind = d.href.empty() ? InlineKind::Strong : InlineKind::Link;
    d.text = date;
    node.spans.push_back(std::move(d));
    if (up >= 0) {
      InlineSpan r;
      r.kind = InlineKind::Link;
      r.href = "#id:" + labels[up];
      node.spans.push_back(std::move(r));
    }
    if (!score.empty()) {
      InlineSpan s;
      s.role = InlineRole::Metadata;
      s.rel = "score";
      s.text = score;
      node.spans.push_back(std::move(s));
    }
    if (!title.empty()) {
      NodeId p = doc.create(NodeKind::Paragraph, msg);
      doc.append_text(p, title, InlineKind::Strong);
    }
    if (const int b = first_in(m, t.body); b >= 0)
      html_append_blocks(
          doc, msg, page.html.substr(els[b].open, els[b].close - els[b].open),
          opt);
  }
}

}  // namespace

HintSelector parse_hint_selector(std::string_view s, std::string* err) {
  HintSelector out;
  size_t i = 0;
  auto fail = [&](const char* msg) {
    if (err && err->empty()) *err = msg;
    out.chain.clear();
    return out;
  };
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    HintCompound c;
    bool any = false;
    auto ident = [&] {
      size_t e = i;
      while (e < s.size() && ident_char(s[e])) ++e;
      std::string v(s.substr(i, e - i));
      i = e;
      return v;
    };
    if (ident_char(s[i])) {
      c.tag = ascii_lower(ident());
      any = true;
    }
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
      const char k = s[i++];
      if (k == '#' || k == '.') {
        std::string v = ident();
        if (v.empty()) return fail("empty #id or .class");
        if (k == '#') c.id = std::move(v);
        else c.classes.push_back(std::move(v));
      } else if (k == '[') {
        size_t close = s.find(']', i);
        if (close == std::string_view::npos) return fail("unclosed [");
        std::string_view in = s.substr(i, close - i);
        i = close + 1;
        size_t eq = in.find('=');
        std::string name(in.substr(0, eq));
        std::string value;
        if (eq != std::string_view::npos) {
          std::string_view v = in.substr(eq + 1);
          if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') &&
              v.back() == v[0])
            v = v.substr(1, v.size() - 2);
          value = std::string(v);
          if (value.empty()) return fail("empty [attr=]");
        }
        if (name.empty()) return fail("empty [attr]");
        c.attrs.emplace_back(ascii_lower(name), std::move(value));
      } else {
        return fail("unexpected character (only tag#id.class[attr=v] and "
                    "spaces between them)");
      }
      any = true;
    }
    if (any) out.chain.push_back(std::move(c));
  }
  return out;
}

bool hint_matches(const HintSelector& sel, const std::vector<HtmlElem>& els,
                  int e) {
  if (sel.chain.empty() || e < 0) return false;
  if (!compound_matches(sel.chain.back(), els[e])) return false;
  int at = els[e].parent;
  for (size_t k = sel.chain.size() - 1; k-- > 0;) {
    while (at >= 0 && !compound_matches(sel.chain[k], els[at]))
      at = els[at].parent;
    if (at < 0) return false;
    at = els[at].parent;
  }
  return true;
}

std::vector<SiteHints> parse_site_hints(std::string_view src,
                                        std::string_view name,
                                        std::string* errors) {
  std::vector<SiteHints> out;
  SheetReader rd{name, errors};
  for (const SExpr& top : SExprParser(src).parse_all()) {
    if (!is_form(top, "site")) {
      rd.error("top level holds only (site \"host\" …) forms");
      continue;
    }
    SiteHints site;
    site.source = std::string(name);
    for (size_t k = 1; k < top.kids.size(); ++k) {
      const SExpr& x = top.kids[k];
      if (x.kind != SExpr::Kind::List) {
        std::string h = ascii_lower(sexpr_text(x));
        if (starts_with(h, "www.")) h.erase(0, 4);
        if (!h.empty()) site.hosts.push_back(std::move(h));
        continue;
      }
      HintRule r;
      if (rd.rule(x, r)) site.rules.push_back(std::move(r));
    }
    if (site.hosts.empty()) {
      rd.error("(site) without a host");
      continue;
    }
    out.push_back(std::move(site));
  }
  return out;
}

bool site_hints_match_host(const SiteHints& h, std::string_view host) {
  std::string lower = ascii_lower(host);
  for (const std::string& want : h.hosts)
    if (lower == want ||
        (lower.size() > want.size() && ends_with(lower, want) &&
         lower[lower.size() - want.size() - 1] == '.'))
      return true;
  return false;
}

std::shared_ptr<const SiteHints> site_hints_for_host(std::string_view host) {
  static Registry reg;
  std::lock_guard<std::mutex> lock(reg.mutex);
  std::string sig;
  std::vector<fs::path> files = sheet_files(&sig);
  if (sig != reg.signature) {
    reg.signature = std::move(sig);
    reg.sites.clear();
    for (const fs::path& p : files) {
      std::ifstream in(p, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      std::string errors;
      for (SiteHints& s : parse_site_hints(ss.str(), p.string(), &errors))
        reg.sites.push_back(std::make_shared<const SiteHints>(std::move(s)));
      if (!errors.empty()) std::fprintf(stderr, "xanatoo: %s", errors.c_str());
    }
  }
  for (auto it = reg.sites.rbegin(); it != reg.sites.rend(); ++it)
    if (site_hints_match_host(**it, host)) return *it;
  return nullptr;
}

std::vector<HtmlElem> html_elements(std::string_view html) {
  std::vector<HtmlElem> els;
  std::vector<int> open;  // stack of element indices
  // Close every element above stack depth `keep`, implied at `at`.
  auto close_above = [&](size_t keep, size_t at) {
    while (open.size() > keep) {
      HtmlElem& el = els[open.back()];
      el.close = el.end = at;
      open.pop_back();
    }
  };
  // Depth of the nearest open `tags` element above any of `stop`, or npos.
  auto find_open = [&](std::initializer_list<std::string_view> tags,
                       std::initializer_list<std::string_view> stop) {
    for (size_t d = open.size(); d-- > 0;) {
      const std::string& t = els[open[d]].tag;
      if (std::find(tags.begin(), tags.end(), t) != tags.end()) return d;
      if (std::find(stop.begin(), stop.end(), t) != stop.end() ||
          one_of(t, kScope))
        return std::string_view::npos;
    }
    return std::string_view::npos;
  };
  size_t i = 0;
  while ((i = html.find('<', i)) != std::string_view::npos) {
    if (html.compare(i, 4, "<!--") == 0) {
      size_t c = html.find("-->", i + 4);
      if (c == std::string_view::npos) break;
      i = c + 3;
      continue;
    }
    const bool closing = i + 1 < html.size() && html[i + 1] == '/';
    const size_t at = i + 1 + (closing ? 1 : 0);
    if (at >= html.size() ||
        !std::isalpha(static_cast<unsigned char>(html[at]))) {
      if (at < html.size() && (html[at] == '!' || html[at] == '?')) {
        size_t c = html.find('>', at);
        if (c == std::string_view::npos) break;
        i = c + 1;
      } else {
        ++i;
      }
      continue;
    }
    size_t gt = html_tag_end(html, i);
    if (gt == std::string_view::npos) break;
    size_t ne = at;
    while (ne < gt && (std::isalnum(static_cast<unsigned char>(html[ne])) ||
                       html[ne] == '-' || html[ne] == ':'))
      ++ne;
    const std::string name = ascii_lower(html.substr(at, ne - at));
    if (closing) {
      for (size_t d = open.size(); d-- > 0;) {
        if (els[open[d]].tag == name) {
          close_above(d + 1, i);
          HtmlElem& el = els[open[d]];
          el.close = i;
          el.end = gt + 1;
          open.pop_back();
          break;
        }
        if (one_of(std::string_view(els[open[d]].tag), kScope) &&
            name != "table" && name != "td" && name != "th" &&
            name != "body" && name != "html")
          break;  // a stray end tag does not reach out of a cell
      }
      i = gt + 1;
      continue;
    }
    // Implied end tags before this start tag.
    size_t d = std::string_view::npos;
    if (name == "li") d = find_open({"li"}, {"ul", "ol", "menu"});
    else if (name == "dt" || name == "dd") d = find_open({"dt", "dd"}, {"dl"});
    else if (name == "tr")
      d = find_open({"tr"}, {"tbody", "thead", "tfoot"});
    else if (name == "td" || name == "th") d = find_open({"td", "th"}, {"tr"});
    else if (name == "option") d = find_open({"option"}, {"select"});
    if (d != std::string_view::npos) close_above(d, i);
    if (one_of(std::string_view(name), kClosesP)) {
      size_t p = find_open({"p"}, {});
      if (p != std::string_view::npos) close_above(p, i);
    }
    HtmlElem el;
    el.lt = i;
    el.open = gt + 1;
    el.tag = name;
    el.attrs = html.substr(ne, gt - ne);
    el.parent = open.empty() ? -1 : open.back();
    std::string_view tagtext = html.substr(i, gt - i);
    const bool self_close = !tagtext.empty() && tagtext.back() == '/';
    const int idx = static_cast<int>(els.size());
    if (self_close || one_of(std::string_view(name), kVoid)) {
      el.close = el.end = gt + 1;
      els.push_back(std::move(el));
      i = gt + 1;
      continue;
    }
    if (one_of(std::string_view(name), kRawText)) {
      size_t c = find_ci(html, "</" + name, gt + 1);
      size_t ce = c == std::string_view::npos ? c : html_tag_end(html, c);
      el.close = c == std::string_view::npos ? html.size() : c;
      el.end = ce == std::string_view::npos ? html.size() : ce + 1;
      els.push_back(std::move(el));
      i = els.back().end;
      continue;
    }
    els.push_back(std::move(el));
    open.push_back(idx);
    i = gt + 1;
  }
  close_above(0, html.size());
  return els;
}

PageHints bind_site_hints(const SiteHints& site, std::string_view html) {
  PageHints page;
  page.html = html;
  page.site = &site;
  page.els = html_elements(html);
  const int n = static_cast<int>(page.els.size());
  page.rule_of.assign(n, -1);
  std::vector<char> in_main(n, 0);
  for (int e = 0; e < n; ++e) {
    const int p = page.els[e].parent;
    const bool inside = p >= 0 && in_main[p];
    for (size_t r = 0; r < site.rules.size(); ++r) {
      const HintRule& rule = site.rules[r];
      const bool hit =
          std::any_of(rule.sels.begin(), rule.sels.end(),
                      [&](const HintSelector& s) {
                        return hint_matches(s, page.els, e);
                      });
      if (!hit) continue;
      if (rule.kind == HintKind::Main) {
        if (!inside && !in_main[e]) page.main.push_back(e);
        in_main[e] = 1;
      } else if (page.rule_of[e] < 0) {
        page.rule_of[e] = static_cast<int>(r);  // the first rule wins
      }
    }
    if (inside) in_main[e] = 1;
  }
  return page;
}

void attach_page_hints(HtmlOptions& opt, const PageHints& page) {
  const PageHints* pg = &page;
  auto elem_at = [pg](const char* lt) -> int {
    const char* base = pg->html.data();
    if (lt < base || lt >= base + pg->html.size()) return -1;
    const size_t off = static_cast<size_t>(lt - base);
    auto it = std::lower_bound(
        pg->els.begin(), pg->els.end(), off,
        [](const HtmlElem& el, size_t o) { return el.lt < o; });
    if (it == pg->els.end() || it->lt != off) return -1;
    return static_cast<int>(it - pg->els.begin());
  };
  opt.hint = [pg, elem_at](const char* lt) {
    HtmlHint h;
    const int e = elem_at(lt);
    if (e < 0 || pg->rule_of[e] < 0) return h;
    const HintRule& rule = pg->site->rules[pg->rule_of[e]];
    const HtmlElem& el = pg->els[e];
    const char* base = pg->html.data();
    h.inner = base + el.open;
    h.inner_end = base + el.close;
    h.end = base + el.end;
    switch (rule.kind) {
      case HintKind::Chrome:
        h.kind = HtmlHint::Kind::Chrome;
        h.role = rule.role;
        break;
      case HintKind::Content: h.kind = HtmlHint::Kind::Content; break;
      case HintKind::Drop: h.kind = HtmlHint::Kind::Drop; break;
      case HintKind::Thread: h.kind = HtmlHint::Kind::Replace; break;
      case HintKind::Main: break;
    }
    return h;
  };
  HtmlOptions inner = opt;  // message bodies: the hints, but no nested threads
  opt.hint_replace = [pg, elem_at, inner](Document& doc, NodeId parent,
                                          const char* lt) {
    const int e = elem_at(lt);
    if (e < 0 || pg->rule_of[e] < 0) return;
    const HintRule& rule = pg->site->rules[pg->rule_of[e]];
    if (rule.kind == HintKind::Thread)
      build_thread(*pg, rule, e, doc, parent, inner);
  };
}

}  // namespace xanatoo
