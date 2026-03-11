# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple


DEFAULT_REPOS = (
    "dashpay/dash",
    "dashpay/platform",
    "dashpay/grovedb",
    "dashpay/rust-dashcore",
)
DEFAULT_STATUSES = ("queued", "in_progress")
DEFAULT_RUNNER_AMD64 = "ubuntu-24.04"
DEFAULT_RUNNER_ARM64 = "ubuntu-24.04-arm"
REQUEST_TIMEOUT_SECONDS = 10


def parse_next_link(link_header: str) -> Optional[str]:
    if not link_header:
        return None

    for part in link_header.split(","):
        match = re.match(r'\s*<([^>]+)>;\s*rel="([^"]+)"', part.strip())
        if match and match.group(2) == "next":
            return match.group(1)

    return None


def request_json(url: str, token: str) -> Tuple[Dict, Dict[str, str]]:
    attempts = [True, False] if token else [False]
    last_error = None

    for use_auth in attempts:
        headers = {
            "Accept": "application/vnd.github+json",
            "User-Agent": "dash-ci-runner-selector",
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if use_auth:
            headers["Authorization"] = "Bearer {}".format(token)

        request = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(
                request, timeout=REQUEST_TIMEOUT_SECONDS
            ) as response:
                payload = json.load(response)
                return payload, dict(response.headers.items())
        except urllib.error.HTTPError as exc:
            last_error = exc
            if use_auth and exc.code in (401, 403, 404):
                continue
            raise

    if last_error is not None:
        raise last_error
    raise RuntimeError("Request failed for {}".format(url))


def iter_pages(
    fetch_json: Callable[[str], Tuple[Dict, Dict[str, str]]],
    url: str,
) -> Iterable[Dict]:
    next_url = url
    while next_url:
        payload, headers = fetch_json(next_url)
        yield payload
        next_url = parse_next_link(headers.get("Link", ""))


def count_queued_jobs(
    fetch_json: Callable[[str], Tuple[Dict, Dict[str, str]]],
    repos: Sequence[str],
    statuses: Sequence[str] = DEFAULT_STATUSES,
) -> int:
    queued_jobs = 0

    for repo in repos:
        run_ids = set()
        for status in statuses:
            runs_url = (
                "https://api.github.com/repos/{}/actions/runs"
                "?status={}&per_page=100"
            ).format(repo, status)
            for payload in iter_pages(fetch_json, runs_url):
                for run in payload.get("workflow_runs", []):
                    run_id = run.get("id")
                    if run_id is not None:
                        run_ids.add(run_id)

        for run_id in sorted(run_ids):
            jobs_url = (
                "https://api.github.com/repos/{}/actions/runs/{}/jobs?per_page=100"
            ).format(repo, run_id)
            for payload in iter_pages(fetch_json, jobs_url):
                for job in payload.get("jobs", []):
                    if job.get("status") == "queued":
                        queued_jobs += 1

    return queued_jobs


def load_event(event_path: str) -> Dict:
    with open(event_path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def select_runners(
    event_name: str,
    event: Dict,
    threshold: int,
    runner_amd64_var: str,
    runner_arm64_var: str,
    fetch_json: Callable[[str], Tuple[Dict, Dict[str, str]]],
    repos: Sequence[str] = DEFAULT_REPOS,
) -> Dict[str, str]:
    label_names = [
        label.get("name", "")
        for label in event.get("pull_request", {}).get("labels", [])
    ]
    label_override = (
        event_name == "pull_request_target" and "blacksmith-ci" in label_names
    )

    backlog_count = "unknown"
    backlog_count_value = None
    measurement_error = None

    try:
        backlog_count_value = count_queued_jobs(fetch_json, repos)
        backlog_count = str(backlog_count_value)
    except Exception as exc:  # noqa: BLE001
        measurement_error = "{}: {}".format(type(exc).__name__, exc)

    use_blacksmith = False
    decision_parts = []

    if label_override:
        use_blacksmith = True
        decision_parts.append("label:blacksmith-ci")
    elif measurement_error is not None:
        decision_parts.append("metric-unavailable")
    elif backlog_count_value > threshold:
        use_blacksmith = True
        decision_parts.append("backlog:{}>{}".format(backlog_count_value, threshold))
    else:
        decision_parts.append("backlog:{}<={}".format(backlog_count_value, threshold))

    runner_amd64 = DEFAULT_RUNNER_AMD64
    runner_arm64 = DEFAULT_RUNNER_ARM64
    fallback_parts: List[str] = []

    if use_blacksmith:
        if runner_amd64_var:
            runner_amd64 = runner_amd64_var
        else:
            fallback_parts.append("amd64-github-fallback")

        if runner_arm64_var:
            runner_arm64 = runner_arm64_var
        else:
            fallback_parts.append("arm64-github-fallback")

    if measurement_error is not None:
        decision_parts.append("error:{}".format(measurement_error[:180]))
    decision_parts.extend(fallback_parts)

    return {
        "runner_amd64": runner_amd64,
        "runner_arm64": runner_arm64,
        "use_blacksmith": "true" if use_blacksmith else "false",
        "backlog_count": backlog_count,
        "decision_reason": ";".join(decision_parts),
        "label_override": "true" if label_override else "false",
    }


def write_github_output(path: Optional[str], outputs: Dict[str, str]) -> None:
    if not path:
        return

    with open(path, "a", encoding="utf-8") as fh:
        for key, value in outputs.items():
            if key == "label_override":
                continue
            fh.write("{}={}\n".format(key, value))


def write_step_summary(path: Optional[str], outputs: Dict[str, str]) -> None:
    if not path:
        return

    with open(path, "a", encoding="utf-8") as fh:
        fh.write("### Runner selection\n")
        fh.write(
            "- PR label override: {}\n".format(
                "yes" if outputs["label_override"] == "true" else "no"
            )
        )
        fh.write("- Aggregated queued jobs: {}\n".format(outputs["backlog_count"]))
        fh.write(
            "- Use Blacksmith: {}\n".format(
                "yes" if outputs["use_blacksmith"] == "true" else "no"
            )
        )
        fh.write("- amd64 runner: `{}`\n".format(outputs["runner_amd64"]))
        fh.write("- arm64 runner: `{}`\n".format(outputs["runner_arm64"]))
        fh.write("- Decision: `{}`\n".format(outputs["decision_reason"]))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Select GitHub Actions runners dynamically.")
    parser.add_argument(
        "--event-name",
        default=os.environ.get("GITHUB_EVENT_NAME", ""),
        help="GitHub event name",
    )
    parser.add_argument(
        "--event-path",
        default=os.environ.get("GITHUB_EVENT_PATH", ""),
        help="Path to GitHub event payload JSON",
    )
    parser.add_argument(
        "--backlog-threshold",
        type=int,
        default=int(os.environ.get("BACKLOG_THRESHOLD", "10")),
        help="Queued job threshold for switching to Blacksmith",
    )
    parser.add_argument(
        "--runner-amd64-var",
        default=os.environ.get("RUNNER_AMD64_VAR", ""),
        help="Blacksmith runner label for amd64",
    )
    parser.add_argument(
        "--runner-arm64-var",
        default=os.environ.get("RUNNER_ARM64_VAR", ""),
        help="Blacksmith runner label for arm64",
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("GH_TOKEN", ""),
        help="GitHub API token",
    )
    parser.add_argument(
        "--repo",
        action="append",
        dest="repos",
        help="Repo to include in backlog counting; may be provided multiple times",
    )
    parser.add_argument(
        "--print-json",
        action="store_true",
        help="Print the selected runners and decision payload as JSON",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)

    if not args.event_name:
        print("error: missing event name", file=sys.stderr)
        return 1
    if not args.event_path:
        print("error: missing event path", file=sys.stderr)
        return 1

    repos = tuple(args.repos or DEFAULT_REPOS)
    event = load_event(args.event_path)
    fetch_json = lambda url: request_json(url, args.token)

    outputs = select_runners(
        event_name=args.event_name,
        event=event,
        threshold=args.backlog_threshold,
        runner_amd64_var=args.runner_amd64_var,
        runner_arm64_var=args.runner_arm64_var,
        fetch_json=fetch_json,
        repos=repos,
    )

    write_github_output(os.environ.get("GITHUB_OUTPUT"), outputs)
    write_step_summary(os.environ.get("GITHUB_STEP_SUMMARY"), outputs)

    if args.print_json or "GITHUB_OUTPUT" not in os.environ:
        json.dump(outputs, sys.stdout, sort_keys=True)
        sys.stdout.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
