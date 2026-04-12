#!/usr/bin/env bash
# release.sh — Create a signed ProvSQL release
#
# Usage: ./release.sh <version>
#   e.g. ./release.sh 1.0.0
#
# Steps:
#   1. Validate version is newer than existing tags
#   2. Prompt for release notes in $EDITOR
#   3. Update provsql.common.control, website/_data/releases.yml,
#      CITATION.cff, CHANGELOG.md, and META.json
#   4. Commit, create signed tag, push, create GitHub release

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

# 0. Preflight checks

command -v gh > /dev/null 2>&1 \
  || die "'gh' is not installed (see https://cli.github.com)"
gh auth status > /dev/null 2>&1 \
  || die "'gh' is not authenticated; run: gh auth login"
command -v gpg > /dev/null 2>&1 \
  || die "'gpg' is not installed"
git config user.signingkey > /dev/null 2>&1 \
  || die "No GPG signing key configured; run: git config user.signingkey <keyid>"
[[ -f provsql.control && -f provsql.common.control ]] \
  || die "Must be run from the root of the provsql repository"
[[ -f website/_data/releases.yml ]] \
  || die "website/_data/releases.yml not found"
[[ -f CITATION.cff ]] \
  || die "CITATION.cff not found"
[[ -f CHANGELOG.md ]] \
  || die "CHANGELOG.md not found"
[[ -f META.json ]] \
  || die "META.json not found"
GH_REPO=$(git remote get-url origin | sed 's|git@github.com:||;s|\.git$||')

# 1. Parse & validate version

VERSION="${1:-}"
[[ -n "$VERSION" ]] || die "Usage: $0 <version>  (e.g. 1.0.0)"

# Must match semver-ish X.Y.Z or X.Y.Z-suffix
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?(-[a-zA-Z0-9._-]+)?$ ]] \
  || die "Version '$VERSION' is not a valid version (expected e.g. 1.0.0 or 1.2.3-alpha)"

TAG="v${VERSION}"

# Check no existing tag with this name
if git rev-parse --verify --quiet "refs/tags/$TAG" > /dev/null 2>&1; then
  die "Tag '$TAG' already exists"
fi

# Find the last tag and sanity-check ordering (best-effort)
PREV_TAG=$(git tag --sort=-version:refname | grep -E '^v[0-9]' | head -1 || true)
if [[ -n "$PREV_TAG" ]]; then
  PREV_VER="${PREV_TAG#v}"
  # Use sort -V to compare; new version must be strictly greater
  LOWEST=$(printf '%s\n%s\n' "$PREV_VER" "$VERSION" | sort -V | head -1)
  if [[ "$LOWEST" != "$PREV_VER" ]]; then
    die "Version '$VERSION' is not newer than existing tag '$PREV_TAG'"
  fi
fi

# 2. Working-tree check

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "WARNING: working tree is not clean:"
  git status --short
  read -r -p "Continue anyway? [y/N] " CONFIRM
  [[ "$CONFIRM" =~ ^[Yy]$ ]] || exit 1
fi

# 2b. Upgrade-script check
#
# ProvSQL supports in-place extension upgrades via ALTER EXTENSION
# provsql UPDATE (see doc/source/dev/build-system.rst).  Every release
# must ship a sql/upgrades/provsql--<prev>--<new>.sql script that
# brings a user on the previous release up to the new version, unless
# there are demonstrably no SQL-surface changes in this release.
#
# Check if the upgrade script exists.  If not, diff the SQL sources
# against the previous tag; if the diff is empty, auto-generate a
# no-op upgrade script.  Otherwise, abort and ask the author to write
# the script by hand.

if [[ -n "$PREV_TAG" ]]; then
  UPGRADE_SCRIPT="sql/upgrades/provsql--${PREV_VER}--${VERSION}.sql"
  if [[ ! -f "$UPGRADE_SCRIPT" ]]; then
    SQL_DIFF=$(git diff "$PREV_TAG"..HEAD -- sql/provsql.common.sql sql/provsql.14.sql 2>&1)
    if [[ -z "$SQL_DIFF" ]]; then
      echo "No SQL source changes since $PREV_TAG; generating empty upgrade script."
      cat > "$UPGRADE_SCRIPT" <<EOF
/**
 * @file
 * @brief ProvSQL upgrade script: $PREV_VER → $VERSION
 *
 * No SQL-surface changes in this release; the upgrade script is a
 * no-op.  PostgreSQL still requires its presence to offer an
 * ALTER EXTENSION provsql UPDATE path between these versions.
 */
SET search_path TO provsql;
EOF
      git add "$UPGRADE_SCRIPT"
    else
      echo "ERROR: $UPGRADE_SCRIPT is missing, and sql/provsql.*.sql has changes since $PREV_TAG."
      echo "       Write the upgrade script by hand, commit it, and re-run release.sh."
      exit 1
    fi
  fi
fi

# 3. Collect release notes

NOTES_FILE=$(mktemp /tmp/provsql-release-notes.XXXXXX.md)
trap 'rm -f "$NOTES_FILE"' EXIT

# Pre-fill with a template
cat > "$NOTES_FILE" <<EOF
<!-- Write release notes for ProvSQL $VERSION below (Markdown).    -->
<!-- Lines starting with <!-- are stripped. Save and close to continue. -->

## What's new in $VERSION

-
EOF

TEMPLATE_SUM=$(md5sum "$NOTES_FILE" | cut -d' ' -f1)

"${EDITOR:-vim}" "$NOTES_FILE"

EDITED_SUM=$(md5sum "$NOTES_FILE" | cut -d' ' -f1)
[[ "$TEMPLATE_SUM" != "$EDITED_SUM" ]] \
  || die "Release notes are unchanged from template; aborting."

