# Finger — public hosts

[Finger](https://en.wikipedia.org/wiki/Finger_protocol) (RFC 1288, port 79)
is a tiny user-info protocol from the 1970s. You ask a host about a user (or
about nobody) and get a short text reply — often a `.plan` status note.

xanatoo opens `finger://` addresses as plain documents. Hosts come and go;
if one fails, try another.

## Public plan hosts

These are modern finger services meant for the public:

- [random@happynetbox.com](finger://random@happynetbox.com) — random plan from Happy Net Box
- [benbrown@happynetbox.com](finger://benbrown@happynetbox.com) — Happy Net Box (Ben Brown)
- [reiver@plan.cat](finger://reiver@plan.cat) — plan.cat (also bridges to ActivityPub)
- [charles@finger.farm](finger://charles@finger.farm) — Finger.Farm example user

## Tilde communities

Shared Unix hosts that still answer on port 79:

- [tilde.town](finger://tilde.town/) — who’s on (empty query)
- [poem@tilde.town](finger://poem@tilde.town)
- [vilmibm@tilde.town](finger://vilmibm@tilde.town)
- [tomasino@cosmic.voyage](finger://tomasino@cosmic.voyage)
- [tilde.team](finger://tilde.team/)

## Classic public-access Unix

- [sdf.org](finger://sdf.org/) — SDF host banner / info (empty query)
- [ring@thebackupbox.net](finger://ring@thebackupbox.net) — finger ring directory

## How to use it here

1. **Ctrl+L**, type a `finger://user@host` address, Enter.
2. Or open this page and follow a link above.
3. An empty user (`finger://host/` or `finger://host`) asks the server for a
   general listing when the server supports it.

See also [Gopher](gopher://gopher.floodgap.com/1/),
[Gemini](gemini://geminiprotocol.net/), and the
[Directory](directory.md).
