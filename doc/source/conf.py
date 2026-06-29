project = 'ProvSQL'
copyright = '2025, Pierre Senellart'
author = 'Pierre Senellart'

extensions = ['sphinx.ext.todo', 'sphinx.ext.graphviz', 'sphinxcontrib.bibtex', 'sphinx_copybutton', 'sphinx.ext.imgmath', 'sphinx_sitemap', 'sphinxext.opengraph']

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
# Match the surrounding prose; the imgmath default (12) and 14 both render
# noticeably smaller than the RTD body text.
imgmath_font_size = 16

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

# Canonical URL of the deployed docs: makes Sphinx emit
# <link rel="canonical"> on every page and anchors the sitemap that
# sphinx_sitemap generates (referenced from the website's robots.txt).
html_baseurl = 'https://provsql.org/docs/'
sitemap_url_scheme = '{link}'
sitemap_excludes = ['search.html', 'genindex.html']

# Explicit title: the default is "<project> <release> documentation" and
# the docs are deployed continuously, so a baked-in release would go
# stale (and an empty one yields "ProvSQL  documentation").
html_title = 'ProvSQL documentation'

# Per-page Open Graph tags + <meta name="description"> (auto-extracted
# from each page's first paragraph) for search-engine and social embeds.
ogp_site_url = 'https://provsql.org/docs/'
ogp_site_name = 'ProvSQL documentation'
ogp_image = 'https://provsql.org/docs/_static/logo.png'
ogp_enable_meta_description = True

html_theme = 'sphinx_rtd_theme'
html_theme_options = {
    'style_external_links': True,
    'collapse_navigation': False,
    'style_nav_header_background': '#6B4FA0',
}
html_static_path = ['_static']
html_logo    = '_static/logo.png'
html_favicon = '_static/favicon.ico'
html_css_files = [
    'custom.css',
    # Font Awesome, same release as Studio's index.html: the :fa: role
    # mirrors the icons Studio shows on its buttons and actions.
    'https://use.fontawesome.com/releases/v5.15.4/css/all.css',
]
html_js_files = [
    ('jquery.js', {'priority': 100}),  # sphinx-rtd-theme requires jQuery
    'back-to-site.js',
    'sidebar-toggle.js',  # floating button to collapse the navigation sidebar
    # Shim runs before copybutton.js (default priority 500) and back-fills
    # DOCUMENTATION_OPTIONS.URL_ROOT for sphinx-copybutton 0.4.0 on Sphinx >= 5.
    ('copybutton-shim.js', {'priority': 450}),
]
html_show_sphinx = False
html_show_copyright = False
html_show_sourcelink = False

# ---------------------------------------------------------------------------
# :sqlfunc:`name` role – renders function name as linked monospace code
# pointing to the Doxygen SQL API reference.
# ---------------------------------------------------------------------------

