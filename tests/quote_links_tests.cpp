// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/quote_links.hpp"

#include <cassert>
#include <string>
#include <vector>

using namespace xanatoo;

namespace {

struct Thread {
  Document doc;
  NodeId thread = INVALID_NODE;

  Thread() {
    doc.root = doc.create(NodeKind::Document);
    thread = doc.create(NodeKind::Thread, doc.root);
  }
  // A message with one paragraph per entry of `paras`; a paragraph starting
  // with "Q:" becomes a BlockQuote node (HN style).
  NodeId msg(const std::string& label, const std::string& parent,
             std::vector<std::string> paras) {
    NodeId m = doc.create(NodeKind::Message, thread);
    doc.get(m).label = label;
    doc.get(m).alt = parent;
    doc.get(m).src = "user" + label;
    for (const std::string& p : paras) {
      const bool q = p.rfind("Q:", 0) == 0;
      NodeId n = doc.create(q ? NodeKind::BlockQuote : NodeKind::Paragraph, m);
      doc.append_text(n, q ? p.substr(2) : p);
    }
    return m;
  }
};

// Text of a selection inside one node (what the tests produce).
std::string text_of(const Document& doc, const Selection& s) {
  assert(s.anchor.node == s.focus.node);
  const Node& n = doc.get(s.anchor.node);
  std::string out;
  for (size_t si = s.anchor.span_index; si <= s.focus.span_index; ++si) {
    const std::string& t = n.spans[si].text;
    size_t ci = 0;
    for (size_t i = 0; i < t.size(); ++ci) {
      size_t len = 1;
      const auto u = static_cast<unsigned char>(t[i]);
      if (u >= 0xf0) len = 4;
      else if (u >= 0xe0) len = 3;
      else if (u >= 0xc0) len = 2;
      const bool in = (si > s.anchor.span_index || ci >= s.anchor.char_index) &&
                      (si < s.focus.span_index || ci < s.focus.char_index);
      if (in) out += t.substr(i, len);
      i += len;
    }
  }
  return out;
}

void test_normalize() {
  assert(quote_normalize("Hello,  World\xE2\x80\x99s \xE2\x80\x94 "
                         "\xE2\x80\x9C" "best" "\xE2\x80\x9D!") ==
         "hello world s best");
  assert(quote_normalize("  ...  ") == "");
  assert(quote_normalize("\xC3\x84pfel UND Birnen") == "\xC3\xA4pfel und birnen");
}

void test_blockquote_parent() {
  Thread t;
  NodeId a = t.msg("1", "", {"Rust compile times are the main reason I stay "
                             "with Go for small tools."});
  NodeId b = t.msg("2", "1", {"Q:the main reason I stay with Go",
                              "Have you tried incremental builds?"});
  auto links = find_quote_links(t.doc);
  assert(links.size() == 1);
  assert(links[0].quoting_msg == b);
  assert(links[0].source_msg == a);
  assert(links[0].score == 1.f);
  assert(!links[0].via_quote);
  assert(text_of(t.doc, links[0].source) == "the main reason I stay with Go");
  assert(text_of(t.doc, links[0].quote) == "the main reason I stay with Go");
}

void test_markdown_lines() {
  Thread t;
  NodeId a = t.msg("a", "", {"I think, honestly, that tabs are better than "
                             "spaces for indentation."});
  t.msg("b", "a", {"> tabs are better than spaces\n\nNo way.\n"
                   ">> unrelated nested quote line here\n"});
  auto links = find_quote_links(t.doc);
  assert(links.size() == 1);
  assert(links[0].source_msg == a);
  assert(text_of(t.doc, links[0].source) == "tabs are better than spaces");
  assert(text_of(t.doc, links[0].quote) == "tabs are better than spaces");
}

void test_ellipsis_pieces() {
  Thread t;
  NodeId a = t.msg("a", "", {"The first part of the argument is sound. Then "
                             "comes a long digression. The conclusion, "
                             "however, does not follow at all."});
  t.msg("b", "a", {"Q:The first part of the argument is sound [...] the "
                   "conclusion, however, does not follow"});
  auto links = find_quote_links(t.doc);
  assert(links.size() == 1);
  assert(links[0].source_msg == a);
  assert(text_of(t.doc, links[0].source) ==
         "The first part of the argument is sound. Then comes a long "
         "digression. The conclusion, however, does not follow");
  // Out of order pieces do not match exactly.
  Thread u;
  u.msg("a", "", {"alpha beta gamma delta. epsilon zeta eta theta."});
  u.msg("b", "a", {"Q:epsilon zeta eta \xE2\x80\xA6 alpha beta gamma"});
  for (const auto& l : find_quote_links(u.doc)) assert(l.score < 1.f);
}

void test_quote_of_quote() {
  Thread t;
  NodeId a = t.msg("a", "", {"Memory safety bugs are seventy percent of all "
                             "security issues in large C++ codebases."});
  NodeId b = t.msg("b", "a", {"Q:seventy percent of all security issues",
                              "Source?"});
  NodeId c = t.msg("c", "b", {"> > seventy percent of all security issues\n"
                              "> Source?\nMicrosoft said so."});
  auto links = find_quote_links(t.doc);
  // b → a, c's nested quote → a (the origin, not b's copy), "Source?" is
  // too short.
  assert(links.size() == 2);
  assert(links[0].quoting_msg == b && links[0].source_msg == a);
  assert(links[1].quoting_msg == c && links[1].source_msg == a);
  (void)b;
}

void test_thresholds() {
  Thread t;
  NodeId a = t.msg("a", "", {"Static typing helps refactoring a lot."});
  NodeId b = t.msg("b", "", {"Unrelated top level comment about editors."});
  t.msg("c", "a", {"Q:helps refactoring", "Q:typing helps refactoring",
                   "Q:about editors here"});
  auto links = find_quote_links(t.doc);
  // Two words: too short. Three words of the parent: fine. Three words of a
  // non-ancestor: too short ("here" also does not match).
  assert(links.size() == 1);
  assert(links[0].source_msg == a);
  (void)b;
  // A non-ancestor is fine with a longer quote.
  Thread u;
  u.msg("h", "", {"The head post."});
  NodeId ua = u.msg("a", "", {"Unrelated top level comment about modal editors."});
  u.msg("b", "", {"Another root comment."});
  u.msg("c", "b", {"Q:top level comment about modal editors",
                   "Q:about modal editors"});
  auto ul = find_quote_links(u.doc);
  assert(ul.size() == 1 && ul[0].source_msg == ua);
}

void test_fuzzy() {
  Thread t;
  NodeId a = t.msg("a", "", {"We moved the whole build to Bazel last year and "
                             "the remote cache cut our CI times in half."});
  t.msg("b", "a", {"Q:we moved the entire build to Bazel last year and the "
                   "remote cache cut our CI times in half"});
  auto links = find_quote_links(t.doc);
  assert(links.size() == 1);
  assert(links[0].source_msg == a);
  assert(links[0].score < 1.f && links[0].score >= 0.6f);
  const std::string src = text_of(t.doc, links[0].source);
  assert(src.find("Bazel last year") != std::string::npos);
  assert(src.find("in half") != std::string::npos);
}

void test_external_and_via_quote() {
  Thread t;
  NodeId a = t.msg("a", "", {"Q:premature optimization is the root of all "
                             "evil in programming",
                             "Knuth said that."});
  NodeId b = t.msg("b", "a", {"Q:premature optimization is the root of all "
                              "evil",
                              "Out of context, though."});
  NodeId c = t.msg("c", "", {"Q:a quote from somewhere else entirely, not here"});
  auto links = find_quote_links(t.doc);
  assert(links.size() == 1);
  assert(links[0].quoting_msg == b && links[0].source_msg == a);
  assert(links[0].via_quote);
  (void)c;
}

void test_head_post() {
  // Quotes of the thread's head (story text, selftext) count as near even
  // when the reply's parent is another comment.
  Thread t;
  NodeId head = t.msg("h", "", {"Ask: which language for embedded work?"});
  t.msg("x", "", {"C, obviously."});
  t.msg("y", "x", {"Q:which language for embedded", "Rust."});
  auto links = find_quote_links(t.doc);
  assert(links.size() == 1 && links[0].source_msg == head);
}

}  // namespace

int main() {
  test_normalize();
  test_blockquote_parent();
  test_markdown_lines();
  test_ellipsis_pieces();
  test_quote_of_quote();
  test_thresholds();
  test_fuzzy();
  test_external_and_via_quote();
  test_head_post();
}
