# Minimal makefile for Sphinx documentation

SPHINXOPTS    ?= -E
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = .
BUILDDIR      = _build

BRANDING      = ../../branding

help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

.PHONY: help Makefile

# Catch-all target: route all unknown targets to Sphinx using the new
# "make mode" option.  $(O) is meant as a shortcut for $(SPHINXOPTS).
%: Makefile
	# Check Python dependencies
	pip install --quiet -r ../requirements.txt
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
	python3 check-sqlfunc-links.py
