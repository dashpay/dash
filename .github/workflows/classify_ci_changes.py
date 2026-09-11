# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import argparse
import json
import os
from pathlib import PurePosixPath
from typing import Dict, List, Optional, Sequence


EXCLUDED_EXACT_PATHS = {
    "doc/.gitignore",
}
EXCLUDED_PREFIXES = (
    ".github/ISSUE_TEMPLATE/",
    ".github/PULL_REQUEST_TEMPLATE/",
)
EXCLUDED_DOC_SUFFIXES = {".1", ".5", ".8", ".png", ".svg", ".txt"}
BUILD_RELEVANT_PATHS = {"COPYING", "doc/README_windows.txt"}


def is_excluded_path(path: str) -> bool:
    if not path or path in BUILD_RELEVANT_PATHS:
        return False
    if path in EXCLUDED_EXACT_PATHS or path.startswith(EXCLUDED_PREFIXES):
        return True

    suffix = PurePosixPath(path).suffix
    if suffix == ".md":
        return True
    return path.startswith("doc/") and suffix in EXCLUDED_DOC_SUFFIXES


def classify_changes(
    paths: Sequence[str], complete: bool, removed_or_renamed: bool = False
) -> Dict[str, object]:
    unique_paths = sorted(set(paths))
    if not complete:
        return {
            "run_build_tests": True,
            "reason": "changed path list is incomplete",
            "paths": unique_paths,
            "triggering_paths": [],
        }
    if removed_or_renamed:
        return {
            "run_build_tests": True,
            "reason": "a changed path was removed or renamed",
            "paths": unique_paths,
            "triggering_paths": [],
        }
    if not unique_paths:
        return {
            "run_build_tests": True,
            "reason": "no changed paths were reported",
            "paths": unique_paths,
            "triggering_paths": [],
        }

    triggering_paths = [path for path in unique_paths if not is_excluded_path(path)]
    if triggering_paths:
        reason = "build-relevant or unclassified paths changed"
    else:
        reason = "all changed paths are in CI exclusion zones"
    return {
        "run_build_tests": bool(triggering_paths),
        "reason": reason,
        "paths": unique_paths,
        "triggering_paths": triggering_paths,
    }


def load_paths(path: str) -> List[str]:
    with open(path, "r", encoding="utf-8") as file:
        paths = json.load(file)
    if not isinstance(paths, list) or not all(isinstance(item, str) for item in paths):
        raise ValueError("changed paths must be a JSON array of strings")
    return paths


def write_github_output(path: Optional[str], result: Dict[str, object]) -> None:
    if not path:
        return
    with open(path, "a", encoding="utf-8") as file:
        file.write(
            "run-build-tests={}\n".format(
                "true" if result["run_build_tests"] else "false"
            )
        )
        file.write("decision-reason={}\n".format(result["reason"]))


def write_step_summary(
    path: Optional[str], result: Dict[str, object], complete: bool
) -> None:
    if not path:
        return

    triggering_paths = result["triggering_paths"]
    with open(path, "a", encoding="utf-8") as file:
        file.write("### Build and test path classification\n")
        file.write(
            "- Changed path list complete: {}\n".format(
                "yes" if complete else "no"
            )
        )
        file.write("- Changed paths examined: {}\n".format(len(result["paths"])))
        file.write(
            "- Full build and test matrix: {}\n".format(
                "required" if result["run_build_tests"] else "skipped"
            )
        )
        file.write("- Decision: `{}`\n".format(result["reason"]))
        if triggering_paths:
            file.write("- Triggering paths (up to 20):\n")
            for changed_path in triggering_paths[:20]:
                formatted_path = json.dumps(changed_path, ensure_ascii=True).replace(
                    "`", "\\u0060"
                )
                file.write(
                    "  - `{}`\n".format(formatted_path)
                )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Classify changed paths for Dash CI.")
    parser.add_argument("paths_json", help="JSON file containing changed paths")
    parser.add_argument(
        "--complete",
        choices=("true", "false"),
        required=True,
        help="Whether the changed path list is known to be complete",
    )
    parser.add_argument(
        "--removed-or-renamed",
        choices=("true", "false"),
        required=True,
        help="Whether a changed path was removed or renamed",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    complete = args.complete == "true"
    result = classify_changes(
        load_paths(args.paths_json), complete, args.removed_or_renamed == "true"
    )
    write_github_output(os.environ.get("GITHUB_OUTPUT"), result)
    write_step_summary(os.environ.get("GITHUB_STEP_SUMMARY"), result, complete)
    print(
        "run-build-tests={} ({})".format(
            "true" if result["run_build_tests"] else "false", result["reason"]
        )
    )
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main(sys.argv[1:]))
