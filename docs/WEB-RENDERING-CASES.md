# Web rendering test cases

This inventory samples widely used sites and reference deployments of
publishing stacks. Stack labels are hypotheses to verify from the fetched
HTML or upstream source before generalizing a rule. Listing URLs are useful
for testing repeated cards and navigation. For follow-up article tests,
record the exact permalink alongside the listing.

## Candidate URLs

| Family | URLs | What to inspect |
|---|---|---|
| WordPress project and block themes | [WordPress News](https://wordpress.org/news/), [Make WordPress Core](https://make.wordpress.org/core/), [WordPress Developer Blog](https://developer.wordpress.org/news/), [WordPress.com Blog](https://wordpress.com/blog/) | Query-loop cards, repeated mobile menus, block captions, post metadata |
| WordPress classic and publisher themes | [TechCrunch](https://techcrunch.com/), [9to5Mac](https://9to5mac.com/), [Boing Boing](https://boingboing.net/), [The Marginalian](https://www.themarginalian.org/), [WPTavern](https://wptavern.com/) | Entry wrappers, lazy images, related posts, newsletters and comments |
| Ghost candidates | [Ghost Changelog](https://ghost.org/changelog/), [404 Media](https://www.404media.co/), [Platformer](https://www.platformer.news/), [Tangle](https://www.readtangle.com/) | Cards, bookmarks, figures, subscription prompts, access boundaries |
| Newsletter platforms | [Simon Willison's newsletter](https://simonw.substack.com/), [Slow Boring](https://www.slowboring.com/), [The Pragmatic Engineer](https://newsletter.pragmaticengineer.com/), [TLDR](https://tldr.tech/tech) | Archive cards, subscribe controls, footnotes and excerpts |
| Hugo | [Hugo Docs](https://gohugo.io/documentation/), [Kubernetes Pods](https://kubernetes.io/docs/concepts/workloads/pods/), [Let's Encrypt](https://letsencrypt.org/docs/), [Hugo PaperMod](https://adityatelange.github.io/hugo-PaperMod/) | Theme-dependent sidebars, local TOCs, admonitions and language switches |
| Jekyll and GitHub Pages | [Jekyll Docs](https://jekyllrb.com/docs/), [GitHub Pages primer](https://pages.github.com/), [Minima demo](https://jekyll.github.io/minima/), [Just the Docs](https://just-the-docs.com/) | Article headings, theme navigation, code and pagination |
| Sphinx | [Python Tutorial](https://docs.python.org/3/tutorial/controlflow.html), [NumPy Quickstart](https://numpy.org/doc/stable/user/quickstart.html), [Requests Quickstart](https://requests.readthedocs.io/en/latest/user/quickstart/), [Sphinx Docs](https://www.sphinx-doc.org/en/master/usage/quickstart.html) | Permalink anchors, literal code, admonitions and version banners |
| MkDocs, Material and Zensical | [MkDocs](https://www.mkdocs.org/user-guide/writing-your-docs/), [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/reference/admonitions/), [FastAPI](https://fastapi.tiangolo.com/tutorial/first-steps/) | Tabs, nested navigation, duplicated snippets and callouts |
| Docusaurus | [Docusaurus](https://docusaurus.io/docs/markdown-features/tabs), [React Native](https://reactnative.dev/docs/getting-started), [Jest](https://jestjs.io/docs/getting-started), [Redux](https://redux.js.org/tutorials/essentials/part-1-overview-concepts) | MDX tabs, copy controls, TOCs and next/previous links |
| Other documentation generators | [Astro](https://docs.astro.build/en/getting-started/), [Vue](https://vuejs.org/guide/introduction.html), [Vite](https://vite.dev/guide/), [Eleventy](https://www.11ty.dev/docs/), [Svelte](https://svelte.dev/docs/svelte/overview), [Rust Book](https://doc.rust-lang.org/book/), [MDN](https://developer.mozilla.org/en-US/docs/Web/HTML) | Starlight, VitePress, Eleventy, mdBook and custom docs shells, example code and link density |
| React and Next.js candidates | [React Learn](https://react.dev/learn), [Next.js Docs](https://nextjs.org/docs/app/getting-started), [Vercel Blog](https://vercel.com/blog), [LessWrong](https://www.lesswrong.com/) | Initial HTML vs. deferred content, hidden server segments, interactive examples |
| Reference and community | [Wikipedia](https://en.wikipedia.org/wiki/Hypertext), [Wiktionary](https://en.wiktionary.org/wiki/hypertext), [ArchWiki](https://wiki.archlinux.org/title/Installation_guide), [Hacker News](https://news.ycombinator.com/item?id=8863), [Lobsters](https://lobste.rs/), [Stack Overflow](https://stackoverflow.com/questions/11227809/why-is-processing-a-sorted-array-faster-than-processing-an-unsorted-array), [Python Discourse](https://discuss.python.org/), [GitHub issues](https://github.com/python/cpython/issues), [Slashdot](https://slashdot.org/) | Tables, footnotes, list vs. thread, reply nesting, native vs. raw HTML view |
| Major publishers | [The Guardian](https://www.theguardian.com/technology), [BBC](https://www.bbc.com/news/technology), [Reuters](https://www.reuters.com/technology/), [Ars Technica](https://arstechnica.com/), [The Verge](https://www.theverge.com/), [WIRED](https://www.wired.com/), [The Register](https://www.theregister.com/), [Engadget](https://www.engadget.com/), [NPR](https://www.npr.org/sections/technology/) | Hero media, headline links, front-page vs. article landmarks, paywalls and fetch failures |
| Independent and classic HTML | [Daring Fireball](https://daringfireball.net/), [Simon Willison's Weblog](https://simonwillison.net/), [Julia Evans](https://jvns.ca/), [LWN](https://lwn.net/), [WHATWG HTML](https://html.spec.whatwg.org/), [Project Gutenberg](https://www.gutenberg.org/ebooks/1342) | Linked titles, long documents, tables, definition lists and plain markup |

## How to evaluate a sample

Use the final response URL as the conversion base. Record HTTP status and
distinguish challenge pages, access restrictions and client-rendered shells
from HTML conversion failures. For a native service, use the raw web view
to evaluate HTML. Compare reader and full-page views, checking the title,
opening paragraphs, sections, links, figures, code and comments as appropriate.
Record missed content and chrome left in the reading flow separately. Test
shared markup on independent deployments before adding a generic rule.

## First sample — 2026-11-26

Eight public URLs returned HTTP 200. A temporary probe linked against
Xanatoo's Nix test build, passed each response and its final URL through
`parse_html_page`, and ran `LayoutEngine` at 1024 px with reader mode on
and off. All 16 layouts produced content. Findings below come from
inspecting the document nodes and corresponding source markup. This
sample covers structural rendering and link preservation. Pixel output,
image downloads and live interaction were outside the probe.

| Page | Stack evidence | Reader result |
|---|---|---|
| [WordPress News](https://wordpress.org/news/) | WordPress generator, `wp-block-post-template` | Release and post lists retain working titles, dates, tags and image references. Both category menus are classified as navigation. Titles and metadata are flattened into each list item. |
| [Make WordPress Core](https://make.wordpress.org/core/) | WordPress generator and theme assets | Linked headings, authors, tables and code survive. Inline glossary expansions interrupt the prose and can run into neighboring words. Compare the plugin markup on another deployment before classifying it as chrome. |
| [Python control flow tutorial](https://docs.python.org/3/tutorial/controlflow.html) | `sphinx_highlight.js`, `a.headerlink` | Section hierarchy and code examples survive, with navigation classified as chrome. The heading permalink glyph appears inline. |
| [Requests Quickstart](https://requests.readthedocs.io/en/latest/user/quickstart/) | `sphinx_highlight.js`, `a.headerlink` | Headings and code survive on a different docs theme. The same inline permalink glyph appears. |
| [FastAPI First Steps](https://fastapi.tiangolo.com/tutorial/first-steps/) | Zensical generator | Main tutorial, code blocks and JSON survive. Heading permalinks appear inline. One terminal example contains literal angle-bracket markup already escaped in the source. |
| [React Quick Start](https://react.dev/learn) | `__NEXT_DATA__` | Prose, nested headings and static code examples survive. This sample exercises Next.js HTML containing the main content in the initial response. |
| [Ghost Changelog](https://ghost.org/changelog/) | Ghost generator, `a.u-permalink` | Card headings, images, excerpts and dates survive. Headlines are plain text because each card's destination is on a separate empty anchor with an accessible name. A split month/day also appears beside the full date. |
| [The Guardian Technology](https://www.theguardian.com/uk/technology) | Final redirected URL, source card markup with empty labeled anchors | Headline text, image references and descriptions survive. Lead card headline links are lost, while conventional links in Most viewed survive. Both Most viewed tab labels appear in the reading flow. |

## Follow-up rule candidates

1. Recover named empty links used as card overlays. Ghost and The Guardian
   demonstrate the same failure with different themes. Start with a link
   limited to its own card and an accessible name matching the visible
   headline. Preserve explicit headline links and check that repeated
   image and text overlays don't add duplicate links.
2. Treat decorative heading permalink glyphs consistently across Sphinx
   and Zensical while preserving heading IDs and section navigation.
   Inspect another generator before choosing a shared markup signature.
3. Compare WordPress post titles and metadata across two independent
   themes, including a single post and its archive. Use the existing
   semantic metadata block where the structure is clear. Evaluate
   glossary expansions against their plugin markup.
4. Use a site hint when a failure depends on a publisher's particular
   structure. First check overlays on a second Ghost theme and compare
   a Guardian article with its section listing.
