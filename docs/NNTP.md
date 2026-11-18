# Usenet / NNTP

Read-only, anonymous Usenet reader over NNTP (RFC 3977) in the same shape
as the forum clients (`docs/FORUMS.md`): group lists and a group's threads
are a Thread of `Article` entries, a discussion is a Thread of `Message`
nodes with the reply tree as indentation. No AUTHINFO, no posting. Gated by
`XANATOO_NNTP` (default ON, plain TCP; off on Emscripten and Windows
minimal builds). Servers to try: `examples/usenet.md`.

Code: `include/xanatoo/nntp.hpp`, `src/nntp.cpp` (pure), `src/nntp_fetch.cpp`
(network). Tests: `tests/nntp_tests.cpp`.

## Addresses (RFC 5538)

| Address | View |
|---------|------|
| `news://HOST[:PORT]/` | hierarchies / groups of the server |
| `news://HOST/WILDMAT` | groups matching (`comp.lang.*`; `?` as `%3F`) |
| `news://HOST/GROUP[?page=N]` | threads of the newest overview window |
| `nntp://HOST/GROUP/N` | article N inside its thread |
| `news://HOST/MSGID` | one article by Message-ID (no `<>`, `/` as `%2F`) |
| `news:GROUP`, `news:MSGID` | same on the default server |
| `nntps://…`, `snews://…` | TLS, port 563, certificates verified |

A path with `@` is a Message-ID, one with `*`, `?` or `[` a wildmat, one
ending in `/digits` an article. Hostless refs go to `nntp.csiph.com`
(`kNntpDefaultServer`), `gmane.*` / `gwene.*` to `news.gmane.io`.
Canonical form: `news://` for lists, groups and Message-IDs, `nntp://` for
articles, `nntps://` for anything over TLS.

## Views

- **Groups**: `LIST ACTIVE [wildmat]`. Up to `kNntpMaxListed` (300) groups
  are listed with their article count, moderation and the
  `LIST NEWSGROUPS` description; more than that (and a wildmat of the form
  `PREFIX*`) fold into the next hierarchy level (`comp.*` → `comp.lang.*`
  with group and article totals). A lone group in a hierarchy stays a group.
  Other wildmats show the first 300 with a note.
- **Group**: `GROUP`, then `OVER` (`XOVER` fallback) over 150 articles per
  page, newest page first. Threads are grouped by the first `References`
  entry (the root), newest activity first; a thread whose root is older
  than the window says "continues an older thread". The heading links the
  thread's earliest article in the window.
- **Thread**: `OVER` 400 articles either side of N; the members are the
  root and everything referencing it; each is fetched with `ARTICLE`
  (at most 150, nearest to N). The parent of a message is its nearest
  `References` entry in the thread; messages whose parent is outside get an
  "In reply to an earlier message" Message-ID link. Subjects are shown only
  where they differ from the thread's.
- **Message-ID**: one `ARTICLE <id>`, with links to its groups, its parent
  and (via `Xref`) its thread.

## Articles

- Headers unfolded; `Subject` / `From` through RFC 2047 (Q and B encoded
  words, adjacent words joined). Unknown code pages fall back to Latin-1,
  never to invalid UTF-8 (`to_utf8` converts only the western ones).
- MIME: the first `text/plain` part (else `text/html`, as HTML) is the
  body, after base64 / quoted-printable and charset conversion; other parts
  are listed as "Attachment: name (type)". `format=flowed` joins soft
  line breaks (RFC 3676); otherwise line breaks are kept.
- Body blocks: runs of `>` lines are BlockQuotes (BlockQuote is a leaf:
  deeper levels keep their `> ` markers), mostly indented / diff-like
  paragraphs are CodeBlocks, a git patch (a `---` or `diff --git` line
  followed by a diff) is one CodeBlock to the end, the signature after the
  last `-- ` line (within 16 lines of the end) a metadata paragraph.

## Not done

- AUTHINFO (servers that need an account), posting, `STARTTLS`.
- Subscriptions / read marks for groups; a newsrc.
- `NEWNEWS` / `NEWGROUPS`; `HDR`-based threading on servers without OVER.
- Non-western charsets beyond the Latin-1 fallback (ISO-8859-2, KOI8-R,
  Shift-JIS, …).
