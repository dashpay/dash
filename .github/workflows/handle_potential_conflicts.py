#!/usr/bin/env python3
# Copyright (c) 2022-2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Find open PRs that conflict with each other and maintain advisory PR comments.

Usage:
    $ ./handle_potential_conflicts.py [--dry-run]

Each run checks every pair of open, ready-for-review PRs that share a base
branch and touch the same files, then rewrites one managed comment per PR from
the results. A pair the run cannot check keeps what the comments already say.
"""

from __future__ import annotations

import argparse
import base64
import html
import http.client
import json
import os
import re
import sys
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any
from urllib.parse import quote


COMMENT_MARKER = "dash-potential-conflicts:v1"
COMMENT_START = f"<!-- {COMMENT_MARKER}"
COMMENT_END = "-->"
TRUSTED_COMMENT_AUTHORS = {"github-actions[bot]"}
MAX_FILE_DETAILS = 12
# How long a PR that was closed or made a draft keeps being checked for a comment to delete.
CLEANUP_WINDOW = timedelta(days=1)
# Error statuses, network errors and timeouts are OSError; a malformed body is ValueError.
REQUEST_ERRORS = (OSError, http.client.HTTPException, ValueError)


@dataclass
class ManagedComment:
    comment_id: int
    body: str
    listed_prs: set[int]


def github_headers() -> dict[str, str]:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"

    return headers


def http_request(method: str, url: str, headers: dict[str, str], payload: Any = None) -> tuple[str, str]:
    """Return the response body and its Link header; raise on an error status."""
    data = None
    if payload is not None:
        data = json.dumps(payload).encode()
        headers = {**headers, "Content-Type": "application/json"}
    request = urllib.request.Request(url, data=data, method=method)
    # Unlike requests, urllib would otherwise send the token on to a redirect's host.
    for name, value in headers.items():
        request.add_unredirected_header(name, value)
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read().decode(), response.headers.get("Link", "")


def next_page_url(link_header: str) -> str | None:
    match = re.search(r'<([^>]+)>;\s*rel="next"', link_header)
    return match.group(1) if match else None


def repo_name() -> str:
    return os.environ.get("GITHUB_REPOSITORY", "dashpay/dash")


def api_url(path: str) -> str:
    return f"https://api.github.com/repos/{repo_name()}/{path.lstrip('/')}"


def github_get(path_or_url: str) -> Any:
    url = path_or_url if path_or_url.startswith("https://") else api_url(path_or_url)
    body, _link = http_request("GET", url, github_headers())
    return json.loads(body)


def github_get_paginated(path: str) -> list[dict[str, Any]]:
    url = api_url(path)
    items = []

    while url:
        body, link = http_request("GET", url, github_headers())
        page = json.loads(body)
        if not isinstance(page, list):
            raise ValueError(f"Expected a list response from {url}")
        items.extend(page)
        url = next_page_url(link)

    return items


def github_patch(path: str, payload: dict[str, Any]) -> None:
    http_request("PATCH", api_url(path), github_headers(), payload)


def github_post(path: str, payload: dict[str, Any]) -> None:
    http_request("POST", api_url(path), github_headers(), payload)


def github_delete(path: str) -> None:
    http_request("DELETE", api_url(path), github_headers())


def listed_prs_from_state(state: Any) -> set[int]:
    if not isinstance(state, dict):
        return set()

    if "conflicts" in state:
        entries = state["conflicts"]
    else:
        # Comments written when each PR event ran its own check stored both merge orders.
        outbound = state.get("outbound")
        inbound = state.get("inbound")
        entries = [
            *(outbound if isinstance(outbound, list) else []),
            *(inbound.values() if isinstance(inbound, dict) else []),
        ]

    numbers = set()
    for entry in entries if isinstance(entries, list) else []:
        try:
            numbers.add(int(entry["number"] if isinstance(entry, dict) else entry))
        except (KeyError, TypeError, ValueError):
            continue
    return numbers


def extract_listed_prs(body: str) -> set[int]:
    # The state block is rendered last, but comments written before that change
    # start with it. Anchor on the last marker unless the body opens with one,
    # so marker-like text in advisory content cannot shadow the real block.
    marker_index = 0 if body.startswith(COMMENT_START) else body.rfind(COMMENT_START)
    if marker_index == -1:
        return set()

    json_start = body.find("\n", marker_index)
    marker_end = body.find(COMMENT_END, marker_index)
    if json_start == -1 or marker_end == -1 or json_start > marker_end:
        return set()

    encoded_state = body[json_start:marker_end].strip()
    if not encoded_state:
        return set()

    try:
        raw_state = base64.b64decode(encoded_state).decode("utf8")
        return listed_prs_from_state(json.loads(raw_state))
    except ValueError:
        print("Warning: managed conflict comment has invalid state; replacing it", file=sys.stderr)
        return set()


def compact_title(title: str) -> str:
    return " ".join(str(title).split())


def escape_markdown_link_text(text: str) -> str:
    return str(text).replace("\\", "\\\\").replace("[", "\\[").replace("]", "\\]")


def format_code_span(text: str) -> str:
    return f"<code>{html.escape(str(text), quote=False)}</code>"


def format_files(files: list[str]) -> str:
    if not files:
        return ""

    visible_files = files[:MAX_FILE_DETAILS]
    formatted = ", ".join(format_code_span(filename) for filename in visible_files)
    remaining = len(files) - len(visible_files)
    if remaining > 0:
        formatted = f"{formatted}, and {remaining} more"

    return f" Changed files: {formatted}."


def format_pr_line(item: dict[str, Any]) -> str:
    number = int(item["number"])
    title = escape_markdown_link_text(compact_title(item.get("title", "Unknown title")))
    url = item.get("url") or f"https://github.com/{repo_name()}/pull/{number}"
    files = [str(filename) for filename in item.get("files", [])]
    return f"- [#{number}: {title}]({url}){format_files(files)}"


def render_comment_body(conflicts: list[dict[str, Any]]) -> str:
    conflicts = sorted(conflicts, key=lambda item: int(item["number"]))
    state_json = json.dumps({"conflicts": [int(item["number"]) for item in conflicts]}, separators=(",", ":"))
    state_encoded = base64.b64encode(state_json.encode("utf8")).decode("ascii")
    lines = [
        "## Potential PR merge conflicts",
        "",
        "This is advisory only. It does not block CI. This PR and each open PR below change the same files and "
        "cannot be merged together cleanly, so whichever of the two merges second will likely need a rebase:",
        "",
    ]
    lines.extend(format_pr_line(item) for item in conflicts)

    # The state block goes last so notification previews start with the advisory text.
    lines.extend([
        "",
        COMMENT_START,
        state_encoded,
        COMMENT_END,
    ])

    return "\n".join(lines) + "\n"


def list_issue_comments(pr_num: int) -> list[dict[str, Any]]:
    return github_get_paginated(f"issues/{pr_num}/comments?per_page=100")


def is_trusted_comment_author(comment: dict[str, Any]) -> bool:
    user = comment.get("user")
    if not isinstance(user, dict):
        return False
    return user.get("login") in TRUSTED_COMMENT_AUTHORS


def find_managed_comments(pr_num: int) -> list[ManagedComment]:
    managed_comments = []

    for comment in list_issue_comments(pr_num):
        if not is_trusted_comment_author(comment):
            continue

        body = comment.get("body") or ""
        if COMMENT_START in body:
            managed_comments.append(ManagedComment(
                comment_id=comment["id"],
                body=body,
                listed_prs=extract_listed_prs(body),
            ))

    return managed_comments


def save_managed_comment(pr_num: int, managed_comments: list[ManagedComment], conflicts: list[dict[str, Any]]) -> None:
    """Make the PR's managed comment list `conflicts`, or delete it if there are none."""
    kept = managed_comments[-1] if conflicts and managed_comments else None
    if conflicts:
        body = render_comment_body(conflicts)
        if kept is None:
            github_post(f"issues/{pr_num}/comments", {"body": body})
            print(f"Created conflict comment on PR #{pr_num}", file=sys.stderr)
        elif kept.body != body:
            github_patch(f"issues/comments/{kept.comment_id}", {"body": body})
            print(f"Updated conflict comment on PR #{pr_num}", file=sys.stderr)

    for comment in managed_comments:
        if comment is not kept:
            github_delete(f"issues/comments/{comment.comment_id}")
            print(f"Deleted {'duplicate' if conflicts else 'stale'} conflict comment on PR #{pr_num}", file=sys.stderr)


