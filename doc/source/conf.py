project = 'ProvSQL'
copyright = '2025, Pierre Senellart'
author = 'Pierre Senellart'

extensions = ['sphinx.ext.todo', 'sphinx.ext.graphviz']
templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']

html_theme = 'sphinx_rtd_theme'
html_theme_options = {
    'style_external_links': True,
    'collapse_navigation': False,
    'style_nav_header_background': '#6B4FA0',
}
html_static_path = ['_static']
html_logo    = '_static/logo.png'
html_favicon = '_static/favicon.ico'
html_css_files = ['custom.css']
html_js_files = ['back-to-site.js']
