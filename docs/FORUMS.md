# Forums (Discourse, Lobsters, 4chan, Stack Exchange, Bluesky, Flarum, LessWrong, Digg)

Read-only, local-only clients for discussion sites with public JSON APIs,
in the same shape as `hn:` and `lemmy:`: listings become a Thread of
`Article` entries (Header = title link, Caption = counts / author / time),
discussions a Thread of `Message` nodes. Digg groups news by event and
provides each post’s original sources. No accounts, no posting, no
cookies. Gated by `XANATOO_FORUMS` (default ON, needs a TLS backend; off in
the minimal port).

Code: `include/xanatoo/forums.hpp` (refs + document builders),
`src/forums.cpp` (refs, shared helpers), `src/forum_<site>.cpp` (pure JSON →
Document mappers), `src/forum_fetch.cpp` (the only network code: API URLs,
`forum_fetch`). Tested offline in `tests/forum_tests.cpp` with cut-down
recorded replies.

## Refs

| Ref | Shows |
|-----|-------|
| `discourse:HOST` | latest topics (`/top`, `/categories`, `/c/SLUG/ID`; `?page=N`) |
| `discourse:HOST/t/SLUG/ID[?page=N]` | topic (also `/t/ID`; `/t/SLUG/ID/N` opens the page with post N) |
| `lobsters:` | hottest stories (`newest`, `active`, `t/TAG`, `~USER/stories`; `?page=N`) |
| `lobsters:s/ID` | story + comments |
| `fourchan:` | board list (work-safe boards first) |
| `fourchan:BOARD/` | board catalog with thumbnails |
| `fourchan:BOARD/thread/NO` | thread |
| `stackexchange:HOST` | recent questions of a site (`/questions/tagged/TAG`; `?page=N`) |
| `stackexchange:HOST/questions/ID[?page=N]` | question, answers by votes, comments |
| `bsky:HANDLE[?cursor=C]` | author feed (handle or `did:plc:…`), profile on top |
| `bsky:HANDLE/post/RKEY` | post thread: parents, the post, replies |
| `bsky:HANDLE/feed/RKEY[?cursor=C]` | custom feed (feed generator), its name and creator on top |
| `bsky:hashtag/TAG[?cursor=C]` | top posts with `#TAG` (TAG percent-encoded) |
| `bsky:search?query=Q[&cursor=C]` | posts found, newest first (search engine `bluesky`, keyword `bs`) |
| `flarum:HOST` | recent discussions (`?page=N`) |
| `flarum:HOST/d/ID[-slug][?page=N]` | discussion |
| `lesswrong:` | frontpage ("magic" sort; `new`, `top`, `curated`, `quicktakes`; `?page=N`) |
| `lesswrong:posts/ID[/slug]` | post + all comments as a reply tree (best first) |
| `lesswrong:users/SLUG` | profile (karma, counts, bio) + posts, newest first |
| `lesswrong:w/SLUG` | tag / wiki page: description + posts by relevance |
| `lesswrong:search?query=Q[&page=N]` | posts found, plus matching tags and users |
| `digg:[TOPIC[/VIEW]]` | Digg feed: `all` (default), `ai`, `tech`, `gaming`, `entertainment`, `world-business`, `science`, `sports`; views `trending` (default), `top`, `best`, `hot`, `newest` (`?cursor=C`) |
| `digg:TOPIC/top/RANGE` | Top over `today` (default), `24h`, `7d`, `30d`, `1y`, `all` |
| `digg:search[/SORT]?query=Q` | stories of the last 90 days; sort `relevance` (default), `score`, `newest` |
| `digg:post/UUID[?page=N]` | story: picture, article, comments, then its sources (20 a page) |
| `digg:TOPIC/ID` | the same story by the short id of its web address (`digg.com/ai/7xbz1s0w`) |
| `digg:post/UUID/comments[/UUID]` | more comments of a story / the replies to one comment |

The 4chan prefix is `fourchan:` because a URI scheme must start with a
letter: `4chan:g/` would be read as a local path.

Discourse and Flarum run on any host, so their web URLs are never guessed;
type the ref. Web pages of the fixed-host sites (lobste.rs,
boards.4chan.org / 4channel.org, stackoverflow.com and the other Stack
Exchange hosts, bsky.app, lesswrong.com: `/posts/ID/slug?commentId=C` →
`#id:C`, `/users/`, `/w/` and `/tag/`, `/quicktakes`, `/search`) map to refs for the "Native view" toggle
(`forum_ref_from_web_url`); `forum_web_url` goes back, `forum_up_ref` is Up
(thread → listing → site).

## Messages

