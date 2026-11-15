# Network and service clients

Developer notes split out of `AGENTS.md`: shared network helpers and the
read-only service clients. Every client is read-only: never post or log in.

## Shared network helpers

- `include/xanatoo/tls.hpp` — `TlsStream` (connect over a socket fd with SNI, peer certificate SHA-256, write_all / read / pending; owns the fd). Backends `src/tls_openssl.cpp` (desktop default) and `src/tls_mbedtls.cpp` (Android; `-DXANATOO_TLS=mbedtls`). Certificate-chain/hostname verification defaults ON; Gemini explicitly opts out of CA verification and enforces `gemini_tofu.cpp` pins before sending URLs. Never call OpenSSL / Mbed TLS directly elsewhere.
- The R36S ArkOS sysroot uses OpenSSL older than 3.0: fetch peer certificates
  through `peer_certificate_owned` in `tls_openssl.cpp`, which selects the
  available OpenSSL API and requires `X509_free` on the returned reference.
  Keep both old and new APIs building when changing TLS code.
- `TlsStream::peer_certificate_details()` supplies printable subject/issuer,
  UTC validity dates and self-issuance/self-signature status for Gemini trust
  review. OpenSSL checks the actual self-signature; Mbed TLS reports unknown
  until that signature can be verified portably. Certificate names are untrusted
  text: do not parse them as Markdown or conflate matching DNs with a valid
  cryptographic self-signature.
- `include/xanatoo/https.hpp` + `src/https.cpp` — HTTPS GET/POST over `TlsStream` (chunked bodies decoded; `https_get_follow` follows redirects without HTTPS→HTTP downgrade) plus `tcp_connect_host` for Gemini / Nostr (built when any network feature is ON). Keep NIP-11 HTTPS-only/public-only/256 KiB limits independent of ordinary verified HTTPS browsing. See `docs/TLS.md`.
- `include/xanatoo/transfer_meter.hpp` — thread-local byte meter for the loading indicator: fetch workers install one (`TransferMeterScope`), network readers call `meter_bytes` / `meter_expect`; a new raw reader loop should feed it too.
- `include/xanatoo/json_min.hpp` — tiny JSON field/object helpers for read-only API clients (header-only). The `*_field` lookups are flat (first match anywhere): use `json_top_level(obj)` when nested objects repeat a key.

## Service refs (`native.hpp`)

- `hn:` / `reddit:` / `lemmy:` refs select the native Thread + Message
  clients; the part after the prefix mirrors the web path (`hn:item?id=1`,
  `reddit:r/linux/`, `lemmy:lemmy.world/c/technology`). `web_url_for`
  maps a ref back to https for the API clients, which still take web URLs.
- Plain https URLs of those sites open as web pages. Links inside native
  views stay native: `native_rewrite_links` (HN, Reddit) or refs emitted by
  `lemmy.cpp` (its path test is too loose for arbitrary links).

## Hacker News

- Read-only (`src/hackernews.cpp`), gated by `XANATOO_HN` (default ON).
- Lists (`/`, `/news`, `/newest`, `/best`, `/ask`, `/show`, `?p=N`): ranked
  ids from the Firebase API + one Algolia search for titles / scores.
- Items (`/item?id=N`): whole tree in one Algolia `/items/N` request; top
  level re-ordered by the Firebase item's `kids` (HN rank).
- Lists contain semantic `Article` entries: `Header` for the linked story
  title (source hostname is an inline `Metadata` span on the same line),
  `Caption` for rank / points / author / time / discussion link. Item pages
  remain `Message` threads. `styles/hackernews.xss` controls sizes and colours;
  do not add site-specific geometry to the layout engine.
- `hn_append_html` maps comment HTML → Paragraph / BlockQuote (`> …`) /
  CodeBlock with Link / Emphasis spans.
- `hn:user?id=NAME` fetches the official Firebase user record (case-sensitive)
  and presents karma, join time and profile text. Links from author names
  resolve here; `rel=external` marks a deliberately web-only link.
- Message header link slots are semantic annotations on `Message.spans`:
  `0` author, `1` date (plus `datetime`), `2` reply-to `#id:PARENT`.
  Layout reconstructs the visible header and preserves those annotations;
  do not hard-code HN URLs into layout. In item pages, the date opens that
  comment's own `hn:item?id=N` view. Right-clicking a comment offers both
  `hn:item?id=STORY#id:COMMENT` and the web permalink for sharing.
- `InlineSpan::datetime` is a generic ISO 8601 instant also populated from
  HTML `<time datetime>`. `hit_test_timestamp` works independently of links;
  `time.cpp` formats UTC, local and relative hover lines. Keep this semantic
  annotation separate from style-sheet roles.

## Feeds (RSS / Atom)

- `src/feed.cpp`, gated by `XANATOO_FEEDS` (default ON). Small tolerant XML
  reader (names lower-cased, prefixes kept: `content:encoded`), RSS 2.0,
  RSS 1.0 (RDF) and Atom (text / html / xhtml content) → Thread + Message.
