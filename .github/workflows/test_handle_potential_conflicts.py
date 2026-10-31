#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import argparse
import base64
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT_PATH = Path(__file__).with_name("handle_potential_conflicts.py")
SPEC = importlib.util.spec_from_file_location("handle_potential_conflicts", SCRIPT_PATH)
handle_potential_conflicts = importlib.util.module_from_spec(SPEC)
sys.modules["handle_potential_conflicts"] = handle_potential_conflicts
SPEC.loader.exec_module(handle_potential_conflicts)


BOT_USER = {"login": "github-actions[bot]", "type": "Bot"}
USER = {"login": "contributor", "type": "User"}


def comment(comment_id, body, user=None):
    return {
        "id": comment_id,
        "body": body,
        "user": user or BOT_USER,
    }


def item(number, title="title", files=()):
    return {
        "number": number,
        "title": title,
        "url": f"https://github.com/dashpay/dash/pull/{number}",
        "files": list(files),
    }


LONG_AGO = "2020-01-01T00:00:00Z"
RECENTLY = (datetime.now(timezone.utc) - timedelta(hours=1)).strftime("%Y-%m-%dT%H:%M:%SZ")


def pr(number, base="develop", draft=False, updated_at=LONG_AGO):
    return {
        "number": number,
        "title": f"pr {number}",
        "html_url": f"https://github.com/dashpay/dash/pull/{number}",
        "base": {"ref": base},
        "head": {"label": f"contributor:branch-{number}"},
        "draft": draft,
        "updated_at": updated_at,
    }


def legacy_body(state):
    encoded = base64.b64encode(json.dumps(state).encode()).decode()
    return f"## Potential PR merge conflicts\n\n{handle_potential_conflicts.COMMENT_START}\n{encoded}\n-->\n"


def managed(comment_id, conflicts):
    body = handle_potential_conflicts.render_comment_body(conflicts)
    return handle_potential_conflicts.ManagedComment(comment_id, body, {int(entry["number"]) for entry in conflicts})


def numbers(conflicts):
    return {number: [entry["number"] for entry in items] for number, items in conflicts.items()}