- Header slots as in HN (`types.hpp`, Message): author, date (with
  `datetime`, linking the post's own ref + `#id:N`), reply-to `#id:N`, then
  the score ("3 likes", "9 votes · accepted", "7 likes · 2 replies").
- Indentation (`alt` = parent label) where the site has a reply tree
  (Lobsters, Stack Exchange comments under their post, Bluesky replies);
  4chan builds one from each post's first quotelink to an earlier post of
  the thread (others stay inline links) and emits posts depth-first;
  flat sites (Discourse, Flarum) only link the post answered.
- Links inside posts stay native: same-thread post links become `#id:N`
  (4chan quotelinks, Lobsters `/c/ID`, Flarum mentions, SE answer links),
  other pages of the same site become refs.
- Bluesky facets (UTF-8 byte ranges) become link spans; quoted posts are
  block quotes (BlockQuote is a leaf, one per paragraph), images and video
  posters are thumbnails, link cards a paragraph.

## API notes

- Discourse: `/latest.json` etc. (topic lists count pages from 0),
  `/t/ID.json?page=P`, `/t/ID/N.json` for a post link (the page is worked
  out from `post_stream.stream`, post numbers have gaps).
- Lobsters: the web path + `.json`.
- 4chan: `a.4cdn.org` (`boards.json`, `B/catalog.json`,
  `B/thread/N.json`), images from `i.4cdn.org` (`TIMs.jpg` thumbnails).
  Gone threads (404) point to the board archive.
- Stack Exchange: API 2.3 with `filter=withbody`, three requests per
  question page (question, answers, comments of all shown posts; a failed
  comment request still shows the page). Anonymous quota is 300 requests
  a day per IP.
- Bluesky: the public AppView `public.api.bsky.app` (getAuthorFeed +
  getProfile, getFeed + getFeedGenerator, getPostThread; handles of posts
  and feeds resolve through resolveHandle first; the head request is only
  made on the first page and may fail). Hashtags and search use searchPosts
  (`q=#TAG&tag=TAG&sort=top`; `q=Q&sort=latest`) on `api.bsky.app`: the
  public AppView answers searchPosts with 403. In every list of posts the
  reply count is a "N comments" / "discuss" link to the thread at the
  bottom of the post, not part of its header. Hashtag facets in posts link `bsky:hashtag/…`.
  Personalised feeds (Following, Popular With Friends) need an account.
  The Services menu opens `examples/services/bluesky.md` (feeds, hashtags,
  accounts, checked 2026-11-24).
- Flarum: JSON:API `/api/discussions` and
  `/api/posts?filter[discussion]=ID&sort=number`, 20 per page; only
  `comment` posts are shown (renames, tag changes are left out).

- LessWrong (ForumMagnum): GraphQL `POST /graphql` with literal arguments
  (`json_quote`d), `input: {terms: {view, limit, offset, …}}` as the RSS
  feed uses (posts views `magic`, `new`, `top`, `curated`, `userPosts` +
  `userId`, `tagRelevance` + `tagId`; comments `postCommentsTop` limit 1000,
  `shortformFrontpage`). A post and its comments come in one request (big
  threads ≈ 1 MB, a few seconds); users and tags resolve the slug to an id
  first. 30 per page; the site caps offsets at 2000 (page ≤ 60). Search is
  the site's own Algolia-style `POST /api/search` with three queries
  (`posts`, `tags`, `users` indexes; words are OR-ed, relevance order).
  Unofficial references: the "Secrets of the LessWrong RSS Feed" post and
  Wei Dai's GraphQL gist; the code is ForumMagnum on GitHub. Not shown:
  sequences, the RSS-only views, karma thresholds, comment-only user pages,
  the Alignment Forum / EA Forum (same software, other hosts).
- The `lesswrong` search engine (keyword `lw`) opens
  `lesswrong:search?query=…`.

### Digg

Digg's public MCP server (`https://digg.com/mcp`, Streamable HTTP, JSON-RPC
`tools/call`, answers as JSON or an event stream) is anonymous and
read-only. Used tools: `digg_get_feed`, `digg_search_posts`,
`digg_get_post`, `digg_get_post_comments` and `digg_get_comment_replies`.
What the tools leave out is read from Digg's own web pages: the feed
thumbnails and, for a story, its picture (with credit) and the editorial
article; the story page also maps the short web id to the UUID the tools
take. Pictures come from Digg's image CDN in the size shown (thumbnails
cropped 240×160).

- Feeds and searches page by `nextCursor` ("More stories"); a cursor can
  expire. Without the MCP server, AI and Technology fall back to the
  published `ai-clusters-ranked.json` / `tech/clusters.json`.
- A story page shows the headline, dateline, picture, Digg's summary and
  "In brief", the article, newer / earlier stories of the same event, the
  Digg comments (replies indented; at most 8 reply calls, then "N replies →"
  links) and the sources (X posts with author and handle, web pages with
  title and host). Further source pages are `?page=N` (the tool stops at
  offset 499).
- Digg web addresses of topics and stories open natively; Up goes from
  a story to its topic.
- No votes, posting or login.

Tests: `test_digg` in `tests/forum_tests.cpp` (refs, web addresses,
tool arguments, envelopes, feed / search / story / comment documents,
web page extraction), offline, shaped after live replies (2026-11-24).

## Not done

- public-inbox (lore.kernel.org). Usenet / NNTP is its own client:
  `docs/NNTP.md`.
- Subscriptions for forum listings; search engines for them except
  LessWrong, Digg and Bluesky.