def build_pr_item(pr_json: dict[str, Any], files: list[str]) -> dict[str, Any]:
    return {
        "number": int(pr_json["number"]),
        "title": compact_title(pr_json.get("title", "Unknown title")),
        "url": pr_json.get("html_url") or f"https://github.com/{repo_name()}/pull/{pr_json['number']}",
        "files": sorted(set(str(filename) for filename in files)),
    }


def file_names_from_pr_file(file_info: dict[str, Any]) -> set[str]:
    names = set()

    filename = file_info.get("filename")
    if filename:
        names.add(str(filename))

    previous_filename = file_info.get("previous_filename")
    if previous_filename:
        names.add(str(previous_filename))

    return names


def get_pr_files(pr_num: int) -> set[str] | None:
    files = set()
    try:
        for file_info in github_get_paginated(f"pulls/{pr_num}/files?per_page=100"):
            files.update(file_names_from_pr_file(file_info))
    except REQUEST_ERRORS as e:
        print(f"Warning: Error fetching the files of PR {pr_num}: {e}", file=sys.stderr)
        return None
    return files


def pr_needs_rebase(pr_num: int) -> bool | None:
    """Return None if the PR could not be fetched or GitHub has not yet checked it against its base."""
    try:
        mergeable_state = github_get(f"pulls/{pr_num}").get("mergeable_state")
    except REQUEST_ERRORS as e:
        print(f"Warning: Error fetching PR {pr_num}: {e}", file=sys.stderr)
        return None
    return None if mergeable_state == "unknown" else mergeable_state == "dirty"


