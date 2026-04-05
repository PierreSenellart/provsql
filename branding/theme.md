# ProvSQL — "Provence" Design Theme

A complete color and typography specification for the ProvSQL PostgreSQL extension branding.

---

## ProvSQL Name Typesetting

### In the logo
The wordmark is set in **EB Garamond semibold italic**, split into two color spans:
- *Prov* — Golden Yellow `#E8B84B` (accent), drawing attention to the distinctive provenance prefix
- *SQL* — Deep Plum `#2A1850` (primary text), receding slightly into the lavender cylinder

### Outside the logo (running text, titles, headings)
Write **ProvSQL** as plain text, no special styling required. The name is one word, mixed case, no space, no hyphen.

### In a display title (hero, cover page, section header)
When used as a standalone title element, ProvSQL may be set in EB Garamond semibold italic with the two-color split reproduced:

```css
/* CSS */
.provsql-wordmark .prov { color: #E8B84B; }
.provsql-wordmark .sql  { color: #2A1850; }
/* font: 'EB Garamond', serif; font-weight: 600; font-style: italic; */
```

```latex
% LaTeX (requires ebgaramond package)
\textit{\textbf{{\color{accent}Prov}{\color{textprimary}SQL}}}
```

### Never do
- Do not write "ProvenanceSQL", "Prov-SQL", "prov_sql", or "PROVSQL".
- Do not apply the two-color split in body text — it is reserved for logo/display use.

---

## Color Palette

| Role             | Name             | Hex       |
|------------------|------------------|-----------|
| Primary          | Lavender Purple  | `#6B4FA0` |
| Accent           | Golden Yellow    | `#E8B84B` |
| Background       | Pale Lilac       | `#F4F0FA` |
| Surface          | Soft Lavender    | `#EDE8F7` |
| Border           | Light Mauve      | `#D6CCF0` |
| Text — Primary   | Deep Plum        | `#2A1850` |
| Text — Secondary | Dusty Mauve      | `#8A7A9B` |
| Highlight        | Warm Terracotta  | `#C4664A` |
| Code text        | Dark Violet      | `#5A3E8C` |
| Code background  | Pale Violet      | `#E8E0F8` |

**Usage notes:**
- `#6B4FA0` (Primary) is used for navbar, hero background, circuit gate fills, and section borders.
- `#E8B84B` (Accent) is used for CTAs, active states, underlines on headings, stat values, and the navbar bottom border.
- `#C4664A` (Highlight / Success) is used sparingly for secondary circuit elements and destructive actions.
- The accent gold on the lavender primary passes WCAG AA contrast for large text. Verify contrast for small UI text independently.

---

## Typography

### Font Stack

| Role        | Typeface    | Weight / Style  | LaTeX package  |
|-------------|-------------|-----------------|----------------|
| Display     | EB Garamond | 600, *italic*   | `ebgaramond`   |
| Body        | EB Garamond | 400, normal     | `ebgaramond`   |
| UI / Nav    | Jost        | 400–600, normal | via `fontspec` |
| Code / Mono | Fira Code   | 400–500, normal | `firacode`     |

### CSS Font Families

```css
--font-display:  'EB Garamond', 'Palatino Linotype', serif;
--font-body:     'EB Garamond', 'Palatino Linotype', serif;
--font-ui:       'Jost', 'Segoe UI', sans-serif;
--font-mono:     'Fira Code', 'Consolas', monospace;
```

### Self-Hosted Font Setup

Download the font files from their canonical sources and place them in your project under `assets/fonts/`.

| Typeface    | Source                                                  | Files needed                                      |
|-------------|---------------------------------------------------------|---------------------------------------------------|
| EB Garamond | https://github.com/octaviopardo/EBGaramond             | `EBGaramond-Regular`, `Italic`, `SemiBold`, `SemiBoldItalic` — `.woff2` + `.woff` |
| Jost        | https://github.com/indestructible-type/Jost             | `Jost-Light`, `Regular`, `Medium`, `SemiBold` — `.woff2` + `.woff` |
| Fira Code   | https://github.com/tonsky/FiraCode                      | `FiraCode-Regular`, `Medium` — `.woff2` + `.woff` |

