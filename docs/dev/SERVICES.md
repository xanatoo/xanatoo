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
- `include/xanatoo/https.hpp` + `src/https.cpp` — HTTPS GET/POST over `TlsStream` (chunked bodies decoded; `Content-Encoding` gzip / deflate undone via `decode_content_encoding` (`inflate.hpp`) even when unrequested — Wayback `id_` captures arrive gzipped despite `Accept-Encoding: identity`; the stream path buffers an encoded body and hands it on decoded at the end; `https_get_follow` follows redirects without HTTPS→HTTP downgrade; such redirects are upgraded via `secure_redirect_target`); `https_get_follow_pooled` keeps connections alive between requests (idle pool per host + TLS settings, 16 max, 20 s, liveness-polled, one fresh retry when a reused connection answers nothing; blocks SIGPIPE in its thread) — the image fetcher uses it; responses end by `HttpResponseFraming` (`http_framing.hpp`: Content-Length / chunked / no-body statuses, unit-tested), which also lets `Connection: close` requests stop reading at the response end plus `tcp_connect_host` for Gemini / Nostr (built when any network feature is ON). Keep NIP-11 HTTPS-only/public-only/256 KiB limits independent of ordinary verified HTTPS browsing. See `docs/TLS.md`.
- `include/xanatoo/transfer_meter.hpp` — thread-local byte meter for the loading indicator: fetch workers install one (`TransferMeterScope`), network readers call `meter_bytes` / `meter_expect`; a new raw reader loop should feed it too. The meter also carries the job's cancel flag (`TransferMeter::cancel` → `FetchShared::abandoned`, set by Stop / Esc / navigating away): every reader loop (HTTP(S) in `https.cpp`, Gemini, Gopher, Spartan, Nex, Finger) checks `transfer_cancelled()` per block and fails with `transfer_cancelled_error()`, so a new reader loop must check it too.
- `include/xanatoo/json_min.hpp` — tiny JSON field/object helpers for read-only API clients (header-only). The `*_field` lookups are flat (first match anywhere): use `json_top_level(obj)` when nested objects repeat a key.

## Social markup (mentions, hashtags)

Format: `docs/XML.md` "Social markup". Sources of `rel="mention"` /
`rel="tag"` links: Bluesky facets (`add_rich_text`, `forum_bluesky.cpp`),
Nostr `NoteLinker::mention` (hashtags stay plain: no tag-feed ref yet),
fediverse / Mastodon HTML in feeds and web pages (`html_social_rel` in
`html_min.cpp`: `rel="tag"`, `class="hashtag"`, `class="mention"`; other
rel values are dropped), Reddit `reddit_mark_users`. Context menu:
`App::context_link_rel` / `context_link_text` → Copy Handle / Copy Hashtag.

## Service refs (`native.hpp`)

- `hn:` / `reddit:` / `lemmy:` / `digg:` refs select the native Thread + Message
  clients; the part after the prefix mirrors the web path (`hn:item?id=1`,
  `reddit:r/linux/`, `lemmy:lemmy.world/c/technology`). `web_url_for`
  maps a ref back to https for the API clients, which still take web URLs.
- "Copy Web Page URL" (page context menu, Edit menu; `CopyPageWebUrl`)
  copies `pane_web_url`: `page_web_url` (`web_url_for` plus `feed:`,
  `wayback:`, `sitemap:`) or the result page of a `search:`. Absent
  (context menu) / greyed (Edit) for http(s) pages and addresses without a
  web form (gemini:, nostr:, …, JSON search engines). A new service that
  has web pages goes into `web_url_for`.
