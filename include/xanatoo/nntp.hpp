// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Read-only Usenet / NNTP client (RFC 3977, URIs after RFC 5538). Same
// shape as the forum clients: group lists and a group's threads become a
// Thread of Article entries, a discussion a Thread of Message nodes. No
// posting, no AUTHINFO. See docs/NNTP.md.
//
//   news://HOST[:PORT]/               hierarchies / groups of the server
//   news://HOST/comp.lang.*           groups matching a wildmat
//   news://HOST/GROUP[?page=N]        recent threads of a group
//   nntp://HOST/GROUP/N               article N with its thread (#id:N)
//   news://HOST/MSGID                 one article by Message-ID (no <>)
//   news:GROUP, news:MSGID            the same on the default server
//   nntps://HOST/…, snews://HOST/…    over TLS (port 563)
//
// Parsers and document builders are pure and tested offline; only
// nntp_fetch (nntp_fetch.cpp) touches the network.

#include "xanatoo/service_bar.hpp"
#include "xanatoo/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// Server for refs without a host: news:GROUP / news:MSGID. Gmane / Gwene
// mailing-list groups go to news.gmane.io.
inline constexpr const char* kNntpDefaultServer = "nntp.csiph.com";
inline constexpr const char* kNntpGmaneServer = "news.gmane.io";

enum class NntpView {
  Groups,     // group list / hierarchies (wildmat may be empty)
  Group,      // threads of one group
  Article,    // article N of a group, shown inside its thread
  MessageId,  // one article by Message-ID
};

struct NntpRef {
  NntpView view = NntpView::Groups;
  std::string scheme = "news";  // news, nntp, nntps, snews (as written)
  std::string host;             // empty only while parsing news:X
  int port = 119;
  bool tls = false;             // nntps: / snews:
  std::string group;            // Group / Article
  std::string wildmat;          // Groups ("" = all)
  int64_t article = 0;          // Article
  std::string message_id;       // MessageId, without <>
  int page = 1;                 // ?page=N (Group)
  std::string fragment;         // after '#', kept for the address
};

bool is_nntp_url(std::string_view s);
std::optional<NntpRef> parse_nntp_url(std::string_view s);
std::string nntp_url_string(const NntpRef& r);
// The service strip of a news address (service_bar.hpp).
ServiceBar nntp_service_bar(const NntpRef& r);
// Handy constructors for links (same scheme / host / port as `on`).
std::string nntp_group_url(const NntpRef& on, std::string_view group,
                           int page = 1);
std::string nntp_article_url(const NntpRef& on, std::string_view group,
                             int64_t number);
std::string nntp_msgid_url(const NntpRef& on, std::string_view message_id);
std::string nntp_groups_url(const NntpRef& on, std::string_view wildmat);
// One level up ("" at the server's group list).
std::string nntp_up_url(const NntpRef& r);

// ---- wire formats (pure) ----

// RFC 3977 wildmat (`*`, `?`, `[...]`, `,` alternatives, `!` negation).
bool nntp_wildmat_match(std::string_view pattern, std::string_view text);

// LIST ACTIVE: "group high low status".
struct NntpGroupInfo {
  std::string name;
  int64_t high = 0, low = 0;
  char status = 'y';
  std::string description;  // from LIST NEWSGROUPS
  int64_t count() const { return high >= low ? high - low + 1 : 0; }
};
std::vector<NntpGroupInfo> parse_nntp_active(std::string_view text);
// LIST NEWSGROUPS: "group<TAB>description" → fills matching descriptions.
void apply_nntp_descriptions(std::vector<NntpGroupInfo>& groups,
                             std::string_view text);

// OVER / XOVER line (default overview format).
struct NntpOverview {
  int64_t number = 0;
  std::string subject;      // RFC 2047 decoded
  std::string from;         // RFC 2047 decoded
  std::string date;         // as sent
  std::string message_id;   // without <>
  std::vector<std::string> references;  // without <>, oldest first
  int64_t bytes = 0, lines = 0;
};
std::optional<NntpOverview> parse_nntp_overview_line(std::string_view line);
std::vector<NntpOverview> parse_nntp_overview(std::string_view text);

// RFC 5322 date ("Wed, 18 Nov 2026 08:43:04 +0100") → ISO 8601 UTC
// ("2026-11-18T07:43:04Z"); "" when unreadable.
std::string nntp_date_iso(std::string_view date);
// RFC 2047 encoded words (=?charset?Q|B?…?=) → UTF-8; other text through
// to_utf8 (Latin-1 / CP1252 bytes become UTF-8).
std::string decode_mime_header(std::string_view s);
// "Name <a@b>" / "a@b (Name)" / "a@b" → shown name.
std::string mail_display_name(std::string_view from);
// "Re: Re: x" → "x" (also "AW:", "Sv:", "Re[2]:").
std::string strip_reply_prefix(std::string_view subject);
// "<a@b> <c@d>" → {"a@b", "c@d"}.
std::vector<std::string> parse_message_ids(std::string_view s);

// A whole article (headers + body, dot-unstuffed, CRLF or LF).
struct NntpArticle {
  int64_t number = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // raw, unfolded
  std::string subject, from, date, message_id;  // decoded
  std::vector<std::string> references;
  std::vector<std::string> newsgroups;
  std::string xref;          // "server group:N group:M"
  std::string body;          // UTF-8 text of the first text/plain part
  bool flowed = false;       // format=flowed (RFC 3676)
  bool html = false;         // body is HTML (no text/plain part)
  std::vector<std::string> attachments;  // other MIME parts: "name (type)"
  std::string header(std::string_view name) const;  // first, case-insensitive
};
NntpArticle parse_nntp_article(std::string_view raw);
// Article number in `group` from an Xref header (0 when absent).
int64_t nntp_xref_number(std::string_view xref, std::string_view group);

// Plain-text Usenet body → blocks: paragraphs (line breaks kept, or joined
// when flowed), runs of "> " lines as BlockQuotes, the signature after
// "-- " as a metadata paragraph.
void append_usenet_body(Document& doc, NodeId parent, std::string_view body,
                        bool flowed);

// ---- documents (pure) ----

// Group list: ≤ kNntpMaxListed groups are listed; more are folded into
// sub-hierarchies (comp.* → comp.lang.*, …) when the wildmat allows it.
inline constexpr size_t kNntpMaxListed = 300;
Document nntp_groups_document(std::vector<NntpGroupInfo> groups,
                              const NntpRef& ref);
// Threads of a group from one overview window (newest thread first).
// `first` / `low` tell whether there are older articles (pager).
Document nntp_group_document(const std::vector<NntpOverview>& over,
                             const NntpRef& ref, int64_t first, int64_t low,
                             int64_t total);
// One thread: articles in reply order; `ref.article` is the one asked for.
Document nntp_thread_document(const std::vector<NntpArticle>& articles,
                              const NntpRef& ref);
// A single article fetched by Message-ID.
Document nntp_article_document(const NntpArticle& a, const NntpRef& ref);

// The articles of the thread of `number` within an overview window, in
// window order (root = first References entry, or the article itself).
std::vector<int64_t> nntp_thread_members(const std::vector<NntpOverview>& over,
                                         int64_t number);

#if defined(XANATOO_NNTP) && XANATOO_NNTP
std::optional<Document> nntp_fetch(std::string_view url,
                                   std::string* err = nullptr);
#endif

}  // namespace xanatoo
