# Video sites (PeerTube, Odysee, Dailymotion, media.ccc.de)

Read-only, local-only, like `youtube:` (`docs/YOUTUBE.md`). Gated by
`XANATOO_VIDEO` (default ON, needs a TLS backend; off in the minimal port).
Public unauthenticated JSON APIs only: no accounts, no posting.

Code: `include/xanatoo/video_sites.hpp`, `src/video_sites.cpp`. Parsers
(`pt_parse_*`, `od_parse_*`, `dm_parse_*`) and document builders
(`vs_listing_document`, `vs_video_document`) are pure and tested offline in
`tests/video_sites_tests.cpp`; only `video_site_fetch` needs the network.

## Refs

| Ref | Shows |
|-----|-------|
| `peertube:HOST[?page=N]` | recent videos of an instance |
| `peertube:HOST/c/NAME`, `/a/NAME` | channel / account header + videos |
| `peertube:HOST/w/ID[?comments=N&thread=ID]` | video, comments; `thread=` shows one thread's replies |
| `peertube:HOST/search?search=Q` | instance search; `sepiasearch.org` searches the fediverse |
| `odysee:@chan:8[?page=N]` | channel header + uploads |
| `odysee:@chan:8/slug:e[?comments=N&thread=ID]` | video, comments (top sort), replies |
| `odysee:search?s=Q` | search (lighthouse → claim_search) |
| `dailymotion:xID` | video, comments |
| `dailymotion:user/ID`, `dailymotion:search?search=Q` | channel videos / search |
| `mediaccc:[?page=N]` | recent talks on media.ccc.de |
| `mediaccc:conferences` | every conference, newest release first |
| `mediaccc:c/ACRONYM` | one conference's talks (`38c3`, `fosdem2025`, …) |
| `mediaccc:v/SLUG` | talk: speakers (→ search), conference, Play / Sound only |
| `mediaccc:search?q=Q` | search (title, speakers, description) |

Search-bar engines: `peertube` (`pt`), `odysee` (`od`), `dailymotion` (`dm`),
`mediaccc` (`ccc`).
Video pages carry "▶ Play (label)" (the stream itself, plays in the Media
panel), "♪ Sound only" when the site has a separate audio file, "Watch on X"
(`external:URL`) and "Watch with mpv" (`external:mpv:URL`) links.

- PeerTube (`pt_pick_stream`): files with picture and sound from `files[]`
  and `streamingPlaylists[].files[]` (web videos first); the highest ≤ 720p,
  else the lowest above. The first sound-only file is "Sound only". When
  only split audio / video pieces exist, the HLS master (`playlistUrl`).
- Odysee: `player.odycdn.com/v6/streams/{claim_id}/{sd_hash[:6]}.mp4` from the
  claim, replaced by the `get` RPC's `streaming_url` on the video page
  (`od_parse_streaming_url`). Audio claims get "▶ Play" on the sound.
  Transcoded (newer) uploads exist only as HLS: their `.mp4` answers
  401 "This content cannot be accessed at the moment" (with a Referer the
  CDN redirects to the playlist). The video page therefore first fetches
  `.../v6/streams/{claim_id}/{sd_hash}/master.m3u8` (`od_hls_url`, no
  Referer needed for it or its segments); when that answers `#EXTM3U` the
  link is "▶ Play (HLS)", else (404, older uploads) the `.mp4` stays.
- media.ccc.de (`ccc_parse_event`, voctoweb's `api.media.ccc.de/public`):
  the recording in the talk's `original_language` first (`deu-eng` files
  carry both tracks and rank after it), H.264 MP4 before WebM / AV1, HD
  before SD, never the `*-slides` folders; MP3 before Opus for "Sound
  only". The site has no comments (`VsVideoInfo::no_comments` drops the
  section). Recent talks (100), the conference list and a conference's
  talks arrive whole and are cut into pages of 50 (`vs_slice_page`);
  search pages are the API's (25).
- Dailymotion: no Play link (the metadata m3u8 answers 403,
  protected delivery). A PeerTube video's ref uses the host it really
lives on (search results can span instances).

Web URLs map through `video_site_ref_from_web_url` (native toggle):
`odysee.com/@…`, `dailymotion.com/video/xID`, `dai.ly`, PeerTube `/w/ID`,
`/videos/watch/ID`, `/video-channels/NAME`, `/accounts/NAME`, and
`media.ccc.de/`, `/v/SLUG`, `/c/ACRONYM`, `/search/?q=`. Other PeerTube
paths are not claimed (an arbitrary `/w/…` is MediaWiki).

## Limits

- Dailymotion comments are untested against a video that has comments.
- Odysee comments: top sort only. Odysee `lbry://@a#8/b#e` is `@a:8/b:e`.
- Not done: Piped, Rumble, Vimeo, Bilibili.