_SQL_FUNC_MAP = {
    'add_provenance':           '/doxygen-sql/html/group__table__management.html#ga33ff696dabb05d813f2c1f914cb97d9a',
    'remove_provenance':        '/doxygen-sql/html/group__table__management.html#gaef50d1a0c3d614e4373737bd07ed3879',
    'provenance':               '/doxygen-sql/html/group__provenance__output.html#gacb0ee8a16e9a316f163f4508a0e7c15d',
    'create_provenance_mapping':'/doxygen-sql/html/group__table__management.html#ga25acf43d252482d775e4ac5cd8daab84',
    'get_gate_type':            '/doxygen-sql/html/group__gate__manipulation.html#ga1731181eef2acf0b0590edd23b3ccb49',
    'get_children':             '/doxygen-sql/html/group__gate__manipulation.html#gad21ff35d95b5d782bf594eec99c1ee03',
    'get_infos':                '/doxygen-sql/html/group__gate__manipulation.html#ga0e9400fe75daab71e71b6c2561ab182b',
    'get_extra':                '/doxygen-sql/html/group__gate__manipulation.html#ga352bdd7824cf64daa25ab07a345796ff',
    'get_nb_gates':             '/doxygen-sql/html/group__gate__manipulation.html#gacc16e47ab21d01de696c6323453d3515',
    'identify_token':           '/doxygen-sql/html/group__circuit__introspection.html#ga5d3eca5ba1f2078973291c96cccae7e5',
    'circuit_subgraph':         '/doxygen-sql/html/group__circuit__introspection.html#gaac1b3097a1f676dd29deb759f5010fd0',
    'resolve_input':            '/doxygen-sql/html/group__circuit__introspection.html#ga9039e7ab8855ae72741cd81b7c569f95',
    'gate_zero':                '/doxygen-sql/html/group__internal__constants.html#gad6483fe9b8be26525d947a1d28c39db7',
    'gate_one':                 '/doxygen-sql/html/group__internal__constants.html#ga5cbc50cf69b66b632d603ce5e30f2a36',

    'set_prob':                 '/doxygen-sql/html/group__gate__manipulation.html#ga94dcc4874244a0c50c60608ab8506cca',
    'create_gate':              '/doxygen-sql/html/group__gate__manipulation.html#gaa4cb8ee40a5a5c6052337e3459265a37',
    'cond':                     '/doxygen-sql/html/group__gate__manipulation.html#gadcfdb768b535ecf0ef01408ea36b5bdb',
    'given':                    '/doxygen-sql/html/group__gate__manipulation.html#ga6a91567195416197b6fa467074a117e1',
    'provenance_not':           '/doxygen-sql/html/group__gate__manipulation.html#ga8c688204268d0eb109c443e41b5d7aa6',
    'assume_boolean':           '/doxygen-sql/html/group__gate__manipulation.html#ga22d113fe91f5c16bf43bc020158b4006',
    'annotate':                 '/doxygen-sql/html/group__gate__manipulation.html#ga7daa014a8fd43c3f924637e576e002d5',
    'inversion_free_key':       '/doxygen-sql/html/group__gate__manipulation.html#gacc16e47ab21d01de696c6323453d3515',
    'provenance_guard':         '/doxygen-sql/html/group__table__management.html#ga978755a7d3a0b8af0abc128b1fc776e2',
    'provenance_aggregate':     '/doxygen-sql/html/group__aggregate__provenance.html#gaaa4ca20eebffb9f086eae1bfd2bcc8b9',
    'provenance_times':         '/doxygen-sql/html/group__semiring__operations.html#ga2ac3939267cec756cc2d5f05314e26ff',
    'provenance_assume':        '/doxygen-sql/html/group__gate__manipulation.html#ga86cba21f20d0cc608ab6852b53fa904e',
    'ucq_joint_provenance':     '/doxygen-sql/html/group__probability.html#gaabc5e4c25277000ffcdd9ec48e8e70cf',
    'ucq_mobius_provenance':    '/doxygen-sql/html/group__probability.html#ga4d1240a7794f600f679716788507d5a4',
    'provenance_plus':          '/doxygen-sql/html/group__semiring__operations.html#gaac5b2a2265ee8f8c9629c07d2d5ada84',
    'get_prob':                 '/doxygen-sql/html/group__gate__manipulation.html#ga859702367ce6d65ff3a020ed3122157d',
    'probability_evaluate':     '/doxygen-sql/html/group__probability.html#gad377e94cb1fff57141b1950cc4169c5e',
    'probability_bounds':       '/doxygen-sql/html/group__probability.html#ga80dd344ea08b2743e2f69c7f854af7f2',
    # Two-terminal network reliability on bounded-treewidth data
    # (columnar internals; the user-facing route is WITH RECURSIVE under
    # provsql.boolean_provenance).
    'provenance_evaluate':      '/doxygen-sql/html/group__semiring__evaluation.html#ga0a30e01db8afbc4c645e2a2b4924d676',
    'choose':                   '/doxygen-sql/html/group__choose__aggregate.html#ga020d50a4e5ed39e5d037dd5e313f8266',
    'explode_table':            '/doxygen-sql/html/group__choose__aggregate.html#ga1d3d4d92bd58980f300ffc53c2b467da',
    'shapley':                  '/doxygen-sql/html/group__probability.html#gac15ed9fd2522383b11c6d248d6d43b2c',
    'shapley_all_vars':         '/doxygen-sql/html/group__probability.html#gac525ad16a481e8ab835b5c3c03320d92',
    'banzhaf':                  '/doxygen-sql/html/group__probability.html#ga90eb00153b761641e8d2de5d273d1832',
    'banzhaf_all_vars':         '/doxygen-sql/html/group__probability.html#gad2b5d049625b2eb6e12c74840de7d7d3',
    'expected':                 '/doxygen-sql/html/group__probability.html#ga362bcce6a7edb8e25174e64017ae9aba',
    'sr_formula':               '/doxygen-sql/html/group__compiled__semirings.html#ga3aad4775805b92307f3ca9e1b9fbd4c5',
    'sr_boolean':               '/doxygen-sql/html/group__compiled__semirings.html#gad6d630b56fc0466db0a7907e67d69777',
    'sr_boolexpr':              '/doxygen-sql/html/group__compiled__semirings.html#gaf221001c4d22d033906fdcaa82864f7a',
    'sr_counting':              '/doxygen-sql/html/group__compiled__semirings.html#gadc339f3d4c3289e3b6fa89a9331ec051',
    'sr_why':                   '/doxygen-sql/html/group__compiled__semirings.html#ga477a71b9fe533fc607eb5fc5a7577680',
    'sr_how':                   '/doxygen-sql/html/group__compiled__semirings.html#ga39ac0dd52708725c6c047823ab37e6c4',
    'sr_which':                 '/doxygen-sql/html/group__compiled__semirings.html#gabae98ab031fa28f65d4c88a10441c8ce',
    'sr_tropical':              '/doxygen-sql/html/group__compiled__semirings.html#gacf4b60b02ee46941e0636801af9f2354',
    'sr_viterbi':               '/doxygen-sql/html/group__compiled__semirings.html#ga49c9145c1f3c80a821e78ce8b46d4eb8',
    'sr_lukasiewicz':           '/doxygen-sql/html/group__compiled__semirings.html#ga51a3fd37af829caf570b3a5b799140f2',
    'sr_minmax':                '/doxygen-sql/html/group__compiled__semirings.html#ga21bc3ee408f01de06e82ff21e7a01458',
    'sr_maxmin':                '/doxygen-sql/html/group__compiled__semirings.html#ga411aa37bbf77e0a2bd1c2e70590f8aed',
    'sr_temporal':              '/doxygen-sql/html/group__temporal__db.html#ga53665747f710e8bacbf9bb985420c173',
    'sr_interval_num':          '/doxygen-sql/html/group__temporal__db.html#gaa35eca68f37290485c5319697b408d3a',
    'sr_interval_int':          '/doxygen-sql/html/group__temporal__db.html#ga62d18db08c8b0b341d271ccad61f2c82',
    'to_provxml':               '/doxygen-sql/html/group__provenance__output.html#ga60e8ffbac337ba6cb9e32b64672b802f',
    'view_circuit':             '/doxygen-sql/html/group__provenance__output.html#ga9c312a8b72b968e3669432ab5ae49928',
    'where_provenance':         '/doxygen-sql/html/group__provenance__output.html#ga8c44230f117ba8a01f6322ec1d32f5db',
    'repair_key':               '/doxygen-sql/html/group__table__management.html#ga6fabf2543dbb5b42ae805874867aa87e',
    'undo':                     '/doxygen-sql/html/group__temporal__db.html#ga0f47a56547d5a63e7d3fe18a0c1c5d00',
    'replace_the_circuit':      '/doxygen-sql/html/group__temporal__db.html#gafe22f0ed3671613461298c2a35e6fdc7',
    'union_tstzintervals':      '/doxygen-sql/html/group__temporal__db.html#ga2ba50d413598d163083ab577e53e844c',
    'get_valid_time':           '/doxygen-sql/html/group__temporal__db.html#gaf71e0f3466ba453b0d27182b6f4b9ad0',
    'timetravel':               '/doxygen-sql/html/group__temporal__db.html#ga73181ef9e1e7b5f293ae379414fe7d63',
    'timeslice':                '/doxygen-sql/html/group__temporal__db.html#gaa3de6e26f960ee27e916a1c35fbb75f0',
    'history':                  '/doxygen-sql/html/group__temporal__db.html#gac96504e5f0f7bf9da1dfc089cdbcdd21',
    'agg_token_value_text':     '/doxygen-sql/html/group__agg__token__type.html#gadf04d87efab356f790602629d12f26f6',
    # Continuous-distribution constructors and aggregates
    'normal':                   '/doxygen-sql/html/group__random__variable__type.html#gac140b1f9bcc4949f21963cf8e5e98c4c',
    'uniform':                  '/doxygen-sql/html/group__random__variable__type.html#ga9f416167141b28eb56f3ae2d340785b8',
    'exponential':              '/doxygen-sql/html/group__random__variable__type.html#ga1b3c2233e34a026b8c32c81828c3ca83',
    'erlang':                   '/doxygen-sql/html/group__random__variable__type.html#ga4a6880ef663e073e187de58628107f2d',
    'categorical':              '/doxygen-sql/html/group__random__variable__type.html#ga7dd6e75bde4c70b80c07e5e29478a801',
    'mixture':                  '/doxygen-sql/html/group__random__variable__type.html#gab2b650b7c42f7e0ab69c91d447a756b0',
    'as_random':                '/doxygen-sql/html/group__random__variable__type.html#ga493b093cde0b627ee7bd02cd54b31802',
    'sum':                      '/doxygen-sql/html/group__random__variable__type.html#ga80cd04941bd7d2fabbfa03b6eea5c280',
    'avg':                      '/doxygen-sql/html/group__random__variable__type.html#ga0243c3a6d53bdc36a89ce5131b38e504',
    'product':                  '/doxygen-sql/html/group__random__variable__type.html#gab9637eb60cf163df2f7c44ded0d5b50b',
    # Polymorphic moment / support dispatchers
    'variance':                 '/doxygen-sql/html/group__probability.html#gace60130ef91b47ec9c08a2c48a111a19',
    'moment':                   '/doxygen-sql/html/group__probability.html#gad4666dfca3ec9eb3b5b7bcb2a1ad51ad',
    'central_moment':           '/doxygen-sql/html/group__probability.html#gaeb533aa10cd3104d5b45b5c4e006d30c',
    'support':                  '/doxygen-sql/html/group__probability.html#gab17c5b012153cf3c202bf1d3bb440cd3',
    # Sampling / histogram / simplifier introspection
    'rv_histogram':             '/doxygen-sql/html/group__circuit__introspection.html#ga53efbd4d68e0de0ca979cf52528c63db',
    'rv_sample':                '/doxygen-sql/html/group__circuit__introspection.html#ga9db6bcdca7f5d52bc46a7c2e671a62fd',
    'rv_analytical_curves':     '/doxygen-sql/html/group__circuit__introspection.html#ga40abf964045d5aa19387546fc3480b2a',
    'simplified_circuit_subgraph': '/doxygen-sql/html/group__circuit__introspection.html#ga8ded8984d59f06ef5f529da410a3e6fe',
    # Knowledge-compilation pipeline surface
    'tseytin_cnf':              '/doxygen-sql/html/group__provenance__output.html#ga4a7fb3fc5b6e7d91aaf878cf7dadf6d2',
    'tseytin_cnf_mapping':      '/doxygen-sql/html/group__provenance__output.html#ga22a56449dce072ad0bdabbb526096cd6',
    'compile_to_ddnnf_dot':     '/doxygen-sql/html/group__provenance__output.html#ga507028f300cd63902474c0a71897199b',
    'compile_to_ddnnf':         '/doxygen-sql/html/group__provenance__output.html#ga0f7528b0ab5eed0088dfe1e297c18407',
    'ddnnf_stats':              '/doxygen-sql/html/group__provenance__output.html#gaf04888a5ff2ef7668754b5a4a2c1bf3a',
    'register_tool':            '/doxygen-sql/html/group__provenance__output.html#gaf7506731547b02eb8889b30f7ef67000',
    'unregister_tool':          '/doxygen-sql/html/group__provenance__output.html#gab1df8104dcc6ed6b3cbdc29590b81287',
    'set_tool_enabled':         '/doxygen-sql/html/group__provenance__output.html#gad65ef060270590aca9c21f9aaec5a5e6',
    'set_tool_preference':      '/doxygen-sql/html/group__provenance__output.html#gab59f1a6075707664059cbb492b117607',
    'tree_decomposition_dot':   '/doxygen-sql/html/group__provenance__output.html#gafe931883491d4249d81a0de78354d278',
    'tool_available':           '/doxygen-sql/html/group__provenance__output.html#ga7be235a742f0afc6ddb56ee2b1c025c1',
    # Per-table provenance metadata + base-ancestor registry
    'set_ancestors':            '/doxygen-sql/html/group__table__management.html#gac21906b8197f152e57cbcced8ba8d279',
    'remove_ancestors':         '/doxygen-sql/html/group__table__management.html#gab2dbd2ae72fccaa2f553df7bc2610637',
    'get_ancestors':            '/doxygen-sql/html/group__table__management.html#ga4e77fb8c922a597ba405b1e18f03d0ba',
}


