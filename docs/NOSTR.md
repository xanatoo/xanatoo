<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com> -->

# Nostr in Xanatoo

## Goal

Move from retrieve-and-forget note viewing to a local, searchable catalogue
of encountered events, profiles, public keys, relay observations, and
eventual user-controlled identities. **It is a partial local observation, not
a directory of every Nostr user or post.** Relay queries return a subset of
the network, may omit old events, and can include spam and invalid signatures.

## Local storage

`state_dir()/nostr.sqlite` holds distinct events (event ID), encountered
public keys (even without profile metadata), event-to-relay observations, and
local identity aliases. NIP-29 group observations are indexed under the pair
`(relay URL, group ID)`; this is **not** the same as a network-wide group ID.
Fetching the same event twice must not increment its
post count. Save all event kinds that a query actually downloads, including
kind-0 profiles fetched to render a note. Track first/last observation and
the newest kind-0 profile per key. Index pubkeys from event authors, `p`
tags and NIP-19 mentions. Cache lookups/search operate without a relay.

Privacy/security: local cache may contain personal data and could grow
indefinitely; future releases need size/export/clear controls. NIP-01 event
IDs are now recalculated from their canonical serialized content. When using
OpenSSL, the Nostr reader also checks BIP-340 Schnorr signatures. On mbedTLS
or network-disabled test builds, an ID match is **hash-only** and cannot
authenticate the author. Signed, hash-only, and legacy unverified events are
counted separately. Historical cache records from before verification remain
in SQLite with `verification=0`, but are hidden from event searches until
refetched and checked. A valid refetch can upgrade the row without deleting
the user's earlier observation. NIP-05 names still require remote
verification: a valid signature proves only control of the public key, not
ownership of the claimed web identifier. `--no-save` disables persistent
cache writes and schema migration.

## Two native dockable panels

### Nostr explorer

- Search cached authors and notes; show total unique events, note count,
  public keys and observed profiles. Display per-author *locally observed*
  note count and last-seen time; no claim of a network-wide total.
- Click an author to navigate to `nostr:npub1…`; click a note to its thread.
- Show relay coverage, kinds, hashtags, follow lists, bookmarks, communities,
  and other locally observed structured events when supported.
- **Groups** tab: list NIP-29 groups observed from relays, their claimed names,
  parent IDs, privacy/join flags and locally observed message counts. These
  counts are relay-scoped and deduplicate the same event received twice.
