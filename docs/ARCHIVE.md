# Internet Archive

archive.org renders its pages with JavaScript, so they cannot be shown as web
pages. `ia:` refs read the public JSON APIs instead: read-only, local-only, no
account. Needs a network build (`XANATOO_HTTPS`).

Code: `include/xanatoo/archive.hpp`, `src/archive.cpp`. Ref parsing and the
JSON → Markdown mapping (`ia_markdown`) are pure and tested offline in
`tests/archive_tests.cpp`; only `ia_fetch` touches the network.

| Ref | Shows |
|-----|-------|
| `ia:ID` | item: thumbnail, details, description, files, reviews |
| `ia:ID?page=N` | page N (50 items, most downloaded first) of a collection's members |
| `ia:uploader:ADDRESS[?page=N]` | everything one account uploaded, 50 per page, most downloaded first |

- Data: `https://archive.org/metadata/ID` (item) and `advancedsearch.php?q=collection:ID`
  (members of a collection; only fetched when `mediatype` is `collection`).
- The infobox groups the cover and the item’s property list. It floats beside
  the description when there is room and appears above it in narrow panes.
- Details: creator, date, publisher, language, collections, subjects, rights,
  license, source; for books volume, page count, ISBN / LCCN, OCLC (WorldCat
  link), Open Library edition / work, call number, contributor, sponsor, scan
  centre + ppi, OCR engine; for films runtime / sound / colour; a lending-library
  note for `access-restricted-item`; added / last updated date, total size and
  file count (`item_size`, `files_count`, `item_last_updated`), notes, and the
  uploader (an e-mail address, as the public API gives it) linking to
  `ia:uploader:ADDRESS` (`advancedsearch` with `q=uploader:"ADDRESS"`; View →
  web toggle opens the same search on archive.org).
- Creator, contributor and language link to `search:archive:FIELD:"VALUE"`, the
  date to `search:archive:date:…` (its year, month or day; a date in another
  form stays text). Like any `archive` search: the 40 most downloaded, no paging.
- Description: HTML, so it does not go through the Markdown. `ia_markdown`
  writes a placeholder paragraph and `ia_insert_description` replaces it in the
  parsed document by the `html_append_blocks` nodes (bold, links, lists, tables,
  images). A blank line also ends a paragraph, and a description without block
  tags keeps its line breaks. Links to archive.org searches become
  `search:archive:…`; links other than http(s), mailto and ftp become text.
- Files link to `archive.org/download/ID/NAME`; bookkeeping files (`*_meta.xml`,
  `*_files.xml`, torrents, item tiles) are left out, all others are listed (the
  metadata reply already carries them). Each file's `source` sorts them:
  "Original files" lists the uploads, each with the derivatives whose
  `original` names it nested below (recursively: raw scans → JP2 → OCR →
  PDF / EPUB). These branches start folded. Derivatives without a listed
  original go to "Other derived files" with "(from NAME)". Metadata files
  go to "Other files". A reply without `source` gives one "Files" list. Lines show format, size, duration (`length`),
  width × height and track number + title (not repeated on a derivative with
  its original's title). PDFs / EPUBs / images route through the usual
  download / viewer handling.
- Reviews (`reviews` in the metadata reply): stars, title, reviewer, date, body
  (plain text, not HTML: blank lines split paragraphs, single newlines stay
  line breaks; `text_md`).
- Movies, audio and etree items get "▶ Play" (the first file this build plays,
  `is_playable_url` passed into `ia_markdown`; opens the Media panel) and a
  "Watch / listen with mpv" link (`external:mpv:URL`).
- Web URLs `archive.org/details|download|stream|embed/ID` map to `ia:ID` (native
  toggle); search, account and Wayback pages stay web pages.
- The `ia` search engine's results link to `ia:ID`.

## Limits

- Writing reviews, the full-text viewer and uploads are not supported.
- Items with restricted access (`is_dark`, lending-only books) only show what
  the metadata API reveals; the files may not download.

## Wayback Machine (`wayback:`)

Old versions of ordinary web pages (`src/wayback.cpp`, `wayback.hpp`; needs
`XANATOO_WEB`):

- `wayback:URL` is the nearest snapshot, `wayback:20150301/URL` the one nearest
  to a date (4–14 digits). The fetch is the ordinary web page reader on
  `https://web.archive.org/web/<ts>id_/URL` (`id_` = as archived, without the
  Wayback toolbar). The Wayback redirect supplies the real capture time; the
  pane address becomes that snapshot URL, which View → Live Page maps back.
- View → **Old Version (archive.org)** opens the focused http(s) page's
  snapshot; on a snapshot the same entry reads **Live Page**.
- A page that fails with 404 / 410 / 5xx gets a "Look for an old version at
  archive.org" link in its error banner.
- Links inside a snapshot are the archived page's own (absolute links go to the
  live web; relative ones stay inside the archive).
- No availability-API lookup yet: the redirect already picks the nearest
  capture; a page without captures shows the archive's 404.
- Not CORS-open: `web.archive.org` snapshots send no `Access-Control-Allow-Origin`
  (only the `archive.org/wayback/available` JSON does), so this feature will
  need a proxy in the WebAssembly build.
