#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Enforce the trust boundary for workflows that execute pull request code.

Workflows are parsed as YAML so equivalent forms (flow mappings, quoted keys,
inline comments) cannot bypass the permission, checkout, and cache-save checks.
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path
from typing import Any

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"


def normalize_gha(node: Any) -> Any:
    """Repair YAML 1.1 bool coercion of the GitHub Actions `on` key.

    Keep boolean values as real bools (needed for `persist-credentials: false`
    and `trusted: false`) while restoring the workflow trigger key name.
    """
    if isinstance(node, dict):
        fixed = {}
        for key, value in node.items():
            if key is True:
                key = "on"
            elif key is False:
                key = "off"
            fixed[key] = normalize_gha(value)
        return fixed
    if isinstance(node, list):
        return [normalize_gha(item) for item in node]
    return node


def load_workflow(path: Path) -> Any:
    return normalize_gha(yaml.safe_load(path.read_text(encoding="utf-8")))


def workflow_data(name: str, workflows: dict[str, Any]) -> Any:
    assert name in workflows, f"missing workflow {name}"
    return workflows[name]


def job(data: Any, name: str) -> dict:
    jobs = (data or {}).get("jobs") or {}
    assert name in jobs, f"missing job {name}"
    block = jobs[name]
    assert isinstance(block, dict), f"job {name} is not a mapping"
    return block


def permissions_map(node: Any) -> dict[str, str] | None:
    """Return a permissions mapping, or None if permissions are absent."""
    if node is None:
        return None
    if not isinstance(node, dict) or "permissions" not in node:
        return None
    perms = node["permissions"]
    if perms is None or perms == {}:
        return {}
    if isinstance(perms, str):
        # Top-level shorthand like `permissions: read-all` / `write-all`.
        return {"__shorthand__": perms}
    assert isinstance(perms, dict), f"permissions must be a mapping, got {perms!r}"
    return {str(k): str(v) for k, v in perms.items()}


def assert_no_write_permissions(node: Any, context: str) -> None:
    perms = permissions_map(node)
    if perms is None:
        return
    if perms.get("__shorthand__") in ("write-all", "write"):
        raise AssertionError(f"{context} must not receive write-capable permissions")
    for key, value in perms.items():
        if key == "__shorthand__":
            continue
        if value == "write":
            raise AssertionError(
                f"{context} must not receive write-capable permissions ({key}: write)"
            )


def walk_steps(data: Any):
    """Yield (job_name, step_dict) for every step in the workflow."""
    jobs = (data or {}).get("jobs") or {}
    if not isinstance(jobs, dict):
        return
    for job_name, job_body in jobs.items():
        if not isinstance(job_body, dict):
            continue
        steps = job_body.get("steps") or []
        if not isinstance(steps, list):
            continue
        for step in steps:
            if isinstance(step, dict):
                yield job_name, step


def uses_action(step: dict, prefix: str) -> bool:
    uses = step.get("uses")
    return isinstance(uses, str) and uses.startswith(prefix)


def check_workflow_invariants(name: str, data: Any) -> None:
    for job_name, step in walk_steps(data):
        uses = step.get("uses")
        if isinstance(uses, str) and uses.startswith("actions/cache@"):
            raise AssertionError(
                f"{name} job {job_name} must use explicit cache restore/save actions, "
                f"not combined actions/cache"
            )

        if uses_action(step, "actions/cache/save@"):
            condition = step.get("if")
            if not isinstance(condition, str):
                raise AssertionError(
                    f"{name} job {job_name}: cache save lacks a positive trusted-run guard"
                )
            trusted_event = re.search(
                r"github\.event_name\s*==\s*'(?:push|schedule)'", condition
            )
            trusted_input = re.search(
                r"(?:^|[\s&(])inputs\.trusted(?:\s*&&|\s*$|\s*\))", condition
            )
            if not (trusted_event or trusted_input):
                raise AssertionError(
                    f"{name} job {job_name}: cache save lacks a positive trusted-run guard"
                )

        with_block = step.get("with") or {}
        if not isinstance(with_block, dict):
            continue
        ref = with_block.get("ref")
        if isinstance(ref, str) and "pull_request.head.sha" in ref:
            if with_block.get("persist-credentials") is not False:
                raise AssertionError(
                    f"{name} job {job_name}: must not persist checkout credentials "
                    f"for isolated PR checkout"
                )
            if "allow-unsafe-pr-checkout" not in with_block:
                raise AssertionError(
                    f"{name} job {job_name}: must explicitly acknowledge the isolated "
                    f"PR checkout"
                )


