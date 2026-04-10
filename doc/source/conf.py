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
html_js_files = [
    ('jquery.js', {'priority': 100}),  # sphinx-rtd-theme requires jQuery
    'back-to-site.js',
]
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
    'create_provenance_mapping_view': '/doxygen-sql/html/group__table__management.html#gae8beeb7a12893f3c861b26e9552a932b',
    'get_gate_type':            '/doxygen-sql/html/group__gate__manipulation.html#ga26773255355efc79f91d3109316bdb8b',
    'get_children':             '/doxygen-sql/html/group__gate__manipulation.html#gad2e6746782ac5b6500ebb8c48f75303b',
    'get_infos':                '/doxygen-sql/html/group__gate__manipulation.html#gae4eb70baaa1d85ecb4bad27cdd6fe708',
    'get_extra':                '/doxygen-sql/html/group__gate__manipulation.html#ga12ce1bcd1609204d091d18d8912b840b',
    'get_nb_gates':             '/doxygen-sql/html/group__gate__manipulation.html#gad694ac39c8517985d39ac6c5fff72d80',
    'identify_token':           '/doxygen-sql/html/group__circuit__introspection.html#gad53f5ec3f9c5d86714dd6299e5a5185e',
    'gate_zero':                '/doxygen-sql/html/group__internal__constants.html#ga13455054a9c6a90a9088d8666b105812',
    'gate_one':                 '/doxygen-sql/html/group__internal__constants.html#ga27aad07a447f5b1f17018ac09ef4ea32',

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


# ---------------------------------------------------------------------------
# :cfunc:`name` role — renders function name as linked monospace code
# pointing to the Doxygen C/C++ API reference.
# ---------------------------------------------------------------------------

