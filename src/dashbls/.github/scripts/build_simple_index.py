#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2026-present, Microsoft Corporation
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: MIT
#

"""Generate a PEP 503 index for released distributions."""

import argparse
import json
import os
import re
import subprocess
import sys
from html import escape
from pathlib import Path

# PEP 503: lowercase, runs of -_. collapsed to a single -
NAME = "dashbls"
NORMALISED = re.sub(r"[-_.]+", "-", NAME).lower()

# Must match requires-python in pyproject.toml.
REQUIRES_PYTHON = ">=3.10"


def releases(repo: str) -> list[dict]:
    """Every release of `repo`, each with the name, URL and digest of its assets."""
    out = subprocess.check_output(  # noqa: S603
        [  # noqa: S607
            "gh",
            "api",
            "--paginate",
            f"repos/{repo}/releases",
            "--jq",
            ".[] | select(.draft | not) | {tag: .tag_name, assets: [.assets[] "
            "| {name, url: .browser_download_url, digest}]}",
        ],
        text=True,
    )
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def anchor(name: str, url: str, digest: str | None) -> str:
    """One link, with the hash pip needs to verify what it downloaded.

    The releases API reports a digest as "sha256:<hex>", which is the PEP 503
    fragment in all but spelling. Without it pip has no integrity check at all
    and --require-hashes has nothing to match against.
    """
    if digest and digest.startswith("sha256:"):
        url = f"{url}#sha256={digest.removeprefix('sha256:')}"
    return (
        f'    <a href="{escape(url)}" '
        f'data-requires-python="{escape(REQUIRES_PYTHON)}">{escape(name)}</a><br>'
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--repo", default=os.environ.get("GITHUB_REPOSITORY", "dashpay/bls-signatures")
    )
    args = parser.parse_args()

    # Sdists matter as much as wheels here: they are the only thing installable
    # on a platform we do not ship a wheel for, and pip will not find one that
    # the index does not list.
    files = []
    for release in releases(args.repo):
        for asset in release["assets"]:
            if asset["name"].endswith((".whl", ".tar.gz")):
                files.append((asset["name"], asset["url"], asset.get("digest")))
    files.sort()
    if not files:
        sys.exit("no distributions found on any release; refusing to publish an empty index")

    project = args.output / "pep503" / NORMALISED
    project.mkdir(parents=True, exist_ok=True)

    # pypi:repository-version is PEP 629; charset because the filenames are
    # written by whatever runner built them.
    head = '<meta charset="utf-8">\n<meta name="pypi:repository-version" content="1.0">'
    anchors = "\n".join(anchor(*dist) for dist in files)
    (project / "index.html").write_text(
        f"<!DOCTYPE html>\n<html><head>{head}\n<title>Links for {NAME}</title></head>\n"
        f"<body>\n<h1>Links for {NAME}</h1>\n{anchors}\n</body></html>\n",
        encoding="utf-8",
    )
    (args.output / "pep503" / "index.html").write_text(
        f"<!DOCTYPE html>\n<html><head>{head}\n<title>Simple Index</title></head>\n"
        "<body>\n"
        f'    <a href="{NORMALISED}/">{NAME}</a><br>\n'
        "</body></html>\n",
        encoding="utf-8",
    )
    print(f"indexed {len(files)} distributions for {NAME}")


if __name__ == "__main__":
    main()
