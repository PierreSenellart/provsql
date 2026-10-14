"""Tests for build_id: when the footer shows a commit after a version."""
from __future__ import annotations

import pytest

from provsql_studio import build_id


@pytest.mark.parametrize(
    "build, version, prefix, expected",
    [
        # Absent, or built outside a git checkout: nothing to add.
        (None, "1.13.0-dev", "v", None),
        ("", "1.13.0-dev", "v", None),
        ("unknown", "1.13.0-dev", "v", None),
        # Exactly the release tag of the version: nothing to add.
        ("v1.12.0", "1.12.0", "v", None),
        ("studio-v1.8.0", "1.8.0", "studio-v", None),
        # Anything else is shown.
        ("v1.12.0-dirty", "1.12.0", "v", "v1.12.0-dirty"),
        ("v1.12.0-487-g45320ea7", "1.13.0-dev", "v", "v1.12.0-487-g45320ea7"),
        ("studio-v1.8.0-489-g45320ea7", "1.8.0", "studio-v",
         "studio-v1.8.0-489-g45320ea7"),
        ("45320ea7", None, "v", "45320ea7"),
    ],
)
def test_untagged(build, version, prefix, expected):
    assert build_id.untagged(build, version, prefix) == expected