Prefer `.woff2` for all modern browsers; include `.woff` as a fallback for older ones. You do not need `.ttf` or `.otf` for web use.

```css
/* EB Garamond */
@font-face {
  font-family: 'EB Garamond';
  src: url('assets/fonts/EBGaramond-Regular.woff2') format('woff2'),
       url('assets/fonts/EBGaramond-Regular.woff')  format('woff');
  font-weight: 400; font-style: normal; font-display: swap;
}
@font-face {
  font-family: 'EB Garamond';
  src: url('assets/fonts/EBGaramond-Italic.woff2') format('woff2'),
       url('assets/fonts/EBGaramond-Italic.woff')  format('woff');
  font-weight: 400; font-style: italic; font-display: swap;
}
@font-face {
  font-family: 'EB Garamond';
  src: url('assets/fonts/EBGaramond-SemiBold.woff2') format('woff2'),
       url('assets/fonts/EBGaramond-SemiBold.woff')  format('woff');
  font-weight: 600; font-style: normal; font-display: swap;
}
@font-face {
  font-family: 'EB Garamond';
  src: url('assets/fonts/EBGaramond-SemiBoldItalic.woff2') format('woff2'),
       url('assets/fonts/EBGaramond-SemiBoldItalic.woff')  format('woff');
  font-weight: 600; font-style: italic; font-display: swap;
}

/* Jost */
@font-face {
  font-family: 'Jost';
  src: url('assets/fonts/Jost-Light.woff2') format('woff2'),
       url('assets/fonts/Jost-Light.woff')  format('woff');
  font-weight: 300; font-style: normal; font-display: swap;
}
@font-face {
  font-family: 'Jost';
  src: url('assets/fonts/Jost-Regular.woff2') format('woff2'),
       url('assets/fonts/Jost-Regular.woff')  format('woff');
  font-weight: 400; font-style: normal; font-display: swap;
}
@font-face {
  font-family: 'Jost';
  src: url('assets/fonts/Jost-Medium.woff2') format('woff2'),
       url('assets/fonts/Jost-Medium.woff')  format('woff');
  font-weight: 500; font-style: normal; font-display: swap;
}
@font-face {
  font-family: 'Jost';
  src: url('assets/fonts/Jost-SemiBold.woff2') format('woff2'),
       url('assets/fonts/Jost-SemiBold.woff')  format('woff');
  font-weight: 600; font-style: normal; font-display: swap;
}

/* Fira Code */
@font-face {
  font-family: 'Fira Code';
  src: url('assets/fonts/FiraCode-Regular.woff2') format('woff2'),
       url('assets/fonts/FiraCode-Regular.woff')  format('woff');
  font-weight: 400; font-style: normal; font-display: swap;
}
@font-face {
  font-family: 'Fira Code';
  src: url('assets/fonts/FiraCode-Medium.woff2') format('woff2'),
       url('assets/fonts/FiraCode-Medium.woff')  format('woff');
  font-weight: 500; font-style: normal; font-display: swap;
}
```

> **Licenses:** EB Garamond is OFL-1.1. Jost is OFL-1.1. Fira Code is OFL-1.1. All three can be self-hosted freely in open and commercial projects.
>
> **Required:** The OFL requires the license text to be distributed alongside the font files. Copy the `OFL.txt` file from each upstream repository into `assets/fonts/` next to the font files. A recommended layout:
>
> ```
> assets/fonts/
>   EBGaramond-Regular.woff2
>   EBGaramond-Italic.woff2
>   ...
>   OFL-EBGaramond.txt
>   Jost-Regular.woff2
>   ...
>   OFL-Jost.txt
>   FiraCode-Regular.woff2
>   ...
>   OFL-FiraCode.txt
> ```
>
> You may not sell the font files as a standalone product, but bundling them in an open-source or commercial project is explicitly permitted by the OFL.

