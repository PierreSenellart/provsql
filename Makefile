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

docs: sql/provsql.sql
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

studio-test: studio-lint
	cd studio && python3 -m pytest tests

.PHONY: default test docs website deploy playground deploy-playground studio studio-lint studio-test tdkc provsql_migrate_mmap

tdkc provsql_migrate_mmap:
	$(MAKE) -f $(INTERNAL) $@ $(ARGS)

EXTVERSION = $(shell grep default_version provsql.common.control | \
             sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

docker-build:
	make clean
	docker build -f docker/Dockerfile \
	  --build-arg PROVSQL_VERSION=$(EXTVERSION) \
	  -t provsql:$(EXTVERSION) .