# ---------------------------------------------------------------------------
# :cfunc:`name` role – renders function name as linked monospace code
# pointing to the Doxygen C/C++ API reference.
# ---------------------------------------------------------------------------

_C_FUNC_MAP = {
    # provsql.c – planner hook and query rewriting
    '_PG_init':                  '/doxygen-c/html/provsql_8c.html#a29e1a0b0688ac19dbde93824e4ae1a59',
    '_PG_fini':                  '/doxygen-c/html/provsql_8c.html#a7192e52d759211f57ad66638304ea072',
    'provsql_planner':           '/doxygen-c/html/provsql_8c.html#aa8f430f67b70c269c4ba8cc5225b8a84',
    'process_query':             '/doxygen-c/html/provsql_8c.html#a93b94031899269cea3d99f82fc7f2bda',
    'has_provenance':            '/doxygen-c/html/provsql_8c.html#af9a93235f73a9ae63ab01cf094d30372',
    'get_provenance_attributes': '/doxygen-c/html/provsql_8c.html#aa4623fbe5deb426ab11b26e0a34b6413',
    'make_provenance_expression':'/doxygen-c/html/provsql_8c.html#acf5596866c308cc3e125eafce39895ed',
    'make_aggregation_expression': '/doxygen-c/html/provsql_8c.html#a03dfbabe195ae9be5b3c13f11b98ba42',
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
    'normalize_bool_agg_having':   '/doxygen-c/html/provsql_8c.html#a2052985298a365c613a97000258920df',
    'normalize_distinct_into_group_by': '/doxygen-c/html/provsql_8c.html#ac200b7613958d5eb50dc38bad6bb297d',
    'MethodCatalog':               '/doxygen-c/html/classprovsql_1_1MethodCatalog.html',
    'MethodCatalog::registerMethod': '/doxygen-c/html/classprovsql_1_1MethodCatalog.html#aa7671ed28fd09070a78978625d0ede79',
    'ProbabilityMethod':           '/doxygen-c/html/classprovsql_1_1ProbabilityMethod.html',
    'createGenericCircuit':        '/doxygen-c/html/classMMappedCircuit.html#a95be2b3f443227f27f1db03470bbc961',
    'insert_agg_token_casts':    '/doxygen-c/html/provsql_8c.html#a539df516de849eb54876a4f98a748861',
    'having_Expr_to_provenance_cmp': '/doxygen-c/html/provsql_8c.html#a0cfaf66fa75b9265bf267b446ac6946f',
    'needs_having_lift':         '/doxygen-c/html/provsql_8c.html#ad70fe33958c5308511eaa33071db07ec',
    'add_eq_from_Quals_to_Expr': '/doxygen-c/html/provsql_8c.html#aa5f16ef0c73e1c7d651b02311994605d',
    'add_eq_from_OpExpr_to_Expr':'/doxygen-c/html/provsql_8c.html#abed26c95056d10b1f670bd37d840d989',
    # provsql.c -- ProcessUtility hook (CTAS / SELECT INTO / matview lineage)
    'provsql_ProcessUtility':       '/doxygen-c/html/provsql_8c.html#ab1ed4f68831024fd2bef99804474c4ff',
    # classify_query.c -- query-time TID/BID/OPAQUE classifier
    'provsql_classify_query':       '/doxygen-c/html/classify__query_8h.html#aafc42e9a94c5f918f87e000a971edcf1',
    'provsql_classify_emit_notice': '/doxygen-c/html/classify__query_8h.html#a66b72aec3f370db5f259ccd410c09118',
    'classify_fromlist_shape_ok':   '/doxygen-c/html/classify__query_8c.html#aaab88d2968bf006992c370d037cc4c29',
    'bid_block_key_preserved':      '/doxygen-c/html/classify__query_8c.html#a664d6fcc2b9196bdc7e3943742d0fd68',
    'try_classify_multi_source_tid':'/doxygen-c/html/classify__query_8c.html#a4808d385dcf66ea8cdec978fe1fa4b20',
    'resolve_through_group_rte':    '/doxygen-c/html/classify__query_8c.html#a8eee1d3eb92521c4c5695f06f8e6dab3',
    # safe_query.c -- safe-query rewriter + propagation pre-passes
    'is_safe_query_candidate':      '/doxygen-c/html/safe__query_8c.html#a91df2747fa3cd9564c35997e464375c1',
    'find_hierarchical_root_atoms': '/doxygen-c/html/safe__query_8c.html#a5bcf3bf32fe73c5efe394d3905863dd9',
    'compact_orphan_rtes':          '/doxygen-c/html/safe__query_8c.html#ac4aca5fe259074c262b928cd700f9bb8',
    # safe_query.c / provsql.c / probability_evaluate.cpp -- inversion-free path
    'detect_inversion_free':        '/doxygen-c/html/safe__query_8c.html#aca9d89ad57555f7370ff4ed6b2da3156',
    'build_inversion_free_marker':  '/doxygen-c/html/provsql_8c.html#a879a9faa9dfff10058519146779b9b81',
    'build_inversion_free_ctx':     '/doxygen-c/html/provsql_8c.html#a64938014e4b430635dded08a3aaa04cf',
    'flatten_spj_subqueries':       '/doxygen-c/html/provsql_8c.html#a608a128b6f1163d24084d84921ae17b9',
    'collect_inversion_free_keys':  '/doxygen-c/html/probability__evaluate_8cpp.html#ad6aecf20efa24056c8aaa8916f0aa0cf',
    'inversion_free_rank':          '/doxygen-c/html/probability__evaluate_8cpp.html#a4af7498c19280bf9922d972376add212',
    # safe_query_cert.h / StructuredDNNF.h -- inversion-free certificate + builder types
    'SafeCert':                     '/doxygen-c/html/structSafeCert.html',
    'SafeCertKey':                  '/doxygen-c/html/structSafeCertKey.html',
    'InvFreeMarkerCtx':             '/doxygen-c/html/structInvFreeMarkerCtx.html',
    'StructuredDNNFBuilder':        '/doxygen-c/html/classStructuredDNNFBuilder.html',
    'InputKey':                     '/doxygen-c/html/structStructuredDNNFBuilder_1_1InputKey.html',
    # provsql_utils.c -- per-backend caches
    'provsql_lookup_table_info':    '/doxygen-c/html/provsql__utils_8c.html#a5c4d2df8376ef2146c9fd66b41209ee2',
    'provsql_lookup_ancestry':      '/doxygen-c/html/provsql__utils_8c.html#a38251c2fff675bfffeef4317ae3e4a84',
    # MMappedCircuit class members
    'MMappedCircuit::setTableInfo':     '/doxygen-c/html/classMMappedCircuit.html#a222e276b0139533d807ae9f70b7a7ee9',
    'MMappedCircuit::setTableAncestry': '/doxygen-c/html/classMMappedCircuit.html#a05f2d145c3f6919954a8c83cb8e17e24',
    # Per-relation metadata struct
    'ProvenanceTableInfo':          '/doxygen-c/html/structProvenanceTableInfo.html',
    # provsql_utils.h – OID cache
    'constants_t':               '/doxygen-c/html/structconstants__t.html',
    'get_constants':             '/doxygen-c/html/provsql__utils_8h.html#a75e7d48321cea0156f8ad4c039c877a0',
    # provsql_mmap – background worker
    'RegisterProvSQLMMapWorker': '/doxygen-c/html/provsql__mmap_8c.html#af31c1c517f22a6923f390b75d36506be',
    'provsql_mmap_worker':       '/doxygen-c/html/provsql__mmap_8h.html#a3f084145f583f08b2532c36a79925697',
    'initialize_provsql_mmap':   '/doxygen-c/html/MMappedCircuit_8cpp.html#aa0bba27d6f73596ef0972bb0541cc244',
    'provsql_mmap_main_loop':    '/doxygen-c/html/MMappedCircuit_8cpp.html#a9215628e0312d309db481dbd27c8dabe',
    'destroy_provsql_mmap':      '/doxygen-c/html/MMappedCircuit_8cpp.html#a77b47980f1cc7b3e38ad65e15bda8118',
    # provsql_shmem – shared memory
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
    'BooleanCircuit::karpLuby':              '/doxygen-c/html/classBooleanCircuit.html#a655b427b6fec576d3085d36071d774b9',
    'BooleanCircuit::karpLubyStopping':      '/doxygen-c/html/classBooleanCircuit.html#a2eab13cfe78e93d4e19e45de9bc79bdb',
    'BooleanCircuit::dnfShape':              '/doxygen-c/html/classBooleanCircuit.html#ab7ce0b72211e82719f8dbdef7747daf4',
    'BooleanCircuit::wmcCount':              '/doxygen-c/html/classBooleanCircuit.html#a82366627e2e907cf56ae4b6d28812eb6',
    'BooleanCircuit::compilation':           '/doxygen-c/html/classBooleanCircuit.html#a667b05ef1cfb933d2cae73692e761786',
    'BooleanCircuit::makeDD':                '/doxygen-c/html/classBooleanCircuit.html#a699b0537bdaf692ff18e36ad32064d6e',
    'dDNNF::probabilityEvaluation':          '/doxygen-c/html/classdDNNF.html#a3c62965b37ba9e45f59fee50f432eb6c',
    'dDNNF::shapley':                        '/doxygen-c/html/classdDNNF.html#a8f101e081f8e8a8c72b2379dfa9a001f',
    'dDNNF::banzhaf':                        '/doxygen-c/html/classdDNNF.html#a92f93d9b218dc575ea4875a13588627a',
    'dDNNF::makeSmooth':                     '/doxygen-c/html/classdDNNF.html#a794e4aeb3d90d0a64256684ea9f9435a',
    'dDNNF::makeGatesBinary':                '/doxygen-c/html/classdDNNF.html#a7f10e16db401187a3b8ab69ecc2afa42',
    'dDNNF::condition':                      '/doxygen-c/html/classdDNNF.html#a346a7c002cd94842b6553d3727c5e9b3',
    'dDNNF::shapley_alpha':                  '/doxygen-c/html/classdDNNF.html#a822375c67b333b1315353f8e12d436d9',
    'dDNNF::shapley_delta':                  '/doxygen-c/html/classdDNNF.html#ae3f15850d0d376b543037bc132810da4',
    'dDNNF::banzhaf_internal':               '/doxygen-c/html/classdDNNF.html#a36f3d2866cd8a1076cf6bd9e9e729179',
    'compute_expectation':                   '/doxygen-c/html/namespaceprovsql.html#ae7b04c5e1aee7a1dc9fd92576786689c',
    'analytical_mean':                       '/doxygen-c/html/namespaceprovsql.html#ad88d76a7b04d884f55767ad2d3426870',
    'parse_distribution_spec':               '/doxygen-c/html/namespaceprovsql.html#acb4c29b0d3026cb1c3c1ad0764767b20',
    'monteCarloRV':                          '/doxygen-c/html/namespaceprovsql.html#ad3b5f2fc7a0f0b0da8a1e10cd2523852',
    'try_truncated_closed_form_sample':      '/doxygen-c/html/namespaceprovsql.html#ad3dfe787a2405f62619971ed5161b990',
    'CircuitFromMMap::applyLoadTimeSimplification': '/doxygen-c/html/CircuitFromMMap_8cpp.html#af118aedd9ef6148318f9ffbc503bb5bc',
    'runHybridSimplifier':                   '/doxygen-c/html/namespaceprovsql.html#aa5ea6b1bb8b2d9726a1f0bc508fa8711',
    'runHybridDecomposer':                   '/doxygen-c/html/namespaceprovsql.html#af4b46c398f275b2dd56a4f6e543ccbb3',
    'runRangeCheck':                         '/doxygen-c/html/namespaceprovsql.html#a56791cfa12bc119d18cfe076800b7631',
    'runAnalyticEvaluator':                  '/doxygen-c/html/namespaceprovsql.html#af6f2f8cf7272ec6aea45225c8ce24658',
    'runAggMarginalEvaluator':               '/doxygen-c/html/namespaceprovsql.html#a1f4c8b0ac5459572c6d92050181e247f',
    'runCountCmpEvaluator':                  '/doxygen-c/html/namespaceprovsql.html#a202da174e018e993ce6e351f6c7119a1',
    'runMinMaxCmpEvaluator':                 '/doxygen-c/html/namespaceprovsql.html#ab2bf8867a429b145000af10a18f4a88f',
    'runSumCmpEvaluator':                    '/doxygen-c/html/namespaceprovsql.html#ae23e77472355e84f31973703ce94b153',
    'GenericCircuit::resolveCmpToBernoulli': '/doxygen-c/html/classGenericCircuit.html#aaeee2a77e24f089ef526797f4b1daff1',
    'BooleanCircuit::interpretAsDD':         '/doxygen-c/html/classBooleanCircuit.html#a16a0f5a4e7b3c65afd2cee3747cb1c9e',
    'BooleanCircuit::parsePaniniDD':         '/doxygen-c/html/classBooleanCircuit.html#a6193c5268eb21297ff2ffb7681e16580',
    'dDNNF::simplify':                       '/doxygen-c/html/classdDNNF.html#a6cfc6e47106f1a7a2c7149933f01f6de',
    'dDNNFTreeDecompositionBuilder::builddDNNF': '/doxygen-c/html/classdDNNFTreeDecompositionBuilder.html#a576d126fe1e385a20c0522322bec82ef',
    'getJointCircuit':                       '/doxygen-c/html/CircuitFromMMap_8h.html#aa005f0d1643bf481ed57ee2bc3134f20',
    'shapley_internal':                      '/doxygen-c/html/shapley_8cpp.html#a4703d29e438b3454874017304a945d67',
    # CircuitCache
    'CircuitCache':                          '/doxygen-c/html/classCircuitCache.html',
    'CircuitCache::insert':                  '/doxygen-c/html/classCircuitCache.html#a9485b5217b632c047ab95e6aaf70f819',
    'circuit_cache_create_gate':             '/doxygen-c/html/circuit__cache_8h.html#a2210995d5ce3cd7f3d160abc8f8e1acf',
    'circuit_cache_get_type':                '/doxygen-c/html/circuit__cache_8h.html#a02ad2ad53de11ae321b83c29854d5f77',
    'circuit_cache_get_children':            '/doxygen-c/html/circuit__cache_8h.html#a0a088536b890aa34c75b319f4484ee03',
    # probability_evaluate.cpp
    'probability_evaluate_internal': '/doxygen-c/html/probability__evaluate_8cpp.html#a3c8a974fa0c6ddff614930936d2d791e',
    'BooleanCircuit::rewriteMultivaluedGates': '/doxygen-c/html/classBooleanCircuit.html#a90ff865c9963480f00000d62c6c37c2f',
    'BooleanCircuit::rewriteMultivaluedGatesRec': '/doxygen-c/html/classBooleanCircuit.html#a3f6c1d03227119c7f4886cbf34090022',
    'BooleanCircuit::TseytinCNF':   '/doxygen-c/html/classBooleanCircuit.html#a5fb5536b66742bb6d68aa1820ee8cba2',
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

    # :fa:`bolt` -- an inline Font Awesome icon mirroring the icon Studio
    # shows on the corresponding button or action (the FA stylesheet is
    # pulled in via html_css_files).  HTML-only; other builders drop it.
    def fa_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        # :fa:`bolt` is solid; an explicit style prefix selects another
        # FA style, e.g. :fa:`fab markdown` for the brand icon.
        parts = text.split()
        style, icon = parts if len(parts) == 2 else ('fas', parts[0])
        html = '<i class="%s fa-%s" aria-hidden="true"></i>' % (style, icon)
        return [nodes.raw(rawtext, html, format='html')], []

    # :msg:`HELLO` -- a KCMCP message-type name, rendered as monospace
    # (like a literal) and hyperlinked to the section describing that
    # message type.  Same-page fragment links to the `.. _kcmcp-...:`
    # targets defined in dev/kc-server-protocol.rst (whose explicit labels
    # produce stable `id="kcmcp-..."` anchors).  We build the reference
    # directly -- as the cfunc/cfile roles do -- rather than via a
    # std :ref:, because an explicit :ref: drops the literal child and
    # loses the monospace styling.
    _MSG_LABELS = {
        'HELLO':    'kcmcp-hello',
        'REQUEST':  'kcmcp-request',
        'RESULT':   'kcmcp-result',
        'ERROR':    'kcmcp-error',
        'PROGRESS': 'kcmcp-progress',
        'CANCEL':   'kcmcp-cancel',
        'PING':     'kcmcp-liveness',
        'PONG':     'kcmcp-liveness',
        'BYE':      'kcmcp-liveness',
    }

    def msg_role(name, rawtext, text, lineno, inliner, options=None, content=None):
        lit = nodes.literal(text, text)
        label = _MSG_LABELS.get(text)
        if not label:
            return [lit], []
        ref = nodes.reference('', '', internal=True, refuri='#' + label)
        ref += lit
        return [ref], []

    app.add_role('sqlfunc', sqlfunc_role)
    app.add_role('cfunc', cfunc_role)
    app.add_role('cfile', cfile_role)
    app.add_role('sqlfile', sqlfile_role)
    app.add_role('sc', sc_role)
    app.add_role('fa', fa_role)
    app.add_role('msg', msg_role)
    return {'version': '0.1', 'parallel_read_safe': True}