- **Discover** without a relay (`nostr:groups`) scans up to five group-hosting
  relays: previously observed NIP-29 hosts, two project-published public NIP-29
  relay addresses, and relay hints from cached public NIP-51 `kind:10009`
  lists. When fewer than five hosts are known, Xanatoo also samples public
  `kind:10009` lists on one general relay to learn more group-hosting relays.
  Each group relay is queried for up to 250 public `kind:39000` metadata
  events; the resulting page reports every attempted relay and its result.
  Automatically followed relay hints must resolve to public IP addresses;
  the explicitly entered relay option remains available for local test hosts.
  The cache retains the discovery results for future browsing; scans can be
  incomplete, and public group lists are untrusted hints, not a global index.
  The seed relays are documented by the projects
  [groups-relay](https://github.com/max21dev/groups-relay) and
  [obelisk-relay](https://github.com/obelisk-app/obelisk-relay), not guaranteed
  to be available at scan time.
- **Discover** with an address searches a specific relay; it accepts plain
  hosts (`relay.example`), `https://` or `wss://` URLs and always uses an
  encrypted WebSocket. For example `nostr:groups@relay.example` is normalized
  to `nostr:groups@wss://relay.example`. Selecting a group opens its
  relay-specific timeline and fetches up to 100 recent `kind:1/9/11`
  `h`-tagged messages. The internally generated address is
  `nostr:group@<hex-encoded-relay>.<hex-encoded-group-id>` (not a
  standardized NIP-19 share link). Encoding both fields protects the group
  link from Xanatoo's URL-selector and fragment parsing. Cached messages
  remain readable when the relay is unavailable.
- Discovery now fetches NIP-11 information from the relay's corresponding
  `https://` endpoint (`Accept: application/nostr+json`), requires a trusted
  certificate chain and hostname, reports whether `supported_nips` advertises
  NIP-29, and checks the top-level **`self`** public key (not `pubkey`, which
  identifies an administrator). An event of kind `39000` receives the
  **`[relay]`** indicator only if its BIP-340 signature is valid AND its
  signer matches this certificate-verified `self` key. The result is saved
  with the exact metadata event ID; re-fetching the same event without NIP-11
  cannot downgrade the recorded result, and an unrelated unverified event
  cannot replace a previously relay-authenticated metadata record.
- The weaker indicators remain distinct: `[sig]` = valid event signature but
  no authenticated binding to the relay's key; `[hash]` = hash only, no verified
  author. A missing/invalid NIP-11 response, missing TLS CA trust store or
  build without Schnorr verification leaves metadata **unbound**; discovery
  still proceeds over certificate-verified WSS and reports the NIP-11 failure.
  All WSS connections now check the certificate chain and hostname, separately
  from whether NIP-11 binds the group metadata to the relay. `[relay]` proves
  signed provenance at
  observation time, **not** current membership, moderation, freshness or
  enforcement of the group's rules. NIP-42 authentication, encrypted private
  messages and group creation/joining are not implemented.
- Add active fetch/search controls separately; local search should remain
  functional offline. NIP-50 relay search is optional and capability-gated.

### Nostr accounts

Accounts are on the **Nostr tab of the Identities panel**
(Services → Nostr Accounts, or Panels → Identities). It shares the panel with
Gemini client certificates but not the data: the keys do not carry over
between protocols. A page's author can be saved from there; an account can be
renamed, made active, opened as a profile, copied as npub, or removed (only
its local label).

An account is either **watch-only** (just the public key) or has its
**signing key** here:

- **New key** makes a key pair (OpenSSL `RAND_priv_bytes`); **Import key**
  takes an `nsec1…` (encrypted with the password given) or an
  `ncryptsec1…` (opened with its own password and kept unchanged).
- Keys are kept **only** as NIP-49 `ncryptsec` (scrypt log N 16, r 8, p 1;
  XChaCha20-Poly1305; key-security byte 2 "unknown") in
  `$XDG_STATE_HOME/xanatoo/nostr-keys`, an owner-only (0600) S-expression
  file apart from the `nostr.sqlite` cache, so clearing the cache never
  loses a key. No plain `nsec` is ever written.
- A key is **locked** after start; its password unlocks it for the session
  (Lock forgets the secret, which is wiped). The panel shows each account as
  locked / unlocked.
- Backup: **Copy backup key** (the ncryptsec), and behind a confirmation
  **Copy nsec** for other clients. **Delete key** (also confirmed) leaves the
  account watch-only; without a backup it is lost for good.
- Passwords are not NFKC-normalized (NIP-49 asks for it): ASCII and
  precomposed text give the same bytes, odd decompositions may not open in
  other clients.
- `--no-save` and builds without OpenSSL (mbedTLS) cannot create or import
  keys; the panel says so.

**Posting:** with the active account unlocked, the tab shows a note box.
Post signs a kind-1 event (BIP-340, `nostr_sign.cpp`; `p` tags for the
`nostr:npub…` mentions in the text) and sends it on a worker to the first
five default relays in parallel (`nostr_publish`), each answer (NIP-01
`OK`) listed per relay. An accepted note goes into the local cache and can
be opened with View note.

**Reply links:** a message's "reply to …" header field is a link: to the
parent's card (`#id:<hex>`) when it is on the page, else to the parent's
thread (`nostr:note1…`, `link_reply_targets`). Long ids are cut short in
the label. Group timelines mark replies too.

**Replies:** right-click a message → Reply… posts as the unlocked active
account (`nostr_reply_plan`, `nostr_reply`; see `docs/REPLIES.md`): kind-1
notes get NIP-10 kind-1 replies, NIP-29 / NIP-C7 chat (kind 9) gets a kind-9
answer with a `q` tag in the same group, everything else with content a
NIP-22 kind-1111 comment (uppercase root scope, lowercase parent). Threads
also fetch kind-1111 comments (`#E`) and quoting chat replies (`#q`) and
show them as messages; group timelines include comments. NIP-42 AUTH is not
done, so relays that require it refuse (the per-relay answer says so).

Not yet: NIP-46 remote signers,
relay preferences (NIP-65), profile editing (kind 0), follow lists
(kind 3), deletion requests (kind 5), private messages.

## Groups, channels, forums and categorization

Nostr has several mechanisms, but **no universal subreddit registry or
hierarchy**:

| Mechanism | Event kinds | Role / limitations |
|---|---|---|
| NIP-29 relay-based groups | `h` group tags, `39000` group metadata, `9` group chat, `9000`-series moderation | Most appropriate current group model. Relay controls access and membership; optional parent/child groups are relay-enforced, not a global hierarchy. |
| NIP-72 moderated communities | `34550` community definition, `4550` approvals, `1111` comments | Reddit-like, but currently marked *unrecommended* in favor of NIP-29. Support reading legacy communities, not make this the new foundation. |
| NIP-28 public chat channels | `40` channel, `41` metadata, `42` messages | Older public group chat; also marked *unrecommended* in favor of NIP-29. |
| NIP-51 user lists | `10009` joined NIP-29 groups, `10005` public chats, `10015` interests, follow sets | Personal categorization, not an authoritative network taxonomy. |
| Hashtags / topic tags | `t` tags on ordinary events | Loose cross-relay topics; no moderation/ownership. |
| NIP-23 articles / NIP-22 comments | `30023` articles, `1111` comments | Articles and threaded discussions, not communities by themselves. |

NIP-29 group identity is **group ID plus enforcing relay**; a group ID alone
is not globally unique. Discovery should show both and retain provenance.
Private groups/messages and NIP-42 authenticated relays require appropriate
credentials, encryption and consent before being fetched or stored.

References: [NIP-29](https://github.com/nostr-protocol/nips/blob/master/29.md),
[NIP-72](https://github.com/nostr-protocol/nips/blob/master/72.md),
[NIP-28](https://github.com/nostr-protocol/nips/blob/master/28.md),
[NIP-51](https://github.com/nostr-protocol/nips/blob/master/51.md),
[NIP-50](https://github.com/nostr-protocol/nips/blob/master/50.md),
[NIP-46](https://github.com/nostr-protocol/nips/blob/master/46.md),
[NIP-49](https://github.com/nostr-protocol/nips/blob/master/49.md).