_C_FUNC_MAP = {
    # provsql.c — planner hook and query rewriting
    '_PG_init':                  '/doxygen-c/html/provsql_8c.html#a29e1a0b0688ac19dbde93824e4ae1a59',
    '_PG_fini':                  '/doxygen-c/html/provsql_8c.html#a7192e52d759211f57ad66638304ea072',
    'provsql_planner':           '/doxygen-c/html/provsql_8c.html#aa8f430f67b70c269c4ba8cc5225b8a84',
    'process_query':             '/doxygen-c/html/provsql_8c.html#a5901d4216b7b5c71ddd9b82956ddb489',
    'has_provenance':            '/doxygen-c/html/provsql_8c.html#af9a93235f73a9ae63ab01cf094d30372',
    'get_provenance_attributes': '/doxygen-c/html/provsql_8c.html#a468eafaad0a1eabc3988d6ab0b824abe',
    'make_provenance_expression':'/doxygen-c/html/provsql_8c.html#ad6c4894f1cd6dac66538064fba556517',
    'add_to_select':             '/doxygen-c/html/provsql_8c.html#a6fe52ea4c7f2cc8eb924135ebf239d85',
    'replace_provenance_function_by_expression': '/doxygen-c/html/provsql_8c.html#a3d5fee9c96595db519504978edba8683',
    'process_insert_select':     '/doxygen-c/html/provsql_8c.html#ac3ee0aa66fe553ba28a2bb2959a440ad',
    'inline_ctes':               '/doxygen-c/html/provsql_8c.html#a7190c262fb133f2796fddfb02c695c3c',
    'remove_provenance_attributes_select': '/doxygen-c/html/provsql_8c.html#ab49a6e80331db4c440e34e0b2ec77f14',
    'rewrite_non_all_into_external_group_by': '/doxygen-c/html/provsql_8c.html#aa6aa776cd1d015ffcc6aa21c8b2d2198',
    'process_set_operation_union':'/doxygen-c/html/provsql_8c.html#a61a2f1f0094bc8b60010673f568f16e7',
    'transform_except_into_join':'/doxygen-c/html/provsql_8c.html#a0fdbe2fb23cbd18fb9bcc873dc9e888c',
    'rewrite_agg_distinct':      '/doxygen-c/html/provsql_8c.html#a94b33c910ea1c3ceac4a2e32e79328f9',
    'transform_distinct_into_group_by': '/doxygen-c/html/provsql_8c.html#a00905dfe8e8acbfb740bafab0c5f366b',
    'build_column_map':          '/doxygen-c/html/provsql_8c.html#acf66b583d7d5c92186c17e172ab34edf',
    'replace_aggregations_by_provenance_aggregate': '/doxygen-c/html/provsql_8c.html#a64dcbec21c99e399191a997b19da111c',
    'migrate_aggtoken_quals_to_having': '/doxygen-c/html/provsql_8c.html#a52800ea34fc58e9157fe76ecb7dfeac7',
    'insert_agg_token_casts':    '/doxygen-c/html/provsql_8c.html#a539df516de849eb54876a4f98a748861',
    'having_Expr_to_provenance_cmp': '/doxygen-c/html/provsql_8c.html#a0cfaf66fa75b9265bf267b446ac6946f',
    'add_eq_from_Quals_to_Expr': '/doxygen-c/html/provsql_8c.html#aa5f16ef0c73e1c7d651b02311994605d',
    'add_select_non_zero':       '/doxygen-c/html/provsql_8c.html#af0e331134cb117618a2d8197a5cf7b76',
    # provsql_utils.h — OID cache
    'constants_t':               '/doxygen-c/html/structconstants__t.html',
    'get_constants':             '/doxygen-c/html/provsql__utils_8h.html#a75e7d48321cea0156f8ad4c039c877a0',
    # provsql_mmap — background worker
    'RegisterProvSQLMMapWorker': '/doxygen-c/html/provsql__mmap_8c.html#af31c1c517f22a6923f390b75d36506be',
    'provsql_mmap_worker':       '/doxygen-c/html/provsql__mmap_8h.html#a3f084145f583f08b2532c36a79925697',
    'initialize_provsql_mmap':   '/doxygen-c/html/MMappedCircuit_8cpp.html#aa0bba27d6f73596ef0972bb0541cc244',
    'provsql_mmap_main_loop':    '/doxygen-c/html/MMappedCircuit_8cpp.html#a9215628e0312d309db481dbd27c8dabe',
    'destroy_provsql_mmap':      '/doxygen-c/html/MMappedCircuit_8cpp.html#a77b47980f1cc7b3e38ad65e15bda8118',
    # provsql_shmem — shared memory
    'provsql_shmem_request':     '/doxygen-c/html/provsql__shmem_8h.html#a6bd8422002b87f1cab0c60e4aa7a7101',
    'provsql_shmem_startup':     '/doxygen-c/html/provsql__shmem_8h.html#a7f431fc2ca237114d13ff1c19f7ab692',
    'provsql_shmem_lock_exclusive': '/doxygen-c/html/provsql__shmem_8h.html#a36adeebed75d77964c2886bf4718d8bc',
    'provsql_shmem_lock_shared': '/doxygen-c/html/provsql__shmem_8h.html#ae1cfc059b74073f9ee4e12f34793be64',
    'provsql_shmem_unlock':      '/doxygen-c/html/provsql__shmem_8h.html#a22c7757d55d371f7fe70c542341c0365',
    # provenance_evaluate_compiled
    'provenance_evaluate_compiled': '/doxygen-c/html/provenance__evaluate__compiled_8hpp.html#a4bf8b02981ab738526f4fe50799894ec',
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

    def cfunc_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        key = text.rstrip('()')
        url = _C_FUNC_MAP.get(key)
        lit = nodes.literal(text, text)
        if url:
            ref = nodes.reference('', '', internal=False, refuri=url)
            ref += lit
            return [ref], []
        return [lit], []

    def cfile_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        # Compute Doxygen URL from filename: _ -> __, . -> _8
        base, ext = text.rsplit('.', 1)
        doxy_name = base.replace('_', '__') + '_8' + ext + '.html'
        url = '/doxygen-c/html/' + doxy_name
        lit = nodes.literal(text, text)
        ref = nodes.reference('', '', internal=False, refuri=url)
        ref += lit
        return [ref], []

    app.add_role('sqlfunc', sqlfunc_role)
    app.add_role('cfunc', cfunc_role)
    app.add_role('cfile', cfile_role)
    return {'version': '0.1', 'parallel_read_safe': True}