- `is_feed_url`: `feed:` prefix (HTML pages autodiscover via
  `<link rel=alternate>`), *.rss / .atom / .rdf / .xml, /feed, /rss,
  /feeds/…, ?feed=rss2. Checked before the other services in dispatch.
- Entry HTML goes through `html_min` (below).

## Wikipedia (`wikipedia.hpp`)

- `src/wikipedia.cpp`, gated by `XANATOO_WIKIPEDIA` (default ON).
  `wikipedia:Title` / `wikipedia:de:Title#frag` fetch the REST Parsoid
  HTML (`/api/rest_v1/page/html/…`, descriptive User-Agent required) and
  map it with `parse_html_page`; `wikipedia_rewrite_links` turns article
  links into `wikipedia:` refs (non-article namespaces stay web links).
- `https://*.wikipedia.org/wiki/…` stays an ordinary web page; the
  explicit "Native view" toggle / `UiCommand::ToggleNative` switches via
  `alternate_view_url` (`native.hpp`) — add new native mappings there,
  never reroute silently.

## Reddit

- Read-only client via public `https://www.reddit.com/…/.json` (OpenSSL HTTPS).
- Gated by `XANATOO_REDDIT` (default ON; needs OpenSSL).
- `reddit:r/<sub>/…` refs (the reddit.com path); https links open as web pages.
- Maps posts/comments to Thread + Message nodes.
- Optional OAuth: `XANATOO_REDDIT_CLIENT_ID` + `XANATOO_REDDIT_CLIENT_SECRET` (script app) when anonymous `.json` returns 403.
- Optional UA override: `XANATOO_REDDIT_USER_AGENT`.
- When a native `reddit:` fetch receives HTTP 403, the error document offers a
  clickable `reddit_rss_url` fallback. It goes through the normal RSS/Atom
  reader, without silently retrying another endpoint. Reddit announced that
  its first-party RSS feeds will end on 2026-11-13.

## Lemmy

- Read-only client via Lemmy **API v3** JSON (`/api/v3/post/list`, `/api/v3/post`).
- Post comments come from `/api/v3/comment/list?post_id=…&type_=All`; without
  `type_=All` anonymous requests get the instance's default listing type
  (often `Local`) and comments from federated users vanish.
- Gated by `XANATOO_LEMMY` (default ON; needs OpenSSL).
- `lemmy:<instance>/c/<community>`, `/u/<person>` and `/post/<id>` refs.
- Maps posts/comments to Thread + Message nodes. Header slots: [0] author
  profile link, [1] date (`datetime` = ISO instant; posts link it to the
  post), then the vote score as a `role=metadata rel="score"` span, which
  the Message layout shows as an extra header field. `summary` is the date
  alone. Posts add an `<h>` (title → post page), the body, the link URL
  and a `<caption>` with community + comments links. `lemmy_post_document`
  builds a post page from JSON without I/O (unit tested).
- **Lemmy Explorer** in Panels is a native, dockable `FloatWindow` with an
  embedded ImGui form (not a separate ImGui window). It offers locally
  discovered Instances, Communities, People and Posts; profile/community/post
  navigation stays native. Search and sort/filter controls are per-panel.
- `lemmy_discovery.cpp` parses successful API v3 JSON responses without I/O;
  `lemmy_directory.cpp` merges bounded, thread-safe observations and persists
  them in `state_dir()/lemmy-directory.txt` (in-memory only under `--no-save`).
  API lists, search results, profile posts and comment authors populate the
  catalogue. Federated peers come from `/api/v3/federated_instances`;
  blocked hosts are omitted, linked/allowed peers retained. There is a small
  built-in seed directory and an Add instance control; no central service or
  account is necessary. Only valid ASCII hostnames are stored.
- API v3 is the existing network client's protocol; Lemmy 1.0 introduced API
  v4 with different endpoints. v4-only servers require a later client update.
- Optional UA override: `XANATOO_LEMMY_USER_AGENT`.

## Nostr

- WSS over `TlsStream` (`tls.hpp`).
- Read-only note/thread fetch gated by `XANATOO_NOSTR` (default ON; needs OpenSSL).
- Relay events and encountered public keys now persist in `nostr.sqlite` via
  `nostr_store.cpp` when SQLite is enabled (no writes under `--no-save`). Events
  are deduplicated by id; distinct source relays, `p` tag/NIP-19 mentioned
  keys, and newest observed kind-0 metadata are indexed. `nostr_verify.cpp`
  reconstructs NIP-01 event IDs; OpenSSL builds additionally verify BIP-340
  Schnorr signatures. Other TLS backends cache **hash-checked, unsigned-status**
  observations only; historic pre-verifier records are preserved as excluded
  `verification=0` rows and require refetch. The UI distinguishes these cases.
  Claimed NIP-05 names are NOT verified. Do not portray the cache as a complete
  or uniformly authenticated Nostr directory. See `docs/NOSTR.md`.