def contains_expr(node: Any, needle: str) -> bool:
    if isinstance(node, str):
        return needle in node
    if isinstance(node, dict):
        return any(contains_expr(v, needle) for v in node.values())
    if isinstance(node, list):
        return any(contains_expr(v, needle) for v in node)
    return False


def count_expr(node: Any, needle: str) -> int:
    if isinstance(node, str):
        return node.count(needle)
    if isinstance(node, dict):
        return sum(count_expr(v, needle) for v in node.values())
    if isinstance(node, list):
        return sum(count_expr(v, needle) for v in node)
    return 0


def has_packages_read(node: Any) -> bool:
    perms = permissions_map(node) or {}
    return perms.get("packages") == "read"


def has_container_credentials(node: Any) -> bool:
    """True if any job uses container.credentials.password: github.token."""
    jobs = (node or {}).get("jobs") or {}
    if not isinstance(jobs, dict):
        return False
    for job_body in jobs.values():
        if not isinstance(job_body, dict):
            continue
        container = job_body.get("container")
        if not isinstance(container, dict):
            continue
        creds = container.get("credentials")
        if not isinstance(creds, dict):
            continue
        password = creds.get("password")
        if isinstance(password, str) and "github.token" in password:
            return True
    return False


def main() -> None:
    workflow_paths = sorted(WORKFLOW_DIR.glob("*.yml"))
    assert workflow_paths, f"no workflows under {WORKFLOW_DIR}"
    workflows: dict[str, Any] = {}
    for path in workflow_paths:
        try:
            workflows[path.name] = load_workflow(path)
        except yaml.YAMLError as exc:
            raise AssertionError(f"{path.name}: YAML parse error: {exc}") from exc

    for name, data in workflows.items():
        check_workflow_invariants(name, data)

    build = workflow_data("build.yml", workflows)
    assert_no_write_permissions(build, "build.yml defaults")
    assert has_packages_read(build), "build.yml defaults must include packages: read"
    # Exactly two jobs may escalate to packages: write (container publishers).
    write_jobs = []
    for job_name, job_body in (build.get("jobs") or {}).items():
        perms = permissions_map(job_body) or {}
        if perms.get("packages") == "write":
            write_jobs.append(job_name)
    assert write_jobs == ["container-publish", "container-slim-publish"] or set(
        write_jobs
    ) == {"container-publish", "container-slim-publish"}, (
        f"build.yml packages: write jobs unexpected: {write_jobs}"
    )
    assert len(write_jobs) == 2

    for name in ("container-publish", "container-slim-publish"):
        block = job(build, name)
        assert block.get("if") and "github.event_name == 'push'" in str(block.get("if")), (
            f"{name} must be push-only"
        )
        perms = permissions_map(block) or {}
        assert perms.get("packages") == "write", f"{name} needs packages: write"

    for name in ("container", "container-slim"):
        block = job(build, name)
        perms = permissions_map(block)
        assert perms == {}, f"{name} must set permissions: {{}}"
        assert contains_expr(block, "github.base_ref"), (
            f"{name} must select images from github.base_ref"
        )

    container = workflow_data("build-container.yml", workflows)
    assert count_expr(container, "github.event_name == 'push'") == 3, (
        "build-container.yml must gate publish paths on push"
    )
    assert not contains_expr(container, "pull_request.head.sha"), (
        "build-container.yml must not check out PR heads"
    )

    for name in (
        "build-depends.yml",
        "build-src.yml",
        "cache-depends-sources.yml",
        "lint.yml",
        "test-src.yml",
    ):
        assert_no_write_permissions(workflow_data(name, workflows), name)

    for name in ("build-depends.yml", "build-src.yml", "lint.yml", "test-src.yml"):
        data = workflow_data(name, workflows)
        assert has_packages_read(data) or any(
            has_packages_read(j) for j in (data.get("jobs") or {}).values()
            if isinstance(j, dict)
        ), f"{name} must grant packages: read"
        assert has_container_credentials(data), (
            f"{name} must supply container credentials with github.token"
        )

    guix = workflow_data("guix-build.yml", workflows)
    assert_no_write_permissions(guix, "guix-build.yml defaults")
    assert has_packages_read(guix), "guix-build.yml defaults must include packages: read"

    build_pr = job(guix, "build-pr")
    assert contains_expr(build_pr, "pull_request.head.sha")
    assert build_pr.get("with", {}).get("trusted") is False
    pr_perms = permissions_map(build_pr) or {}
    assert pr_perms.get("packages") == "read"
    assert_no_write_permissions(build_pr, "Guix PR build")

    build_image = job(guix, "build-image")
    image_perms = permissions_map(build_image) or {}
    assert image_perms.get("packages") == "write"
    assert not contains_expr(build_image, "github.event_name == 'pull_request_target'")

    build_trusted = job(guix, "build-trusted")
    assert build_trusted.get("with", {}).get("trusted") is True
    trusted_perms = permissions_map(build_trusted) or {}
    assert trusted_perms.get("packages") == "read"
    assert trusted_perms.get("id-token") == "write"
    assert trusted_perms.get("attestations") == "write"

    guix_worker = workflow_data("guix-build-worker.yml", workflows)
    assert_no_write_permissions(guix_worker, "Guix worker")
    # Ensure the worker entrypoint job still exists (walk_steps covers its steps).
    job(guix_worker, "build")
    # Checkout isolation
    checkout_steps = [
        s for _, s in walk_steps(guix_worker)
        if uses_action(s, "actions/checkout@")
    ]
    assert checkout_steps, "guix-build-worker must check out source"
    for step in checkout_steps:
        with_block = step.get("with") or {}
        assert with_block.get("ref") == "${{ inputs.source-ref }}"
        assert with_block.get("persist-credentials") is False
        assert with_block.get("allow-unsafe-pr-checkout") == (
            "${{ github.event_name == 'pull_request_target' }}"
        )

    # Registry credentials removed before untrusted code runs further? At least
    # present as docker logout and password: github.token for the pull.
    assert contains_expr(guix_worker, "github.token")
    assert any(
        isinstance(s.get("run"), str) and "docker logout ghcr.io" in s["run"]
        for _, s in walk_steps(guix_worker)
    )
    # Cache saves / attestations gated on inputs.trusted
    for _, step in walk_steps(guix_worker):
        if uses_action(step, "actions/cache/save@") or uses_action(step, "actions/attest@"):
            condition = step.get("if")
            assert isinstance(condition, str) and "inputs.trusted" in condition, (
                "Guix worker privileged steps must be gated on inputs.trusted"
            )


