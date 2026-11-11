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


def add_queued_jobs(responses, label, count):
    """Add `count` queued jobs asking for `label` to backlog_responses() output."""
    jobs_url = next(url for url in responses if "/jobs?" in url)
    payload, headers = responses[jobs_url]
    payload["jobs"] = payload["jobs"] + [{"status": "queued", "labels": [label]}] * count
    return responses


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

        self.assertEqual(MODULE.count_queued_jobs(fetch_json, [repo])[0], 2)

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

        self.assertEqual(MODULE.count_queued_jobs(fetch_json, [repo])[0], 1)

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
        self.assertIn("amd64-default-fallback", outputs["decision_reason"])

    def test_default_runner_vars_replace_hosted_defaults(self):
        def fetch_json(_url):
            raise RuntimeError("boom")

        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="",
            runner_arm64_var="",
            fetch_json=fetch_json,
            repos=[REPO],
            runner_default_amd64_var="default-amd64",
            runner_default_arm64_var="default-arm64",
        )

        self.assertEqual(outputs["use_blacksmith"], "false")
        self.assertEqual(outputs["runner_amd64"], "default-amd64")
        self.assertEqual(outputs["runner_arm64"], "default-arm64")
        self.assertEqual(outputs["runner_prelint_arm64"], "default-arm64")
        self.assertEqual(outputs["runner_fast"], "default-amd64")
        self.assertEqual(outputs["runner_build"], "default-amd64")

    def test_escalation_overrides_default_runner_vars(self):
        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda url: backlog_responses(11)[url],
            repos=[REPO],
            runner_default_amd64_var="default-amd64",
            runner_default_arm64_var="default-arm64",
        )

        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_arm64"], "default-arm64")

    def test_tiers_use_default_runner_var_in_place_of_github_hosted(self):
        outputs = self._select(queued_jobs=0, runner_default_amd64_var="default-amd64")
        self.assertEqual(outputs["runner_fast"], "default-amd64")
        self.assertEqual(outputs["runner_build"], "default-amd64")

        outputs = self._select(
            queued_jobs=40,
            event=pull_request_event("mallory"),
            actor="mallory",
            runner_default_amd64_var="default-amd64",
        )
        self.assertEqual(outputs["runner_fast"], "default-amd64")
        self.assertEqual(outputs["fast_decision_reason"], "actor-not-allowed")

        outputs = self._select(
            queued_jobs=20,
            runner_amd64_var="",
            runner_selfhosted_build_var="",
            runner_default_amd64_var="default-amd64",
        )
        self.assertEqual(outputs["runner_build"], "default-amd64")
        self.assertEqual(outputs["build_decision_reason"], "selfhosted-build-disabled")

    # --- amd64 tiers ---------------------------------------------------------

    def _select(self, **overrides):
        kwargs = dict(
            event_name="pull_request_target",
            event=pull_request_event("PastaPastaPasta"),
            threshold=10,
            arm64_threshold=30,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            repos=[REPO],
            selfhosted_authors=ALLOWED,
            actor="PastaPastaPasta",
            runner_selfhosted_fast_var="dash-x64-fast",
            runner_selfhosted_build_var="dash-x64-build",
        )
        queued_jobs = overrides.pop("queued_jobs", 3)
        selfhosted_queued = overrides.pop("selfhosted_queued", {})
        kwargs.update(overrides)
        if "fetch_json" not in kwargs:
            responses = backlog_responses(queued_jobs)
            for label, count in selfhosted_queued.items():
                add_queued_jobs(responses, label, count)
            kwargs["fetch_json"] = lambda url: responses[url]
        return MODULE.select_runners(**kwargs)

    def test_fast_tier_is_hosted_only_while_nothing_is_queued(self):
        for queued, expected, reason in (
            (0, MODULE.DEFAULT_RUNNER_AMD64, "hosted:queued:0"),
            (1, "dash-x64-fast", "selfhosted:queued:0<16"),
        ):
            with self.subTest(queued=queued):
                outputs = self._select(queued_jobs=queued)
                self.assertEqual(outputs["runner_fast"], expected)
                self.assertEqual(outputs["fast_decision_reason"], reason)

    def test_build_tier_is_hosted_until_twenty_queued(self):
        for queued, expected, reason in (
            (19, MODULE.DEFAULT_RUNNER_AMD64, "hosted:queued:19<20"),
            (20, "dash-x64-build", "selfhosted:queued:0<24"),
        ):
            with self.subTest(queued=queued):
                outputs = self._select(queued_jobs=queued)
                self.assertEqual(outputs["runner_build"], expected)
                self.assertEqual(outputs["build_decision_reason"], reason)

    def test_each_tier_saturates_on_its_own_label(self):
        outputs = self._select(
            queued_jobs=20,
            selfhosted_queued={"dash-x64-fast": 16, "dash-x64-build": 23},
        )

        # Jobs waiting for our own runners are not hosted backlog.
        self.assertEqual(outputs["backlog_count"], "20")
        self.assertEqual(outputs["runner_fast"], "blacksmith-amd64")
        self.assertEqual(outputs["fast_decision_reason"], "selfhosted-saturated:queued:16>=16")
        self.assertEqual(outputs["runner_build"], "dash-x64-build")
        self.assertEqual(outputs["build_decision_reason"], "selfhosted:queued:23<24")

        outputs = self._select(
            queued_jobs=20,
            selfhosted_queued={"dash-x64-fast": 15, "dash-x64-build": 24},
        )

        self.assertEqual(outputs["runner_fast"], "dash-x64-fast")
        self.assertEqual(outputs["runner_build"], "blacksmith-amd64")
        self.assertEqual(
            outputs["build_decision_reason"], "selfhosted-saturated:queued:24>=24"
        )

    def test_tiers_sharing_a_label_share_its_queue(self):
        outputs = self._select(
            queued_jobs=20,
            runner_selfhosted_fast_var="dash-x64-build",
            selfhosted_queued={"dash-x64-build": 20},
        )

        self.assertEqual(outputs["runner_fast"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_build"], "dash-x64-build")

    def test_unset_tier_label_skips_only_that_tier(self):
        outputs = self._select(queued_jobs=20, runner_selfhosted_build_var="")

        self.assertEqual(outputs["runner_fast"], "dash-x64-fast")
        self.assertEqual(outputs["runner_build"], "blacksmith-amd64")
        self.assertEqual(outputs["build_decision_reason"], "selfhosted-build-disabled")

        outputs = self._select(queued_jobs=20, runner_selfhosted_fast_var="")

        self.assertEqual(outputs["runner_fast"], "blacksmith-amd64")
        self.assertEqual(outputs["fast_decision_reason"], "selfhosted-fast-disabled")
        self.assertEqual(outputs["runner_build"], "dash-x64-build")

    def test_blacksmith_step_is_runner_amd64_below_the_hosted_ladder_threshold(self):
        # One queued hosted job keeps the hosted ladder on GitHub-hosted, but a
        # trusted fast tier past its self-hosted step still goes to Blacksmith.
        outputs = self._select(queued_jobs=1, selfhosted_queued={"dash-x64-fast": 16})

        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_fast"], "blacksmith-amd64")

        outputs = self._select(queued_jobs=1, runner_selfhosted_fast_var="")
        self.assertEqual(outputs["runner_fast"], "blacksmith-amd64")

    def test_blacksmith_steps_are_github_hosted_when_runner_amd64_is_unset(self):
        outputs = self._select(
            queued_jobs=20,
            runner_amd64_var="",
            runner_selfhosted_build_var="",
            selfhosted_queued={"dash-x64-fast": 16},
        )

        self.assertEqual(outputs["runner_fast"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_build"], MODULE.DEFAULT_RUNNER_AMD64)

    def test_untrusted_runs_stay_github_hosted(self):
        for author, actor, head_owner in (
            ("mallory", "PastaPastaPasta", "mallory"),
            ("PastaPastaPasta", "mallory", "PastaPastaPasta"),
            ("PastaPastaPasta", "PastaPastaPasta", "mallory"),
            ("knstfoo", "knstfoo", "knstfoo"),
        ):
            with self.subTest(author=author, actor=actor, head_owner=head_owner):
                outputs = self._select(
                    queued_jobs=40,
                    event=pull_request_event(author, head_owner=head_owner),
                    actor=actor,
                )
                for tier in ("fast", "build"):
                    self.assertEqual(outputs["runner_" + tier], MODULE.DEFAULT_RUNNER_AMD64)
                    self.assertEqual(
                        outputs[tier + "_decision_reason"], "actor-not-allowed"
                    )
                # The hosted ladder itself is unchanged.
                self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")

                # Even with both tiers' labels unset, they never reach Blacksmith.
                outputs = self._select(
                    queued_jobs=40,
                    event=pull_request_event(author, head_owner=head_owner),
                    actor=actor,
                    runner_selfhosted_fast_var="",
                    runner_selfhosted_build_var="",
                )
                self.assertEqual(outputs["runner_fast"], MODULE.DEFAULT_RUNNER_AMD64)
                self.assertEqual(outputs["runner_build"], MODULE.DEFAULT_RUNNER_AMD64)

    def test_untrusted_events_and_heads(self):
        missing_head = pull_request_event("PastaPastaPasta")
        del missing_head["pull_request"]["head"]
        missing_both = pull_request_event("PastaPastaPasta")
        del missing_both["pull_request"]["head"]
        del missing_both["pull_request"]["base"]

        for name, overrides in (
            ("empty allowlist", dict(selfhosted_authors="")),
            ("unknown event", dict(event_name="workflow_dispatch", event={})),
            ("missing head repo", dict(event=missing_head)),
            ("missing head and base", dict(event=missing_both)),
            ("feature push by unlisted actor",
             dict(event_name="push", event={"ref": "refs/heads/feature"}, actor="mallory")),
        ):
            with self.subTest(name):
                outputs = self._select(queued_jobs=20, **overrides)
                self.assertEqual(outputs["runner_fast"], MODULE.DEFAULT_RUNNER_AMD64)
                self.assertEqual(outputs["runner_build"], MODULE.DEFAULT_RUNNER_AMD64)

    def test_trusted_runs_use_selfhosted(self):
        for name, overrides in (
            ("allowlisted fork", {}),
            ("same-repo head", dict(event=pull_request_event("PastaPastaPasta", head_owner="dashpay"))),
            ("case-insensitive allowlist", dict(
                event=pull_request_event("pastapastapasta"),
                actor="PASTAPASTAPASTA",
                selfhosted_authors="pastapastapasta, KWVG , udjinm6",
            )),
            ("feature push by listed actor",
             dict(event_name="push", event={"ref": "refs/heads/feature"}, actor="knst")),
            ("develop push by anyone",
             dict(event_name="push", event={"ref": "refs/heads/develop"}, actor="mallory")),
            ("master push by anyone",
             dict(event_name="push", event={"ref": "refs/heads/master"}, actor="mallory")),
        ):
            with self.subTest(name):
                outputs = self._select(queued_jobs=20, **overrides)
                self.assertEqual(outputs["runner_fast"], "dash-x64-fast")
                self.assertEqual(outputs["runner_build"], "dash-x64-build")

    def test_tiers_yield_to_label_and_measurement_errors(self):
        outputs = self._select(
            queued_jobs=0,
            event=pull_request_event("mallory", labels=["blacksmith-ci"]),
            actor="mallory",
        )
        for tier in ("fast", "build"):
            self.assertEqual(outputs["runner_" + tier], "blacksmith-amd64")
            self.assertEqual(outputs[tier + "_decision_reason"], "label:blacksmith-ci")

        def fetch_json(_url):
            raise RuntimeError("boom")

        outputs = self._select(fetch_json=fetch_json)
        for tier in ("fast", "build"):
            self.assertEqual(outputs["runner_" + tier], MODULE.DEFAULT_RUNNER_AMD64)
            self.assertEqual(outputs[tier + "_decision_reason"], "metric-unavailable")

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

    def test_selfhosted_jobs_do_not_count_as_hosted_backlog(self):
        job = {"status": "queued", "labels": ["dash-x64-build"]}

        self.assertTrue(MODULE.targets_github_hosted_runner(job))
        self.assertFalse(MODULE.targets_github_hosted_runner(job, ["dash-x64-build"]))
        self.assertFalse(MODULE.targets_github_hosted_runner(job, ["DASH-X64-BUILD"]))
        self.assertTrue(MODULE.targets_github_hosted_runner(job, [""]))

    def test_backlog_excludes_jobs_on_moved_default_runners(self):
        responses = add_queued_jobs(backlog_responses(0), "own-amd64", 11)

        moved = self._select(
            runner_default_amd64_var="own-amd64",
            fetch_json=lambda url: responses[url],
        )
        self.assertEqual(moved["backlog_count"], "0")
        self.assertEqual(moved["runner_amd64"], "own-amd64")
        self.assertEqual(moved["runner_fast"], "own-amd64")
        self.assertEqual(moved["fast_decision_reason"], "hosted:queued:0")

        responses_arm64 = add_queued_jobs(backlog_responses(0), "own-arm64", 31)
        moved_arm64 = self._select(
            runner_default_arm64_var="own-arm64",
            fetch_json=lambda url: responses_arm64[url],
        )
        self.assertEqual(moved_arm64["backlog_count"], "0")
        self.assertEqual(moved_arm64["runner_arm64"], "own-arm64")

        # A default naming one of GitHub's images still counts its jobs.
        explicit = self._select(
            queued_jobs=11,
            runner_default_amd64_var=MODULE.DEFAULT_RUNNER_AMD64,
        )
        self.assertEqual(explicit["backlog_count"], "11")
        self.assertEqual(explicit["runner_amd64"], "blacksmith-amd64")

        responses_other = add_queued_jobs(backlog_responses(0), "ubuntu-22.04", 11)
        other_image = self._select(
            runner_default_amd64_var="ubuntu-22.04",
            fetch_json=lambda url: responses_other[url],
        )
        self.assertEqual(other_image["backlog_count"], "11")
        self.assertEqual(other_image["runner_amd64"], "blacksmith-amd64")

    def test_github_hosted_image_labels(self):
        for label in ("ubuntu-24.04", "ubuntu-22.04-arm", "ubuntu-latest", "ubuntu-slim",
                      "windows-11-arm", "macos-15-intel", "macos-latest-xlarge"):
            with self.subTest(label=label):
                self.assertTrue(MODULE.GITHUB_HOSTED_IMAGE_LABEL.fullmatch(label))
        for label in ("ubuntu-core", "own-amd64", "dash-x64-build", "ubuntu-24.04 "):
            with self.subTest(label=label):
                self.assertFalse(MODULE.GITHUB_HOSTED_IMAGE_LABEL.fullmatch(label))

    # --- arm64 pre-lint rung -------------------------------------------------

    def test_prelint_arm64_is_off_until_its_label_is_set(self):
        outputs = self._select()

        self.assertEqual(outputs["runner_prelint_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertEqual(
            outputs["prelint_arm64_decision_reason"], "selfhosted-arm64-disabled"
        )

    def test_prelint_arm64_uses_its_label_for_allowlisted_runs(self):
        outputs = self._select(runner_selfhosted_arm64_var="ubuntu-core-arm64")
        self.assertEqual(outputs["runner_prelint_arm64"], "ubuntu-core-arm64")
        self.assertEqual(
            outputs["prelint_arm64_decision_reason"], "selfhosted:backlog:3>0;queued:0<=6"
        )

        outputs = self._select(
            runner_selfhosted_arm64_var="ubuntu-core-arm64",
            event=pull_request_event("mallory"),
            actor="mallory",
        )
        self.assertEqual(outputs["runner_prelint_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertEqual(outputs["prelint_arm64_decision_reason"], "actor-not-allowed")

    def test_prelint_arm64_stays_hosted_without_backlog(self):
        outputs = self._select(queued_jobs=0, runner_selfhosted_arm64_var="ubuntu-core-arm64")

        self.assertEqual(outputs["runner_prelint_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertEqual(outputs["prelint_arm64_decision_reason"], "backlog:0<=0")

    def test_prelint_arm64_yields_to_label_and_measurement_errors(self):
        outputs = self._select(
            runner_selfhosted_arm64_var="ubuntu-core-arm64",
            event=pull_request_event("PastaPastaPasta", labels=["blacksmith-ci"]),
        )
        self.assertEqual(outputs["runner_prelint_arm64"], "blacksmith-arm64")
        self.assertEqual(outputs["prelint_arm64_decision_reason"], "label:blacksmith-ci")

        def fetch_json(_url):
            raise RuntimeError("boom")

        outputs = self._select(
            runner_selfhosted_arm64_var="ubuntu-core-arm64", fetch_json=fetch_json
        )
        self.assertEqual(outputs["runner_prelint_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertEqual(outputs["prelint_arm64_decision_reason"], "metric-unavailable")

    def test_prelint_arm64_saturates_above_six_queued(self):
        for queued, expected in ((6, "ubuntu-core-arm64"), (7, MODULE.DEFAULT_RUNNER_ARM64)):
            with self.subTest(queued=queued):
                outputs = self._select(
                    runner_selfhosted_arm64_var="ubuntu-core-arm64",
                    selfhosted_queued={"ubuntu-core-arm64": queued},
                )
                self.assertEqual(outputs["backlog_count"], "3")
                self.assertEqual(outputs["runner_prelint_arm64"], expected)

    # --- remote ccache write gate --------------------------------------------

    def test_cache_write_on_any_push(self):
        # A push only ever sees its own repository's secret, and pushing there
        # already requires write access, so even an unlisted actor qualifies.
        for actor in ("knst", "mallory"):
            with self.subTest(actor=actor):
                outputs = self._select(event_name="push", event={}, actor=actor)
                self.assertEqual(outputs["cache_write"], "true")

    def test_cache_write_for_trusted_pull_requests(self):
        for head_owner in ("PastaPastaPasta", "dashpay"):
            with self.subTest(head_owner=head_owner):
                outputs = self._select(
                    event=pull_request_event("PastaPastaPasta", head_owner=head_owner),
                )
                self.assertEqual(outputs["cache_write"], "true")

    def test_no_cache_write_for_untrusted_pull_requests(self):
        for author, actor, head_owner in (
            ("mallory", "PastaPastaPasta", "mallory"),
            ("PastaPastaPasta", "mallory", "PastaPastaPasta"),
            ("PastaPastaPasta", "PastaPastaPasta", "mallory"),
        ):
            with self.subTest(author=author, actor=actor, head_owner=head_owner):
                outputs = self._select(
                    event=pull_request_event(author, head_owner=head_owner),
                    actor=actor,
                )
                self.assertEqual(outputs["cache_write"], "false")

    def test_cache_write_does_not_depend_on_the_tiers(self):
        # The write gate shares the allowlist, not the self-hosted decision.
        outputs = self._select(queued_jobs=0)

        self.assertEqual(outputs["runner_fast"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["cache_write"], "true")

    def test_no_cache_write_for_other_events(self):
        for event_name in ("workflow_dispatch", "schedule", "pull_request"):
            with self.subTest(event_name=event_name):
                outputs = self._select(
                    event_name=event_name,
                    event=pull_request_event("PastaPastaPasta"),
                )
                self.assertEqual(outputs["cache_write"], "false")


if __name__ == "__main__":
    unittest.main()
