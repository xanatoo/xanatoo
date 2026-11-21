# Matrix — homeservers and rooms

[← Services](../services.md)

[Matrix](https://matrix.org/) is an open, federated chat network. Rooms live on many
*homeservers* at once; every server keeps a copy of the rooms its users joined. xanatoo
reads public rooms without an account — it never logs in, joins or sends anything.

| Address | Shows |
|---------|-------|
| `matrix:directory/SERVER` | the server's public room directory (`&q=WORDS` searches it) |
| `matrix:r/ROOM:SERVER` | a room by its alias (`#room:server` without the `#`) |
| `matrix:u/USER:SERVER` | a user's profile |

Pasting `#room:server`, `!roomid:server` or a `https://matrix.to/#/…` link into **Ctrl+L**
opens the room too.

## Room directories

Most servers only share their directory with their own logged-in users. These answered
anonymously on 2026-11-21 (number of public rooms listed then):

- [matrix.org](matrix:directory/matrix.org) — the Matrix.org Foundation's big public server
- [tchncs.de](matrix:directory/tchncs.de) — German community server, ~1,600 rooms
- [tu-dresden.de](matrix:directory/tu-dresden.de) — TU Dresden university, ~1,700 rooms
- [midov.pl](matrix:directory/midov.pl) — Polish community server, ~480 rooms
- [fedora.im](matrix:directory/fedora.im) — Fedora Project, ~250 rooms
- [asra.gr](matrix:directory/asra.gr) — Greek community server, ~230 rooms
- [gnome.org](matrix:directory/gnome.org) — GNOME project, ~180 rooms
- [nixos.org](matrix:directory/nixos.org) — NixOS community, ~140 rooms
- [the-apothecary.club](matrix:directory/the-apothecary.club) — community server, ~120 rooms
- [opensuse.org](matrix:directory/opensuse.org) — openSUSE project, ~100 rooms
- [utwente.io](matrix:directory/utwente.io) — University of Twente students, ~80 rooms
- [nope.chat](matrix:directory/nope.chat) — community server, ~60 rooms
- [fsfe.org](matrix:directory/fsfe.org) — Free Software Foundation Europe, ~30 rooms
- [4d2.org](matrix:directory/4d2.org) — community server, ~20 rooms
- [grapheneos.org](matrix:directory/grapheneos.org) — GrapheneOS project rooms
- [archlinux.org](matrix:directory/archlinux.org) — Arch Linux rooms

Search one directory: [matrix.org for "linux"](matrix:directory/matrix.org?q=linux) ·
[tchncs.de for "open source"](matrix:directory/tchncs.de?q=open%20source).

## Rooms to try

- [Matrix HQ](matrix:r/matrix-hq:matrix.org) — the protocol's main room
- [Synapse admins](matrix:r/synapse:matrix.org) · [Element Web](matrix:r/element-web:matrix.org)
- [KDE](matrix:r/kde:kde.org) · [GNOME](matrix:r/gnome:gnome.org) · [Fedora](matrix:r/fedora:fedoraproject.org)
- [Arch Linux](matrix:r/archlinux:archlinux.org) · [FSFE](matrix:r/fsfe:fsfe.org)
- [Rust](matrix:r/rust:matrix.org) · [Python](matrix:r/python:matrix.org)

Finding more: [matrixrooms.info](https://matrixrooms.info/) indexes public rooms across
servers; [servers.joinmatrix.org](https://servers.joinmatrix.org/) lists servers open
for sign-up.
