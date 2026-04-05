#!/usr/bin/env bash
# release.sh — Create a signed ProvSQL release
#
# Usage: ./release.sh <version>
#   e.g. ./release.sh 1.0.0
#
# Steps:
#   1. Validate version is newer than existing tags
#   2. Prompt for release notes in $EDITOR
#   3. Update provsql.control, provsql.common.control, website/_data/releases.yml
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

"${EDITOR:-vi}" "$NOTES_FILE"

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

# 6. Commit

git add provsql.common.control "$RELEASES_FILE"
git commit -m "Release version $VERSION"

# 7. Signed tag

# Build tag message: first line of notes as subject, full notes as body
TAG_SUBJECT="ProvSQL $VERSION"
git tag -s "$TAG" -m "$TAG_SUBJECT"$'\n\n'"$NOTES"

echo "Created signed tag $TAG"

# 8. Push

read -r -p "Push commit and tag to origin? [Y/n] " PUSH_CONFIRM
if [[ ! "$PUSH_CONFIRM" =~ ^[Nn]$ ]]; then
  git push origin master
  git push origin "$TAG"
fi

# 9. GitHub Release

read -r -p "Create GitHub Release? [Y/n] " GH_CONFIRM
if [[ ! "$GH_CONFIRM" =~ ^[Nn]$ ]]; then
  gh release create "$TAG" \
    --title "ProvSQL $VERSION" \
    --notes "$NOTES" \
    --verify-tag
  echo "GitHub release created: https://github.com/PierreSenellart/provsql/releases/tag/$TAG"
fi

echo ""
echo "Done. Release $VERSION is complete."
echo "  Tag:          $TAG"
echo "  Tarball:      https://github.com/PierreSenellart/provsql/archive/refs/tags/$TAG.tar.gz"
echo "  GitHub:       https://github.com/PierreSenellart/provsql/releases/tag/$TAG"