- Every importer titles its Thread with `set_thread_title` (a `Header` as
  the thread's first child, `thread_title` finds it), never `summary`: the
  title is drawn in the thread band and the Heading Outline lists it with
  the messages' own headings (post titles) one level below. Markdown
  `subject=` / S-expression `:subject` are read as that title and saved as
  a `heading`; a bare `summary` is only a layout fallback.
- Plain https URLs of those sites open as web pages. Links inside native
  views stay native: `native_rewrite_links` (HN, Reddit) or refs emitted by
  `lemmy.cpp` (its path test is too loose for arbitrary links).

## Service strips (`service_bar.hpp`)

- Generated service pages carry no navigation rows ("Hacker News: Top ·
  New · …", forum sort tabs, Digg topic / sort / range rows, Usenet
  hierarchy breadcrumbs): the service's sections are on its **service
  strip** (docs/UI.md "Service strips"), computed from the address only.
- A new native service adds a `*_service_bar(ref)` (id, name, home, accent
  colour, rows; mark the page's section `current`) and a branch in
  `ref_bar()` (`service_bar.cpp`), keyed on the native ref form only:
  web URLs reach it through `native_url_for`, and `service_bar()` maps the
  links back to the page's form (`web_url_for` / `native_url_for`). Forums add a
  `forum::<site>_service_rows` and a case in `forum_service_bar`
  (forums.cpp). Anything that needs the fetched data (a post's topic, a
  message's group) stays in the page as an ordinary link.
- `start_document(doc, title, thread_label)` (forum_common.hpp) no longer
  takes nav items.

## Hacker News

- Read-only (`src/hackernews.cpp`), gated by `XANATOO_HN` (default ON).
- Lists (`/`, `/news`, `/newest`, `/best`, `/ask`, `/show`, `?p=N`): ranked
  ids from the Firebase API + one Algolia search for titles / scores.
- Items (`/item?id=N`): whole tree in one Algolia `/items/N` request; top
  level re-ordered by the Firebase item's `kids` (HN rank).
- Lists contain semantic `Article` entries: `Header` for the linked story
  title (source hostname is an inline `Metadata` span on the same line),
  `Metadata` for rank / points / author / time / discussion link. Item pages
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

## Quote links (`quote_links.hpp`)

- `find_quote_links(doc)` links quotes in `Message` bodies (`BlockQuote`
  nodes, `>` lines in raw Markdown text) to the message they came from.
  Pure, no SDL; derived data, never stored. Design: `docs/QUOTES.md`.
- `append_foreign_markdown(doc, msg, md, base)` (`markdown_body.hpp`) is how
  Markdown written by others becomes blocks (Reddit and Lemmy bodies): it
  parses with `parse_markup` and keeps only plain Markdown structure
  (`:::` blocks unwrapped, `:::include` literal, no pages, no math, no
  labels, links resolved against the site, no `file:` targets). Use it for
  any new service that delivers Markdown.

## Feeds (RSS / Atom)

- `src/feed.cpp`, gated by `XANATOO_FEEDS` (default ON). Small tolerant XML
  reader (names lower-cased, prefixes kept: `content:encoded`), RSS 2.0,
  RSS 1.0 (RDF) and Atom (text / html / xhtml content). A feed with any
  titled entry is a list of articles: `Section` (label `feed`, `Header` =
  feed title linked to the site) of `Article`s (label `e1`…, `Header` =
  title linked to the entry, `(untitled)` when missing; `Metadata` = author
  · date), like the HN / StackExchange lists, so the Heading Outline lists
  the entries. The entry's own `<h1>`… are demoted so the shallowest is
  level 3 (`demote_headings`). An entry with a summary (RSS `description`
  next to `content:encoded`, Atom `summary` next to `content`) of at least
  20 characters and a full text at least 200 longer (`summarizes`) is a
  collapsed two-body `Stretch`: the summary has its lead picture (else the
  `media:thumbnail`) floated left and loses the "…read more" link and
  WordPress' "The post … appeared first on …" (`trim_summary`). Feeds without any titles (micro-blogs) stay
  a `Thread` of `Message` cards with a Permalink. RFC 4685 threading
  (`thr:in-reply-to`) is not read.
- `is_feed_url`: `feed:` prefix (HTML pages autodiscover via
  `<link rel=alternate>`), *.rss / .atom / .rdf / .xml, /feed, /rss,
  /feeds/…, ?feed=rss2. Checked before the other services in dispatch.
- URL-bar feed button: `feed_links` reads the `<link rel=alternate>`
  RSS / Atom feeds from the `<head>` of the focused web page's raw source;
  `focused_page_info` (app_sitemap.cpp, `App::page_info`, see CONTENT.md
  "Page links") caches them. One feed opens on click, several in a
  menu (`UiCommand::OpenPageFeed`, `feed:` prefixed when `is_feed_url`
  cannot tell). No icon on local pages, native views or feeds themselves;
  no guessing of `/feed.xml` (pages without the link get no icon).
- Entry HTML goes through `html_min` (below). An entry shows one body:
  `content:encoded` / `content`, else `description` / `summary` (never both).
- `media:thumbnail` is an image floated left before the body, left out when
  the body shows the same file in another size (`image_identity` ignores
  scheme, query and WordPress' `-800x445`).

## Wikipedia (`wikipedia.hpp`)

- `src/wikipedia.cpp`, gated by `XANATOO_WIKIPEDIA` (default ON).
  `wikipedia:Title` / `wikipedia:de:Title#frag` fetch the REST Parsoid
  HTML (`/api/rest_v1/page/html/…`, descriptive User-Agent required) and
  map it with `parse_html_page`; `wikipedia_rewrite_links` turns article
  links into `wikipedia:` refs (non-article namespaces stay web links).
  Citations become footnotes in `parse_html_page` (`html_footnotes.hpp`,
  docs/dev/CONTENT.md "Web pages"). It passes `host_line = false`: no
  "host · Open in browser · Open as native view" line on top; the URL bar
  has the Native toggle, and "Open in Browser" (`UiCommand::OpenInBrowser`,
  `pane_browser_url`) is in the URL-bar and page context menus.
- The URL bar's site-search button is hidden while the focused page's
  service strip has a search field for the same engine (Wikipedia, HN, …).
- `https://*.wikipedia.org/wiki/…` stays an ordinary web page; the
  explicit "Native view" toggle / `UiCommand::ToggleNative` switches via
  `alternate_view_url` (`native.hpp`) — add new native mappings there,
  never reroute silently.

## Reddit

- Read-only client via public `https://www.reddit.com/…/.json` (OpenSSL HTTPS).
- Gated by `XANATOO_REDDIT` (default ON; needs OpenSSL).
- `reddit:r/<sub>/…` refs (the reddit.com path); https links open as web pages.
- Maps posts/comments to Thread + Message nodes; bodies (selftext / comment
  Markdown) go through `append_foreign_markdown` (`markdown_body.hpp`).
- `reddit_mark_users` (pure, outside `XANATOO_REDDIT`, tested): user-page
  links and `u/NAME` / `/u/NAME` in post and comment bodies become
  `rel="mention"` links (not in code blocks). `r/sub` is not linked yet.
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
- Native view tries service-specific URL mappings before the Lemmy path
  heuristic, so a Bluesky post URL opens as `bsky:`.
- `lemmy:<instance>/` is the instance's post listing (the explicit ref only;
  `is_lemmy_url` keeps rejecting bare site roots). Lemmy UI's
  `?listingType=All|Local|Subscribed` becomes the API's `type_`;
  `lemmy:lemmy.world/?listingType=All` is the federated feed (Lemmy's
  `/r/all`) behind the Services menu entry.
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
- **PieFed** (`piefed.social`, …) has no `/api/v3`; it serves a
  Lemmy-shaped API under `/api/alpha/` (`post/list`, `post`, `comment/list`,
  `user`, `community/list`, `federated_instances`). `lemmy_fetch` retries a
  404 at `lemmy_alpha_api_url` and then fetches comments under the same
  prefix. Field aliases: post `title` (Lemmy `name`), creator `user_name`
  plus display `title` (Lemmy `name` / `display_name`), comment `body`
  (Lemmy `content`); `parse_post_view` / `parse_comment_view` and
  `lemmy_discovery.cpp` read either spelling. Other hard-coded `/api/v3`
  callers rely on the 404 fallback. (`piefed.diffrint.org` is a Lemmy
  server despite its name.)
- Optional UA override: `XANATOO_LEMMY_USER_AGENT`.

## Matrix

- Read-only Client-Server API client (`src/matrix.cpp`), gated by
  `XANATOO_MATRIX` (default ON; HTTPS; also in the Wasm build, the API
  requires CORS). Never registers, logs in, joins rooms or sends events.
- Addresses are the spec's `matrix:` URIs (sigil dropped):
  `matrix:r/room:server`, `matrix:roomid/id[:server]?via=s`,
  `matrix:roomid/id/e/event`, `matrix:u/user:server`. Extension:
  `matrix:directory/server[?since=…][&q=…]` (public room directory;
  `matrix:directory/` = configured / matrix.org); rooms take `from=` for
  "Earlier messages". `parse_matrix_ref` / `format_matrix_ref` round-trip;
  `https://matrix.to/#/…` links map both ways (`alternate_view_url`,
  and links inside Matrix documents are rewritten to refs, except
  `rel=external`).
- Server names (`matrix_server_name`) are validated before they enter a
  URL; the API base comes from `/.well-known/matrix/client` (cached,
  https-only base_url), else `https://server`.
- Anonymous (what servers allow without a token, checked against
  matrix.org): `GET /v3/publicRooms` of the server itself (no `server=`,
  no search), `/v3/directory/room/{alias}`, `/v1/room_summary/{room}`
  (fallback `im.nheko.summary`), `/v3/profile/{user}`. Room messages
  (`/messages`, `/context`), space contents (`/hierarchy`) and directory
  search / other servers' directories (`POST /publicRooms?server=`) need
  an access token: `CatalogKey::MatrixHomeserver` + `MatrixAccessToken`
  (Credentials dialog, or `XANATOO_MATRIX_HOMESERVER` /
  `XANATOO_MATRIX_ACCESS_TOKEN`). Then every request goes to that
  homeserver and the token is only ever sent to its base URL. Reading a
  room without joining works for `world_readable` history; otherwise a
  Note explains it (no join). Guest accounts are deliberately not used
  (registration is an account write).
- Documents (pure, unit tested): `matrix_directory_document` (Article
  entries: Header link, first topic line, Caption with alias / members),
  `matrix_room_document` (heading, Caption, topic, space children,
  "Messages" Thread: `/messages` arrives newest first and is reversed;
  edits (`m.replace`) replace the shown text, reply fallbacks are stripped,
  `m.in_reply_to` / `m.thread` → `alt` threading, `formatted_body` through
  `html_append_blocks`, media and encrypted events as metadata captions,
  member displaynames from `state`), `matrix_profile_document`.
- **Matrix Explorer** (Panels): dockable `FloatWindow` like the Lemmy
  Explorer. `matrix_discover_payload` (pure) collects servers, rooms,
  spaces and people from responses; `matrix_directory.cpp` keeps them in
  `state_dir()/matrix-directory.txt` (memory only under `--no-save`), with
  a few seed servers. The Open field takes `#room:server`, `@user:server`,
  `!id`, matrix.to links, `matrix:` refs or a server name (added).

## Nostr

- Services → **Nostr** opens the bundled `examples/services/nostr.md` directory,
  also linked from All services. It includes native links to recent notes,
  profiles and public relay groups alongside web links to ecosystem projects.

- WSS over `TlsStream` (`tls.hpp`).
- NIP-19 `naddr` references route to Nostr and display an unsupported-address
  error. Fetching addressable events is not implemented.
- Note/thread fetch gated by `XANATOO_NOSTR` (default ON; needs TLS).
- secp256k1 backend (`nostr_verify.hpp`, `XANATOO_NOSTR_CRYPTO`):
  `XANATOO_NOSTR_VERIFY_OPENSSL` (OpenSSL libcrypto; with
  `XANATOO_TLS=openssl`) or `XANATOO_NOSTR_SECP256K1` (libsecp256k1 for
  BIP-340, Mbed TLS for ChaCha20-Poly1305 / PBKDF2, scrypt's ROMix in
  `nostr_sign.cpp`, `/dev/urandom`; with `XANATOO_TLS=mbedtls` when
  pkg-config finds `libsecp256k1`, and on Android via
  `mk/android/secp256k1/Android.mk`). Neither: hash-only, no signing.
  `xanatoo_nostr_secp256k1_tests` runs the signing / NIP-49 vectors against
  the libsecp256k1 backend whenever it is installed (the dev shell has it).
- Relay events and encountered public keys now persist in `nostr.sqlite` via
  `nostr_store.cpp` when SQLite is enabled (no writes under `--no-save`). Events
  are deduplicated by id; distinct source relays, `p` tag/NIP-19 mentioned
  keys, and newest observed kind-0 metadata are indexed. `nostr_verify.cpp`
  reconstructs NIP-01 event IDs; builds with a secp256k1 backend additionally
  verify BIP-340 Schnorr signatures. Builds without one cache **hash-checked, unsigned-status**
  observations only; historic pre-verifier records are preserved as excluded
  `verification=0` rows and require refetch. The UI distinguishes these cases.
  Claimed NIP-05 names are NOT verified. Do not portray the cache as a complete
  or uniformly authenticated Nostr directory. See `docs/NOSTR.md`.
- Panels → **Nostr Explorer** (native dockable `FloatWindow`, ImGui only for
  the controls inside the native frame) searches local events/people and shows
  observed counts. Named public keys (`local_identities` in
  `nostr.sqlite`) live on the **Nostr tab of the Identities panel**
  (`app_identity.cpp`; Services → Nostr Accounts opens that tab): save / rename
  (also "Save this author" on an npub page), make active, view profile, copy
  npub, remove (`nostr_remove_local_identity`, only the local label).
  Signing keys: `nostr_keys.cpp` (`state_dir()/nostr-keys`, 0600, NIP-49
  ncryptsec, or a plain `(nsec …)` for a password-less key:
  `nostr_key_plain`; `refresh_nostr_identities` opens those into
  `nostr_unlocked` and wipes the nsec from `App::nostr_keys`;
  `NostrSetKeyPassword` sets / changes / removes a password), crypto in `nostr_sign.cpp` (either backend: BIP-340 sign,
  nsec / ncryptsec, scrypt + XChaCha20-Poly1305, `nostr_sign_event`),
  Bech32 shared in `bech32.cpp`. `App::nostr_unlocked` holds secrets for
  the session only (wiped on lock). `nostr_publish` (nostr.cpp) sends
  `["EVENT",…]` to `nostr_publish_relays()` in parallel and collects `OK`;
  the panel posts on a detached worker (`NostrPostJob`, polled by
  `update_nostr_post`). Reply panel: `send_reply` (app_identity.cpp) runs
  `nostr_reply` (parent from `nostr_cached_event_json` or relays by id,
  `nostr_reply_plan`: NIP-10 kind 1 / NIP-C7 kind 9 / NIP-22 kind 1111;
  group replies only to the group relay; sign, publish) on `App::nostr_reply`; success
  refetches the page. Explorer also has a NIP-29
  Groups tab, with `(relay,group ID)` identity, `kind:39000` metadata and
  `h`-tagged event counts. `nostr:groups@wss://relay` discovers public group
  metadata; clicking a group opens its relay-specific timeline. NIP-11 relay
  master-key binding, membership and private groups remain unimplemented.
  Posting: kind-1 notes, replies (kind 1 / 9 / 1111); NIP-42 AUTH, NIP-46, NIP-65 open.
  `shown_kind` (1, 9, 11, 1111) decides which events become messages.
- Opens `note1` / `nevent1` / `nostr:…` / 64-hex via Ctrl+L → Thread+Message layout.
- `npub1` / `nprofile1` open the author's profile + recent notes. NIP-19 decoding
  accepts up to 5,000 Bech32 characters, including relay hints. Message
  headers link the author (`nostr:npub1…`) and date (`nostr:note1…`) via
  Link spans on the Message node (`[0]` author, `[1]` date; see `types.hpp`).
- Note text goes through `nostr_append_content`: URLs / `nostr:` refs /
  NIP-08 `#[n]` → links (mentions as `@name`). Profile mentions retain the
  original `nprofile1` target and its relay hints. Media is labelled `[image]` /
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
- Identities (client certificates, `gemini_identity.hpp/.cpp`, panel
  `app_identity.cpp`, `WindowRole::Identities`, `IdentityPanel`; the panel's
  Gemini tab, beside the Nostr tab): a server
  knows the client by the certificate's fingerprint, so an identity is an
  account and its key the password. Store `state_dir()/gemini-identities`
  (S-expression with PEM cert + key, written 0600 via temp file + rename;
  `--no-save` keeps changes in a per-process session overlay). Each identity
  has URL-prefix scopes (`gemini://host[:port]/path`, segment-boundary match,
  longest wins; one identity per scope, which is how switching works).
  `gemini_fetch_once` looks the identity up **per request** (so a redirect
  out of the scope goes without it) and sets `TlsStream::set_client_identity`
  plus a `set_peer_guard` that runs the TOFU check inside the handshake:
  a server whose pin changed never receives the certificate (the fetch
  reconnects without it and reports the change as usual). Status 60/61/62
  set `FetchShared::identity_status`; `load_error_doc` explains it and
  `note_gemini_identity` opens the panel on that page (Create for this
  site / Use on this site / page; no Enter/Esc prompts). Certificates are
  made by the TLS backend (`tls_make_client_identity`: EC P-256, CN = name,
  valid to 9999). Tests: `gemini_identity_tests.cpp` (scopes, format,
  store), `tls_peer_tests.cpp` (the server sees the fingerprint; a refused
  guard sends nothing). Server certificates stay in the Certificates panel.

## Gopher (`gopher.hpp`) and Finger (`finger.hpp`)

- `src/gopher.cpp` / `src/finger.cpp`, gated by `XANATOO_GOPHER` /
  `XANATOO_FINGER` (default ON; plain TCP). Pure parsers are tested in
  `tests/smallnet_url_tests.cpp` (network compiled out).
- URLs follow RFC 4266: `gopher://host[:port]/<type><selector>`, the
  selector percent-encoded in the URL and decoded on the wire. Only type 7
  (search) takes a `?query` (or `%09query`); for other types `?` is part of
  the selector. Type 7 without a query asks for input before connecting.
- Menus become Markdown: each run of info lines (`i`, `3`) is one
  preformatted block (ASCII art keeps its spacing), items are list links
  built by `gopher_item_url`; `h` items with a `URL:<url>` selector link to
  that URL. Link labels are verbatim up to the first `](` in `parse.cpp`,
  so only that sequence is broken up.
- Relative references: `/<type><selector>` and `?query` per the URL form,
  anything else relative to the selector's directory with the same type.
- Transfers: `gopher_fetch` takes `GopherReadHooks`; `fetch_gopher`
  (`app_fetch.cpp`) returns `Cancel` on `DownloadProgress::cancel`
  (Downloads panel); Stop / Esc goes through `transfer_cancelled()` like
  every reader. A Download streams binary items straight into
  the `DownloadFile` (phase Active, live `received`, no 32 MiB cap).
  Navigate / Tab sniff the first 512 bytes: images are read in full and
  shown, any other binary stops there (`Enough`) and is offered, so a
  confirmed download fetches the item only once.
- Finger replies are one code block; `markdown_code_fence`
  (`text_escape.hpp`) picks a fence longer than any backtick run inside.

## Spartan (`spartan.hpp`)

- `src/spartan.cpp`, gated by `XANATOO_SPARTAN` (default ON; plain TCP,
  port 300). `fetch_spartan` in `app_fetch.cpp`; URL tests in
  `tests/smallnet_url_tests.cpp`.
- `=:` input lines become links ending in an empty `?`
  (`spartan_rewrite_prompts`); `parse_spartan_url` marks them
  `SpartanUrl::prompt`, and `fetch_spartan` answers with an input request
  (`input_status` 10, the Gemini / Gopher-search flow) without contacting
  the server. The typed query is percent-decoded (`SpartanUrl::data`) and
  sent as the request's data block.
- `text/gemini` (or no type) is gemtext, `text/plain` is plain text.

## YouTube

- `src/youtube.cpp`, gated by `XANATOO_YOUTUBE` (default ON; needs TLS).
  Read-only InnerTube client (unofficial JSON API, no key/account) for
  channel / playlist video lists and video comments: `youtube:@handle`,
  `youtube:watch?v=ID`, … Parsers are pure (`json_dom.hpp`) and tested with
  recorded replies; never post or log in. See `docs/YOUTUBE.md`.

## Video sites

- `src/video_sites.cpp`, gated by `XANATOO_VIDEO` (default ON; needs TLS).
  Read-only PeerTube / Odysee / Dailymotion / media.ccc.de clients over
  public JSON APIs: `peertube:HOST/w/ID`, `odysee:@chan:8/slug:e`,
  `dailymotion:xID`, `mediaccc:c/38c3` / `mediaccc:v/SLUG`, plus
  channel and search refs and comments. Parsers are pure and tested; never
  post or log in. See `docs/VIDEO_SITES.md`.

## Usenet / NNTP (`nntp.hpp`)

- `src/nntp.cpp` (pure: refs, wildmat, LIST / OVER lines, RFC 5322 dates,
  RFC 2047 headers, MIME bodies, documents) + `src/nntp_fetch.cpp`
  (network), gated by `XANATOO_NNTP` (default ON, plain TCP like Gopher;
  `nntps:` / `snews:` need TLS and verify certificates). Offline tests in
  `tests/nntp_tests.cpp`. Builds documents with the forum helpers
  (`forum_common.hpp`). See `docs/NNTP.md`.
- One connection per page: greeting, `MODE READER`, the reader commands,
  `QUIT`. Never AUTHINFO or POST; 480 becomes "needs a login to read".
- Refs keep RFC 5538's shape; links into threads are
  `nntp://host/group/N#id:N` so the page lands on the article. Message-IDs
  are percent-encoded with `/` escaped (no `//` reaches the locator).
- `up_url` asks `nntp_up_url` before the generic `://` rule (article →
  group → hierarchy → server).

## Forums (`forums.hpp`)

- `src/forums.cpp` + `src/forum_<site>.cpp` (pure mappers) +
  `src/forum_fetch.cpp` (network), gated by `XANATOO_FORUMS` (default ON;
  needs TLS). Read-only Discourse / Lobsters / 4chan / Stack Exchange /
  Bluesky / Flarum / LessWrong: `discourse:HOST/t/…`, `lobsters:s/ID`,
  `fourchan:B/thread/N`, `stackexchange:HOST/questions/ID`,
  `bsky:HANDLE/post/RKEY` (also `/feed/RKEY`, `bsky:hashtag/TAG`; bare `bsky:` parses as the Discover feed `bsky.app/feed/whats-hot`), `flarum:HOST/d/ID`, `lesswrong:posts/ID`.
  Subscriptions use `forum_has_post_listing` to recognize feeds and topic
  listings. See `docs/FORUMS.md`.
- LessWrong asks GraphQL by POST (`lw_query` in `forum_fetch.cpp`, a
  GraphQL `errors` reply without `data` becomes the error text); search
  is `lesswrong:search?query=Q` (`ForumRef::query`, decoded) over the
  site's `/api/search`, also the `lw` search engine.
- Shared builders in `src/forum_common.hpp` (`add_message` fills the Message
  header slots, `start_document`, `add_pager`, `map_links`); new sites add
  a mapper file, a `kSites` row and a `forum_fetch` case.
- Ref schemes must start with a letter (`looks_like_local_path`): hence
  `fourchan:`, not `4chan:`.
- A thumbnail with a full-size original (4chan `s.jpg`, Bluesky `thumb` /
  `fullsize`) is an Image node with an empty Link span to the original, as
  `html_min` does for `<a><img>`: a click and the `//gallery` show the
  original. Hovering such a block image sets `hover.href` to that link too,
  so the Context panel and status line name the original. Bluesky's `…@jpeg` CDN names pass `is_image_url`.

## GitHub and GitLab (`forge.hpp`)

- `github:owner/repo` and `gitlab:group[/subgroup]/project` open the
  default-branch file tree. GitLab uses gitlab.com. Routes mirror the web
  sections (`issues`, GitHub `pulls` / `pull/N`, GitLab
  `-/merge_requests[/N]`). Native file routes keep the revision in
  `?ref=`, so slash-containing branches can be percent-encoded without
  ambiguity. The path after `tree`, `blob` or `raw` is the file path.
- `src/forge.cpp` uses public APIs through the shared HTTPS
  transport. The service strip switches between files, issues and change
  requests. Lists and discussions page through results on demand. GitHub
  pull requests have separate conversation, review and inline comment views.
  GitLab uses anonymous GraphQL for discussions and retains reply relationships. Descriptions and comments
  use Thread + Message markup, with GitHub's rendered HTML or ordinary
  Markdown blocks from GitLab.
- The built-in GitHub and GitLab search engines put a project search field
  in each service strip. Their `search?q=` refs page through GitHub's
  repository search and GitLab's public project list. Result links open
  the native file tree for the selected project.
- About renders the default-branch README and shows license metadata and a
  link to the license file when the host exposes it. GitHub supplies README
  HTML. GitLab supplies the README URL and license via project metadata,
  then the client renders the raw README.
- Releases show formatted notes, source archives and asset links. GitHub
  uses REST and GitLab uses anonymous GraphQL with cursors. Tags use the
  REST tag lists. Release and tag headings open the corresponding file tree.
- File previews show up to 256 KiB of UTF-8 content. Raw refs offer the
  standard download confirmation and stream the file after confirmation.
  GitHub's Contents API caps a directory listing at 1,000 entries. Public
  visibility and anonymous API rate limits apply to both services.

## Git file downloads (`git:`)

- `parse_git_ref` in `forge.cpp` maps `git:github.com/OWNER/REPO?path=PATH&ref=REVISION`
  and `git:gitlab.com/GROUP/PROJECT?path=PATH&ref=REVISION` to the same raw
  endpoints as the forge browsers. An omitted revision uses `HEAD`.
- GitHub and GitLab addresses also accept `?blob=SHA` with a full SHA-1.
  The SHA is the download filename. GitHub uses the Git blobs endpoint
  with the raw Accept media type. GitLab uses its raw blob endpoint.
- `git:radicle/SEED/rad:RID?path=PATH&ref=COMMIT` uses the HTTPS seed's
  `/raw/` endpoint and requires a full commit SHA.
- Fetch routing offers the standard download prompt and streams the raw
  response after confirmation. No local Git installation is required.
  A generic Git transport would need server-side object filtering and
  access to the requested revision and blob.

## Internet Archive

- `src/archive.cpp`, no feature switch of its own (needs `XANATOO_HTTPS`).
  Read-only `ia:ID` items and collections over the public `/metadata` and
  `advancedsearch` JSON APIs; `ia_markdown` (pure, tested) writes Markdown that
  the ordinary parser turns into the page. archive.org itself needs
  JavaScript. The HTML description is put into the parsed page afterwards
  (`ia_insert_description`). The `ia` search engine links to `ia:`. See
  `docs/ARCHIVE.md`.

## Wayback Machine

- `wayback:[TS/]URL` (`wayback.hpp`, pure, `tests/wayback_tests.cpp`) fetches
  `web.archive.org/web/<ts>id_/URL` through `web_fetch`; View → Old Version /
  Live Page is `UiCommand::ToggleArchive`; HTTP error banners link to it
  (`add_http_banner`). See `docs/ARCHIVE.md`.

## Similar websites (`related_sites.hpp`)

Related Sites panel source. `related_domains` drops `www.` and falls
back from a subdomain to its parent (`en.wikipedia.org` → `wikipedia.org`,
never to `co.uk`); the first domain with any result wins.

- **similarsites.com** (no key, always asked): `GET /site/<domain>` is a
  Next.js page; the list is the embedded `__NEXT_DATA__` JSON at
  `props/pageProps/siteData/SimilarSites` (`Site`, `Title`, `Description`,
  `Category`, `Grade` 0..1). An unknown site answers 200 with no list.
  Checked 2026-11-17. If the page shape changes, the panel shows the note
  from `parse_similarsites_page`.
- **webXtrend** (free account token, 100 calls/day): `POST
  https://api.webxtrend.com/v1.0/similar` with `{"token","query"}`; rows
  `domain`, `similarity`, `title`, `description`. Asked only with
  `CatalogKey::WebxtrendToken` (`XANATOO_WEBXTREND_TOKEN`). The reply shape
  beyond the documented fields is unverified (no token at hand); the
  parser accepts the list at top level or under `data` / `results`.
- **similars.net** (free key, 1000 requests/month, max 5 rows): `GET
  /api/<domain>` with `Authorization: Bearer`; asked only with
  `CatalogKey::SimilarsNetKey`. Their terms require a link back: the panel
  shows a "similars.net" credit button when rows came from it.
- Skipped: similarsitesearch.com (API page gone, 404), RapidAPI
  "similarsites" (key required, JS-only docs), data.similarweb.com (403 to
  non-browser clients), Marginalia's similar-sites view (behind an
  anti-bot wait page; not scraped).

Keys live with the catalog credentials (`catalog-credentials.conf`,
Edit → Credentials…); they are read on the main thread when a load starts.

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

## Services menu and `examples/services.md`

- The Services menu (`kServices` in `src/app_frame.cpp`, drawn in
  `src/ui.cpp`) is quick access only, grouped by what the client speaks:
  Reference, News and forums, Social and chat, Video, Small net,
  Directories. `UiService::group` puts a `SeparatorText` heading before an
  entry. A protocol with many hosts (Lemmy, Discourse, Flarum, Stack
  Exchange, Matrix, PeerTube, Gemini, Gopher, Spartan, Nex) opens its
  directory page instead of one arbitrary host; `probe` is the ref whose
  `remote_fetch_supported` greys the entry out.
- Everything in the menu is also hypertext: `examples/services.md` is the
  hub (more entry points per service), the host directories live in
  `examples/services/*.md` (sub-pages, so not in `kExamples`). Keep menu
  and hub in step when adding a client.
- The big host lists were generated once (2026-11-21) and are plain
  Markdown now; refresh them by hand or regenerate from the same sources:
  PeerTube from `instances.joinpeertube.org/api/v1/instances` (not NSFW,
  health ≥ 90, ≥ 10 local videos, grouped by first language), Lemmy from
  `data.lemmyverse.net` (not suspicious / private; ≥ 10 monthly active
  users, plus a "smaller" tier with ≥ 1 and ≥ 20 posts), Stack Exchange
  from `api.stackexchange.com/2.3/sites` (main sites), Discourse from forums
  that answered `/latest.json` (titles from `/site/basic-info.json`).
  Matrix directories, Flarum forums and the small-net hosts were probed
  by hand (anonymous `publicRooms`, `/api`, a request on the protocol's
  port) and only those that answered are listed.

## Digg

- Refs and user view: `docs/FORUMS.md`. `digg_path` (`src/forum_digg.cpp`)
  parses the ref path into a `DiggPath` (Feed / Search / Post / Story /
  Comments / Replies); validation, web addresses and Up use it too.
- `src/forum_digg.cpp` is pure: `digg_mcp_decode` (JSON or SSE envelope,
  `structuredContent`, else the JSON text content; `isError` → message),
  the `digg_*_arguments` builders, the web extraction (`digg_web_images`:
  story UUID → CDN picture from a feed page; `digg_web_story`: UUID,
  picture, credit and `data-cluster-article-body` of a story page) and the
  three documents. `src/forum_fetch.cpp` makes the calls
  (`digg_mcp_call`, POST with `user_agent("read-only forum client")`).
- The tool schema is strict (unknown arguments are rejected). Shapes seen
  live (2026-11-24): feed and search entries have `id`, `title`, `tldr`,
  `description`, `url` (relative for cluster feeds), `topic`, `score`,
  `sourceCount`, `uniqueAuthors`, `createdAt` and `nextCursor`;
  `digg_get_post` returns `story`, `follows` / `newer` (same event),
  `sources` (`kind` `x` or `webpage`) and `nextSourceOffset`; comments have
  `author{name,username,url}`, `state`, `diggCount`, `replyCount`,
  `editedAt` and `parentId`. Neither tool returns pictures or the article.
- Calls per story: `digg_get_post`, the story web page, comments and up to
  8 `digg_get_comment_replies`. A `TOPIC/ID` ref reads the web page first
  for the UUID; a web page of another (merged) story is not used.
- Pictures: `cdn_image` asks the CDN for the shown size
  (`cdn-cgi/image/width=240,height=160,fit=cover,…` for thumbnails,
  `width=1080` for the story picture, linked to the original).
- The article goes through `html_append_blocks` with `sections = true`, so
  its subheadings open sections instead of being lifted above the body.