# Strip comment lines and leading/trailing blank lines
NOTES=$(sed '/^<!--/d' "$NOTES_FILE" | sed -e '/./,$!d' -e :a -e '/^\n*$/{$d;N;ba}')

[[ -n "$NOTES" ]] || die "Release notes are empty; aborting."

# 4. Update version in control files

TODAY=$(date +%Y-%m-%d)

sed -i "s/^default_version = .*/default_version = '$VERSION'/" \
  provsql.common.control

# 5. Update website/_data/releases.yml

RELEASES_FILE="website/_data/releases.yml"

# Build the YAML block for the new release
YAML_NOTES=$(echo "$NOTES" | sed 's/^/    /')  # 4-space indent for block scalar

# Prepend entry to releases.yml (newest first)
TMP_YAML=$(mktemp)
cat > "$TMP_YAML" <<YAML
- version: "$VERSION"
  date: $TODAY
  tag: "$TAG"
  notes: |
$YAML_NOTES

YAML
cat "$RELEASES_FILE" >> "$TMP_YAML"
mv "$TMP_YAML" "$RELEASES_FILE"

# 6. Update CITATION.cff (top-level version and date-released only —
#    anything under preferred-citation is indented and unaffected)

sed -i "s/^version: .*/version: \"$VERSION\"/" CITATION.cff
sed -i "s/^date-released: .*/date-released: \"$TODAY\"/" CITATION.cff

# 6b. Update META.json (PGXN Meta Spec) — top-level "version" and
#     the version under provides.provsql. Both are bare strings at
#     predictable indents in the file we maintain, so sed is safe.
sed -i "s/^   \"version\": \"[^\"]*\"/   \"version\": \"$VERSION\"/" META.json
sed -i "s/^         \"version\": \"[^\"]*\"/         \"version\": \"$VERSION\"/" META.json

# 7. Prepend the new release to CHANGELOG.md, mirroring the
#    website/_data/releases.yml entry we just added.  The new block
#    consists of a "## [VERSION] - DATE" heading followed by the
#    release notes, with any leading "## What's new in VERSION" line
#    from the notes stripped (it would duplicate the heading).

CHANGELOG_FILE="CHANGELOG.md"
CHANGELOG_ENTRY=$(mktemp)
TMP_CHANGELOG=$(mktemp)

{
  echo "## [$VERSION] - $TODAY"
  echo ""
  echo "$NOTES" | sed "/^## What's new in /d" | sed -e '/./,$!d'
  echo ""
} > "$CHANGELOG_ENTRY"

awk -v entry="$CHANGELOG_ENTRY" '
  /^## \[/ && !done {
    while ((getline line < entry) > 0) print line
    close(entry)
    done = 1
  }
  { print }
' "$CHANGELOG_FILE" > "$TMP_CHANGELOG"
mv "$TMP_CHANGELOG" "$CHANGELOG_FILE"
rm -f "$CHANGELOG_ENTRY"

# 8. Commit

git add provsql.common.control "$RELEASES_FILE" CITATION.cff CHANGELOG.md META.json
git commit -m "Release version $VERSION"

# 9. Signed tag

# Build tag message: first line of notes as subject, full notes as body
TAG_SUBJECT="ProvSQL $VERSION"
git tag -s "$TAG" -m "$TAG_SUBJECT"$'\n\n'"$NOTES"

echo "Created signed tag $TAG"

# 10. Push

read -r -p "Push commit and tag to origin? [Y/n] " PUSH_CONFIRM
if [[ ! "$PUSH_CONFIRM" =~ ^[Nn]$ ]]; then
  git push origin master
  git push origin "$TAG"
fi

# 11. GitHub Release

read -r -p "Create GitHub Release? [Y/n] " GH_CONFIRM
if [[ ! "$GH_CONFIRM" =~ ^[Nn]$ ]]; then
  gh release create "$TAG" \
    --repo "$GH_REPO" \
    --title "ProvSQL $VERSION" \
    --notes "$NOTES"
  echo "GitHub release created: https://github.com/PierreSenellart/provsql/releases/tag/$TAG"
fi

# 12. Post-release: bump default_version on master to next -dev

# Default: bump minor, reset patch, append -dev (e.g. 1.0.0 -> 1.1.0-dev).
# Override by setting NEXT_VERSION in the environment (e.g. NEXT_VERSION=1.0.1-dev).
BASE_VERSION="${VERSION%%-*}"  # strip any pre-release suffix before computing next
NEXT_DEV="${NEXT_VERSION:-$(awk -F. '{print $1"."($2+1)".0-dev"}' <<<"$BASE_VERSION")}"

read -r -p "Bump default_version to '$NEXT_DEV' on master? [Y/n] " BUMP_CONFIRM
if [[ ! "$BUMP_CONFIRM" =~ ^[Nn]$ ]]; then
  sed -i "s/^default_version = .*/default_version = '$NEXT_DEV'/" \
    provsql.common.control
  git add provsql.common.control
  git commit -m "Post-release: bump default_version to $NEXT_DEV"
  read -r -p "Push post-release bump to origin? [Y/n] " BUMP_PUSH
  if [[ ! "$BUMP_PUSH" =~ ^[Nn]$ ]]; then
    git push origin master
  fi
fi

echo ""
echo "Done. Release $VERSION is complete."
echo "  Tag:          $TAG"
echo "  Tarball:      https://github.com/PierreSenellart/provsql/archive/refs/tags/$TAG.tar.gz"
echo "  GitHub:       https://github.com/PierreSenellart/provsql/releases/tag/$TAG"
echo ""
echo "Don't forget to deploy the website:  make deploy"
