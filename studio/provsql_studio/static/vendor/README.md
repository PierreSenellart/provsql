# Vendored third-party front-end libraries

Self-hosted (no CDN at run time -- the Playground build asserts zero
off-origin requests). Loaded lazily by `notebook.js` for Markdown-cell
rendering.

| File                 | Library            | Version | License               | Source |
|----------------------|--------------------|---------|-----------------------|--------|
| `marked.min.js`      | marked             | 12.0.2  | MIT                   | https://github.com/markedjs/marked |
| `purify.min.js`      | DOMPurify          | 3.1.6   | Apache-2.0 OR MPL-2.0 | https://github.com/cure53/DOMPurify |
| `katex.min.js`       | KaTeX              | 0.16.11 | MIT                   | https://github.com/KaTeX/KaTeX |
| `katex.min.css`      | KaTeX              | 0.16.11 | MIT                   | https://github.com/KaTeX/KaTeX |
| `auto-render.min.js` | KaTeX auto-render  | 0.16.11 | MIT                   | https://github.com/KaTeX/KaTeX |
| `fonts/*.woff2`      | KaTeX fonts        | 0.16.11 | SIL OFL 1.1 / MIT     | https://github.com/KaTeX/KaTeX |

The js/css files are unmodified upstream builds; their license headers are
embedded at the top of each file. KaTeX renders the ``$…$`` / ``$$…$$`` math
that the rst ``:math:`` roles become in Markdown cells; only the ``woff2``
fonts are vendored (every supported browser prefers them, so the ``woff`` /
``ttf`` fallbacks the CSS also lists are never requested). To upgrade, replace
the file with the matching `cdn.jsdelivr.net/npm/<pkg>@<version>` artifact (for
KaTeX also refresh `fonts/` from `katex@<version>/dist/fonts/*.woff2`) and
update this table.
