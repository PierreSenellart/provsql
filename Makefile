INTERNAL = Makefile.internal
ARGS = with_llvm=no
ifdef DEBUG
	ARGS+=DEBUG=1
endif
UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
	PAGER ?= less
else
	PAGER ?= pager
endif

default:
	$(MAKE) -f $(INTERNAL) $(ARGS)

%:
	$(MAKE) -f $(INTERNAL) $@ $(ARGS)

test: tdkc
	bash -c "set -o pipefail && bash test/kcmcp/with-tdkc.sh make installcheck 2>&1 | tee test.log" || $(PAGER) `grep regression.diffs test.log | perl -pe 's/.*?"//;s/".*//'`

# Test coverage: rebuild the C/C++ extension instrumented, then run the full
# suite against a throwaway PostgreSQL cluster owned by the invoking user (see
# test/coverage/run-coverage.sh). Produces a gcovr line+branch report under
# coverage/ plus coverage/zero_call.txt, the provsql functions the suite never
# calls. The cluster loads the instrumented build from a private staging prefix,
# so NOTHING is installed into the system PostgreSQL and no sudo is needed; this
# requires PostgreSQL >= 18. See test/coverage/README.md. Needs gcovr
# (pipx install gcovr). tdkc is (re)built uninstrumented so the kcmcp client
# tests run instead of skipping.
#
#   make coverage
#   make coverage PROVSQL_COVERAGE_PORT=55000 GCOVR=~/.local/bin/gcovr
GCOVR ?= gcovr
coverage:
	$(MAKE) -f $(INTERNAL) clean $(ARGS)
	$(MAKE) -f $(INTERNAL) -j $(NPROC) COVERAGE=1 $(ARGS)
	$(MAKE) -f $(INTERNAL) tdkc $(ARGS)
	GCOVR="$(GCOVR)" bash test/coverage/run-coverage.sh
	@echo "The system PostgreSQL was untouched; restore the normal local build with: make"

# Upgrade-chain parity: a database upgraded from 1.0.0 must be
# catalog-identical to a fresh install (the strong form of the
# extension_upgrade canary; run before every release). Pass psql
# options via PSQL_ARGS, e.g. make upgrade-parity-test PSQL_ARGS=--port=5434
upgrade-parity-test:
	test/upgrade_parity.sh $(PSQL_ARGS)

docs: sql/provsql.sql notebooks
	cd doc/source && make html

website: docs
	# Copy branding assets into website source
	cp -r branding/fonts/. website/assets/fonts
	cp    branding/logo.png    website/assets/images/logo.png
	cp    branding/favicon.ico website/assets/images/favicon.ico
	cp    branding/favicon.ico website/favicon.ico
	# Generate SCSS partial for fonts (adjust path from fonts/ to ../fonts/)
	sed "s|url('fonts/|url('../fonts/|g" branding/fonts-face.css > website/assets/css/_fonts-face.scss
	# Copy generated docs into Jekyll source tree so jekyll serve also sees them.
	# rsync --delete so files removed upstream (e.g. retired sql/index.rst)
	# don't linger as stale, half-styled artifacts under website/docs/.
	# --omit-dir-times: setting a directory's mtime needs ownership, not just
	# write access, so it fails when the staging dirs are owned by another user
	# sharing the repo (group-writable); file times are still preserved.
	mkdir -p website/docs website/doxygen-sql/html website/doxygen-c/html
	rsync -a --omit-dir-times --delete doc/source/_build/html/ website/docs/
	rsync -a --omit-dir-times --delete doc/doxygen-sql/html/   website/doxygen-sql/html/
	rsync -a --omit-dir-times --delete doc/doxygen-c/html/     website/doxygen-c/html/
	cd website && bundle exec jekyll build

deploy: website
	# -c hashes content so Jekyll's fresh mtimes don't trigger spurious transfers
	rsync -avzcP website/_site/ provsql:/var/www/provsql/

# Assemble the ProvSQL Playground doc-root (the in-browser build). The heavy
# WASM artifacts (the matched PGlite dist + provsql.tar.gz, from wasm/; see
# studio/web/README.md) are needed only the first time, then reused in place:
#
#   make playground PGLITE_DIST=<dir> PROVSQL_TARGZ=<file>   # first build
#   make playground                                          # re-assemble (reuse)
#
# Re-running picks up the current Studio frontend/backend, case studies and
# vendored deps without rebuilding the WASM core.
# Reproduce the GitHub `wasm` workflow's build locally: build the PGlite WASM
# core + the ProvSQL extension against the Emscripten builder image, then
# assemble the Playground from the freshly built artifacts. Needs a container
# runtime (podman or docker), Node/corepack, and Boost headers; it is heavy
# (pulls the multi-GB builder image and compiles the PG tree). The browser e2e
# is a separate step, `make playground-test`. See wasm/build-wasm.sh.
wasm:
	wasm/build-wasm.sh

playground:
	cd studio/web && ./build.sh \
	  $(if $(PGLITE_DIST),--pglite "$(PGLITE_DIST)") \
	  $(if $(PROVSQL_TARGZ),--provsql "$(PROVSQL_TARGZ)")

# Build (above) then deploy to provsql.org/playground/. The build is
# path-portable, so it needs no server config beyond serving the files (the
# shipped .htaccess adds the WASM MIME type and belt-and-suspenders redirects).
deploy-playground: playground
	rsync -avzcP --delete \
	  --exclude=build.sh --exclude=vendor.sh --exclude=build-casestudies.py \
	  --exclude=serve.py --exclude=README.md --exclude=.gitignore \
	  studio/web/ provsql:/var/www/provsql/playground/

studio:
	cd studio && python3 -m provsql_studio

studio-lint:
	cd studio && ruff check .

# Regenerate the Studio example notebooks (tutorial + case studies)
# from the annotated user-guide .rst sources. Also a prerequisite of
# `make docs`, so editing an annotated .rst cannot leave the committed
# .ipynb files under studio/provsql_studio/notebooks/ stale. Skipped
# with a warning where pandoc is missing (e.g. the docs CI runner):
# regeneration is a repo-maintenance step, not a docs artifact.
notebooks:
	@if command -v pandoc >/dev/null; then \
		python3 studio/scripts/rst2nb.py; \
	else \
		echo "WARNING: pandoc not found; skipping example-notebook regeneration"; \
	fi

studio-test: studio-lint
	# tests/web (browser/PGlite e2e) needs the assembled doc-root + headless
	# Chromium; run it separately with `make playground-test`.
	cd studio && python3 -m pytest tests --ignore=tests/web

# Browser e2e for the assembled Playground (headless Chromium via
# pytest-playwright). Assembles the doc-root first; run `make wasm` beforehand
# to test freshly built WASM artifacts rather than the in-place ones.
playground-test: playground
	cd studio && python3 -m pytest tests/web

.PHONY: default test coverage upgrade-parity-test docs website deploy wasm playground deploy-playground playground-test studio studio-lint studio-test tdkc provsql_migrate_mmap

tdkc provsql_migrate_mmap:
	$(MAKE) -f $(INTERNAL) $@ $(ARGS)

EXTVERSION = $(shell grep default_version provsql.common.control | \
             sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

docker-build:
	make clean
	docker build -f docker/Dockerfile \
	  --build-arg PROVSQL_VERSION=$(EXTVERSION) \
	  -t provsql:$(EXTVERSION) .