def branches_can_merge(our_pr_label: str, conflict_pr_label: str) -> bool | None:
    merge_check_url = (
        f"https://github.com/{repo_name()}/branches/pre_mergeable/"
        f"{quote(our_pr_label, safe='')}...{quote(conflict_pr_label, safe='')}"
    )
    headers = {"User-Agent": "dash-potential-conflicts"}

    try:
        text, _link = http_request("GET", merge_check_url, headers)
    except REQUEST_ERRORS as e:
        print(f"Error checking mergeability at {merge_check_url}: {e}", file=sys.stderr)
        return None

    if "These branches can be automatically merged." in text:
        return True
    if "Can't automatic" in text or "octicon octicon-x" in text:
        return False

    print(f"Warning: Unexpected mergeability response: {merge_check_url}", file=sys.stderr)
    return None


def find_conflicts(prs: list[dict[str, Any]]) -> tuple[dict[int, list[dict[str, Any]]], dict[int, dict[int, dict[str, Any]]]]:
    """Return the PRs each PR conflicts with, and the PRs each one overlaps but could not be checked against."""
    files = {int(pr["number"]): get_pr_files(int(pr["number"])) for pr in prs}
    conflicts: dict[int, list[dict[str, Any]]] = {number: [] for number in files}
    unchecked: dict[int, dict[int, dict[str, Any]]] = {number: {} for number in files}
    needs_rebase: dict[int, bool | None] = {}

    for index, pr in enumerate(prs):
        for other in prs[index + 1:]:
            if pr["base"]["ref"] != other["base"]["ref"]:
                continue
            a, b = int(pr["number"]), int(other["number"])
            files_a, files_b = files[a], files[b]
            if files_a is None or files_b is None:
                # Without both file lists the overlap is unknown, so the pair stays unchecked.
                overlap = []
            else:
                overlap = sorted(files_a & files_b)
                if not overlap:
                    continue

            can_merge = None
            if overlap:
                for number in (a, b):
                    if number not in needs_rebase:
                        needs_rebase[number] = pr_needs_rebase(number)
                # A PR that already needs a rebase is checked again once it is rebased.
                if needs_rebase[a] is False and needs_rebase[b] is False:
                    # A merge conflict is symmetric, so one check covers both merge orders.
                    can_merge = branches_can_merge(pr["head"]["label"], other["head"]["label"])

            if can_merge is None:
                unchecked[a][b] = build_pr_item(other, overlap)
                unchecked[b][a] = build_pr_item(pr, overlap)
            elif not can_merge:
                conflicts[a].append(build_pr_item(other, overlap))
                conflicts[b].append(build_pr_item(pr, overlap))

    return conflicts, unchecked


def recently_updated(pr: dict[str, Any], since: datetime) -> bool:
    return datetime.fromisoformat(pr["updated_at"].replace("Z", "+00:00")) >= since


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Find conflicting open PRs and maintain advisory comments.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the comment bodies instead of creating, updating, or deleting comments.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    open_prs = github_get_paginated("pulls?state=open&per_page=100")
    ready_prs = [pr for pr in open_prs if not pr.get("draft", False)]
    # Closing a PR or making it a draft updates it, so this finds every PR that left the
    # checked set lately, even if no comment lists it or an earlier run failed to clean it.
    since = datetime.now(timezone.utc) - CLEANUP_WINDOW
    closed_prs = github_get("pulls?state=closed&sort=updated&direction=desc&per_page=100")
    left_prs = [pr for pr in open_prs if pr.get("draft", False)] + closed_prs
    recently_left = {int(pr["number"]) for pr in left_prs if recently_updated(pr, since)}
    conflicts, unchecked = find_conflicts(ready_prs)

    failed = []
    previously_listed = set()
    for number, items in sorted(conflicts.items()):
        try:
            managed_comments = find_managed_comments(number)
            listed = set().union(*(comment.listed_prs for comment in managed_comments))
            previously_listed |= listed
            # Keep what the comment already says about PRs this run could not check.
            items = items + [item for other, item in unchecked[number].items() if other in listed]
            if args.dry_run:
                print(f"PR #{number}:\n{render_comment_body(items) if items else 'no comment'}\n")
            else:
                save_managed_comment(number, managed_comments, items)
        except REQUEST_ERRORS as e:
            print(f"Warning: could not update the conflict comment on PR #{number}: {e}", file=sys.stderr)
            failed.append(number)

    # Drafts and closed PRs are not checked; clear any comment they still have
    # from when they were open and ready for review.
    for number in sorted((previously_listed | recently_left) - conflicts.keys()):
        if args.dry_run:
            print(f"PR #{number}: delete any conflict comment\n")
            continue
        try:
            save_managed_comment(number, find_managed_comments(number), [])
        except REQUEST_ERRORS as e:
            print(f"Warning: could not delete the conflict comment on PR #{number}: {e}", file=sys.stderr)
            failed.append(number)

    if failed:
        sys.exit(f"Error: could not update conflict comments on PRs {failed}")


if __name__ == "__main__":
    main()
