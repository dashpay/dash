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
# Any queued GitHub-hosted job at all is enough to prefer our own hardware for
# the pre-lint jobs.
SELFHOSTED_BACKLOG_THRESHOLD = 0
# Above this many queued jobs for a self-hosted label, our own runners are the
# bottleneck and the pre-lint jobs go back to the hosted ladder.
DEFAULT_SELFHOSTED_MAX_QUEUED = 6
# The fast path's build and test jobs hold a runner for 5-20 minutes rather
# than seconds, so a much shorter wait for one already loses to hosted.
DEFAULT_SELFHOSTED_FAST_MAX_QUEUED = 2
# Pushes to these branches only ever carry merged code, so they may use the
# fast path whoever pushed them.
FAST_PATH_PUSH_REFS = ("refs/heads/develop", "refs/heads/master")


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
) -> Tuple[int, Dict[str, int]]:
    """Count queued jobs in one scan of repos.

    Returns the number of queued GitHub-hosted jobs, and for each label in
    non_hosted_labels (lowercased) the number of queued jobs asking for it.
    """
    queued_jobs = 0
    queued_by_label = {label.lower(): 0 for label in non_hosted_labels if label}

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
                    if job.get("status") != "queued":
                        continue
                    if targets_github_hosted_runner(job, non_hosted_labels):
                        queued_jobs += 1
                    labels = {
                        label.lower()
                        for label in job.get("labels", [])
                        if isinstance(label, str)
                    }
                    for label in queued_by_label:
                        if label in labels:
                            queued_by_label[label] += 1

    return queued_jobs, queued_by_label


def load_event(event_path: str) -> Dict:
    with open(event_path, "r", encoding="utf-8") as fh:
        return json.load(fh)


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
        # while the actor is whoever triggered this event - the pusher on
        # synchronize, the person who opened or reopened the pull request on
        # opened/reopened. A fork branch can be pushed to by someone other than
        # the PR author.
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
        if head_name != base_name:
            head_owner = (head_repo.get("owner") or {}).get("login") or ""
            if head_owner.strip().lower() not in allowed_authors:
                return False

        # This gate trusts allowlisted people and the repositories they own,
        # not individual commits. Whoever an allowlisted fork owner lets push
        # to their fork is trusted like anyone with write access to a same-repo
        # head, and an allowlisted user who opens, reopens or pushes to a pull
        # request vouches for its head. So an opened/reopened run whose head
        # was pushed by a fork collaborator is intentionally allowed: limiting
        # forks to synchronize would not change who is trusted, it would only
        # send every first run to hosted runners.
        return True

    return False


def is_fast_path_allowed(
    event_name: str,
    event: Dict,
    actor: str,
    allowed_authors: Set[str],
) -> bool:
    """Whether this event may build and test on the self-hosted fast path.

    The pre-lint rung's trust, plus pushes to develop and master by anyone:
    pushing there already requires write access, and they carry merged code.
    """
    if event_name == "push" and event.get("ref") in FAST_PATH_PUSH_REFS:
        return True
    return is_selfhosted_allowed(event_name, event, actor, allowed_authors)


def allows_cache_write(
    event_name: str,
    event: Dict,
    actor: str,
    allowed_authors: Set[str],
) -> bool:
    """Whether this run may receive the remote ccache write credential.

    The credential is the repository's own secret, so any push may have it:
    pushing already requires write access to that repository. A pull request
    executes its head, so it gets the credential only when the people and the
    repository behind that head are trusted to run on our own hardware too.
    """
    return event_name == "push" or is_selfhosted_allowed(
        event_name, event, actor, allowed_authors
    )