def _write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def self_test() -> None:
    """Fixtures proving YAML-equivalent bypasses are rejected."""
    # Flow-mapping write permissions must be detected.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "flow-write.yml"
        _write(
            path,
            "on: push\npermissions: {contents: write}\njobs:\n  x:\n    runs-on: ubuntu-latest\n    steps: []\n",
        )
        data = load_workflow(path)
        try:
            assert_no_write_permissions(data, "flow-write")
        except AssertionError:
            pass
        else:
            raise AssertionError("flow-mapping permissions: write was not detected")

    # Quoted key write permissions must be detected.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "quoted-write.yml"
        _write(
            path,
            'on: push\npermissions:\n  "contents": write\njobs:\n  x:\n    runs-on: ubuntu-latest\n    steps: []\n',
        )
        data = load_workflow(path)
        try:
            assert_no_write_permissions(data, "quoted-write")
        except AssertionError:
            pass
        else:
            raise AssertionError("quoted-key permissions write was not detected")

    # Inline comment after write value must still be treated as write.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "comment-write.yml"
        _write(
            path,
            "on: push\npermissions:\n  contents: write # temporary\njobs:\n  x:\n    runs-on: ubuntu-latest\n    steps: []\n",
        )
        data = load_workflow(path)
        try:
            assert_no_write_permissions(data, "comment-write")
        except AssertionError:
            pass
        else:
            raise AssertionError("comment-suffixed write permission was not detected")

    # Cache save without trusted guard must fail.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "cache.yml"
        _write(
            path,
            (
                "on: push\n"
                "jobs:\n"
                "  x:\n"
                "    runs-on: ubuntu-latest\n"
                "    steps:\n"
                "      - uses: actions/cache/save@v5\n"
                "        with:\n"
                "          path: x\n"
                "          key: k\n"
            ),
        )
        data = load_workflow(path)
        try:
            check_workflow_invariants(path.name, data)
        except AssertionError as exc:
            assert "trusted-run guard" in str(exc)
        else:
            raise AssertionError("unguarded cache save was not rejected")

    # PR checkout without persist-credentials: false must fail.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "checkout.yml"
        _write(
            path,
            (
                "on: pull_request_target\n"
                "jobs:\n"
                "  x:\n"
                "    runs-on: ubuntu-latest\n"
                "    steps:\n"
                "      - uses: actions/checkout@v6\n"
                "        with:\n"
                "          ref: ${{ github.event.pull_request.head.sha }}\n"
            ),
        )
        data = load_workflow(path)
        try:
            check_workflow_invariants(path.name, data)
        except AssertionError as exc:
            assert "persist checkout credentials" in str(exc)
        else:
            raise AssertionError("PR checkout without persist-credentials: false passed")

    # Combined actions/cache must fail.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "combined-cache.yml"
        _write(
            path,
            (
                "on: push\n"
                "jobs:\n"
                "  x:\n"
                "    runs-on: ubuntu-latest\n"
                "    steps:\n"
                "      - uses: actions/cache@v5\n"
                "        with:\n"
                "          path: x\n"
                "          key: k\n"
            ),
        )
        data = load_workflow(path)
        try:
            check_workflow_invariants(path.name, data)
        except AssertionError as exc:
            assert "explicit cache restore/save" in str(exc)
        else:
            raise AssertionError("combined actions/cache was not rejected")

    # `on` must not remain coerced to True after normalize_gha.
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "on-key.yml"
        _write(path, "on: push\njobs: {}\n")
        data = load_workflow(path)
        assert "on" in data and True not in data, data

    # Boolean false values must remain bools, not the string "false".
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bool-false.yml"
        _write(
            path,
            (
                "on: pull_request_target\n"
                "jobs:\n"
                "  x:\n"
                "    runs-on: ubuntu-latest\n"
                "    steps:\n"
                "      - uses: actions/checkout@v6\n"
                "        with:\n"
                "          ref: ${{ github.event.pull_request.head.sha }}\n"
                "          persist-credentials: false\n"
                "          allow-unsafe-pr-checkout: true\n"
            ),
        )
        data = load_workflow(path)
        step = data["jobs"]["x"]["steps"][0]
        assert step["with"]["persist-credentials"] is False
        check_workflow_invariants(path.name, data)


if __name__ == "__main__":
    try:
        self_test()
    except AssertionError as exc:
        print(f"ERROR: lint-ci-workflow-security self-test failed: {exc}", file=sys.stderr)
        sys.exit(1)
    try:
        main()
    except AssertionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
    print("OK: workflow trust-boundary checks passed")
