"""The commit Studio and the ProvSQL library are built from.

A version number names a release, and stays the same for every commit of
the development cycle that follows it. When what runs is not that release,
the footer also shows the commit, as `git describe` gives it.
"""
from __future__ import annotations

import functools
import subprocess
from pathlib import Path

from . import __version__

_PKG_DIR = Path(__file__).resolve().parent


@functools.lru_cache(maxsize=1)
def studio_build_id() -> str | None:
    """`git describe` of the checkout Studio runs from (e.g.,
    `studio-v1.8.0-489-g45320ea7`), or None.

    The Playground build, which has no git at run time, records it in a
    `_build_id` module. Otherwise git is asked, provided this package is
    tracked in the repository around it: an install into a virtualenv
    that happens to sit inside some git checkout must not report that
    checkout's commit. No git, a source archive, an installed wheel, or a
    repository git refuses to read (owned by another user): None."""
    try:
        from ._build_id import BUILD_ID  # type: ignore[import-not-found]
        return BUILD_ID or None
    except ImportError:
        pass
    try:
        tracked = subprocess.run(
            ["git", "-C", str(_PKG_DIR), "ls-files", "--error-unmatch",
             "__init__.py"],
            capture_output=True, timeout=5,
        )
        if tracked.returncode != 0:
            return None
        out = subprocess.run(
            ["git", "-C", str(_PKG_DIR), "describe", "--tags",
             "--match", "studio-v*", "--always", "--dirty"],
            capture_output=True, text=True, timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip() or None


def untagged(build_id: str | None, version: str | None,
             tag_prefix: str) -> str | None:
    """`build_id`, unless it says nothing beyond `version`: absent,
    `unknown` (the extension built outside a git checkout), or exactly the
    release tag of `version`."""
    if not build_id or build_id == "unknown":
        return None
    if version and build_id == f"{tag_prefix}{version}":
        return None
    return build_id


def studio_build() -> str | None:
    """The commit Studio runs from, when it is not the release tag of
    `__version__`."""
    return untagged(studio_build_id(), __version__, "studio-v")
