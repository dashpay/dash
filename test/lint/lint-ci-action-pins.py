#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Require remote GitHub Actions to be pinned to immutable commit SHAs.

Parses workflow YAML so equivalent forms (quoted keys, flow mappings, nested
jobs/steps) cannot bypass the pin check. Version comments are verified from
source text because YAML discards comments.
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable

import yaml


ROOT = Path(__file__).resolve().parents[2]
GITHUB_DIR = ROOT / ".github"

# owner/name[/path...]@ref
REMOTE_ACTION_RE = re.compile(
    r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*@(?P<ref>[^\s#]+)$"
)
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
VERSION_COMMENT_RE = re.compile(r"^v?\d+(?:\.\d+){0,3}$")
# Source-text match for a remote pin plus an exact release-version comment.
PIN_WITH_VERSION_RE = re.compile(
    r"(?P<ref>[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*@[0-9a-f]{40})"
    r"[^\n#]*#\s*(?P<version>\S+)"
)


def normalize_gha(node: Any) -> Any:
    """Repair YAML 1.1 bool coercion of the GitHub Actions `on` key.

    PyYAML's SafeLoader turns the bare key `on:` into boolean True. Values such
    as `true`/`false` must remain real booleans for permission and checkout
    checks elsewhere, so only mapping keys are rewritten.
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


def load_yaml(text: str) -> Any:
    return normalize_gha(yaml.safe_load(text))


def iter_uses(node: Any) -> Iterable[str]:
    """Yield every mapping value whose key is the string 'uses'."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "uses":
                if isinstance(value, str):
                    yield value
                else:
                    yield f"<non-string uses: {value!r}>"
            else:
                yield from iter_uses(value)
    elif isinstance(node, list):
        for item in node:
            yield from iter_uses(item)


def versioned_pins(text: str) -> dict[str, str]:
    """Map full remote pin refs to their trailing version-comment token."""
    found: dict[str, str] = {}
    for match in PIN_WITH_VERSION_RE.finditer(text):
        found[match.group("ref")] = match.group("version")
    return found


def check_workflow(path: Path, rel: Path, text: str, data: Any) -> list[str]:
    errors: list[str] = []
    if data is None:
        # Empty YAML documents have nothing to pin.
        return errors

    pins = versioned_pins(text)
    for ref in iter_uses(data):
        if ref.startswith("./"):
            continue
        if ref.startswith("docker://"):
            errors.append(
                f"{rel}: unsupported docker:// action {ref!r}; "
                f"pin remote GitHub Actions with owner/name@<40-hex-sha>"
            )
            continue

        remote = REMOTE_ACTION_RE.match(ref)
        if remote is None:
            errors.append(
                f"{rel}: unsupported `uses:` reference {ref!r} "
                f"(expected owner/name[/...]@<40-hex-sha> or local ./ path)"
            )
            continue

        pin = remote.group("ref")
        if not SHA_RE.fullmatch(pin):
            errors.append(
                f"{rel}: mutable GitHub Action ref {ref!r}; "
                f"pin to a full 40-character commit SHA"
            )
            continue

        version = pins.get(ref)
        if not version or not VERSION_COMMENT_RE.fullmatch(version):
            errors.append(
                f"{rel}: pinned action {ref!r} must include an exact "
                f"release-version comment (for example `# v6.1.0`)"
            )
    return errors


def check_github_dir(github_dir: Path, root: Path | None = None) -> list[str]:
    root = root or github_dir.parent
    errors: list[str] = []
    workflow_files = sorted(github_dir.rglob("*.yml")) + sorted(github_dir.rglob("*.yaml"))
    if not workflow_files:
        return [f"ERROR: no workflow YAML files found under {github_dir}"]

    for path in workflow_files:
        rel = path.relative_to(root)
        text = path.read_text(encoding="utf-8")
        try:
            data = load_yaml(text)
        except yaml.YAMLError as exc:
            errors.append(f"{rel}: YAML parse error: {exc}")
            continue
        errors.extend(check_workflow(path, rel, text, data))
    return errors


def _write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def self_test() -> None:
    """Negative and positive fixtures for alternate YAML uses: forms."""
    good_sha = "d23441a48e516b6c34aea4fa41551a30e30af803"
    good_ref = f"actions/checkout@{good_sha}"

    cases: list[tuple[str, str, bool, str]] = [
        (
            "quoted-key-mutable",
            f'jobs:\n  x:\n    steps:\n      - "uses": actions/checkout@v6\n',
            False,
            "mutable",
        ),
        (
            "single-quoted-key-mutable",
            "jobs:\n  x:\n    steps:\n      - 'uses': actions/cache@v5\n",
            False,
            "mutable",
        ),
        (
            "flow-mapping-mutable",
            "jobs:\n  x:\n    steps:\n      - {uses: actions/checkout@v6}\n",
            False,
            "mutable",
        ),
        (
            "nested-job-mutable",
            (
                "on: push\n"
                "jobs:\n"
                "  build:\n"
                "    steps:\n"
                "      - name: hi\n"
                "        uses: actions/setup-python@v5\n"
            ),
            False,
            "mutable",
        ),
        (
            "good-pin-with-version",
            (
                "jobs:\n"
                "  x:\n"
                "    steps:\n"
                f"      - uses: {good_ref} # v6.1.0\n"
            ),
            True,
            "",
        ),
        (
            "good-pin-quoted-key",
            (
                "jobs:\n"
                "  x:\n"
                "    steps:\n"
                f'      - "uses": {good_ref} # v6.1.0\n'
            ),
            True,
            "",
        ),
        (
            "flow-mapping-good",
            (
                "jobs:\n"
                "  x:\n"
                "    steps:\n"
                f"      - {{uses: {good_ref}}} # v6.1.0\n"
            ),
            True,
            "",
        ),
        (
            "missing-version-comment",
            (
                "jobs:\n"
                "  x:\n"
                "    steps:\n"
                f"      - uses: {good_ref}\n"
            ),
            False,
            "release-version comment",
        ),
        (
            "local-action-ok",
            "jobs:\n  x:\n    steps:\n      - uses: ./.github/actions/local\n",
            True,
            "",
        ),
        (
            "inline-comment-mutable",
            (
                "jobs:\n"
                "  x:\n"
                "    steps:\n"
                "      - uses: actions/checkout@v6  # not a pin\n"
            ),
            False,
            "mutable",
        ),
        (
            "docker-action-rejected",
            "jobs:\n  x:\n    steps:\n      - uses: docker://alpine:3.19\n",
            False,
            "docker://",
        ),
    ]

    for name, body, expect_ok, needle in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root / ".github" / "workflows" / f"{name}.yml", body)
            # Keep `on` un-coerced in fixtures that use it.
            errors = check_github_dir(root / ".github", root=root)
            ok = not errors
            if ok != expect_ok:
                raise AssertionError(
                    f"self-test {name}: expected ok={expect_ok}, errors={errors}"
                )
            if not expect_ok and needle and not any(needle in e for e in errors):
                raise AssertionError(
                    f"self-test {name}: expected error containing {needle!r}, got {errors}"
                )

    # Malformed YAML must fail closed.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _write(root / ".github" / "workflows" / "broken.yml", "jobs: [\n")
        errors = check_github_dir(root / ".github", root=root)
        if not errors or "YAML parse error" not in errors[0]:
            raise AssertionError(f"self-test broken-yaml: expected parse error, got {errors}")


def main() -> int:
    try:
        self_test()
    except AssertionError as exc:
        print(f"ERROR: lint-ci-action-pins self-test failed: {exc}", file=sys.stderr)
        return 1

    errors = check_github_dir(GITHUB_DIR, root=ROOT)
    # Distinguish missing-files from pin violations.
    if len(errors) == 1 and errors[0].startswith("ERROR: no workflow"):
        print(errors[0], file=sys.stderr)
        return 1

    if errors:
        print("ERROR: GitHub Actions must be pinned to immutable SHAs with version comments:")
        for error in errors:
            print(f"  {error}")
        return 1

    print("OK: all remote GitHub Actions are pinned to immutable SHAs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
