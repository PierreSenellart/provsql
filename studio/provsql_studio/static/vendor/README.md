# Vendored third-party front-end libraries

Self-hosted (no CDN at run time -- the Playground build asserts zero
off-origin requests). Loaded lazily by `notebook.js` for Markdown-cell
rendering.

| File            | Library   | Version | License                | Source |
|-----------------|-----------|---------|------------------------|--------|
| `marked.min.js` | marked    | 12.0.2  | MIT                    | https://github.com/markedjs/marked |
| `purify.min.js` | DOMPurify | 3.1.6   | Apache-2.0 OR MPL-2.0  | https://github.com/cure53/DOMPurify |

Both files are unmodified upstream UMD builds; their license headers are
embedded at the top of each file. To upgrade, replace the file with the
matching `cdn.jsdelivr.net/npm/<pkg>@<version>` artifact and update this
table.
