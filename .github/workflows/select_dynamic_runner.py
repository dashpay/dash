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
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Set, Tuple


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
# Prefixes — any label starting with one of these indicates a non-GitHub-hosted runner
NON_GITHUB_HOSTED_RUNNER_PREFIXES = ("blacksmith-",)
SELF_HOSTED_LABEL = "self-hosted"
# Any queued GitHub-hosted job at all is enough to prefer our own hardware for lint.
SELFHOSTED_BACKLOG_THRESHOLD = 0


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


def targets_github_hosted_runner(
    job: Dict,
    non_hosted_labels: Sequence[str] = (),
) -> bool:
    """Return True if the job targets GitHub-hosted runners, not Blacksmith or self-hosted.

    non_hosted_labels carries any additional bare labels that name our own
    hardware. A job asking for one of those is not competing for the
    account-wide GitHub-hosted concurrency limit, so counting it would inflate
    the very backlog figure used to decide whether to escalate to Blacksmith.
    """
    extra = {label.lower() for label in non_hosted_labels if label}
    for label in job.get("labels", []):
        if not isinstance(label, str):
            continue
        if label == SELF_HOSTED_LABEL or label.lower() in extra:
            return False
        for prefix in NON_GITHUB_HOSTED_RUNNER_PREFIXES:
            if label.startswith(prefix):
                return False
    return True


