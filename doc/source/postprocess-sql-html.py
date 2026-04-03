#!/usr/bin/env python3
"""Post-process Doxygen HTML for the SQL API to restore SQL type names.

Same replacements as postprocess-sql-xml.py but applied to the HTML output.
Only modifies text inside HTML tags that are part of function signatures
(code elements, member declarations, etc.), not prose text.
"""

import re
import sys
from pathlib import Path

REPLACEMENTS = [
    (r"\bDOUBLE_PRECISION\b", "DOUBLE PRECISION"),
    (r"\bCHARACTER_VARYING\b", "CHARACTER VARYING"),
    (r"\b(\w+)_array\b", r"\1[]"),
    (r"\bSETOF_(\w+)\b", r"SETOF \1"),
]


def apply_replacements(text):
    for pattern, replacement in REPLACEMENTS:
        text = re.sub(pattern, replacement, text)
    return text


def process_html_file(path):
    content = path.read_text(encoding="utf-8")
    original = content
    for pattern, replacement in REPLACEMENTS:
        content = re.sub(pattern, replacement, content)
    if content != original:
        path.write_text(content, encoding="utf-8")
        return True
    return False


def main():
    html_dir = Path(__file__).parent.parent / "doxygen-sql" / "html"
    if not html_dir.is_dir():
        print(f"Error: {html_dir} not found", file=sys.stderr)
        sys.exit(1)

    modified = 0
    for html_file in html_dir.glob("*.html"):
        if process_html_file(html_file):
            modified += 1
    for js_file in html_dir.glob("*.js"):
        if process_html_file(js_file):
            modified += 1

    print(f"Post-processed {modified} HTML/JS files in {html_dir}")


if __name__ == "__main__":
    main()
