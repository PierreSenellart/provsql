project = 'ProvSQL'
copyright = '2025, Pierre Senellart'
author = 'Pierre Senellart'

extensions = ['sphinx.ext.todo', 'sphinx.ext.graphviz', 'sphinxcontrib.bibtex']

bibtex_bibfiles = ['../../website/_bibliography/references.bib']
bibtex_default_style = 'pdf_link'
bibtex_reference_style = 'author_year'

# Custom pybtex style: link the title to the PDF when a 'pdf' field is present.
from pybtex.style.formatting.unsrt import Style as UnsrtStyle
from pybtex.style.template import href, field, sentence
from pybtex.plugin import register_plugin

class PDFLinkStyle(UnsrtStyle):
    def format_title(self, e, which_field, as_sentence=True):
        if 'pdf' in e.fields:
            title = href[
                field('pdf', raw=True),
                field(which_field, apply_func=lambda text: text.capitalize())
            ]
            return sentence[title] if as_sentence else title
        return super().format_title(e, which_field, as_sentence)

register_plugin('pybtex.style.formatting', 'pdf_link', PDFLinkStyle)
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
html_show_sphinx = False
html_show_copyright = False
html_show_sourcelink = False

# ---------------------------------------------------------------------------
# :sqlfunc:`name` role — renders function name as linked monospace code
# pointing to the Doxygen SQL API reference.
# ---------------------------------------------------------------------------

_SQL_FUNC_MAP = {
    'add_provenance':           '/doxygen-sql/html/group__table__management.html#ga00f0d0b04b2b693c974e72aaf095cb3b',
    'remove_provenance':        '/doxygen-sql/html/group__table__management.html#ga91ced31ffd25c227390042e5089cb200',
    'provenance':               '/doxygen-sql/html/group__provenance__output.html#ga93c314c9412aff767f5f5f994da091c9',
    'create_provenance_mapping':'/doxygen-sql/html/group__table__management.html#ga9830f6222200004bceb4b50e0a35cd91',
    'create_provenance_mapping_view': '/doxygen-sql/html/group__temporal__db.html#gae8beeb7a12893f3c861b26e9552a932b',
    'get_gate_type':            '/doxygen-sql/html/group__gate__manipulation.html#ga26773255355efc79f91d3109316bdb8b',
    'get_children':             '/doxygen-sql/html/group__gate__manipulation.html#gad2e6746782ac5b6500ebb8c48f75303b',
    'create_gate':              '/doxygen-sql/html/group__gate__manipulation.html#ga47adb345beee16ad65566b8e2ca96f8a',
    'set_prob':                 '/doxygen-sql/html/group__gate__manipulation.html#ga17334fe1d3969d969eaa18e32a93b4e0',
    'get_prob':                 '/doxygen-sql/html/group__gate__manipulation.html#gad8d28be45c1fa36422538d51c7624ce5',
    'probability_evaluate':     '/doxygen-sql/html/group__probability.html#gabffa40fef0e37a75d39d4c39c4a1ec0f',
    'provenance_evaluate':      '/doxygen-sql/html/group__semiring__evaluation.html#gac2c8dfcd3eb924ad277d4dd9de05beb9',
    'aggregation_evaluate':     '/doxygen-sql/html/group__semiring__evaluation.html#gaa8a1ecbed9cc64fef68f1d90cd598bcc',
    'choose':                   '/doxygen-sql/html/group__choose__aggregate.html#ga97d3c5ea62acf16a07b53fbb31546b0e',
    'shapley':                  '/doxygen-sql/html/group__probability.html#gaa123c9e2955d344d760bf3597dbf2a81',
    'shapley_all_vars':         '/doxygen-sql/html/group__probability.html#ga7e17cfee20ca3b3aa25245a20d47904f',
    'banzhaf':                  '/doxygen-sql/html/group__probability.html#gab6aafaefb4263c72df327525c6918083',
    'banzhaf_all_vars':         '/doxygen-sql/html/group__probability.html#ga661df5e48131390dea4fb75a094e14d0',
    'expected':                 '/doxygen-sql/html/group__probability.html#ga7124b41224adc29ff5405d5ad6db277e',
    'sr_formula':               '/doxygen-sql/html/group__compiled__semirings.html#ga76c32e829ab40658af1103ffc22717a6',
    'sr_boolean':               '/doxygen-sql/html/group__compiled__semirings.html#ga80ae99ffbdec6d1e298a53d0bbb1ec1b',
    'sr_boolexpr':              '/doxygen-sql/html/group__compiled__semirings.html#ga0a4057956b4751b4263fc1a913161012',
    'sr_counting':              '/doxygen-sql/html/group__compiled__semirings.html#gad3e2b6c5dc5d0041fc29ee086add1de6',
    'sr_why':                   '/doxygen-sql/html/group__compiled__semirings.html#ga7501c5d75cf9a2920e9176169c310a7f',
    'to_provxml':               '/doxygen-sql/html/group__provenance__output.html#gacc9e8b2a47ade6f5c87c27f64b4707b1',
    'view_circuit':             '/doxygen-sql/html/group__provenance__output.html#ga1c6caf1bb91c0cfdfc56c33666d41897',
    'where_provenance':         '/doxygen-sql/html/group__provenance__output.html#ga6ddf85fea18edbd973c118020ff4551b',
    'repair_key':               '/doxygen-sql/html/group__table__management.html#gacfbe67438895e5f153b01bb9352cbc54',
    'undo':                     '/doxygen-sql/html/group__temporal__db.html#ga0f47a56547d5a63e7d3fe18a0c1c5d00',
    'union_tstzintervals':      '/doxygen-sql/html/group__temporal__db.html#ga2ba50d413598d163083ab577e53e844c',
    'get_valid_time':           '/doxygen-sql/html/group__temporal__db.html#gaf71e0f3466ba453b0d27182b6f4b9ad0',
    'timetravel':               '/doxygen-sql/html/group__temporal__db.html#ga73181ef9e1e7b5f293ae379414fe7d63',
    'timeslice':                '/doxygen-sql/html/group__temporal__db.html#gaa3de6e26f960ee27e916a1c35fbb75f0',
    'history':                  '/doxygen-sql/html/group__temporal__db.html#gac96504e5f0f7bf9da1dfc089cdbcdd21',
}


def setup(app):
    from docutils import nodes

    def sqlfunc_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        key = text.rstrip('()')
        url = _SQL_FUNC_MAP.get(key)
        lit = nodes.literal(text, text)
        if url:
            ref = nodes.reference('', '', internal=False, refuri=url)
            ref += lit
            return [ref], []
        return [lit], []

    app.add_role('sqlfunc', sqlfunc_role)
    return {'version': '0.1', 'parallel_read_safe': True}
