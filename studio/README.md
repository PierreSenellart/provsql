# ProvSQL Studio

A Python-backed web UI for ProvSQL, replacing the unmaintained `where_panel/`. Two surfaces:

* **Where Panel**: textarea SQL runner with hover-highlight where-provenance against source tables. Writes allowed (CREATE / INSERT / etc.) so users can paste a tutorial or case-study script and step through it.
* **Circuit Visualizer**: interactive DAG view of the provenance circuit for a result tuple. × / + gates, hover to highlight subtree, click to pin, lazy expansion.

This directory is currently a planning skeleton. See [`TODO.md`](TODO.md) for the implementation plan.

## Layout

```
studio/
├── TODO.md         implementation plan: stages, source-code work, distribution
├── README.md       (this file)
└── design/         vendored Claude Design "Provence" handoff bundle (HTML / CSS / JS only)
    ├── colors_and_type.css   CSS variables + @font-face (imported by both kits)
    ├── ui_kits/
    │   ├── where_panel/      static prototype to vendor in stage 2
    │   └── circuit/          static prototype to vendor in stage 3
    └── screenshots/          reference for verification + docs figures
```

Fonts (`*.woff2` + OFL licences), logo, favicon, dataflow SVG, and
institution logos live upstream and are not duplicated here:

* `branding/fonts/` and `branding/fonts-face.css` (richer @font-face block
  than the bundle's: Greek + Latin-extended subsets included)
* `branding/logo.png`, `branding/logo.svg`, `branding/favicon.ico`
* `website/assets/images/dataflow.svg`
* `website/assets/images/institutions/{cnrs.svg, ens-psl.png, inria.svg}`

The HTML / CSS in `design/` reference these via relative paths that no
longer resolve in this directory; the paths are rewritten when the
prototypes are vendored into `provsql_studio/static/` (see TODO stage 0).
