project = 'ProvSQL'
copyright = '2025, Pierre Senellart'
author = 'Pierre Senellart'

extensions = ['sphinx.ext.todo', 'sphinx.ext.graphviz', 'sphinxcontrib.bibtex', 'sphinx_copybutton', 'sphinx.ext.imgmath']

# Render math at build time as SVG via LaTeX + dvisvgm rather than
# letting Sphinx's default MathJax handler fetch
# https://cdn.jsdelivr.net/npm/mathjax@3/... at every page load.
# Files go to _images/math/<content-hash>.svg so unchanged formulas
# stay cached across builds (imgmath_embed=True would inline them in
# the HTML, but then there's no on-disk cache and each build re-runs
# LaTeX on every formula -- ~50s for ~136 formulas).  Local relative
# img refs are still zero outgoing requests.
imgmath_image_format = 'svg'
imgmath_use_preview = True
imgmath_font_size = 14

# |cpp| / |cpp17| substitutions: render "C++" and "C++17" as non-breaking
# spans so the browser never wraps between "C+" and the final "+".
rst_prolog = """
.. |cpp| raw:: html

   <span style="white-space: nowrap">C++</span>

.. |cpp17| raw:: html

   <span style="white-space: nowrap">C++17</span>
"""

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
    # Shim runs before copybutton.js (default priority 500) and back-fills
    # DOCUMENTATION_OPTIONS.URL_ROOT for sphinx-copybutton 0.4.0 on Sphinx >= 5.
    ('copybutton-shim.js', {'priority': 450}),
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
    'circuit_subgraph':         '/doxygen-sql/html/group__circuit__introspection.html#ga268dd3b17c88aab9942f9f510a4d36f8',
    'resolve_input':            '/doxygen-sql/html/group__circuit__introspection.html#gace5412596beb2701ce516bbbac62aab2',
    'gate_zero':                '/doxygen-sql/html/group__internal__constants.html#ga13455054a9c6a90a9088d8666b105812',
    'gate_one':                 '/doxygen-sql/html/group__internal__constants.html#ga27aad07a447f5b1f17018ac09ef4ea32',

    'set_prob':                 '/doxygen-sql/html/group__gate__manipulation.html#ga17334fe1d3969d969eaa18e32a93b4e0',
    'create_gate':              '/doxygen-sql/html/group__gate__manipulation.html#ga47adb345beee16ad65566b8e2ca96f8a',
    'provenance_aggregate':     '/doxygen-sql/html/group__aggregate__provenance.html#ga05b57063566479cfcb7af3e0b361aef6',
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
    'sr_boolexpr':              '/doxygen-sql/html/group__compiled__semirings.html#ga41016a3000b008b59848687485b79425',
    'sr_counting':              '/doxygen-sql/html/group__compiled__semirings.html#gad3e2b6c5dc5d0041fc29ee086add1de6',
    'sr_why':                   '/doxygen-sql/html/group__compiled__semirings.html#ga7501c5d75cf9a2920e9176169c310a7f',
    'sr_how':                   '/doxygen-sql/html/group__compiled__semirings.html#gaa4900646bb610cc5159c8dad0b74bb95',
    'sr_which':                 '/doxygen-sql/html/group__compiled__semirings.html#ga86da4e2a0f5c0f865e9e30714748679c',
    'sr_tropical':              '/doxygen-sql/html/group__compiled__semirings.html#gaa2a6ec05f977d18204b1b6181dee28b5',
    'sr_viterbi':               '/doxygen-sql/html/group__compiled__semirings.html#ga67d375630a9674bfecd4f87d0d003d31',
    'sr_lukasiewicz':           '/doxygen-sql/html/group__compiled__semirings.html#ga27ff464e570641a7b195ed3578a66ab1',
    'sr_minmax':                '/doxygen-sql/html/group__compiled__semirings.html#ga1861588ff5f551d207f332bccab4626c',
    'sr_maxmin':                '/doxygen-sql/html/group__compiled__semirings.html#gaebd28a3deead5cd455176f19a848e620',
    'sr_temporal':              '/doxygen-sql/html/group__temporal__db.html#ga53665747f710e8bacbf9bb985420c173',
    'sr_interval_num':          '/doxygen-sql/html/group__temporal__db.html#gaa35eca68f37290485c5319697b408d3a',
    'sr_interval_int':          '/doxygen-sql/html/group__temporal__db.html#ga62d18db08c8b0b341d271ccad61f2c82',
    'to_provxml':               '/doxygen-sql/html/group__provenance__output.html#gacc9e8b2a47ade6f5c87c27f64b4707b1',
    'view_circuit':             '/doxygen-sql/html/group__provenance__output.html#ga1c6caf1bb91c0cfdfc56c33666d41897',
    'where_provenance':         '/doxygen-sql/html/group__provenance__output.html#ga6ddf85fea18edbd973c118020ff4551b',
    'repair_key':               '/doxygen-sql/html/group__table__management.html#gacfbe67438895e5f153b01bb9352cbc54',
    'undo':                     '/doxygen-sql/html/group__temporal__db.html#ga0f47a56547d5a63e7d3fe18a0c1c5d00',
    'replace_the_circuit':      '/doxygen-sql/html/group__temporal__db.html#gafe22f0ed3671613461298c2a35e6fdc7',
    'union_tstzintervals':      '/doxygen-sql/html/group__temporal__db.html#ga2ba50d413598d163083ab577e53e844c',
    'get_valid_time':           '/doxygen-sql/html/group__temporal__db.html#gaf71e0f3466ba453b0d27182b6f4b9ad0',
    'timetravel':               '/doxygen-sql/html/group__temporal__db.html#ga73181ef9e1e7b5f293ae379414fe7d63',
    'timeslice':                '/doxygen-sql/html/group__temporal__db.html#gaa3de6e26f960ee27e916a1c35fbb75f0',
    'history':                  '/doxygen-sql/html/group__temporal__db.html#gac96504e5f0f7bf9da1dfc089cdbcdd21',
    'agg_token_value_text':     '/doxygen-sql/html/group__agg__token__type.html#gadec6242b3b9213ae9dc16c1a15831b03',
    # Continuous-distribution constructors and aggregates
    'normal':                   '/doxygen-sql/html/group__random__variable__type.html#ga1a974bad82d83b110e9d158083b113ce',
    'uniform':                  '/doxygen-sql/html/group__random__variable__type.html#ga35f5bd84e907e0a7bffd9281abd55c68',
    'exponential':              '/doxygen-sql/html/group__random__variable__type.html#ga763f1ce322fcbb082ffe25defbe68e47',
    'erlang':                   '/doxygen-sql/html/group__random__variable__type.html#gaebceaebb5cdae98469affb8742bb83a9',
    'categorical':              '/doxygen-sql/html/group__random__variable__type.html#ga41d074e6a6e06d585efda49edd32f0a5',
    'mixture':                  '/doxygen-sql/html/group__random__variable__type.html#gabb228422bc96460b22ee9f75c5a4144e',
    'as_random':                '/doxygen-sql/html/group__random__variable__type.html#ga0826bcc083d15b685e71783a006395f4',
    'sum':                      '/doxygen-sql/html/group__random__variable__type.html#ga833d8a50c45061fdb7f302067a9f0bf1',
    'avg':                      '/doxygen-sql/html/group__random__variable__type.html#ga3e227bce63085af57849b6f021d3992e',
    'product':                  '/doxygen-sql/html/group__random__variable__type.html#ga1e24aafc2de11c41db2f0239feb490de',
    # Polymorphic moment / support dispatchers
    'variance':                 '/doxygen-sql/html/group__probability.html#gab1d52fb442dd262c0ac950d349a87ca3',
    'moment':                   '/doxygen-sql/html/group__probability.html#ga99a2c256122559c1580459699288c20d',
    'central_moment':           '/doxygen-sql/html/group__probability.html#ga738a4694f2c2a43e74414cbbc19739a8',
    'support':                  '/doxygen-sql/html/group__probability.html#ga52dbe84c73d98c50a25784afa173183f',
    # Sampling / histogram / simplifier introspection
    'rv_histogram':             '/doxygen-sql/html/group__circuit__introspection.html#ga4e287ff30a597e203f43c80d12098c89',
    'rv_sample':                '/doxygen-sql/html/group__circuit__introspection.html#gaec7d70d0f94f8225861e3377682ce348',
    'simplified_circuit_subgraph': '/doxygen-sql/html/group__circuit__introspection.html#ga7717079ec6b1f50ecb1a5a9fd5b15531',
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
    'make_aggregation_expression': '/doxygen-c/html/provsql_8c.html#abaae0dcfec89c61c6af4c309eb61c8b4',
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
    'migrate_probabilistic_quals': '/doxygen-c/html/provsql_8c.html#a807c78bed512a1e85cfd8da28a75078e',
    'insert_agg_token_casts':    '/doxygen-c/html/provsql_8c.html#a539df516de849eb54876a4f98a748861',
    'having_Expr_to_provenance_cmp': '/doxygen-c/html/provsql_8c.html#a0cfaf66fa75b9265bf267b446ac6946f',
    'add_eq_from_Quals_to_Expr': '/doxygen-c/html/provsql_8c.html#aa5f16ef0c73e1c7d651b02311994605d',
    'add_eq_from_OpExpr_to_Expr':'/doxygen-c/html/provsql_8c.html#abed26c95056d10b1f670bd37d840d989',
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
    # Aggregation dispatch
    'getAggregationOperator':    '/doxygen-c/html/Aggregation_8h.html#a6f4361e91675b6d5fda821466ba599ea',
    'makeAggregator':            '/doxygen-c/html/Aggregation_8h.html#ad322b2be8077685d576affbc4d4b9aff',
    # C++ classes
    'BooleanCircuit':            '/doxygen-c/html/classBooleanCircuit.html',
    'GenericCircuit':            '/doxygen-c/html/classGenericCircuit.html',
    'WhereCircuit':              '/doxygen-c/html/classWhereCircuit.html',
    'WhereGate':                 '/doxygen-c/html/WhereCircuit_8h.html#ad1db0a23d0d94896d8008c8e65df2ece',
    'Semiring':                  '/doxygen-c/html/classsemiring_1_1Semiring.html',
    'CircuitException':          '/doxygen-c/html/classCircuitException.html',
    'dDNNF':                     '/doxygen-c/html/classdDNNF.html',
    'TreeDecomposition':         '/doxygen-c/html/classTreeDecomposition.html',
    'dDNNFTreeDecompositionBuilder': '/doxygen-c/html/classdDNNFTreeDecompositionBuilder.html',
    'MMappedCircuit':            '/doxygen-c/html/classMMappedCircuit.html',
    'GateInformation':           '/doxygen-c/html/structGateInformation.html',
    'MMappedUUIDHashTable':      '/doxygen-c/html/classMMappedUUIDHashTable.html',
    'MMappedVector':             '/doxygen-c/html/classMMappedVector.html',
    'Aggregator':                '/doxygen-c/html/structAggregator.html',
    'SumAgg':                    '/doxygen-c/html/structSumAgg.html',
    'MinAgg':                    '/doxygen-c/html/structMinAgg.html',
    'AvgAgg':                    '/doxygen-c/html/structAvgAgg.html',
    'agg_token':                 '/doxygen-c/html/structagg__token.html',
    'provsqlSharedState':        '/doxygen-c/html/structprovsqlSharedState.html',
    'SemiringException':         '/doxygen-c/html/classsemiring_1_1SemiringException.html',
    'BoolExpr':                  '/doxygen-c/html/classsemiring_1_1BoolExpr.html',
    'Boolean':                   '/doxygen-c/html/classsemiring_1_1Boolean.html',
    'Counting':                  '/doxygen-c/html/classsemiring_1_1Counting.html',
    'IntervalUnion':             '/doxygen-c/html/classsemiring_1_1IntervalUnion.html',
    # Enums and types
    'AggregationOperator':       '/doxygen-c/html/Aggregation_8h.html#a07e6885296f8f80441d5428bcf72af5a',
    'gate_type':                 '/doxygen-c/html/provsql__utils_8h.html#a8266c44a6cbf9e08d5f2b12cd3c3d92f',
    # Other functions
    'aggregation_evaluate':      '/doxygen-c/html/aggregation__evaluate_8c.html#a4f4a0f60a801ee663671454787aecbab',
    'getGenericCircuit':         '/doxygen-c/html/CircuitFromMMap_8h.html#aea0e3c624d65cc59c68d313fbe0306af',
    'getBooleanCircuit':         '/doxygen-c/html/CircuitFromMMap_8h.html#a66a011a61d08d6fd604c4668a62d46d0',
    'provenance_evaluate_compiled_internal': '/doxygen-c/html/provenance__evaluate__compiled_8cpp.html#a3df67864ec62b62e0b10e5a3d7945c21',
    'list_insert_nth':           '/doxygen-c/html/compatibility_8c.html#af40069a7906dd7dabbd6ab5fcd429090',
    # Macros
    'STARTWRITEM':               '/doxygen-c/html/provsql__mmap_8h.html#a71fa51d40b2e5caeaf49eaf95b85033a',
    'ADDWRITEM':                 '/doxygen-c/html/provsql__mmap_8h.html#a8ff2a88528de32f06edc2c2df8aec35c',
    'SENDWRITEM':                '/doxygen-c/html/provsql__mmap_8h.html#a97801fbf53859b521789af0fe79632d8',
    # BooleanCircuit methods
    'BooleanCircuit::independentEvaluation': '/doxygen-c/html/classBooleanCircuit.html#aec2e2de236b6ba8915e2088ae3bfa256',
    'BooleanCircuit::possibleWorlds':        '/doxygen-c/html/classBooleanCircuit.html#a211a22d87946d2c2878f5da2cd28109c',
    'BooleanCircuit::monteCarlo':            '/doxygen-c/html/classBooleanCircuit.html#a85ca1a4d91d62aa1b4660c42b31ceadd',
    'BooleanCircuit::WeightMC':              '/doxygen-c/html/classBooleanCircuit.html#a08b28d72e2ddf6be066630bb1c5a59b0',
    'BooleanCircuit::compilation':           '/doxygen-c/html/classBooleanCircuit.html#ad3bc6b4c5643a25d46083bb72e1fb485',
    'BooleanCircuit::makeDD':                '/doxygen-c/html/classBooleanCircuit.html#a699b0537bdaf692ff18e36ad32064d6e',
    'dDNNF::probabilityEvaluation':          '/doxygen-c/html/classdDNNF.html#a3c62965b37ba9e45f59fee50f432eb6c',
    'dDNNF::shapley':                        '/doxygen-c/html/classdDNNF.html#a8f101e081f8e8a8c72b2379dfa9a001f',
    'dDNNF::banzhaf':                        '/doxygen-c/html/classdDNNF.html#a92f93d9b218dc575ea4875a13588627a',
    'dDNNF::makeSmooth':                     '/doxygen-c/html/classdDNNF.html#a794e4aeb3d90d0a64256684ea9f9435a',
    'dDNNF::makeGatesBinary':                '/doxygen-c/html/classdDNNF.html#a7f10e16db401187a3b8ab69ecc2afa42',
    'dDNNF::condition':                      '/doxygen-c/html/classdDNNF.html#a346a7c002cd94842b6553d3727c5e9b3',
    'shapley_internal':                      '/doxygen-c/html/shapley_8cpp.html#a4703d29e438b3454874017304a945d67',
    # CircuitCache
    'CircuitCache':                          '/doxygen-c/html/classCircuitCache.html',
    'CircuitCache::insert':                  '/doxygen-c/html/classCircuitCache.html#a9485b5217b632c047ab95e6aaf70f819',
    'circuit_cache_create_gate':             '/doxygen-c/html/circuit__cache_8h.html#a6b4f0989a71a5a3c4e9f9fe0c7be9387',
    'circuit_cache_get_type':                '/doxygen-c/html/circuit__cache_8h.html#a02ad2ad53de11ae321b83c29854d5f77',
    'circuit_cache_get_children':            '/doxygen-c/html/circuit__cache_8h.html#a0a088536b890aa34c75b319f4484ee03',
    # probability_evaluate.cpp
    'probability_evaluate_internal': '/doxygen-c/html/probability__evaluate_8cpp.html#aea39f7b497660c1c1e384b1e090d7db9',
    'BooleanCircuit::rewriteMultivaluedGates': '/doxygen-c/html/classBooleanCircuit.html#a90ff865c9963480f00000d62c6c37c2f',
    'BooleanCircuit::rewriteMultivaluedGatesRec': '/doxygen-c/html/classBooleanCircuit.html#a3f6c1d03227119c7f4886cbf34090022',
    'BooleanCircuit::Tseytin':   '/doxygen-c/html/classBooleanCircuit.html#a63c58e27582f0e598fae1d0c9329f53f',
    'GenericCircuit::evaluate':  '/doxygen-c/html/classGenericCircuit.html#a3ff1b0c90156515393986082def83900',
    'TreeDecomposition::MAX_TREEWIDTH': '/doxygen-c/html/classTreeDecomposition.html#ab09a529b1ef0e64fecf2260345c795d3',
    'TreeDecomposition::makeFriendly': '/doxygen-c/html/classTreeDecomposition.html#ad4ddde20abe4dae4f282945c3b666b05',
    # DotCircuit
    'DotCircuit::render':        '/doxygen-c/html/classDotCircuit.html#aef126e92723bd4f229e656c4e8ff704e',
    # External-tool helpers
    'run_external_tool':         '/doxygen-c/html/external__tool_8cpp.html#a0fc04107a884e66f0393356432a354c2',
    'find_external_tool':        '/doxygen-c/html/external__tool_8cpp.html#afc4e5f1da9e5c67b5476d87a80726739',
    'format_external_tool_status': '/doxygen-c/html/external__tool_8cpp.html#a9dea3c210f8e5188da8c3d56d6d21f2a',
    # Global state
    'provsql_interrupted':       '/doxygen-c/html/provsql__utils_8h.html#a9692a0205a857ed2cc29558470c2ed77',
    # Error reporting macros
    'provsql_error':             '/doxygen-c/html/provsql__error_8h.html#aad553d1e9e68bc5ad84b4a1d3a5302d1',
    'provsql_warning':           '/doxygen-c/html/provsql__error_8h.html#ae09e94e07fbfb516602a4107a22f585f',
    'provsql_notice':            '/doxygen-c/html/provsql__error_8h.html#afbe294b31b29e4751ea314a56f55c34d',
    'provsql_log':               '/doxygen-c/html/provsql__error_8h.html#ab9cf852f1266fcb34de6de14df33a9f9',
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

    def sqlfile_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        # Same encoding as cfile but pointing at the SQL Doxygen output.
        base, ext = text.rsplit('.', 1)
        doxy_name = base.replace('_', '__') + '_8' + ext + '.html'
        url = '/doxygen-sql/html/' + doxy_name
        lit = nodes.literal(text, text)
        ref = nodes.reference('', '', internal=False, refuri=url)
        ref += lit
        return [ref], []

    def sc_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        return [nodes.inline(rawtext, text, classes=['smallcaps'])], []

    app.add_role('sqlfunc', sqlfunc_role)
    app.add_role('cfunc', cfunc_role)
    app.add_role('cfile', cfile_role)
    app.add_role('sqlfile', sqlfile_role)
    app.add_role('sc', sc_role)
    return {'version': '0.1', 'parallel_read_safe': True}