### LaTeX Preamble (XeLaTeX / LuaLaTeX)

```latex
\usepackage{fontspec}
\usepackage{ebgaramond}
\setmainfont{EB Garamond}
\setsansfont{Jost}[Scale=MatchLowercase]
\setmonofont{Fira Code}[Scale=MatchLowercase, Contextuals=Alternate]

\usepackage{xcolor}
\definecolor{provsqlPrimary}{HTML}{6B4FA0}
\definecolor{provsqlAccent}{HTML}{E8B84B}
\definecolor{provsqlText}{HTML}{2A1850}
\definecolor{provsqlMuted}{HTML}{8A7A9B}
\definecolor{provsqlHighlight}{HTML}{C4664A}
```

---

## Typographic Rules

- **Display headings** (h1, hero): EB Garamond 600, *italic*, ~2.7–2.8rem, line-height 1.15.
- **Section headings** (h2): EB Garamond 600, *italic*, ~1.45–1.5rem, underlined with a 2px `#E8B84B` rule.
- **Body text**: EB Garamond 400, normal, ~1rem–1.1rem, line-height 1.7–1.75.
- **UI labels / nav / badges**: Jost 500, normal, 0.75–0.9rem, slight letter-spacing (0.02–0.05em). Use uppercase + tracking for small metadata labels.
- **Code blocks**: Fira Code 400, with `font-variant-ligatures: contextual` enabled. Border-left accent stripe in `#E8B84B`. Background `#E8E0F8`, text `#5A3E8C`.
- **Inline code**: Fira Code 400, same colors, no border.
- **Stat/number display**: EB Garamond 600 italic, ~1.9rem, colored `#E8B84B`.

---

## Component Defaults

### Navbar
- Background: `#6B4FA0`
- Bottom border: 3px solid `#E8B84B`
- Logo: EB Garamond italic, white
- Nav links: Jost 500, `rgba(255,255,255,0.82)`

### Hero / Banner
- Background: `#6B4FA0`
- Badge pill: Jost 600 uppercase, background `#E8B84B`
- H1: EB Garamond 600 italic, white, 2.7rem
- Body copy: EB Garamond 400, `rgba(255,255,255,0.78)`
- Primary CTA button: background `#E8B84B`, Jost 600
- Secondary CTA button: transparent, border `rgba(255,255,255,0.35)`, Fira Code

### Cards / Surfaces
- Background: `#EDE8F7`
- Border: 1px solid `#D6CCF0`
- Card title: Jost 600, `#2A1850`
- Card body: EB Garamond 400, `#8A7A9B`

### Footer
- Background: `#EDE8F7`
- Top border: 1px solid `#D6CCF0`
- Copyright: Fira Code, `#8A7A9B`
- Tagline: Jost 500, `#E8B84B`

---

## CSS Custom Properties (full set)

```css
:root {
  /* Colors */
  --color-primary:        #6B4FA0;
  --color-accent:         #E8B84B;
  --color-bg:             #F4F0FA;
  --color-surface:        #EDE8F7;
  --color-border:         #D6CCF0;
  --color-text-primary:   #2A1850;
  --color-text-secondary: #8A7A9B;
  --color-highlight:      #C4664A;
  --color-code:           #5A3E8C;
  --color-code-bg:        #E8E0F8;

  /* Typography */
  --font-display:  'EB Garamond', 'Palatino Linotype', serif;
  --font-body:     'EB Garamond', 'Palatino Linotype', serif;
  --font-ui:       'Jost', 'Segoe UI', sans-serif;
  --font-mono:     'Fira Code', 'Consolas', monospace;

  /* Type scale */
  --text-hero:    2.75rem;
  --text-h2:      1.5rem;
  --text-body:    1.05rem;
  --text-small:   0.82rem;
  --text-ui:      0.875rem;
  --text-label:   0.7rem;

  /* Spacing */
  --radius-sm:    3px;
  --radius-md:    6px;
  --border-accent: 2px solid var(--color-accent);
}
```
