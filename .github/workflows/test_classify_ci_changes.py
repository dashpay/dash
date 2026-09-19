import importlib.util
import json
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("classify_ci_changes.py")
SPEC = importlib.util.spec_from_file_location("classify_ci_changes", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ClassifyCIChangesTest(unittest.TestCase):
    def test_markdown_files_are_excluded_at_any_depth(self):
        for path in ("README.md", "doc/release-notes-1234.md", "depends/README.md"):
            with self.subTest(path=path):
                self.assertTrue(MODULE.is_excluded_path(path))

    def test_static_documentation_files_are_excluded(self):
        for path in (
            "doc/assets/diagram.png",
            "doc/assets/diagram.svg",
            "doc/man/dashd.1",
            "doc/notes.txt",
        ):
            with self.subTest(path=path):
                self.assertTrue(MODULE.is_excluded_path(path))

    def test_documentation_build_inputs_are_not_excluded(self):
        for path in ("doc/Doxyfile.in", "doc/man/Makefile.am"):
            with self.subTest(path=path):
                self.assertFalse(MODULE.is_excluded_path(path))

    def test_repository_templates_are_excluded(self):
        for path in (
            ".github/ISSUE_TEMPLATE/config.yml",
            ".github/PULL_REQUEST_TEMPLATE/release.md",
            ".github/PULL_REQUEST_TEMPLATE.md",
        ):
            with self.subTest(path=path):
                self.assertTrue(MODULE.is_excluded_path(path))

    def test_windows_installer_inputs_are_not_excluded(self):
        for path in ("COPYING", "doc/README_windows.txt"):
            with self.subTest(path=path):
                self.assertFalse(MODULE.is_excluded_path(path))

    def test_build_relevant_and_unknown_paths_require_tests(self):
        for path in (
            "src/net.cpp",
            "test/functional/p2p_invalid_messages.py",
            "depends/packages/boost.mk",
            "ci/dash/matrix.sh",
            ".github/workflows/build.yml",
            "configure.ac",
            "new-area/README.txt",
        ):
            with self.subTest(path=path):
                self.assertFalse(MODULE.is_excluded_path(path))

    def test_only_excluded_paths_skip_builds_and_tests(self):
        result = MODULE.classify_changes(
            ["README.md", "doc/assets/diagram.svg", ".github/ISSUE_TEMPLATE/config.yml"],
            complete=True,
        )

        self.assertFalse(result["run_build_tests"])
        self.assertEqual(result["triggering_paths"], [])

    def test_mixed_changes_require_builds_and_tests(self):
        result = MODULE.classify_changes(
            ["doc/release-notes-1234.md", "src/net.cpp"], complete=True
        )

        self.assertTrue(result["run_build_tests"])
        self.assertEqual(result["triggering_paths"], ["src/net.cpp"])

    def test_previous_rename_path_can_require_builds_and_tests(self):
        result = MODULE.classify_changes(
            ["doc/retired-code.md", "src/retired-code.cpp"], complete=True
        )

        self.assertTrue(result["run_build_tests"])
        self.assertEqual(result["triggering_paths"], ["src/retired-code.cpp"])

    def test_incomplete_or_empty_path_lists_fail_open(self):
        for paths, complete in ((["README.md"], False), ([], True)):
            with self.subTest(paths=paths, complete=complete):
                result = MODULE.classify_changes(paths, complete)
                self.assertTrue(result["run_build_tests"])

    def test_removed_or_renamed_documentation_fails_open(self):
        result = MODULE.classify_changes(
            ["doc/man/dashd.1"], complete=True, removed_or_renamed=True
        )

        self.assertTrue(result["run_build_tests"])
        self.assertIn("removed or renamed", result["reason"])

    def test_load_paths_rejects_malformed_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "paths.json"
            path.write_text(json.dumps({"path": "README.md"}), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "JSON array of strings"):
                MODULE.load_paths(str(path))

    def test_step_summary_escapes_markdown_backticks_in_paths(self):
        result = MODULE.classify_changes(["src/name` [link](example).cpp"], complete=True)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "summary.md"
            MODULE.write_step_summary(str(path), result, complete=True)
            summary = path.read_text(encoding="utf-8")

        self.assertIn(r"name\u0060 [link](example).cpp", summary)
        self.assertNotIn("name` [link](example).cpp", summary)


if __name__ == "__main__":
    unittest.main()
