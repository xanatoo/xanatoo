// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for the Usenet / NNTP client (nntp.hpp): refs, wire
// formats, MIME decoding and the documents, fed with cut-down recorded
// server replies. Part of xanatoo_tests (test_nntp is called from
// unit_tests.cpp).

#include "xanatoo/nntp.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {
int g_passed = 0;
int g_failed = 0;
void expect(bool cond, const char* expr, const char* file, int line) {
  if (cond) {
    ++g_passed;
    return;
  }
  ++g_failed;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
}
}  // namespace

#define EXPECT(c) expect(!!(c), #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) expect((a) == (b), #a " == " #b, __FILE__, __LINE__)

using namespace xanatoo;

namespace {

NntpRef ref(std::string_view s) {
  auto r = parse_nntp_url(s);
  EXPECT(r.has_value());
  return r ? *r : NntpRef{};
}

std::string all_text(const Document& d) {
  std::string out;
  for (const Node& n : d.nodes) {
    for (const InlineSpan& s : n.spans) out += s.text + "|";
    out += n.src + "|";
  }
  return out;
}

bool has_href(const Document& d, std::string_view href) {
  for (const Node& n : d.nodes)
    for (const InlineSpan& s : n.spans)
      if (s.href == href) return true;
  return false;
}

size_t count_kind(const Document& d, NodeKind k) {
  size_t n = 0;
  for (const Node& node : d.nodes)
    if (node.kind == k && node.id != INVALID_NODE) ++n;
  return n;
}

void test_refs() {
  EXPECT(is_nntp_url("news:comp.lang.c"));
  EXPECT(is_nntp_url("NNTP://h/g/1"));
  EXPECT(!is_nntp_url("news:"));
  EXPECT(!is_nntp_url("newsletter.md"));

  NntpRef g = ref("news:comp.lang.c");
  EXPECT(g.view == NntpView::Group);
  EXPECT_EQ(g.host, std::string(kNntpDefaultServer));
  EXPECT_EQ(g.group, "comp.lang.c");
  EXPECT_EQ(nntp_url_string(g), std::string("news://") + kNntpDefaultServer + "/comp.lang.c");
  EXPECT_EQ(ref("news:gmane.comp.version-control.git").host, "news.gmane.io");

  NntpRef a = ref("nntp://News.Example.org:1119/comp.lang.c/404390");
  EXPECT(a.view == NntpView::Article);
  EXPECT_EQ(a.host, "news.example.org");
  EXPECT_EQ(a.port, 1119);
  EXPECT_EQ(a.article, 404390);
  EXPECT_EQ(nntp_url_string(a), "nntp://news.example.org:1119/comp.lang.c/404390");
  EXPECT_EQ(nntp_up_url(a), "news://news.example.org:1119/comp.lang.c");
  EXPECT_EQ(nntp_up_url(ref("news://h/comp.lang.c")), "news://h/comp.lang.*");
  EXPECT_EQ(nntp_up_url(ref("news://h/comp.lang.*")), "news://h/comp.*");
  EXPECT_EQ(nntp_up_url(ref("news://h/comp.*")), "news://h/");
  EXPECT_EQ(nntp_up_url(ref("news://h/")), "");

  NntpRef m = ref("news://h/%3Cabc$1@dont-email.me%3E");
  EXPECT(m.view == NntpView::MessageId);
  EXPECT_EQ(m.message_id, "abc$1@dont-email.me");
  EXPECT_EQ(nntp_msgid_url(m, "x/y@z"), "news://h/x%2Fy@z");  // no "//"
  EXPECT_EQ(ref("news:abc@example.com").message_id, "abc@example.com");

  NntpRef w = ref("news://h/comp.lang.*");
  EXPECT(w.view == NntpView::Groups);
  EXPECT_EQ(w.wildmat, "comp.lang.*");
  EXPECT(ref("news://h").view == NntpView::Groups);
  EXPECT(ref("news://h/").wildmat.empty());

  NntpRef p = ref("news://h/comp.lang.c?page=3#id:7");
  EXPECT_EQ(p.page, 3);
  EXPECT_EQ(p.fragment, "id:7");
  EXPECT_EQ(nntp_url_string(p), "news://h/comp.lang.c?page=3#id:7");

  NntpRef t = ref("nntps://h/alt.test");
  EXPECT(t.tls);
  EXPECT_EQ(t.port, 563);
  EXPECT(ref("snews://h/alt.test").tls);
  EXPECT_EQ(nntp_article_url(t, "alt.test", 5), "nntps://h/alt.test/5#id:5");

  EXPECT(!parse_nntp_url("nntp:comp.lang.c"));  // nntp: needs a server
  EXPECT(!parse_nntp_url("nntp://h/g/x1"));
  EXPECT(!parse_nntp_url("news://h:99999/g"));
}

void test_wire() {
  EXPECT(nntp_wildmat_match("comp.*", "comp.lang.c"));
  EXPECT(!nntp_wildmat_match("comp.*", "alt.comp"));
  EXPECT(nntp_wildmat_match("comp.*,!comp.lang.*", "comp.os.linux"));
  EXPECT(!nntp_wildmat_match("comp.*,!comp.lang.*", "comp.lang.c"));
  EXPECT(nntp_wildmat_match("a?c", "abc"));
  EXPECT(nntp_wildmat_match("[ab]*", "beta"));
  EXPECT(!nntp_wildmat_match("[ab]*", "gamma"));
  EXPECT(nntp_wildmat_match("", "anything"));

  auto groups = parse_nntp_active(
      "ab.general 0000007078 0000006494 y\r\n"
      "comp.lang.c 0000404404 0000001824 m\r\n"
      "broken line\r\n");
  EXPECT_EQ(groups.size(), 2u);
  EXPECT_EQ(groups[1].name, "comp.lang.c");
  EXPECT_EQ(groups[1].count(), 404404 - 1824 + 1);
  EXPECT_EQ(groups[1].status, 'm');
  apply_nntp_descriptions(groups, "comp.lang.c\t\tDiscussion about C.\r\nab.general\t?\r\n");
  EXPECT_EQ(groups[1].description, "Discussion about C.");
  EXPECT(groups[0].description.empty());

  auto o = parse_nntp_overview_line(
      "404390\tRe: =?UTF-8?Q?Gr=C3=BC=C3=9Fe?=\tDavid Brown <david@example.no>\t"
      "Wed, 18 Nov 2026 08:43:04 +0100\t<11djku9$14q9v$1@dont-email.me>\t"
      "<root@x> <mid@x>\t1234\t20\tXref: h comp.lang.c:404390\r");
  EXPECT(o.has_value());
  EXPECT_EQ(o->number, 404390);
  EXPECT_EQ(o->subject, "Re: Grüße");
  EXPECT_EQ(o->message_id, "11djku9$14q9v$1@dont-email.me");
  EXPECT_EQ(o->references.size(), 2u);
  EXPECT_EQ(o->references[0], "root@x");
  EXPECT_EQ(o->lines, 20);
  EXPECT(!parse_nntp_overview_line("x\ty"));

  EXPECT_EQ(nntp_date_iso("Wed, 18 Nov 2026 08:43:04 +0100"), "2026-11-18T07:43:04Z");
  EXPECT_EQ(nntp_date_iso("18 Nov 2026 23:30 -0800"), "2026-11-19T07:30:00Z");
  EXPECT_EQ(nntp_date_iso("Mon, 1 Jan 96 00:00:00 GMT"), "1996-01-01T00:00:00Z");
  EXPECT_EQ(nntp_date_iso("Tue, 2 Jan 2001 10:00:00 EST"), "2001-01-02T15:00:00Z");
  EXPECT_EQ(nntp_date_iso("yesterday"), "");

  EXPECT_EQ(decode_mime_header("=?ISO-8859-1?Q?J=F6rg?= =?ISO-8859-1?Q?_M=FCller?="),
            "Jörg Müller");
  EXPECT_EQ(decode_mime_header("=?utf-8?B?SGFsbMO2?= Welt"), "Hallö Welt");
  EXPECT_EQ(decode_mime_header("plain \xe4"), "plain ä");  // raw Latin-1
  EXPECT_EQ(decode_mime_header("a =? b"), "a =? b");
  // A code page to_utf8 does not know is read as Latin-1, never left invalid.
  EXPECT_EQ(decode_mime_header("D=?iso-8859-13?q?=FF?=O"), "D\u00ffO");
  EXPECT_EQ(mail_display_name("\"Chris M. Thomasson\" <c@example.com>"),
            "Chris M. Thomasson");
  EXPECT_EQ(mail_display_name("jdoe@example.com (John Doe)"), "John Doe");
  EXPECT_EQ(mail_display_name("<jdoe@example.com>"), "jdoe@example.com");
  EXPECT_EQ(strip_reply_prefix("Re: AW: Re[2]: Hello"), "Hello");
  EXPECT_EQ(nntp_xref_number("news.example comp.lang.c:42 alt.test:7", "alt.test"), 7);
  EXPECT_EQ(nntp_xref_number("news.example comp.lang.c:42", "alt.test"), 0);
}

void test_articles() {
  NntpArticle a = parse_nntp_article(
      "From: =?UTF-8?Q?J=C3=B6rg?= <j@example.org>\r\n"
      "Newsgroups: comp.lang.c, alt.test\r\n"
      "Subject: Re: long\r\n"
      " folded subject\r\n"
      "Date: Wed, 18 Nov 2026 08:43:04 +0100\r\n"
      "Message-ID: <b@x>\r\n"
      "References: <a@x>\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: text/plain; charset=ISO-8859-1; format=flowed\r\n"
      "Content-Transfer-Encoding: quoted-printable\r\n"
      "Xref: h comp.lang.c:12\r\n"
      "\r\n"
      "> quoted=\r\n"
      " line\r\n"
      "\r\n"
      "Gr=FC=DFe, this is \r\n"
      "one paragraph.\r\n"
      "-- \r\n"
      "sig\r\n");
  EXPECT_EQ(a.from, "Jörg <j@example.org>");
  EXPECT_EQ(a.subject, "Re: long folded subject");
  EXPECT_EQ(a.message_id, "b@x");
  EXPECT_EQ(a.newsgroups.size(), 2u);
  EXPECT_EQ(a.newsgroups[1], "alt.test");
  EXPECT(a.flowed);
  EXPECT(a.body.find("Grüße, this is \n") != std::string::npos);
  EXPECT(a.body.find("> quoted line") != std::string::npos);  // QP soft break

  Document d;
  NodeId root = d.create(NodeKind::Document);
  append_usenet_body(d, root, a.body, a.flowed);
  EXPECT_EQ(count_kind(d, NodeKind::BlockQuote), 1u);
  const std::string t = all_text(d);
  EXPECT(t.find("Grüße, this is one paragraph.") != std::string::npos);
  EXPECT(t.find("-- \nsig") != std::string::npos);

  // Multipart: the text/plain part (base64) is the body, the rest listed.
  NntpArticle mp = parse_nntp_article(
      "Subject: files\n"
      "Message-ID: <m@x>\n"
      "MIME-Version: 1.0\n"
      "Content-Type: multipart/mixed; boundary=\"XYZ\"\n"
      "\n"
      "preamble\n"
      "--XYZ\n"
      "Content-Type: text/plain; charset=utf-8\n"
      "Content-Transfer-Encoding: base64\n"
      "\n"
      "SGFsbG8gV2VsdA==\n"
      "--XYZ\n"
      "Content-Type: image/png; name=\"pic.png\"\n"
      "Content-Disposition: attachment; filename=\"pic.png\"\n"
      "\n"
      "iVBORw0KGgo=\n"
      "--XYZ--\n");
  EXPECT_EQ(mp.body, "Hallo Welt");
  EXPECT_EQ(mp.attachments.size(), 1u);
  EXPECT_EQ(mp.attachments[0], "pic.png (image/png)");

  // A git patch becomes code from the "---" line on.
  Document p;
  NodeId proot = p.create(NodeKind::Document);
  append_usenet_body(p, proot,
                     "Fix a thing.\n\nSigned-off-by: A <a@x>\n---\n f.c | 2 +-\n\n"
                     "diff --git a/f.c b/f.c\n@@ -1 +1 @@\n-old\n+new\n",
                     false);
  EXPECT_EQ(count_kind(p, NodeKind::CodeBlock), 1u);
  EXPECT_EQ(count_kind(p, NodeKind::Paragraph), 2u);
}

std::vector<NntpOverview> sample_overview() {
  return parse_nntp_overview(
      "100\tHello\tAnn <a@x>\tWed, 18 Nov 2026 08:00:00 +0000\t<r@x>\t\t10\t1\n"
      "101\tRe: Hello\tBob <b@x>\tWed, 18 Nov 2026 09:00:00 +0000\t<s@x>\t<r@x>\t10\t1\n"
      "102\tOther\tCy <c@x>\tWed, 18 Nov 2026 09:30:00 +0000\t<o@x>\t\t10\t1\n"
      "103\tRe: Hello\tAnn <a@x>\tWed, 18 Nov 2026 10:00:00 +0000\t<t@x>\t<r@x> <s@x>\t10\t1\n"
      "104\tRe: Old\tDee <d@x>\tWed, 18 Nov 2026 10:30:00 +0000\t<u@x>\t<gone@x>\t10\t1\n");
}

void test_documents() {
  NntpRef on = ref("news://h/comp.test");
  auto over = sample_overview();
  Document g = nntp_group_document(over, on, 100, 1, 104);
  EXPECT_EQ(count_kind(g, NodeKind::Article), 3u);
  EXPECT(has_href(g, "nntp://h/comp.test/100#id:100"));
  EXPECT(has_href(g, "nntp://h/comp.test/104#id:104"));
  EXPECT(has_href(g, "news://h/comp.test?page=2"));  // older articles exist
  const std::string gt = all_text(g);
  EXPECT(gt.find("3 messages") != std::string::npos);
  EXPECT(gt.find("continues an older thread") != std::string::npos);
  // Newest thread first: "Old" (104), then "Hello" (103), then "Other".
  EXPECT(gt.find("Old|") < gt.find("Hello|"));
  EXPECT(gt.find("Hello|") < gt.find("Other|"));

  auto members = nntp_thread_members(over, 101);
  EXPECT_EQ(members.size(), 3u);
  EXPECT_EQ(members[2], 103);
  EXPECT(nntp_thread_members(over, 999).empty());

  std::vector<NntpArticle> arts;
  for (const char* raw : {
           "Subject: Hello\nFrom: Ann <a@x>\nMessage-ID: <r@x>\nDate: Wed, 18 Nov 2026 08:00:00 +0000\n\nroot text\n",
           "Subject: Re: Hello\nFrom: Bob <b@x>\nMessage-ID: <s@x>\nReferences: <r@x>\n\n> root text\n\nreply\n",
           "Subject: Re: Hello (was: Hi)\nFrom: Ann <a@x>\nMessage-ID: <t@x>\nReferences: <r@x> <s@x>\n\nagain\n"}) {
    arts.push_back(parse_nntp_article(raw));
  }
  arts[0].number = 100;
  arts[1].number = 101;
  arts[2].number = 103;
  NntpRef ar = ref("nntp://h/comp.test/101");
  Document t = nntp_thread_document(arts, ar);
  EXPECT_EQ(t.meta.title, "Hello");
  EXPECT_EQ(count_kind(t, NodeKind::Message), 3u);
  EXPECT(has_href(t, "#id:101"));  // 103 answers 101
  EXPECT(has_href(t, "nntp://h/comp.test/103#id:103"));
  const std::string tt = all_text(t);
  EXPECT(tt.find("Re: Hello (was: Hi)") != std::string::npos);  // changed subject shown
  EXPECT(tt.find("Ann|") != std::string::npos);

  NntpArticle one = parse_nntp_article(
      "Subject: Re: Q\nFrom: x@y\nMessage-ID: <q2@x>\nReferences: <q1@x>\n"
      "Newsgroups: comp.test\nXref: h comp.test:55\n\nbody\n");
  Document m = nntp_article_document(one, ref("news://h/q2@x"));
  EXPECT(has_href(m, "news://h/comp.test"));
  EXPECT(has_href(m, "news://h/q1@x"));
  EXPECT(has_href(m, "nntp://h/comp.test/55#id:55"));

  std::vector<NntpGroupInfo> few = parse_nntp_active("a.b 10 1 y\na.c 0 1 y\n");
  Document l = nntp_groups_document(few, ref("news://h/a.*"));
  EXPECT_EQ(count_kind(l, NodeKind::Article), 2u);
  EXPECT(has_href(l, "news://h/a.b"));
  EXPECT(has_href(l, "news://h/"));  // breadcrumb to the server

  // Many groups fold into hierarchies.
  std::string active;
  for (int i = 0; i < 400; ++i)
    active += "comp.lang.g" + std::to_string(i) + " 5 1 y\n";
  active += "comp.os.linux 5 1 y\ncomp.misc 5 1 y\nalt.test 1 1 y\n";
  Document f = nntp_groups_document(parse_nntp_active(active), ref("news://h/comp.*"));
  EXPECT(has_href(f, "news://h/comp.lang.*"));
  EXPECT(has_href(f, "news://h/comp.os.linux"));  // a lone group stays a group
  EXPECT(has_href(f, "news://h/comp.misc"));
  EXPECT(!has_href(f, "news://h/alt.test"));
  EXPECT_EQ(count_kind(f, NodeKind::Article), 3u);
  EXPECT(all_text(f).find("400 groups") != std::string::npos);
}

}  // namespace

void test_nntp(int& passed, int& failed) {
  test_refs();
  test_wire();
  test_articles();
  test_documents();
  passed += g_passed;
  failed += g_failed;
}
