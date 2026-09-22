import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("select_dynamic_runner.py")
SPEC = importlib.util.spec_from_file_location("select_dynamic_runner", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


REPO = "dashpay/dash"
ALLOWED = "PastaPastaPasta,kwvg,UdjinM6,thepastaclaw,DCG-Claude,knst"


def backlog_responses(queued_jobs):
    """Canned API responses describing `queued_jobs` queued GitHub-hosted jobs."""
    queued_url = (
        "https://api.github.com/repos/{}/actions/runs?status=queued&per_page=100"
    ).format(REPO)
    in_progress_url = (
        "https://api.github.com/repos/{}/actions/runs?status=in_progress&per_page=100"
    ).format(REPO)
    jobs_url = (
        "https://api.github.com/repos/{}/actions/runs/101/jobs?per_page=100"
    ).format(REPO)

    return {
        queued_url: ({"workflow_runs": [{"id": 101}]}, {}),
        in_progress_url: ({"workflow_runs": []}, {}),
        jobs_url: (
            {"jobs": [{"status": "queued", "labels": ["ubuntu-24.04"]}] * queued_jobs},
            {},
        ),
    }


def pull_request_event(author, head_owner=None, labels=()):
    """A pull_request_target payload, by default a fork PR owned by `author`."""
    head_owner = author if head_owner is None else head_owner
    return {
        "pull_request": {
            "user": {"login": author},
            "labels": [{"name": name} for name in labels],
            "head": {
                "repo": {
                    "full_name": "{}/dash".format(head_owner),
                    "owner": {"login": head_owner},
                }
            },
            "base": {"repo": {"full_name": REPO}},
        }
    }


class SelectDynamicRunnerTest(unittest.TestCase):
    def test_count_queued_jobs_deduplicates_runs_across_status_queries(self):
        repo = "dashpay/dash"
        queued_url = (
            "https://api.github.com/repos/{}/actions/runs?status=queued&per_page=100"
        ).format(repo)
        in_progress_url = (
            "https://api.github.com/repos/{}/actions/runs?status=in_progress&per_page=100"
        ).format(repo)
        jobs_url = (
            "https://api.github.com/repos/{}/actions/runs/101/jobs?per_page=100"
        ).format(repo)

        responses = {
            queued_url: ({"workflow_runs": [{"id": 101}]}, {}),
            in_progress_url: ({"workflow_runs": [{"id": 101}]}, {}),
            jobs_url: (
                {
                    "jobs": [
                        {"status": "queued", "labels": ["ubuntu-24.04"]},
                        {"status": "queued", "labels": ["ubuntu-24.04"]},
                        {"status": "in_progress", "labels": ["ubuntu-24.04"]},
                    ]
                },
                {},
            ),
        }

        def fetch_json(url):
            return responses[url]

        self.assertEqual(MODULE.count_queued_jobs(fetch_json, [repo]), 2)

    def test_count_queued_jobs_excludes_blacksmith_jobs(self):
        repo = "dashpay/dash"
        queued_url = (
            "https://api.github.com/repos/{}/actions/runs?status=queued&per_page=100"
        ).format(repo)
        in_progress_url = (
            "https://api.github.com/repos/{}/actions/runs?status=in_progress&per_page=100"
        ).format(repo)
        jobs_url = (
            "https://api.github.com/repos/{}/actions/runs/101/jobs?per_page=100"
        ).format(repo)

        responses = {
            queued_url: ({"workflow_runs": [{"id": 101}]}, {}),
            in_progress_url: ({"workflow_runs": []}, {}),
            jobs_url: (
                {
                    "jobs": [
                        {"status": "queued", "labels": ["ubuntu-24.04"]},
                        {"status": "queued", "labels": ["blacksmith-4vcpu-ubuntu-2404"]},
                        {"status": "queued", "labels": ["blacksmith-4vcpu-ubuntu-2404-arm"]},
                        {"status": "queued", "labels": ["self-hosted", "linux"]},
                    ]
                },
                {},
            ),
        }

        def fetch_json(url):
            return responses[url]

        self.assertEqual(MODULE.count_queued_jobs(fetch_json, [repo]), 1)

    def test_label_override_selects_blacksmith_even_with_low_backlog(self):
        outputs = MODULE.select_runners(
            event_name="pull_request_target",
            event={"pull_request": {"labels": [{"name": "blacksmith-ci"}]}},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda _url: ({"workflow_runs": []}, {}),
            repos=["dashpay/dash"],
        )

        self.assertEqual(outputs["use_blacksmith"], "true")
        self.assertEqual(outputs["use_blacksmith_amd64"], "true")
        self.assertEqual(outputs["use_blacksmith_arm64"], "true")
        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_arm64"], "blacksmith-arm64")
        self.assertIn("label:blacksmith-ci", outputs["decision_reason"])

    def test_backlog_threshold_selects_blacksmith_amd64_only(self):
        repo = "dashpay/dash"
        queued_url = (
            "https://api.github.com/repos/{}/actions/runs?status=queued&per_page=100"
        ).format(repo)
        in_progress_url = (
            "https://api.github.com/repos/{}/actions/runs?status=in_progress&per_page=100"
        ).format(repo)
        jobs_url = (
            "https://api.github.com/repos/{}/actions/runs/101/jobs?per_page=100"
        ).format(repo)

        responses = {
            queued_url: ({"workflow_runs": [{"id": 101}]}, {}),
            in_progress_url: ({"workflow_runs": []}, {}),
            jobs_url: ({"jobs": [{"status": "queued", "labels": ["ubuntu-24.04"]}] * 11}, {}),
        }

        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda url: responses[url],
            repos=[repo],
        )

        self.assertEqual(outputs["use_blacksmith"], "true")
        self.assertEqual(outputs["use_blacksmith_amd64"], "true")
        self.assertEqual(outputs["use_blacksmith_arm64"], "false")
        self.assertEqual(outputs["backlog_count"], "11")
        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertIn("amd64:backlog:11>10", outputs["decision_reason"])
        self.assertIn("arm64:backlog:11<=30", outputs["decision_reason"])

    def test_arm64_requires_higher_backlog_threshold(self):
        repo = "dashpay/dash"
        queued_url = (
            "https://api.github.com/repos/{}/actions/runs?status=queued&per_page=100"
        ).format(repo)
        in_progress_url = (
            "https://api.github.com/repos/{}/actions/runs?status=in_progress&per_page=100"
        ).format(repo)
        jobs_url = (
            "https://api.github.com/repos/{}/actions/runs/101/jobs?per_page=100"
        ).format(repo)

        responses = {
            queued_url: ({"workflow_runs": [{"id": 101}]}, {}),
            in_progress_url: ({"workflow_runs": []}, {}),
            jobs_url: ({"jobs": [{"status": "queued", "labels": ["ubuntu-24.04"]}] * 31}, {}),
        }

        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda url: responses[url],
            repos=[repo],
        )

        self.assertEqual(outputs["use_blacksmith"], "true")
        self.assertEqual(outputs["use_blacksmith_amd64"], "true")
        self.assertEqual(outputs["use_blacksmith_arm64"], "true")
        self.assertEqual(outputs["backlog_count"], "31")
        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_arm64"], "blacksmith-arm64")
        self.assertIn("amd64:backlog:31>10", outputs["decision_reason"])
        self.assertIn("arm64:backlog:31>30", outputs["decision_reason"])

    def test_measurement_failure_falls_back_to_github(self):
        def fetch_json(_url):
            raise RuntimeError("boom")

        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=fetch_json,
            repos=["dashpay/dash"],
        )

        self.assertEqual(outputs["use_blacksmith"], "false")
        self.assertEqual(outputs["use_blacksmith_amd64"], "false")
        self.assertEqual(outputs["use_blacksmith_arm64"], "false")
        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertEqual(outputs["backlog_count"], "unknown")
        self.assertIn("metric-unavailable", outputs["decision_reason"])

    def test_missing_runner_vars_fall_back_per_arch(self):
        outputs = MODULE.select_runners(
            event_name="pull_request_target",
            event={"pull_request": {"labels": [{"name": "blacksmith-ci"}]}},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda _url: ({"workflow_runs": []}, {}),
            repos=["dashpay/dash"],
        )

        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_arm64"], "blacksmith-arm64")
        self.assertIn("amd64-github-fallback", outputs["decision_reason"])

    # --- lint runner selection (self-hosted rung) ---------------------------

    def _select(self, **overrides):
        kwargs = dict(
            event_name="pull_request_target",
            event=pull_request_event("PastaPastaPasta"),
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            repos=[REPO],
            runner_selfhosted_var="ubuntu-core",
            selfhosted_authors=ALLOWED,
            actor="PastaPastaPasta",
        )
        queued_jobs = overrides.pop("queued_jobs", 3)
        kwargs.update(overrides)
        if "fetch_json" not in kwargs:
            responses = backlog_responses(queued_jobs)
            kwargs["fetch_json"] = lambda url: responses[url]
        return MODULE.select_runners(**kwargs)

    def test_lint_uses_selfhosted_when_backlog_and_author_allowed(self):
        outputs = self._select()

        self.assertEqual(outputs["runner_lint"], "ubuntu-core")
        self.assertEqual(outputs["lint_decision_reason"], "selfhosted:backlog:3>0")
        # The rest of the ladder is untouched.
        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)

    def test_lint_allowlist_is_case_insensitive(self):
        outputs = self._select(
            event=pull_request_event("pastapastapasta"),
            actor="PASTAPASTAPASTA",
            selfhosted_authors="pastapastapasta, KWVG , udjinm6",
        )

        self.assertEqual(outputs["runner_lint"], "ubuntu-core")

    def test_lint_falls_back_when_author_not_allowed(self):
        # An allowlisted maintainer may not lend our hardware to a fork PR
        # opened by someone who is not on the list.
        outputs = self._select(
            event=pull_request_event("mallory"),
            actor="PastaPastaPasta",
        )

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_falls_back_when_actor_not_allowed_even_if_author_is(self):
        # A fork branch can be pushed to by someone other than the PR author.
        outputs = self._select(
            event=pull_request_event("PastaPastaPasta"),
            actor="mallory",
        )

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_falls_back_without_backlog(self):
        outputs = self._select(queued_jobs=0)

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "backlog:0<=0")

    def test_lint_falls_back_when_selfhosted_label_unset(self):
        outputs = self._select(runner_selfhosted_var="")

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "selfhosted-disabled")

    def test_lint_falls_back_when_allowlist_empty(self):
        outputs = self._select(selfhosted_authors="")

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_falls_back_on_measurement_error(self):
        def fetch_json(_url):
            raise RuntimeError("boom")

        outputs = self._select(fetch_json=fetch_json)

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "metric-unavailable")

    def test_lint_uses_selfhosted_on_push_from_allowed_actor(self):
        outputs = self._select(event_name="push", event={}, actor="knst")

        self.assertEqual(outputs["runner_lint"], "ubuntu-core")

    def test_lint_ignores_unknown_event_types(self):
        outputs = self._select(event_name="workflow_dispatch", event={})

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_falls_back_for_cross_fork_head(self):
        # A pull request may be opened from any readable fork, so an allowlisted
        # author is not proof that the executing tree is allowlisted.
        outputs = self._select(
            event=pull_request_event("PastaPastaPasta", head_owner="mallory"),
        )

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_allows_same_repo_head(self):
        outputs = self._select(
            event=pull_request_event("PastaPastaPasta", head_owner="dashpay"),
        )

        self.assertEqual(outputs["runner_lint"], "ubuntu-core")

    def test_lint_requires_an_exact_login_match(self):
        # "knstfoo" must not be accepted on the strength of "knst" being listed.
        outputs = self._select(
            event=pull_request_event("knstfoo"),
            actor="knstfoo",
        )

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_each_identity_is_matched_exactly(self):
        # Author, actor and head repository owner are three separate gates;
        # pin exact membership on each one independently of the others.
        allowed = MODULE.parse_author_allowlist(ALLOWED)

        for author, actor, head_owner in (
            ("knstfoo", "knst", "knst"),
            ("knst", "knstfoo", "knst"),
            ("knst", "knst", "knstfoo"),
        ):
            with self.subTest(author=author, actor=actor, head_owner=head_owner):
                self.assertFalse(
                    MODULE.is_selfhosted_allowed(
                        "pull_request_target",
                        pull_request_event(author, head_owner=head_owner),
                        actor,
                        allowed,
                    )
                )

        self.assertTrue(
            MODULE.is_selfhosted_allowed(
                "pull_request_target",
                pull_request_event("knst"),
                "knst",
                allowed,
            )
        )

    def test_lint_falls_back_when_head_and_base_are_both_missing(self):
        # Neither side identifies a repository, so the tree cannot be attributed
        # to anyone; that must refuse rather than fall through as "same repo".
        event = pull_request_event("PastaPastaPasta")
        del event["pull_request"]["head"]
        del event["pull_request"]["base"]

        outputs = self._select(event=event)

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_falls_back_when_head_repo_missing(self):
        event = pull_request_event("PastaPastaPasta")
        del event["pull_request"]["head"]

        outputs = self._select(event=event)

        self.assertEqual(outputs["runner_lint"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["lint_decision_reason"], "actor-not-allowed")

    def test_lint_yields_to_an_explicit_blacksmith_label(self):
        outputs = self._select(
            event=pull_request_event("PastaPastaPasta", labels=["blacksmith-ci"]),
        )

        self.assertEqual(outputs["runner_lint"], "blacksmith-amd64")
        self.assertEqual(outputs["lint_decision_reason"], "label:blacksmith-ci")

    def test_selfhosted_jobs_do_not_count_as_hosted_backlog(self):
        # Queued lint jobs on our own hardware must not inflate the figure that
        # decides whether to escalate the rest of the matrix to Blacksmith.
        job = {"status": "queued", "labels": ["ubuntu-core"]}

        self.assertTrue(MODULE.targets_github_hosted_runner(job))
        self.assertFalse(MODULE.targets_github_hosted_runner(job, ["ubuntu-core"]))
        self.assertFalse(MODULE.targets_github_hosted_runner(job, ["UBUNTU-CORE"]))
        self.assertTrue(MODULE.targets_github_hosted_runner(job, [""]))

    def test_a_hosted_label_is_never_excluded_from_the_backlog(self):
        # Naming a hosted or Blacksmith label here would zero the backlog and
        # silently switch the existing Blacksmith ladder off, so refuse it.
        safe = MODULE.selfhosted_backlog_exclusions(
            "ubuntu-core", "blacksmith-amd64", "blacksmith-arm64"
        )
        self.assertEqual(safe, ("ubuntu-core",))

        for label in (
            MODULE.DEFAULT_RUNNER_AMD64,
            MODULE.DEFAULT_RUNNER_ARM64,
            MODULE.DEFAULT_RUNNER_AMD64.upper(),
            "  ubuntu-24.04  ",
            "blacksmith-amd64",
            "blacksmith-4vcpu-ubuntu-2404",
            "",
        ):
            with self.subTest(label=label):
                self.assertEqual(
                    MODULE.selfhosted_backlog_exclusions(
                        label, "blacksmith-amd64", "blacksmith-arm64"
                    ),
                    (),
                )

    def test_backlog_is_unaffected_when_the_label_collides_with_hosted(self):
        outputs = self._select(queued_jobs=11, runner_selfhosted_var="ubuntu-24.04")

        # The 11 hosted jobs still count, so Blacksmith escalation is untouched.
        self.assertEqual(outputs["backlog_count"], "11")
        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")

    def test_backlog_excludes_queued_selfhosted_lint_jobs(self):
        responses = backlog_responses(3)
        jobs_url = next(url for url in responses if "/jobs?" in url)
        payload, headers = responses[jobs_url]
        payload["jobs"] = payload["jobs"] + [
            {"status": "queued", "labels": ["ubuntu-core"]}
        ] * 9
        responses[jobs_url] = (payload, headers)

        outputs = self._select(fetch_json=lambda url: responses[url])

        # 12 queued jobs, but only the 3 hosted ones count - so the amd64
        # threshold of 10 is not crossed and Blacksmith is not paid for.
        self.assertEqual(outputs["backlog_count"], "3")
        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_lint"], "ubuntu-core")

    def test_lint_inherits_blacksmith_when_rung_unavailable(self):
        # Declining self-hosted must leave lint on whatever amd64 decided,
        # which above the threshold is Blacksmith rather than GitHub-hosted.
        outputs = self._select(
            queued_jobs=11,
            event=pull_request_event("mallory"),
            actor="mallory",
        )

        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_lint"], "blacksmith-amd64")


if __name__ == "__main__":
    unittest.main()
