# xanatoo — Agent Notes

## Project goal

Interactive document composition engine (C++ / SDL2).
See `PROPOSAL.md` for the full vision (stretchtext, live editing,
addressable passages, Hypercard/Xanadu/Canon-Cat spirit).

Keep the system **minimalist and portable**. Prefer small, well-scoped
libraries (SDL2, FreeType, FontConfig, optionally HarfBuzz).
Dear ImGui is allowed for chrome (menus/toolbars). No Qt/GTK/KDE.

## Build and test

Always build inside the Nix develop shell, also for one-off commands:

```sh
nix develop -c xanatoo-build   # configure if needed, then ninja
nix develop -c xanatoo-test    # build, then run xanatoo_tests
nix develop -c xanatoo-run examples/welcome.md
```

- Running `cmake --build /tmp/xanatoo-build` (or `ninja`) from a plain shell
  compiles fine but **fails to link** with `cannot find -lfontconfig` /
  `cannot find -lfreetype`: those libraries are only on the linker path
  inside `nix develop`. That is an environment error, not a code error —
  rerun through `nix develop -c …`.
- The shell prints a long banner first; pipe through `tail` to see results.
- Build dir defaults to `/tmp/xanatoo-build` (`XANATOO_BUILD_DIR`); the
  helpers need `XANATOO_SOURCE`, which the shellHook sets. Native Ubuntu
  builds without Nix: `docs/ENVIRONMENT.md`.
- Tests: `tests/*_tests.cpp` → one offline `xanatoo_tests` binary (no live
  network fetches). Pure code gets tests that run without SDL.

## Where to read more

Read the matching notes before working in an area:

| Area | Notes |
|------|-------|
| Source map (which file is which system), views / panes / tiling, navigation, buffers, prompts, drafts, key focus, floating windows and docks | `docs/dev/APP.md` |
| Section tree, layout virtualization, HTML mapping, web pages / reader mode, tables, inline images and asides, collections, site maps, pandoc import, images / thumtoo, style sheet details | `docs/dev/CONTENT.md` |
| TLS / HTTPS helpers, service refs, HN, feeds, Wikipedia, Reddit, Lemmy, Nostr, Gemini, YouTube, video sites, Internet Archive, Wayback, ebooks, link directory | `docs/dev/SERVICES.md` |
| Versioning, Windows / Emscripten ports, SDL3 transition, Android | `docs/dev/PLATFORM.md` |

Feature design docs live next to them in `docs/` (`PASSAGES.md`, `UI.md`,
`TILING.md`, `REVISIONS.md`, `PAGED.md`, `MATH.md`, `TLS.md`, …).
When you add or change a subsystem, update its `docs/dev/` notes; keep this
file to rules that apply everywhere.

## Architecture principles

- Data-oriented / ECS-friendly: stable IDs for content, separate layout
  geometry from document identity, systems that transform data.
- Content model is the source of truth; layout and rendering are derived.
- User-facing semantic titles are spelled `heading`: S-expression
  `(heading ...)`, Markdown `:::heading`, and stylesheet `(role heading ...)`;
  XML follows XHTML 2 with `<h>` (ranked legacy headings are `<h1>`…`<h6>`).
  `header` is a page-layout word (alongside `footer`), not the canonical
  title tag. Older `header` (and XML `<heading>`) title spellings are accepted
  only as migration aliases; serializers emit the canonical spelling. The internal
  `NodeKind::Header` and `BlockRole::Header` identifiers remain unchanged for
  now; `NodeKind::Heading` is the separate legacy ranked kind.
- Immediate visual feedback after every edit.
- Local-only for now; network is a later optional layer.


## App rules (details: `docs/dev/APP.md`)

`main()` only creates the SDL window/renderer and loops `run_frame`. All
state is the plain-data `xanatoo::app::App` (`include/xanatoo/app.hpp`);
behaviour is free functions ("systems") taking `App&`.

- No behaviour on data types; new modal state goes in `App` (small records
  like `FloatPanel` / `Hover`); menus, toolbar and shortcuts route through
  `execute_command`.
- Views: use `focus_view`, `live_views` (never `pi < 2`), `other_pane` (never
  `1 - app.focus`), `pane_at`, `content_y`. After replacing a buffer's
  document call `refresh_views_of_buf`; to change a page without moving the
  view call `keep_views(app)` before the change.
- Addresses: show / store `pane_address`, never `path_of`; only the base of
  `base//selector#selector` reaches buffers, caches and fetches.
- Buffers: by `BufferId` through `buf` / `find_buf` / `add_buffer` /
  `remove_buffer` / `close_tab`; never index `app.buffers`, never hold a
  `DocBuffer&` across add/remove.
- Unsaved edits are never discarded: `stash_draft` / `stash_or_confirm`
  before replacing or dropping a buffer, `attach_draft` after loading,
  `note_saved` after writing, `mark_dirty` / `note_edit` (not `dirty = true`).
