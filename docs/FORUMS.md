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
| `bsky:HANDLE[?cursor=C]` | author feed (handle or `did:plc:…`) |
| `bsky:HANDLE/post/RKEY` | post thread: parents, the post, replies |
| `flarum:HOST` | recent discussions (`?page=N`) |
| `flarum:HOST/d/ID[-slug][?page=N]` | discussion |
| `lesswrong:` | frontpage ("magic" sort; `new`, `top`, `curated`, `quicktakes`; `?page=N`) |
| `lesswrong:posts/ID[/slug]` | post + all comments as a reply tree (best first) |
| `lesswrong:users/SLUG` | profile (karma, counts, bio) + posts, newest first |
| `lesswrong:w/SLUG` | tag / wiki page: description + posts by relevance |
| `lesswrong:search?query=Q[&page=N]` | posts found, plus matching tags and users |
| `digg:` | trending stories across topics |
| `digg:TOPIC` | AI, technology, gaming, entertainment, world-business, science or sports |
| `digg:TOPIC/top` / `digg:TOPIC/newest` | select a feed view; `trending`, `best`, `hot` are also supported |
| `digg:all/top` | a feed view across all topics |
| `digg:search?query=WORDS` | published Digg stories found by keyword/semantic search |
| `digg:post/UUID` | Digg post summary and its attributed X/web sources (UUID from a feed/search) |

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
- Bluesky: the public AppView `public.api.bsky.app` (getAuthorFeed,
  getPostThread; handles resolve through resolveHandle first).
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

### Digg MCP

The official `https://digg.com/mcp` Streamable HTTP endpoint offers
anonymous, read-only tools. xanatoo currently uses `digg_get_feed`,
`digg_search_posts`, and `digg_get_post`, sending JSON-RPC `tools/call` requests directly
(accepting JSON or SSE), without an API key or browser, and renders the
structured response as native articles. Feed results carry an opaque
`nextCursor` to fetch more stories, which may expire; search covers only
recent published stories, not the whole historic archive. `digg_get_post`
uses a UUID (not the short ID in a Digg web page URL), and its sources
link to the original articles or X posts. The current implementation shows
the first source page only; remaining sources can be read on Digg’s site.

If MCP is unavailable, the documented `ai-clusters-ranked.json` and
`tech/clusters.json` JSON feeds provide a limited fallback for AI and
Technology listings (the latter is **not** rank-ordered). There is no
published public write/authentication interface for making Diggs or
posting comments. Digg web **topic** URLs are convertible to native
refs; post web URLs cannot be converted to an MCP UUID, so they remain
ordinary web pages. See https://digg.com/llms.txt and
https://digg.com/mcp/connect for the published interfaces.

The parser / MCP envelope tests are offline (`xanatoo_digg_tests`), including
full 20-story feeds and a regression for use-after-free when unwrapping an
MCP `result` nested inside its owning JSON object. The regression triggers
AddressSanitizer on the old implementation and passes on the corrected one.
The transport has not yet been live-tested against Digg from this environment.

## Not done

- public-inbox (lore.kernel.org). Usenet / NNTP is its own client:
  `docs/NNTP.md`.
- Subscriptions for forum listings; search engines for them except
  LessWrong and Digg (the existing `bluesky` search engine still links
  bsky.app web pages).
