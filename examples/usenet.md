# Usenet — news servers

[Usenet](https://en.wikipedia.org/wiki/Usenet) is the discussion network of
newsgroups from 1980, still carried by many servers over
[NNTP](https://en.wikipedia.org/wiki/Network_News_Transfer_Protocol)
(port 119, or 563 with TLS). xanatoo reads it directly: a server's group
list, a group's recent threads, and threads with quoted replies.

Reading only, and only where the server allows it without an account:
xanatoo sends no login and never posts. Servers that need registration
answer “the server needs a login to read”.

## Addresses

| Address | Shows |
|---------|-------|
| `news://host/` | the server's hierarchies (`comp.*`, `alt.*`, …) |
| `news://host/comp.lang.*` | groups matching a pattern |
| `news://host/comp.lang.c` | recent threads of a group (`?page=2` older) |
| `nntp://host/comp.lang.c/123` | article 123 inside its thread |
| `news://host/id@example.org` | one article by its Message-ID |
| `news:comp.lang.c` | a group on the default server (`nntp.csiph.com`) |
| `nntps://host/…` | the same over TLS (port 563) |

**Up** goes from an article to its group, from a group to its hierarchy.

## Mailing-list and RSS gateways

- [news.gmane.io](news://news.gmane.io/) — Gmane: thousands of mailing lists
  as newsgroups ([gmane.*](news://news.gmane.io/gmane.*)), Gwene: RSS feeds
  ([gwene.*](news://news.gmane.io/gwene.*)). Try
  [git](news://news.gmane.io/gmane.comp.version-control.git) or
  [Emacs devel](news://news.gmane.io/gmane.emacs.devel).

## Independent communities and alternative networks

- [news.tilde.club](news://news.tilde.club/) — Tildeverse groups
  ([tilde.club](news://news.tilde.club/tilde.club),
  [tilde.art.ascii](news://news.tilde.club/tilde.art.ascii)); also
  [over TLS](nntps://news.tilde.club/)
- [news.tilde.team](news://news.tilde.team/) — Tildeverse
- [news.tildeverse.org](news://news.tildeverse.org/) — Tildeverse gateway
- [cosmic.voyage](news://cosmic.voyage/) — Tildeverse with local groups
  ([TLS](nntps://cosmic.voyage/))
- [yourtilde.com](news://yourtilde.com/) — Tildeverse
- [news.phreaknet.org](news://news.phreaknet.org/) — telecom, technology and
  hobby groups ([TLS](nntps://news.phreaknet.org/))
- [toolazy.synchro.net](news://toolazy.synchro.net/) — Synchronet BBS
  message areas (DOVE-Net, FidoNet echoes)
- [bbsgamer.ddns.net](news://bbsgamer.ddns.net/) — Synchronet BBS
- sybershock.com — independent web / NNTP forums (did not answer when
  this page was written)

## Free text-only Usenet, readable without an account

Big Eight (`comp`, `sci`, `rec`, …), `alt`, `de`, `fr` and more:

- [nntp.csiph.com](news://nntp.csiph.com/) — long retention, the default
  server ([comp.lang.c](news:comp.lang.c), [TLS](nntps://nntp.csiph.com/))
- [news.blueworldhosting.com](news://news.blueworldhosting.com/)
  ([TLS](nntps://news.blueworldhosting.com/))
- [paganini.bofh.team](news://paganini.bofh.team/)
  ([TLS](nntps://paganini.bofh.team/))
- [news.mixmin.net](news://news.mixmin.net/) ([TLS](nntps://news.mixmin.net/))
- [news.neodome.net](news://news.neodome.net/) — read-only
- [news.dizum.com](news://news.dizum.com/) — read-only
- [news.usenet.ovh](news://news.usenet.ovh/) — French / international
- [news.bbs.nz](news://news.bbs.nz/) — Agency News
- [news.chmurka.net](news://news.chmurka.net/) — Polish Usenet
  ([pl.*](news://news.chmurka.net/pl.*), [TLS](nntps://news.chmurka.net/))
- [news.gegeweb.org](news://news.gegeweb.org/) — French Usenet (local groups)
- [debian.trudheim.com](news://debian.trudheim.com/) — read-only, Big Eight /
  Linux
- [news.in-ulm.de](news://news.in-ulm.de/) — local Ulm groups (full access
  is for members)

Some servers' TLS certificates are self-signed; xanatoo verifies
certificates, so use their plain `news://` address.

## Free with registration (not readable here yet)

- news.eternal-september.org — free account for Usenet; without one only its
  own [eternal-september.*](news://news.eternal-september.org/eternal-september.*)
  groups
- news.nntp4.net — free, registration
- news.solani.org — free, registration
- news.i2pn2.org — free Usenet (name did not resolve when this page was
  written)
- news.hispagatos.org — Spanish Usenet
- photonic.trudheim.com — read-only, Big Eight / Linux (name did not resolve
  when this page was written)

## German / Open News Network (account needed)

- news.babsi.de — free with an Open News Network account
- news.mb-net.net — free with an account
- news.roellig-ltd.de — free with an Open News Network account

## Paid / membership

- news.individual.net — paid; shutdown planned for 2027-09-30
- news.bawue.net — paid / membership
- news.in-ulm.de — membership (the local groups above are open)
- news.ping.de — membership
- news.united-newsserver.de — paid

Servers change; if one fails, try another.
