# Minimal makefile for Sphinx documentation

SPHINXOPTS    ?= -E
# Use `python3 -m sphinx` rather than the `sphinx-build` entry script:
# on Debian-derived systems with both python3-sphinx-rtd-theme (old) and
# a user-local sphinx-rtd-theme (>= 7.2-compatible) installed, the entry
# script loads the old /usr/share/sphinx_rtd_theme/layout.html instead
# of the user-local one, which silently breaks the data-content_root
# attribute on <html> and therefore Sphinx search summaries.
SPHINXBUILD   ?= python3 -m sphinx
SOURCEDIR     = .
BUILDDIR      = _build

BRANDING      = ../../branding

help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

.PHONY: help Makefile

# Catch-all target: route all unknown targets to Sphinx using the new
# "make mode" option.  $(O) is meant as a shortcut for $(SPHINXOPTS).
%: Makefile
	# Verify Python dependencies meet minimum versions
	python3 -c "from importlib.metadata import version; assert version('sphinx')>='8'; assert version('sphinx-rtd-theme')>='3'; assert version('sphinxcontrib-bibtex')>='2.4'; import pybtex"
	# Copy branding assets for Sphinx
	cp $(BRANDING)/logo.png    _static/logo.png
	cp $(BRANDING)/favicon.ico _static/favicon.ico
	cp -r $(BRANDING)/fonts/.  _static/fonts
	cp $(BRANDING)/fonts-face.css _static/fonts-face.css
	# Build Doxygen outputs
	doxygen Doxyfile-c
	doxygen Doxyfile-sql
	# Copy fonts into each Doxygen HTML output (for self-hosted @font-face)
	cp -r $(BRANDING)/fonts/. ../doxygen-c/html/fonts
	cp -r $(BRANDING)/fonts/. ../doxygen-sql/html/fonts
	cp $(BRANDING)/fonts-face.css ../doxygen-c/html/fonts-face.css
	cp $(BRANDING)/fonts-face.css ../doxygen-sql/html/fonts-face.css
	python3 postprocess-sql-html.py
	@$(SPHINXBUILD) -M $@ "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)
	# Force-overwrite Sphinx-cached static files with latest branding
	cp -f $(BRANDING)/logo.png    $(BUILDDIR)/html/_static/logo.png
	cp -f $(BRANDING)/favicon.ico $(BUILDDIR)/html/_static/favicon.ico
	# Check :sqlfunc: link coherence
	python3 check-doc-links.py
