# Bluesky — places to start

[← Services](../services.md)

[Bluesky](https://bsky.app/) is a microblogging network built on the open
[AT Protocol](https://atproto.com/). Every account is a domain-style handle
(`alice.bsky.social`, or a domain its owner controls, like `nature.com`), and
anyone can publish a *custom feed*: an algorithm or a curated list that picks
posts from the whole network. xanatoo reads it through the public AppView —
no account, read-only.

| Address | Shows |
|---------|-------|
| `bsky:` | the Discover feed (also `https://bsky.app/`) |
| `bsky:HANDLE` | an account's posts and reposts, its profile on top |
| `bsky:HANDLE/replies` · `/media` · `/feeds` | the same with replies, only pictures and video, or the feeds the account publishes |
| `bsky:HANDLE/post/RKEY` | a post with the thread above it and every reply |
| `bsky:HANDLE/feed/RKEY` | a custom feed published by `HANDLE` |
| `bsky:hashtag/TAG` | the top posts tagged `#TAG` |
| `bsky:search?query=WORDS` | posts found, newest first |

Links to `bsky.app` profiles, posts, feeds and hashtags open natively, and so
do the mentions and hashtags inside posts. To search all posts, use the search
keyword `bs` (see [Search](../directory/search.md)) —
[search: “hypertext”](bsky:search?query=hypertext). Replies to a post are
behind its *comments* link. Every account also has an
RSS feed at `https://bsky.app/profile/HANDLE/rss`.

## Feeds

Custom feeds that work without logging in (the personalised ones —
*Following*, *Popular With Friends*, *For You* — need an account). Checked
2026-11-24.

- [Discover](bsky:bsky.app/feed/whats-hot) — Bluesky's trending posts ·
  [What's Hot Classic](bsky:bsky.app/feed/hot-classic) ·
  [Bluesky Team](bsky:bsky.app/feed/bsky-team)
- [📰 News](bsky:aendra.com/feed/verified-news) — headlines from verified news
  organisations, newest first
- [Science](bsky:bossett.social/feed/for-science) — curated posts from working
  scientists
- [Astronomy](bsky:emily.space/feed/astro) — posts by astronomers
- [SciArt](bsky:flyingtrilobite.com/feed/aaadepeaxdboc) — where art meets science
- [What's History](bsky:rauchway.bsky.social/feed/aaaby3zazwhrq) — historians
- [GreenSky](bsky:ketanjoshi.co/feed/aaabkznpn5x2g) — climate and energy
- [Urbanism+](bsky:fema.monster/feed/aaaotfjzjplna) — transit, cycling, cities
- [Game Dev](bsky:trezy.codes/feed/game-dev) — every side of making games
- [BookSky](bsky:clarabelle.xyz/feed/aaabrbjcg4hmk) — reading and books
- [📽️ Film](bsky:francesmeh.reviews/feed/aaaotdzmoni2q) — film lovers
- [What's Cooking](bsky:clarabelle.xyz/feed/aaai43yetqshu) — cooking, baking,
  fermentation
- [Gardening](bsky:eepy.bsky.social/feed/aaao6g552b33o) — houseplants, flowers,
  chickens
- [Birds!](bsky:daryllmarie.bsky.social/feed/aaagllxbcbsje) ·
  [Marine Life!](bsky:daryllmarie.bsky.social/feed/aaacjerk7gwek)
- [Artists: Trending](bsky:bsky.art/feed/art-new) — image posts from artists

## Hashtags

- [#science](bsky:hashtag/science) · [#astronomy](bsky:hashtag/astronomy) ·
  [#space](bsky:hashtag/space) · [#history](bsky:hashtag/history)
- [#linux](bsky:hashtag/linux) · [#opensource](bsky:hashtag/opensource) ·
  [#gamedev](bsky:hashtag/gamedev) · [#pixelart](bsky:hashtag/pixelart) ·
  [#retrocomputing](bsky:hashtag/retrocomputing)
- [#art](bsky:hashtag/art) · [#photography](bsky:hashtag/photography) ·
  [#books](bsky:hashtag/books) · [#birds](bsky:hashtag/birds)
- [#atproto](bsky:hashtag/atproto) — the protocol and the apps built on it

## Accounts

### Bluesky and the AT Protocol

- [@bsky.app](bsky:bsky.app) — the official account: announcements, features
- [@atproto.com](bsky:atproto.com) — protocol developers
- [@safety.bsky.app](bsky:safety.bsky.app) — trust and safety updates
- [@jay.bsky.team](bsky:jay.bsky.team) (founder) ·
  [@pfrazee.com](bsky:pfrazee.com) (CTO) ·
  [@bnewbold.net](bsky:bnewbold.net) (protocol engineer)
- [@danabra.mov](bsky:danabra.mov) — Dan Abramov, essays on how atproto works
- [@mackuba.eu](bsky:mackuba.eu) — Kuba Suder, tools and deep dives on the network

### Built on atproto

- [@leaflet.pub](bsky:leaflet.pub) — blogs and newsletters
- [@whtwnd.com](bsky:whtwnd.com) — WhiteWind, Markdown blogs
- [@frontpage.fyi](bsky:frontpage.fyi) — a federated link aggregator
- [@tangled.org](bsky:tangled.org) — a code forge
- [@smokesignal.events](bsky:smokesignal.events) — events
- [@skyfeed.app](bsky:skyfeed.app) — a client with a feed builder

### Science

- [@nature.com](bsky:nature.com) · [@science.org](bsky:science.org) ·
  [@newscientist.com](bsky:newscientist.com) ·
  [@quantamagazine.org](bsky:quantamagazine.org)
- [@esa.int](bsky:esa.int) — European Space Agency
- [@uahirise.bsky.social](bsky:uahirise.bsky.social) — HiRISE images of Mars

### Technology and the open web

- [@eff.org](bsky:eff.org) · [@archive.org](bsky:archive.org) ·
  [@wikipedia.org](bsky:wikipedia.org) · [@w3.org](bsky:w3.org) ·
  [@mozilla.org](bsky:mozilla.org)
- [@timbl.bsky.social](bsky:timbl.bsky.social) — Tim Berners-Lee
- [@doctorow.pluralistic.net](bsky:doctorow.pluralistic.net) — Cory Doctorow
- [@molly.wiki](bsky:molly.wiki) — Molly White, *Citation Needed*
- [@simonwillison.net](bsky:simonwillison.net) — Simon Willison
- [@kottke.org](bsky:kottke.org) — "fine hypertext products since 1998"
- [@werd.io](bsky:werd.io) — Ben Werdmuller
- [@404media.co](bsky:404media.co) · [@arstechnica.com](bsky:arstechnica.com) ·
  [@theverge.com](bsky:theverge.com) · [@lwndotnet.bsky.social](bsky:lwndotnet.bsky.social)
- [@kde.org](bsky:kde.org) · [@gnome.org](bsky:gnome.org) ·
  [@github.com](bsky:github.com)

### News

- [@apnews.com](bsky:apnews.com) · [@reuters.com](bsky:reuters.com) ·
  [@npr.org](bsky:npr.org) · [@theguardian.com](bsky:theguardian.com) ·
  [@nytimes.com](bsky:nytimes.com) · [@propublica.org](bsky:propublica.org)

### Writers and people

- [@xkcd.com](bsky:xkcd.com) — Randall Munroe
- [@hankgreen.bsky.social](bsky:hankgreen.bsky.social) — Hank Green
- [@neilhimself.neilgaiman.com](bsky:neilhimself.neilgaiman.com) — Neil Gaiman
- [@stephenfry.bsky.social](bsky:stephenfry.bsky.social) — Stephen Fry
- [@georgetakei.bsky.social](bsky:georgetakei.bsky.social) — George Takei
- [@markhamillofficial.bsky.social](bsky:markhamillofficial.bsky.social) — Mark Hamill
- [@craigmod.com](bsky:craigmod.com) — Craig Mod, writer and photographer

## More

- [Social and the Fediverse](../directory/social.md) — Bluesky accounts as RSS
  feeds, and the other networks
- [Bluesky feed directory (goodfeeds.co)](https://goodfeeds.co/) ·
  [atproto apps (atproto.com)](https://atproto.com/)