class TestPotentialConflictComments(unittest.TestCase):
    def test_rendered_comment_lists_prs_and_round_trips(self):
        title = f"title with {handle_potential_conflicts.COMMENT_START} --> marker-like text"
        body = handle_potential_conflicts.render_comment_body([item(12, title, ["src/net.cpp"]), item(9, "refactor: source")])

        self.assertEqual({9, 12}, handle_potential_conflicts.extract_listed_prs(body))
        self.assertTrue(body.startswith("## Potential PR merge conflicts"))
        self.assertTrue(body.rstrip("\n").endswith(handle_potential_conflicts.COMMENT_END))
        self.assertLess(body.index("[#9: refactor: source]"), body.index("[#12: title with"))

    def test_extract_listed_prs_from_legacy_comments(self):
        state = {
            "outbound": [item(2), {"title": "missing number"}, "not-a-dict"],
            "inbound": {"3": item(3), "bad": {"title": "missing number"}},
        }
        body = legacy_body(state)
        marker_index = body.rfind(handle_potential_conflicts.COMMENT_START)
        marker_first_body = body[marker_index:] + body[:marker_index]

        self.assertEqual({2, 3}, handle_potential_conflicts.extract_listed_prs(body))
        self.assertEqual({2, 3}, handle_potential_conflicts.extract_listed_prs(marker_first_body))

    def test_invalid_state_is_treated_as_empty(self):
        body = """<!-- dash-potential-conflicts:v1
not-json
-->
## Potential PR merge conflicts
"""

        self.assertEqual(set(), handle_potential_conflicts.extract_listed_prs(body))

    def test_rendered_comment_escapes_markdown_content(self):
        body = handle_potential_conflicts.render_comment_body([item(12, "fix [link] title", ["src/`odd`.cpp"])])

        self.assertIn(r"[#12: fix \[link\] title](https://github.com/dashpay/dash/pull/12)", body)
        self.assertIn("<code>src/`odd`.cpp</code>", body)

    def test_user_forged_managed_comment_is_ignored(self):
        body = handle_potential_conflicts.render_comment_body([item(12, "forged")])
        with mock.patch.object(handle_potential_conflicts, "list_issue_comments", return_value=[comment(123, body, USER)]):
            self.assertEqual([], handle_potential_conflicts.find_managed_comments(1))

    def save(self, comments, conflicts):
        with mock.patch.object(handle_potential_conflicts, "github_patch") as patch, \
                mock.patch.object(handle_potential_conflicts, "github_delete") as delete, \
                mock.patch.object(handle_potential_conflicts, "github_post") as post:
            handle_potential_conflicts.save_managed_comment(10, comments, conflicts)
        return ([call.args[0] for call in post.call_args_list],
                [call.args[0] for call in patch.call_args_list],
                [call.args[0] for call in delete.call_args_list])

    def test_save_managed_comment_writes_only_what_changed(self):
        self.assertEqual((["issues/10/comments"], [], []), self.save([], [item(13)]))
        self.assertEqual(([], ["issues/comments/3"], ["issues/comments/1"]),
                         self.save([managed(1, [item(11)]), managed(3, [item(12)])], [item(13)]))
        self.assertEqual(([], [], []), self.save([managed(3, [item(13)])], [item(13)]))
        self.assertEqual(([], [], ["issues/comments/1", "issues/comments/3"]),
                         self.save([managed(1, [item(11)]), managed(3, [item(12)])], []))

    def find_conflicts(self, prs, files, mergeable_state, can_merge):
        def github_get(path):
            number = int(path.removeprefix("pulls/"))
            if mergeable_state[number] is None:
                raise OSError("boom")
            return {"mergeable_state": mergeable_state[number]}

        with mock.patch.object(handle_potential_conflicts, "get_pr_files", side_effect=files.get), \
                mock.patch.object(handle_potential_conflicts, "github_get", side_effect=github_get), \
                mock.patch.object(handle_potential_conflicts, "branches_can_merge",
                                  side_effect=lambda a, b: can_merge[(int(a.split("-")[-1]), int(b.split("-")[-1]))]) as check:
            conflicts, unchecked = handle_potential_conflicts.find_conflicts(prs)

        checked = [tuple(int(label.split("-")[-1]) for label in call.args) for call in check.call_args_list]
        return checked, numbers(conflicts), {number: sorted(items) for number, items in unchecked.items() if items}

    def test_find_conflicts_checks_each_overlapping_pair_once(self):
        # 4 shares a file with 1 but targets another base; 5 shares nothing with 1-3.
        prs = [pr(1), pr(2), pr(3), pr(4, base="v23.x"), pr(5)]
        files = {1: {"a"}, 2: {"a", "b"}, 3: {"b"}, 4: {"a"}, 5: {"c"}}
        mergeable_state = {1: "clean", 2: "blocked", 3: "unstable", 5: "clean"}
        checked, conflicts, unchecked = self.find_conflicts(prs, files, mergeable_state, {(1, 2): False, (2, 3): True})

        self.assertEqual([(1, 2), (2, 3)], checked)
        self.assertEqual({1: [2], 2: [1], 3: [], 4: [], 5: []}, conflicts)
        self.assertEqual({}, unchecked)

    def test_find_conflicts_leaves_pairs_it_cannot_check_unchecked(self):
        # The merge check fails for 1 and 2; 3 needs a rebase, 4 could not be fetched,
        # GitHub has not yet checked 5 against its base, and 8's files could not be listed,
        # so their pairs are never checked.
        prs = [pr(number) for number in range(1, 9)] + [pr(9, base="v23.x")]
        files = {1: {"a"}, 2: {"a"}, 3: {"b"}, 4: {"b"}, 5: {"c"}, 6: {"c"}, 7: {"b"}, 8: None, 9: {"a"}}
        mergeable_state = {1: "clean", 2: "clean", 3: "dirty", 4: None, 5: "unknown", 6: "clean", 7: "clean"}
        checked, conflicts, unchecked = self.find_conflicts(prs, files, mergeable_state, {(1, 2): None})

        self.assertEqual([(1, 2)], checked)
        self.assertEqual({number: [] for number in range(1, 10)}, conflicts)
        self.assertEqual({1: [2, 8], 2: [1, 8], 3: [4, 7, 8], 4: [3, 7, 8], 5: [6, 8], 6: [5, 8], 7: [3, 4, 8],
                          8: [1, 2, 3, 4, 5, 6, 7]}, unchecked)

        with mock.patch.object(handle_potential_conflicts, "github_get_paginated", side_effect=OSError("boom")):
            self.assertIsNone(handle_potential_conflicts.get_pr_files(8))

    def run_main(self, open_prs, conflicts, unchecked, find_managed_comments, closed_prs=(), dry_run=False):
        saved = []
        exit_code = None
        with mock.patch.object(handle_potential_conflicts, "parse_args", return_value=argparse.Namespace(dry_run=dry_run)), \
                mock.patch.object(handle_potential_conflicts, "github_get_paginated", return_value=open_prs), \
                mock.patch.object(handle_potential_conflicts, "github_get", return_value=list(closed_prs)), \
                mock.patch.object(handle_potential_conflicts, "find_conflicts", return_value=(conflicts, unchecked)) as find, \
                mock.patch.object(handle_potential_conflicts, "find_managed_comments", side_effect=find_managed_comments), \
                mock.patch.object(handle_potential_conflicts, "save_managed_comment",
                                  side_effect=lambda number, _comments, items: saved.append((number, [entry["number"] for entry in items]))), \
                mock.patch("sys.stdout"):
            try:
                handle_potential_conflicts.main()
            except SystemExit as e:
                exit_code = e.code
        return [entry["number"] for entry in find.call_args.args[0]], saved, exit_code

    def test_main_rewrites_comments_and_clears_prs_no_longer_checked(self):
        # 7 became a draft and 8 was closed since the comments were written. 1 and 3 could
        # not be checked against 4 this run; only 1's comment already listed 4. 5 and 6 listed
        # only each other and both closed recently, and 9 was made a draft recently, so nothing
        # lists them any more; 10 was closed long ago.
        comments = {
            1: [managed(11, [item(2), item(4), item(7)])],
            2: [managed(12, [item(1), item(8)])],
            7: [managed(17, [item(1)])],
            8: [managed(18, [item(2)])],
        }
        checked, saved, exit_code = self.run_main(
            [pr(1), pr(2), pr(3), pr(4), pr(7, draft=True), pr(9, draft=True, updated_at=RECENTLY)],
            {1: [item(2)], 2: [item(1)], 3: [], 4: []},
            {1: {4: item(4)}, 2: {}, 3: {4: item(4)}, 4: {1: item(1), 3: item(3)}},
            lambda number: comments.get(number, []),
            closed_prs=[pr(5, updated_at=RECENTLY), pr(6, updated_at=RECENTLY), pr(10)],
        )

        self.assertEqual([1, 2, 3, 4], checked)
        self.assertEqual([(1, [2, 4]), (2, [1]), (3, []), (4, []), (5, []), (6, []), (7, []), (8, []), (9, [])], saved)
        self.assertIsNone(exit_code)

    def test_main_does_not_write_a_comment_it_could_not_read(self):
        def find_managed_comments(number):
            if number in (1, 5):
                raise OSError("boom")
            return []

        _checked, saved, exit_code = self.run_main(
            [pr(1), pr(2)], {1: [item(2)], 2: [item(1)]}, {1: {}, 2: {}}, find_managed_comments,
            closed_prs=[pr(5, updated_at=RECENTLY)])

        self.assertEqual([(2, [1])], saved)
        self.assertIn("[1, 5]", exit_code)

    def test_dry_run_writes_nothing(self):
        _checked, saved, exit_code = self.run_main(
            [pr(1), pr(2)], {1: [item(2)], 2: [item(1)]}, {1: {}, 2: {}}, lambda number: [managed(number, [item(8)])],
            closed_prs=[pr(5, updated_at=RECENTLY)], dry_run=True)

        self.assertEqual([], saved)
        self.assertIsNone(exit_code)

    def test_format_files_limits_long_file_lists(self):
        files = [f"src/file_{i}.cpp" for i in range(handle_potential_conflicts.MAX_FILE_DETAILS + 2)]

        formatted = handle_potential_conflicts.format_files(files)

        self.assertIn("and 2 more", formatted)
        self.assertIn("<code>src/file_0.cpp</code>", formatted)
        self.assertNotIn(f"<code>src/file_{handle_potential_conflicts.MAX_FILE_DETAILS}.cpp</code>", formatted)

    def test_next_page_url_reads_link_header(self):
        link = (
            '<https://api.github.com/repositories/1/issues/5/comments?page=1>; rel="prev", '
            '<https://api.github.com/repositories/1/issues/5/comments?page=3>; rel="next", '
            '<https://api.github.com/repositories/1/issues/5/comments?page=9>; rel="last"'
        )

        self.assertEqual(
            "https://api.github.com/repositories/1/issues/5/comments?page=3",
            handle_potential_conflicts.next_page_url(link),
        )
        self.assertIsNone(handle_potential_conflicts.next_page_url('<https://x/?page=1>; rel="prev"'))
        self.assertIsNone(handle_potential_conflicts.next_page_url(""))


if __name__ == "__main__":
    unittest.main()
