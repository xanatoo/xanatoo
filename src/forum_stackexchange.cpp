// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Stack Exchange API 2.3 (api.stackexchange.com, keyless: about 300
// requests a day per address). `site` takes the site's domain. Question
// lists become Thread + Article entries; a question page is the question,
// its answers (by votes) and the comments of both as threaded Messages.
// Titles and names arrive HTML-escaped; bodies are HTML.

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <map>

namespace xanatoo {

using namespace forum;

namespace {

std::string se_ref(const ForumRef& base, std::string path, std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::StackExchange;
  r.host = base.host;
  r.path = std::move(path);
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::string owner_name(const Json& post) {
  const Json* o = json_get(post, "owner");
  const std::string name = o ? html_decode_entities(str(*o, "display_name")) : std::string();
  return name;
}

std::string owner_link(const Json& post) {
  const Json* o = json_get(post, "owner");
  return o ? str(*o, "link") : std::string();
}

void add_tags(Document& doc, NodeId cap, const Json& q, const ForumRef& ref) {
  for (const Json& t : arr(q, "tags")) {
    if (t.type != Json::Type::String) continue;
    add_sep(doc, cap);
    add_link(doc, cap, t.s, se_ref(ref, "questions/tagged/" + t.s), InlineRole::Metadata);
  }
}

}  // namespace

Document se_list_document(const Json& reply, const ForumRef& ref) {
  std::string what = "Recent questions";
  if (starts_with(ref.path, "questions/tagged/"))
    what = "Questions tagged " + ref.path.substr(17);
  Document doc;
  NodeId thread = start_document(doc, ref.host + " — " + what, ref.host,
                                 {{"Questions", se_ref(ref, ""), ref.path.empty()}},
                                 "stackexchange");
  const std::vector<Json>& items = arr(reply, "items");
  for (const Json& q : items) {
    const std::string id = std::to_string(num(q, "question_id"));
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = id;
    NodeId head = doc.create(NodeKind::Header, art);
    add_link(doc, head, html_decode_entities(str(q, "title")),
             se_ref(ref, "questions/" + id));
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_meta(doc, cap, count(num(q, "score"), "vote", "votes"));
    add_sep(doc, cap);
    std::string answers = count(num(q, "answer_count"), "answer", "answers");
    if (num(q, "accepted_answer_id") > 0) answers += " ✓";
    add_meta(doc, cap, answers);
    add_sep(doc, cap);
    add_meta(doc, cap, count(num(q, "view_count"), "view", "views"));
    add_tags(doc, cap, q, ref);
    const std::string asked = iso_from_unix(num(q, "creation_date"));
    if (!asked.empty()) {
      add_sep(doc, cap);
      InlineSpan date;
      date.role = InlineRole::Metadata;
      date.text = "asked " + short_time(asked);
      date.datetime = asked;
      doc.get(cap).spans.push_back(std::move(date));
    }
    if (const std::string who = owner_name(q); !who.empty()) {
      add_meta(doc, cap, " by ");
      add_link(doc, cap, who, owner_link(q), InlineRole::Metadata);
    }
  }
  if (items.empty()) add_note(doc, doc.root, "No questions.");
  const Json* more = json_get(reply, "has_more");
  add_pager(doc, ref, more && more->b);
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document se_question_document(const Json& question, const Json& answers,
                              const Json* comments, const ForumRef& ref) {
  const std::vector<Json>& qs = arr(question, "items");
  Document doc;
  if (qs.empty()) {
    start_document(doc, ref.host, ref.host, {{"Questions", se_ref(ref, "")}}, "stackexchange");
    add_note(doc, doc.root, "Question not found.");
    return doc;
  }
  const Json& q = qs[0];
  const std::string qid = std::to_string(num(q, "question_id"));
  const std::string title = html_decode_entities(str(q, "title"));
  NodeId thread = start_document(doc, title, ref.host,
                                 {{"Questions", se_ref(ref, "")}}, "stackexchange");
  // Comments by the post they belong to, oldest first.
  std::map<long long, std::vector<const Json*>> by_post;
  if (comments)
    for (const Json& c : arr(*comments, "items")) by_post[num(c, "post_id")].push_back(&c);
  const std::string page = "questions/" + qid;
  auto add_comments = [&](long long post) {
    for (const Json* c : by_post[post]) {
      MsgHead h;
      h.label = "c" + std::to_string(num(*c, "comment_id"));
      h.author = owner_name(*c);
      h.author_href = owner_link(*c);
      h.iso = iso_from_unix(num(*c, "creation_date"));
      h.reply_to = std::to_string(post);
      h.indent = true;
      if (long long s = num(*c, "score"); s != 0) h.score = count(s, "vote", "votes");
      NodeId msg = add_message(doc, thread, h);
      append_html(doc, msg, str(*c, "body"), "https://" + ref.host + "/", false);
    }
  };
  auto post_head = [&](const Json& p, const std::string& id) {
    MsgHead h;
    h.label = id;
    h.author = owner_name(p);
    h.author_href = owner_link(p);
    h.iso = iso_from_unix(num(p, "creation_date"));
    h.permalink = se_ref(ref, page, "id:" + id);
    h.score = count(num(p, "score"), "vote", "votes");
    return h;
  };

  NodeId qmsg = add_message(doc, thread, post_head(q, qid));
  doc.append_text(doc.create(NodeKind::Header, qmsg), title, InlineKind::Strong);
  append_html(doc, qmsg, str(q, "body"), "https://" + ref.host + "/");
  NodeId cap = doc.create(NodeKind::Caption, qmsg);
  add_meta(doc, cap, count(num(q, "answer_count"), "answer", "answers"));
  add_sep(doc, cap);
  add_meta(doc, cap, count(num(q, "view_count"), "view", "views"));
  add_tags(doc, cap, q, ref);
  add_comments(num(q, "question_id"));

  for (const Json& a : arr(answers, "items")) {
    const std::string aid = std::to_string(num(a, "answer_id"));
    MsgHead h = post_head(a, aid);
    if (json_get(a, "is_accepted") && json_get(a, "is_accepted")->b) h.score += " · accepted";
    NodeId msg = add_message(doc, thread, h);
    append_html(doc, msg, str(a, "body"), "https://" + ref.host + "/");
    add_comments(num(a, "answer_id"));
  }
  // Question links of this site stay native; links to answers here jump.
  map_links(doc, [&](std::string& href) {
    std::string r = forum_ref_from_web_url(href);
    if (r.empty() || !starts_with(r, "stackexchange:" + ref.host + "/")) return;
    const std::string self = se_ref(ref, page);
    if (starts_with(r, self + "#")) r = r.substr(self.size());
    href = r;
  });
  const Json* more = json_get(answers, "has_more");
  add_pager(doc, ref, more && more->b, "More answers");
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