def select_selfhosted_runner(
    allowed: bool,
    backlog_count_value: Optional[int],
    measurement_error: Optional[str],
    selfhosted_label: str,
    fallback_runner: str,
    label_override: bool = False,
    selfhosted_queued: int = 0,
    selfhosted_max_queued: int = DEFAULT_SELFHOSTED_MAX_QUEUED,
    disabled_reason: str = "selfhosted-disabled",
) -> Tuple[str, str]:
    """Pick between a self-hosted label and fallback_runner, and the reason.

    Inserts a self-hosted rung into the hosted -> Blacksmith ladder: once any
    GitHub-hosted job is queued, the jobs go to our own hardware instead of
    competing for the account-wide concurrency limit, unless our own runners
    already have more than selfhosted_max_queued jobs waiting. When the rung
    is not available they keep the caller's existing decision for the arch.

    allowed says whether this event may run on our own hardware at all.
    """
    if not selfhosted_label:
        return fallback_runner, disabled_reason

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

    if not allowed:
        return fallback_runner, "actor-not-allowed"

    if selfhosted_queued > selfhosted_max_queued:
        return fallback_runner, "selfhosted-saturated:queued:{}>{}".format(
            selfhosted_queued, selfhosted_max_queued
        )

    return selfhosted_label, "selfhosted:backlog:{}>{};queued:{}<={}".format(
        backlog_count_value, SELFHOSTED_BACKLOG_THRESHOLD,
        selfhosted_queued, selfhosted_max_queued,
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
    runner_selfhosted_arm64_var: str = "",
    selfhosted_max_queued: int = DEFAULT_SELFHOSTED_MAX_QUEUED,
    runner_selfhosted_fast_var: str = "",
    selfhosted_fast_max_queued: int = DEFAULT_SELFHOSTED_FAST_MAX_QUEUED,
    runner_default_amd64_var: str = "",
    runner_default_arm64_var: str = "",
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
    selfhosted_queued: Dict[str, int] = {}
    measurement_error = None

    try:
        backlog_count_value, selfhosted_queued = count_queued_jobs(
            fetch_json,
            repos,
            non_hosted_labels=(
                runner_selfhosted_var,
                runner_selfhosted_arm64_var,
                runner_selfhosted_fast_var,
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

    runner_amd64 = runner_default_amd64_var or DEFAULT_RUNNER_AMD64
    runner_arm64 = runner_default_arm64_var or DEFAULT_RUNNER_ARM64
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

    allowed_authors = parse_author_allowlist(selfhosted_authors)

    def selfhosted(
        allowed: bool,
        selfhosted_label: str,
        fallback_runner: str,
        max_queued: int,
        disabled_reason: str,
    ) -> Tuple[str, str]:
        return select_selfhosted_runner(
            allowed=allowed,
            backlog_count_value=backlog_count_value,
            measurement_error=measurement_error,
            selfhosted_label=selfhosted_label,
            fallback_runner=fallback_runner,
            label_override=label_override,
            selfhosted_queued=selfhosted_queued.get(selfhosted_label.lower(), 0),
            selfhosted_max_queued=max_queued,
            disabled_reason=disabled_reason,
        )

    # The pre-lint jobs (lint, cache-sources, the container builds and
    # manifest) take seconds but can wait most of an hour for a hosted runner.
    # Every one but the manifest runs pull request head code (lint scripts,
    # `make -C depends download`, Dockerfile RUN steps), so all of them go
    # through the same allowlist as lint did on its own.
    prelint_allowed = is_selfhosted_allowed(event_name, event, actor, allowed_authors)
    runner_prelint, prelint_decision_reason = selfhosted(
        allowed=prelint_allowed,
        selfhosted_label=runner_selfhosted_var,
        fallback_runner=runner_amd64,
        max_queued=selfhosted_max_queued,
        disabled_reason="selfhosted-disabled",
    )
    runner_prelint_arm64, prelint_arm64_decision_reason = selfhosted(
        allowed=prelint_allowed,
        selfhosted_label=runner_selfhosted_arm64_var,
        fallback_runner=runner_arm64,
        max_queued=selfhosted_max_queued,
        disabled_reason="selfhosted-arm64-disabled",
    )
    # The fast path: the linux64 depends, build and functional test jobs.
    runner_fast, fast_decision_reason = selfhosted(
        allowed=is_fast_path_allowed(event_name, event, actor, allowed_authors),
        selfhosted_label=runner_selfhosted_fast_var,
        fallback_runner=runner_amd64,
        max_queued=selfhosted_fast_max_queued,
        disabled_reason="selfhosted-fast-disabled",
    )
    cache_write = allows_cache_write(event_name, event, actor, allowed_authors)

    return {
        "runner_amd64": runner_amd64,
        "runner_arm64": runner_arm64,
        "runner_prelint": runner_prelint,
        "prelint_decision_reason": prelint_decision_reason,
        "runner_prelint_arm64": runner_prelint_arm64,
        "prelint_arm64_decision_reason": prelint_arm64_decision_reason,
        "runner_fast": runner_fast,
        "fast_decision_reason": fast_decision_reason,
        "use_blacksmith": "true" if use_blacksmith_amd64 or use_blacksmith_arm64 else "false",
        "use_blacksmith_amd64": "true" if use_blacksmith_amd64 else "false",
        "use_blacksmith_arm64": "true" if use_blacksmith_arm64 else "false",
        "backlog_count": backlog_count,
        "cache_write": "true" if cache_write else "false",
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
        fh.write("- pre-lint amd64 runner: `{}`\n".format(outputs["runner_prelint"]))
        fh.write("- pre-lint arm64 runner: `{}`\n".format(outputs["runner_prelint_arm64"]))
        fh.write("- linux64 fast path runner: `{}`\n".format(outputs["runner_fast"]))
        fh.write("- Decision: `{}`\n".format(outputs["decision_reason"]))
        fh.write("- Pre-lint amd64 decision: `{}`\n".format(outputs["prelint_decision_reason"]))
        fh.write("- Pre-lint arm64 decision: `{}`\n".format(
            outputs["prelint_arm64_decision_reason"]))
        fh.write("- Fast path decision: `{}`\n".format(outputs["fast_decision_reason"]))
        fh.write(
            "- Remote ccache writes: {}\n".format(
                "yes" if outputs["cache_write"] == "true" else "no"
            )
        )


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
        "--runner-default-amd64-var",
        default=os.environ.get("RUNNER_DEFAULT_AMD64_VAR", ""),
        help="amd64 runner label used without escalation; empty means {}".format(DEFAULT_RUNNER_AMD64),
    )
    parser.add_argument(
        "--runner-default-arm64-var",
        default=os.environ.get("RUNNER_DEFAULT_ARM64_VAR", ""),
        help="arm64 runner label used without escalation; empty means {}".format(DEFAULT_RUNNER_ARM64),
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
        help="Self-hosted amd64 runner label for the pre-lint jobs; empty disables the rung",
    )
    parser.add_argument(
        "--runner-selfhosted-arm64-var",
        default=os.environ.get("RUNNER_SELFHOSTED_ARM64_VAR", ""),
        help="Self-hosted arm64 runner label for the pre-lint jobs; empty disables the rung",
    )
    parser.add_argument(
        "--selfhosted-max-queued",
        type=int,
        default=env_int("SELFHOSTED_MAX_QUEUED", DEFAULT_SELFHOSTED_MAX_QUEUED),
        help="Use a self-hosted label only while at most this many jobs are queued for it",
    )
    parser.add_argument(
        "--runner-selfhosted-fast-var",
        default=os.environ.get("RUNNER_SELFHOSTED_FAST_VAR", ""),
        help="Self-hosted amd64 runner label for the linux64 build and test fast path; empty disables it",
    )
    parser.add_argument(
        "--selfhosted-fast-max-queued",
        type=int,
        default=env_int("SELFHOSTED_FAST_MAX_QUEUED", DEFAULT_SELFHOSTED_FAST_MAX_QUEUED),
        help="Use the fast path label only while at most this many jobs are queued for it",
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
        runner_selfhosted_arm64_var=args.runner_selfhosted_arm64_var,
        selfhosted_max_queued=args.selfhosted_max_queued,
        runner_selfhosted_fast_var=args.runner_selfhosted_fast_var,
        selfhosted_fast_max_queued=args.selfhosted_fast_max_queued,
        runner_default_amd64_var=args.runner_default_amd64_var,
        runner_default_arm64_var=args.runner_default_arm64_var,
    )

    write_github_output(os.environ.get("GITHUB_OUTPUT"), outputs)
    write_step_summary(os.environ.get("GITHUB_STEP_SUMMARY"), outputs)

    if args.print_json or "GITHUB_OUTPUT" not in os.environ:
        json.dump(outputs, sys.stdout, sort_keys=True)
        sys.stdout.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