- Two native dockable `FloatWindow` panels are exposed in Panels: **Nostr
  Explorer** searches local events/people and shows observed counts; **Nostr
  Accounts** manages multiple named *watch-only* public keys. They use ImGui
  only as controls within the native client frame. Explorer also has a NIP-29
  Groups tab, with `(relay,group ID)` identity, `kind:39000` metadata and
  `h`-tagged event counts. `nostr:groups@wss://relay` discovers public group
  metadata; clicking a group opens its relay-specific timeline. NIP-11 relay
  master-key binding, membership and private groups remain unimplemented.
  No signing-capable accounts, private-key input or outbound publication exist
  yet; add NIP-46/NIP-49 key management before offering actual identity
  creation or posting.
- Opens `note1` / `nevent1` / `nostr:…` / 64-hex via Ctrl+L → Thread+Message layout.
- `npub1` / `nprofile1` open the author's profile + recent notes. Message
  headers link the author (`nostr:npub1…`) and date (`nostr:note1…`) via
  Link spans on the Message node (`[0]` author, `[1]` date; see `types.hpp`).
- Note text goes through `nostr_append_content`: URLs / `nostr:` refs /
  NIP-08 `#[n]` → links (mentions as `@name`), media labelled `[image]` /
  `[video]` / `[audio]` (NIP-92 `imeta` type, else extension), images also
  as remote Image nodes.

## Gemini

- Gemtext parser is always built (`.gmi` / `.gemini`).
- Network client is gated by `XANATOO_GEMINI` (default ON; needs OpenSSL).
- URL entry is Ctrl+L / `UiCommand::OpenUrlBar` → `navigate_to_url`.
- TOFU without questions: a first host:port certificate is pinned and the
  request sent (`gemini_fetch` reports it in `GeminiPeer::first_contact`).
  Only a **changed** certificate stops the fetch before the URL is sent.
  There are no Enter/Esc prompts (touch has neither): `note_gemini_peer`
  (`app_certs.cpp`) records what each fetch met in `App::gemini_certs`, the
  address bar shows the Gemini icon in the favicon's place (warning variant
  on a change, `UiState::site_identity`), and a click opens the dockable
  Certificates panel (`WindowRole::Certificates`, `CertPanel`): view pins,
  Trust new / Keep old for a change, Forget. Acceptance is compare-and-swap
  against saved/session trust and the reload forces a fresh TLS handshake.
  `--no-save` approvals stay in memory. See `docs/TLS.md` and the pure
  `gemini_tofu_tests.cpp` suite; do not bypass this with another fetch path.

## YouTube

- `src/youtube.cpp`, gated by `XANATOO_YOUTUBE` (default ON; needs TLS).
  Read-only InnerTube client (unofficial JSON API, no key/account) for
  channel / playlist video lists and video comments: `youtube:@handle`,
  `youtube:watch?v=ID`, … Parsers are pure (`json_dom.hpp`) and tested with
  recorded replies; never post or log in. See `docs/YOUTUBE.md`.

## Video sites

- `src/video_sites.cpp`, gated by `XANATOO_VIDEO` (default ON; needs TLS).
  Read-only PeerTube / Odysee / Dailymotion clients over public JSON APIs:
  `peertube:HOST/w/ID`, `odysee:@chan:8/slug:e`, `dailymotion:xID`, plus
  channel and search refs and comments. Parsers are pure and tested; never
  post or log in. See `docs/VIDEO_SITES.md`.

## Internet Archive

- `src/archive.cpp`, no feature switch of its own (needs `XANATOO_HTTPS`).
  Read-only `ia:ID` items and collections over the public `/metadata` and
  `advancedsearch` JSON APIs; `ia_markdown` (pure, tested) writes Markdown that
  the ordinary parser turns into the page. archive.org itself needs
  JavaScript. The `ia` search engine links to `ia:`. See `docs/ARCHIVE.md`.

## Wayback Machine

- `wayback:[TS/]URL` (`wayback.hpp`, pure, `tests/wayback_tests.cpp`) fetches
  `web.archive.org/web/<ts>id_/URL` through `web_fetch`; View → Old Version /
  Live Page is `UiCommand::ToggleArchive`; HTTP error banners link to it
  (`add_http_banner`). See `docs/ARCHIVE.md`.

## Ebooks

- `src/ebooks.cpp`, gated by `XANATOO_EBOOKS` (default ON).
- Standard Ebooks catalogue pages (`/ebooks`, `?query=`, author pages,
  `/subjects/x`) are scraped into a Markdown book list.
- Book URLs (Standard Ebooks book pages, Gutenberg `/ebooks/N`, any `.epub`
  link) download into `~/.cache/xanatoo/downloads/` and open through the
  pandoc EPUB import; the buffer keeps the book URL as its address and the
  derived `content.md` as `DocBuffer::content_path`. Never previewed on hover.
- `examples/books.md` is the curated entry page.

## Directory (`examples/directory.md`)

- A hand-written link directory (categories under `examples/directory/`):
  feeds, native refs (`hn:` / `reddit:` / `lemmy:` / `wikipedia:`) and plain
  pages. Check a new URL with curl before adding it; use the `feed:` prefix
  where `is_feed_url` cannot tell it is a feed. Sub-pages live in the
  subdirectory so the Examples-menu test only lists the front page.