- Gate document edits on `browsing(app)`, never on `app.edit_mode` alone
  (foreign content is read-only).
- Prompts: one `App::prompt`, via `open_prompt` / `close_prompt` /
  `commit_prompt`.
- Key focus: panels take plain keys only while `panel_has_keys(app, …)`;
  the lit title bar is `float_active(app, w)`, never a constant.
- Windows: set `FloatWindow::dock` only through the dock helpers (no
  per-panel "docked" flag); test `float_shaded(w)`; draw no client content
  when `draw_floating_frame` returns false.
- Mouse cursor feedback uses the same hit-test as its action
  (`float_resize_at`, `dock_split_at`, `tile_divider_rect`).

## Directories (`paths.hpp`)

- Never build a user path by hand: `config_dir()` (view.conf),
  `state_dir()` (revstore, sidestore, bookmarks, history),
  `user_data_dir()` (user `styles/`), `cache_dir()`; each may be empty
  (no usable dir, error already printed) — skip the write then. Call
  `ensure_dir` before writing; write small files with `write_file_atomic`
  (util.hpp) and write nothing config / state when `no_save()`.
- Installed data: `data_dir()` / `data_file("examples/x.md")` (from
  `--data-dir`, `<exe>/../share/xanatoo`, the install prefix; Android
  sets it to the unpacked assets). Never the cwd, no env variables.
  The build tree has `share/xanatoo/` with symlinks into the source.
- New options go into `cli.cpp`, `print_usage` and `docs/xanatoo.1`
  (`checks.man` / `checks.help`).
- Version strings: `version_string()`; network User-Agents via
  `user_agent("purpose")` (`version.hpp`).

## Tech stack (current)

- C++20, CMake + Nix flake
- SDL2 (SDL3 transition in progress, see `docs/dev/PLATFORM.md`)
- FreeType (glyph rasterization), FontConfig (styled faces; optional)
- Dear ImGui (menu bar / toolbar / panel widgets; vendored under external/imgui)

## Style sheets

- `styles/*.xss` — S-expression sheets kept apart from the markup; they style
  semantic roles (no selectors). See `docs/STYLESHEETS.md`.
- Every sheet starts from `builtin_style()`; `styles/default.xss` mirrors it
  (a unit test keeps them equal). `(inherit "x")` + late-bound `(define …)`.
- Never hard-code document colours/spacing in layout/render; add a role or
  property to `DocStyle` instead. UI chrome keeps using `theme.hpp`.
- Canvas chrome text (status line, window titles, outline, backlinks,
  tooltips) uses `app.ui_font` / `app.ui_font_bold` (`ui_font_face()`, the
  ImGui face at `kUiFontPx`), never `app.fonts` — those follow the style
  sheet and zoom. Cut long labels with `fit_text`.
  More: `docs/dev/CONTENT.md`.

## Shared helpers

- `sexpr.hpp`: header-only `SExprParser` (`parse_top` / `parse_all`) + `is_atom` /
  `is_form` / `sexpr_*` helpers; used by the `.sexp` format and style sheets.
- `util.hpp`: `starts_with` / `ends_with` / `starts_with_ci`, `ascii_lower`
  (string or string_view), `trim_ascii` (string_view edges),
  `html_decode_entities` (named basics + numeric → UTF-8).

## Coding rules

- GPLv3+ with short SPDX headers on every source file.
- Author: Ingo Ruhnke <grumbel@gmail.com> (SPDX headers and git commits).
- Commit directly to git. Git author **and** committer are always
  `Ingo Ruhnke <grumbel@gmail.com>`, never the agent, a placeholder
  ("xanatoo contributor", "Fix", "CI", …) or an `example.com` address.
  If `git config user.email` is empty or different, commit with
  `git -c user.name="Ingo Ruhnke" -c user.email=grumbel@gmail.com commit …`;
  do not pass `--author`, set `GIT_AUTHOR_*` / `GIT_COMMITTER_*`, or change
  the repository's git config.
- Credit the AI agent only through a trailer in the last paragraph of the
  message, one per agent, e.g. `Co-authored-by: Grok <grok@x.ai>`.
  Check with `git log -1 --format='%an <%ae> / %cn <%ce>'` after committing
  and `git commit --amend --reset-author` if it is wrong.
- Always commit your working changes when a task is done (no need to ask); never leave finished work uncommitted.
- Prefer clear data layouts over clever abstractions.
- No drive-by feature removal; discuss first.

## Continuity

Read `TODO.md` first. Record plans and outcomes there.
Longer design notes: `docs/BRAINSTORM.md`.
Work is committed directly to git. Git bundles are only for Web chat
handovers: when running in Web chat, deliver cumulative bundles from the
original `568a91b` base through the current `HEAD`, and keep the next bundle
number in `TODO.md`. When running as CLI, do not create bundles; the bundle
number is reserved for the next Web chat handover. Never create a parallel
history or distribute patch files.
