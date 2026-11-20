# Site hints

Some web pages do not say in their markup which part is the content:
there is no `<main>`, there are two `<article>`s, and comments are
nested `<ul>` lists with no structure beyond class names. Reader mode
then guesses wrong. A **site hint sheet** tells the page reader what the
parts of such a site *are*. It does not change how anything looks; that
stays with the style sheets (`docs/STYLESHEETS.md`).

Sheets are S-expression files ending in `.xsite`:

- `sites/` in the installed data (`data_dir()/sites`) ships sheets for
  known sites (`sites/slashdot.xsite`);
- `user_data_dir()/sites/` (e.g. `~/.local/share/xanatoo/sites/`) holds
  your own sheets. They are read after the installed ones, so a site
  named there replaces the shipped sheet for that host.

Sheets are reread when a file changes; the next page load uses them.
Problems (an unknown rule, a bad selector or regex) are printed to
stderr with the file name; the rest of the sheet still loads.

## Example

```lisp
(site "slashdot.org"
  (main "article.fhitem-story" "#commentlisting")
  (chrome sidebar "#slashboxes")
  (thread "#commentlisting"
    (heading "Comments")
    (message "li.comment")
    (title ".title a")
    (author ".by a")
    (date ".otherdetails" "on (.*@\\S+)")
    (score ".score" "Score:\\s*([^)]*)")
    (body ".commentBody")))
```

## Hosts

`(site "host" "host2" … rules…)`: a host matches itself and its
subdomains (`slashdot.org` covers `tech.slashdot.org`); a leading `www.`
is ignored. Hints apply to remote pages only, not to local `.html` files.

## Selectors

A small CSS subset: `tag`, `#id`, `.class`, `[attr]`, `[attr=value]`
(quoted values allowed), combined into compounds (`li.comment.full`) and
chained by spaces for "inside" (`#comments ul li`). There are no `>`,
`+`, `~` or pseudo-classes. Matching runs on a tolerant parse of the page
(implied `</li>`, `</p>`, `</td>`…; stray end tags are ignored).

## Rules

Each rule takes one or more selectors. For one element, the first rule
in the sheet that matches it wins.

| Rule | Meaning |
|------|---------|
| `(main SEL…)` | The content. Several matches are kept in page order; everything else on the page is chrome. Without `main` the usual guess (`<main>`, a lone `<article>`, `<body>`) applies. |
| `(chrome ROLE SEL…)` | Page chrome with a role (`navigation`, `sidebar`, …); reader mode hides it. The role is a bare word and optional. |
| `(content SEL…)` | Not chrome, even when the built-in heuristics (`<nav>`, share bars, …) would say so. |
| `(drop SEL…)` | Left out entirely. |
| `(thread SEL… fields…)` | A discussion, turned into a Thread of Messages (below). |

### Threads

Inside `(thread …)`:

- `(heading "Comments")`: the thread's title (optional);
- `(message SEL)`: one comment (required). A message inside another one
  is its reply; nesting gives the reply tree;
- `(title SEL)`, `(author SEL)`, `(date SEL)`, `(score SEL)`: fields of a
  message, from the first matching element inside it that is not inside
  a reply. An optional second string is a regular expression (ECMAScript):
  group 1, or the whole match, is the value. A link in the author, date
  or title element is kept as the field's link;
- `(body SEL)`: the comment text (rendered as ordinary blocks).

A message with no author, title or body (a collapsed placeholder that the
site's script fills in) is skipped; its replies go to the message around
it.

## Implementation

`include/xanatoo/site_hints.hpp`, `src/site_hints.cpp`; the hook into
the HTML reader is `HtmlOptions::hint` (`html_min.hpp`). Notes:
`docs/dev/CONTENT.md` "Web pages".
