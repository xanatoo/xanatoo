# Services

Every site and protocol xanatoo reads **natively** — through its API or its
own protocol, not by scraping web pages. The *Services* menu is the quick
way in; this page is the long version, with more entry points and a
directory page for each protocol that runs on many hosts.

None of these clients log in, post or vote: they read public data only.
An entry the build left out (no TLS, no video support, …) still opens its
directory page, but its links say what is missing.

## Reference

- [Wikipedia](wikipedia:Main_Page) — any article: `wikipedia:Title`; another language: `wikipedia:de:Title`, `wikipedia:fr:Title`, …
- [Internet Archive](ia:prelinger) — items and collections as `ia:ID`: [Prelinger films](ia:prelinger) · [Computer Chronicles](ia:computerchronicles) · [NASA](ia:nasa) · [old-time radio](ia:oldtimeradio)
- **Wayback Machine** — `wayback:URL` shows the newest archived copy, `wayback:YYYYMMDD/URL` the one nearest that date (View → *Old Version*). Example: [xanadu.com in 1999](wayback:19990101/http://www.xanadu.com/)
- [Standard Ebooks](https://standardebooks.org/ebooks) and more in [Books and long reads](books.md) — EPUBs open as documents

## Repositories

- [GitHub](github:) · [xanatoo](github:xanatoo/xanatoo) — enter `github:owner/repo`.
- [GitLab](gitlab:) · [GitLab project](gitlab:gitlab-org/gitlab) — enter `gitlab:group/project`, including any subgroups.

Use the service strip to switch between files, issues and pull or merge requests.
Open a file to preview it or choose **Download** to save it. To browse a branch,
tag or commit, append `/tree?ref=REVISION` on GitHub or `/-/tree?ref=REVISION`
on GitLab. Encode a slash in a revision as `%2F`.

## News and forums

- [Hacker News](hn:) · [new](hn:newest) · [Ask HN](hn:ask) · [Show HN](hn:show)
- [Lobsters](lobsters:) · [newest](lobsters:newest) · [tag: programming](lobsters:t/programming)
- [Digg](digg:) · [Technology](digg:tech) · [Science](digg:science) · [AI](digg:ai) · [search: “graphics”](digg:search?query=graphics)
- [LessWrong](lesswrong:) · [search: “forecasting”](lesswrong:search?query=forecasting)
- [Reddit: r/popular](reddit:r/popular/) · [r/programming](reddit:r/programming/) · [r/space](reddit:r/space/) · [r/AskHistorians](reddit:r/AskHistorians/)
- [4chan](fourchan:) — board list, work-safe boards first; [/g/ technology](fourchan:g/) · [/sci/ science](fourchan:sci/)

Forum engines with many sites:

- [**Lemmy instances**](services/lemmy.md) — federated Reddit-like communities: 170 instances. Try [lemmy.world](lemmy:lemmy.world/) · [lemmy.ml/c/linux](lemmy:lemmy.ml/c/linux)
- [**Discourse forums**](services/discourse.md) — 105 project and community forums. Try [NixOS](discourse:discourse.nixos.org) · [Python](discourse:discuss.python.org) · [Discourse Meta](discourse:meta.discourse.org)
- [**Flarum forums**](services/flarum.md) — Try [Flarum Community](flarum:discuss.flarum.org)
- [**Stack Exchange sites**](services/stackexchange.md) — all 183 Q&A sites. Try [Stack Overflow](stackexchange:stackoverflow.com) · [Unix & Linux](stackexchange:unix.stackexchange.com) · [Mathematics](stackexchange:math.stackexchange.com)
- [**Usenet servers**](usenet.md) — newsgroups over NNTP: [Gmane mailing lists](news://news.gmane.io/gmane.*) · [Tildeverse](news://news.tilde.club/)

## Social and chat

- [**Bluesky**](services/bluesky.md) — feeds, hashtags and accounts to start with; any account as `bsky:HANDLE`. Try [Discover](bsky:bsky.app/feed/whats-hot) · [Science](bsky:bossett.social/feed/for-science) · [#astronomy](bsky:hashtag/astronomy) · [@bsky.app](bsky:bsky.app)
- [Nostr: recent notes](nostr:recent) — paste any `npub1…`, `note1…` or `nevent1…`; the **Nostr Explorer** panel follows relays
- [**Matrix homeservers**](services/matrix.md) — public room directories and rooms: [matrix.org](matrix:directory/matrix.org) · [Matrix HQ](matrix:r/matrix-hq:matrix.org)
- Mastodon, Pixelfed and other fediverse accounts publish feeds: see [Social and the Fediverse](directory/social.md)

## Video

- [media.ccc.de](mediaccc:) — the Chaos Computer Club's free archive of
  conference talks, from the Chaos Communication Congress to FOSDEM and
  hundreds of smaller events, every recording free to watch and download:
  [recent talks](mediaccc:) · [all conferences](mediaccc:conferences) ·
  [39C3](mediaccc:c/39c3) · [38C3](mediaccc:c/38c3) · [Camp 2023](mediaccc:c/camp2023) ·
  [GPN 24](mediaccc:c/gpn24) · [FrOSCon 2025](mediaccc:c/froscon2025) ·
  [search: “rust”](mediaccc:search?q=rust) (search keyword `ccc`)
- [**PeerTube instances**](services/peertube.md) — 1,200 federated video servers by language. Try [Blender](peertube:video.blender.org) · [Framatube](peertube:framatube.org) · [TILvids](peertube:tilvids.com) · search everything with [SepiaSearch](peertube:sepiasearch.org/search?search=linux) (keyword `pt`)
- [YouTube](youtube:@GoogleDevelopers) — channels `youtube:@handle`, playlists, videos with comments: [@NASA](youtube:@NASA) · [@Computerphile](youtube:@Computerphile) · [search: “demoscene”](youtube:results?search_query=demoscene) (keyword `yt`)
- [Odysee](odysee:search?s=linux) — LBRY's video site: `odysee:@channel:N`, `odysee:search?s=WORDS`
- [Dailymotion](dailymotion:search?search=documentary) — `dailymotion:user/NAME`, `dailymotion:search?search=WORDS`
- More channels as feeds, podcasts and archive films: [Video and podcasts](directory/media.md); how playback works: [Video](video.md)

Video pages have **▶ Play** (the Media panel) where the build has a decoder,
and *Watch with mpv* links otherwise.

## The small net

Protocols older or simpler than the web, each with its own directory of hosts:

- [**Gemini capsules**](services/gemini.md) — TLS, gemtext, a lively community: [geminiprotocol.net](gemini://geminiprotocol.net/) · [Kennedy search](gemini://kennedy.gemi.dev/)
- [**Gopher holes**](services/gopher.md) — the 1991 menu protocol: [Floodgap](gopher://gopher.floodgap.com/1/) · [Gopherpedia](gopher://gopherpedia.com/1/)
- [**Spartan hosts**](services/spartan.md) — Gemini without TLS: [spartan.mozz.us](spartan://spartan.mozz.us/)
- [**Nex hosts**](services/nex.md) — the tiniest of them: [Nightfall City](nex://nightfall.city/)
- [**Finger hosts**](finger.md) — user `.plan` notes: [tilde.town](finger://tilde.town/) · [plan.cat](finger://reiver@plan.cat)
- [**Usenet servers**](usenet.md) — newsgroups (also listed under forums)

## Directories of HTML sites

Everything that is not its own protocol is an ordinary web page, read in
reader mode, or a feed:

- [Directory](directory.md) — a hand-made link directory by topic: blogs, news, science, tech, culture, games, the old web
- [Feeds](feeds.md) — RSS and Atom; `feed:https://site/` finds any page's feed
- [Books and long reads](books.md) — ebooks, Project Gutenberg, Standard Ebooks
- [Search](directory/search.md) — the location bar's search keywords (`w`, `hn`, `pt`, `ccc`, `yt`, `ia`, …)

## Typing addresses

**Ctrl+L** takes any of the address forms above. Most web links to these
sites (`https://lemmy.world/c/…`, `https://media.ccc.de/v/…`,
`https://stackoverflow.com/questions/…`, a PeerTube `/w/` link, …) open in
the native view by themselves; View → *Web Page* shows the site's own HTML.