def count_queued_jobs(
    fetch_json: Callable[[str], Tuple[Dict, Dict[str, str]]],
    repos: Sequence[str],
    statuses: Sequence[str] = DEFAULT_STATUSES,
    non_hosted_labels: Sequence[str] = (),
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
                    if job.get("status") == "queued" and targets_github_hosted_runner(
                        job, non_hosted_labels
                    ):
                        queued_jobs += 1

    return queued_jobs


def load_event(event_path: str) -> Dict:
    with open(event_path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def selfhosted_backlog_exclusions(
    runner_selfhosted_var: str,
    runner_amd64_var: str,
    runner_arm64_var: str,
) -> Tuple[str, ...]:
    """Labels to leave out of the GitHub-hosted backlog count.

    Queued jobs on our own hardware are not competing for the account-wide
    hosted concurrency limit, so counting them would inflate the figure used to
    decide whether to escalate to Blacksmith. But the exclusion is a bare label
    match driven by a repository variable, and naming a hosted or Blacksmith
    label there would silently zero the backlog and switch the existing
    escalation ladder off with nothing appearing broken. Refuse to exclude
    anything that some other rung already routes to.
    """
    label = runner_selfhosted_var.strip().lower()
    if not label:
        return ()

    reserved = {
        DEFAULT_RUNNER_AMD64.lower(),
        DEFAULT_RUNNER_ARM64.lower(),
        runner_amd64_var.strip().lower(),
        runner_arm64_var.strip().lower(),
    } - {""}
    if label in reserved or label.startswith(NON_GITHUB_HOSTED_RUNNER_PREFIXES):
        return ()

    return (label,)


def parse_author_allowlist(raw: str) -> Set[str]:
    """Split a comma/whitespace separated list of logins into a lowercased set.

    GitHub logins are case-insensitive and this list is hand-maintained in a
    repository variable, so normalise rather than trust the exact spelling.
    """
    return {login.lower() for login in raw.replace(",", " ").split()}


def is_selfhosted_allowed(
    event_name: str,
    event: Dict,
    actor: str,
    allowed_authors: Set[str],
) -> bool:
    """Whether this event may run on our own hardware.

    build.yml runs on pull_request_target and checks out the pull request head,
    so the code that executes is supplied by the head repository - which is a
    fork for effectively every Dash pull request. A persistent self-hosted
    runner must therefore only be offered to a fixed allowlist of logins.
    """
    if actor.strip().lower() not in allowed_authors:
        return False

    if event_name == "push":
        # Pushing to this repository already requires write access.
        return True

    if event_name == "pull_request_target":
        pull_request = event.get("pull_request") or {}

        # Both must be allowlisted: the author is stable across synchronize,
        # while the actor is whoever pushed the head that is about to run. A
        # fork branch can be pushed to by someone other than the PR author.
        author = (pull_request.get("user") or {}).get("login") or ""
        if author.strip().lower() not in allowed_authors:
            return False

        # The tree that executes comes from the head repository, which is not
        # necessarily owned by the author: a pull request may be opened from any
        # readable fork. Trusting the people without checking whose repository
        # supplies the code would leave the gate open to a cross-fork head.
        head_repo = (pull_request.get("head") or {}).get("repo") or {}
        base_repo = (pull_request.get("base") or {}).get("repo") or {}
        head_name = head_repo.get("full_name") or ""
        base_name = base_repo.get("full_name") or ""
        if not head_name or not base_name:
            # Cannot attribute the tree to a repository; refuse rather than guess.
            return False
        if head_name.lower() != base_name.lower():
            head_owner = (head_repo.get("owner") or {}).get("login") or ""
            if head_owner.strip().lower() not in allowed_authors:
                return False

        return True

    return False


def select_lint_runner(
    event_name: str,
    event: Dict,
    actor: str,
    backlog_count_value: Optional[int],
    measurement_error: Optional[str],
    selfhosted_label: str,
    allowed_authors: Set[str],
    fallback_runner: str,
    label_override: bool = False,
) -> Tuple[str, str]:
    """Pick the runner for the lint job, and the reason for the pick.

    Inserts a self-hosted rung into the hosted -> Blacksmith ladder: once any
    GitHub-hosted job is queued, lint goes to our own hardware instead of
    competing for the account-wide concurrency limit. When the rung is not
    available lint keeps the caller's existing amd64 decision unchanged.
    """
    if not selfhosted_label:
        return fallback_runner, "selfhosted-disabled"

    if label_override:
        # Someone asked for Blacksmith explicitly; do not quietly send them to
        # our own hardware instead.
        return fallback_runner, "label:blacksmith-ci"

    if measurement_error is not None:
        return fallback_runner, "metric-unavailable"

    if backlog_count_value is None or backlog_count_value <= SELFHOSTED_BACKLOG_THRESHOLD:
        return fallback_runner, "backlog:{}<={}".format(
            backlog_count_value, SELFHOSTED_BACKLOG_THRESHOLD
        )

    if not is_selfhosted_allowed(event_name, event, actor, allowed_authors):
        return fallback_runner, "actor-not-allowed"

    return selfhosted_label, "selfhosted:backlog:{}>{}".format(
        backlog_count_value, SELFHOSTED_BACKLOG_THRESHOLD
    )


def select_runners(
    event_name: str,
    event: Dict,
    threshold: int,
    arm64_threshold: int,
    runner_amd64_var: str,
    runner_arm64_var: str,
    fetch_json: Callable[[str], Tuple[Dict, Dict[str, str]]],
    repos: Sequence[str] = DEFAULT_REPOS,
    runner_selfhosted_var: str = "",
    selfhosted_authors: str = "",
    actor: str = "",
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
        backlog_count_value = count_queued_jobs(
            fetch_json,
            repos,
            non_hosted_labels=selfhosted_backlog_exclusions(
                runner_selfhosted_var, runner_amd64_var, runner_arm64_var
            ),
        )
        backlog_count = str(backlog_count_value)
    except Exception as exc:  # noqa: BLE001
        measurement_error = "{}: {}".format(type(exc).__name__, exc)

    decision_parts = []

    if label_override:
        decision_parts.append("label:blacksmith-ci")
    elif measurement_error is not None:
        decision_parts.append("metric-unavailable")
    else:
        decision_parts.append(
            "amd64:backlog:{}{}".format(
                backlog_count_value,
                ">{}".format(threshold)
                if backlog_count_value > threshold else "<={}".format(threshold),
            )
        )
        decision_parts.append(
            "arm64:backlog:{}{}".format(
                backlog_count_value,
                ">{}".format(arm64_threshold)
                if backlog_count_value > arm64_threshold else "<={}".format(arm64_threshold),
            )
        )

    use_blacksmith_amd64 = label_override or (
        measurement_error is None and backlog_count_value > threshold
    )
    use_blacksmith_arm64 = label_override or (
        measurement_error is None and backlog_count_value > arm64_threshold
    )

    runner_amd64 = DEFAULT_RUNNER_AMD64
    runner_arm64 = DEFAULT_RUNNER_ARM64
    fallback_parts: List[str] = []

    if use_blacksmith_amd64:
        if runner_amd64_var:
            runner_amd64 = runner_amd64_var
        else:
            fallback_parts.append("amd64-github-fallback")

    if use_blacksmith_arm64:
        if runner_arm64_var:
            runner_arm64 = runner_arm64_var
        else:
            fallback_parts.append("arm64-github-fallback")

    if measurement_error is not None:
        decision_parts.append("error:{}".format(measurement_error[:180]))
    decision_parts.extend(fallback_parts)

    runner_lint, lint_decision_reason = select_lint_runner(
        event_name=event_name,
        event=event,
        actor=actor,
        backlog_count_value=backlog_count_value,
        measurement_error=measurement_error,
        selfhosted_label=runner_selfhosted_var,
        allowed_authors=parse_author_allowlist(selfhosted_authors),
        fallback_runner=runner_amd64,
        label_override=label_override,
    )

    return {
        "runner_amd64": runner_amd64,
        "runner_arm64": runner_arm64,
        "runner_lint": runner_lint,
        "lint_decision_reason": lint_decision_reason,
        "use_blacksmith": "true" if use_blacksmith_amd64 or use_blacksmith_arm64 else "false",
        "use_blacksmith_amd64": "true" if use_blacksmith_amd64 else "false",
        "use_blacksmith_arm64": "true" if use_blacksmith_arm64 else "false",
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
            "- Use Blacksmith amd64: {}\n".format(
                "yes" if outputs["use_blacksmith_amd64"] == "true" else "no"
            )
        )
        fh.write(
            "- Use Blacksmith arm64: {}\n".format(
                "yes" if outputs["use_blacksmith_arm64"] == "true" else "no"
            )
        )
        fh.write("- amd64 runner: `{}`\n".format(outputs["runner_amd64"]))
        fh.write("- arm64 runner: `{}`\n".format(outputs["runner_arm64"]))
        fh.write("- lint runner: `{}`\n".format(outputs["runner_lint"]))
        fh.write("- Decision: `{}`\n".format(outputs["decision_reason"]))
        fh.write("- Lint decision: `{}`\n".format(outputs["lint_decision_reason"]))


def env_int(name: str, default: int) -> int:
    value = os.environ.get(name, "")
    if value == "":
        return default
    return int(value)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    default_backlog_threshold = env_int("BACKLOG_THRESHOLD", 10)
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
        default=default_backlog_threshold,
        help="Queued job threshold for switching to Blacksmith",
    )
    parser.add_argument(
        "--arm64-backlog-threshold",
        type=int,
        default=env_int("BACKLOG_THRESHOLD_ARM64", default_backlog_threshold * 3),
        help="Queued job threshold for switching arm64 jobs to Blacksmith",
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
        "--runner-selfhosted-var",
        default=os.environ.get("RUNNER_SELFHOSTED_VAR", ""),
        help="Self-hosted runner label for the lint job; empty disables the rung",
    )
    parser.add_argument(
        "--selfhosted-authors",
        default=os.environ.get("SELFHOSTED_LINT_AUTHORS", ""),
        help="Comma separated logins allowed to run on self-hosted runners",
    )
    parser.add_argument(
        "--actor",
        default=os.environ.get("GITHUB_ACTOR", ""),
        help="Login that triggered this run",
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
        arm64_threshold=args.arm64_backlog_threshold,
        runner_amd64_var=args.runner_amd64_var,
        runner_arm64_var=args.runner_arm64_var,
        fetch_json=fetch_json,
        repos=repos,
        runner_selfhosted_var=args.runner_selfhosted_var,
        selfhosted_authors=args.selfhosted_authors,
        actor=args.actor,
    )

    write_github_output(os.environ.get("GITHUB_OUTPUT"), outputs)
    write_step_summary(os.environ.get("GITHUB_STEP_SUMMARY"), outputs)

    if args.print_json or "GITHUB_OUTPUT" not in os.environ:
        json.dump(outputs, sys.stdout, sort_keys=True)
        sys.stdout.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
