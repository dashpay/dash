import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("select_dynamic_runner.py")
SPEC = importlib.util.spec_from_file_location("select_dynamic_runner", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


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
                        {"status": "queued"},
                        {"status": "queued"},
                        {"status": "in_progress"},
                    ]
                },
                {},
            ),
        }

        def fetch_json(url):
            return responses[url]

        self.assertEqual(MODULE.count_queued_jobs(fetch_json, [repo]), 2)

    def test_label_override_selects_blacksmith_even_with_low_backlog(self):
        outputs = MODULE.select_runners(
            event_name="pull_request_target",
            event={"pull_request": {"labels": [{"name": "blacksmith-ci"}]}},
            threshold=10,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda _url: ({"workflow_runs": []}, {}),
            repos=["dashpay/dash"],
        )

        self.assertEqual(outputs["use_blacksmith"], "true")
        self.assertEqual(outputs["runner_amd64"], "blacksmith-amd64")
        self.assertEqual(outputs["runner_arm64"], "blacksmith-arm64")
        self.assertIn("label:blacksmith-ci", outputs["decision_reason"])

    def test_backlog_threshold_selects_blacksmith(self):
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
            jobs_url: ({"jobs": [{"status": "queued"}] * 11}, {}),
        }

        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda url: responses[url],
            repos=[repo],
        )

        self.assertEqual(outputs["use_blacksmith"], "true")
        self.assertEqual(outputs["backlog_count"], "11")
        self.assertIn("backlog:11>10", outputs["decision_reason"])

    def test_measurement_failure_falls_back_to_github(self):
        def fetch_json(_url):
            raise RuntimeError("boom")

        outputs = MODULE.select_runners(
            event_name="push",
            event={},
            threshold=10,
            runner_amd64_var="blacksmith-amd64",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=fetch_json,
            repos=["dashpay/dash"],
        )

        self.assertEqual(outputs["use_blacksmith"], "false")
        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_arm64"], MODULE.DEFAULT_RUNNER_ARM64)
        self.assertEqual(outputs["backlog_count"], "unknown")
        self.assertIn("metric-unavailable", outputs["decision_reason"])

    def test_missing_runner_vars_fall_back_per_arch(self):
        outputs = MODULE.select_runners(
            event_name="pull_request_target",
            event={"pull_request": {"labels": [{"name": "blacksmith-ci"}]}},
            threshold=10,
            runner_amd64_var="",
            runner_arm64_var="blacksmith-arm64",
            fetch_json=lambda _url: ({"workflow_runs": []}, {}),
            repos=["dashpay/dash"],
        )

        self.assertEqual(outputs["runner_amd64"], MODULE.DEFAULT_RUNNER_AMD64)
        self.assertEqual(outputs["runner_arm64"], "blacksmith-arm64")
        self.assertIn("amd64-github-fallback", outputs["decision_reason"])


if __name__ == "__main__":
    unittest.main()
