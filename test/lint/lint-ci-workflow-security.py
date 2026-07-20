#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Enforce the trust boundary for workflows that execute pull request code."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"


def workflow(name):
    return (WORKFLOW_DIR / name).read_text(encoding="utf-8")


def job_block(contents, name):
    match = re.search(
        rf"^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
        contents,
        flags=re.MULTILINE | re.DOTALL,
    )
    assert match is not None, f"missing job {name}"
    return match.group("body")


def assert_no_write_permissions(contents, context):
    assert not re.search(r"^\s+[a-z-]+: write$", contents, re.MULTILINE), (
        f"{context} must not receive write-capable permissions"
    )


def main():
    workflows = {path.name: path.read_text(encoding="utf-8") for path in WORKFLOW_DIR.glob("*.yml")}

    for name, contents in workflows.items():
        assert "uses: actions/cache@" not in contents, (
            f"{name} must use explicit cache restore/save actions"
        )

        lines = contents.splitlines()
        for index, line in enumerate(lines):
            if re.match(r"^\s+ref:", line) and "pull_request.head.sha" in line:
                checkout = "\n".join(lines[index:index + 8])
                assert "persist-credentials: false" in checkout, (
                    f"{name}:{index + 1} must not persist checkout credentials"
                )
                assert "allow-unsafe-pr-checkout:" in checkout, (
                    f"{name}:{index + 1} must explicitly acknowledge the isolated PR checkout"
                )

            if "uses: actions/cache/save@" in line:
                save_step = "\n".join(lines[max(0, index - 8):index + 1])
                assert any(
                    guard in save_step
                    for guard in (
                        "github.event_name == 'push'",
                        "github.event_name == 'schedule'",
                        "inputs.trusted",
                    )
                ), f"{name}:{index + 1} cache save lacks a positive trusted-run guard"

    build = workflow("build.yml")
    assert_no_write_permissions(build.split("\njobs:", 1)[0], "build.yml defaults")
    assert build.count("packages: write") == 2
    for name in ("container-publish", "container-slim-publish"):
        block = job_block(build, name)
        assert "github.event_name == 'push'" in block
        assert "packages: write" in block
    for name in ("container", "container-slim"):
        block = job_block(build, name)
        assert "permissions: {}" in block
        assert "github.event.repository.default_branch" in block

    container = workflow("build-container.yml")
    assert container.count("if: github.event_name == 'push'") == 3
    assert "pull_request.head.sha" not in container

    for name in (
        "build-depends.yml",
        "build-src.yml",
        "cache-depends-sources.yml",
        "lint.yml",
        "test-src.yml",
    ):
        assert_no_write_permissions(workflow(name), name)

    guix = workflow("guix-build.yml")
    assert_no_write_permissions(guix.split("\njobs:", 1)[0], "guix-build.yml defaults")
    assert "pull_request.head.sha" in job_block(guix, "build-pr")
    assert "trusted: false" in job_block(guix, "build-pr")
    assert_no_write_permissions(job_block(guix, "build-pr"), "Guix PR build")
    assert "packages: write" in job_block(guix, "build-image")
    assert "github.event_name == 'pull_request_target'" not in job_block(guix, "build-image")
    assert "trusted: true" in job_block(guix, "build-trusted")
    assert "id-token: write" in job_block(guix, "build-trusted")
    assert "attestations: write" in job_block(guix, "build-trusted")

    guix_worker = workflow("guix-build-worker.yml")
    assert_no_write_permissions(guix_worker, "Guix worker")
    assert "allow-unsafe-pr-checkout:" in guix_worker
    assert "if: inputs.trusted" in guix_worker


if __name__ == "__main__":
    main()
