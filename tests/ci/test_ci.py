"""Exercise CI selection, fail-closed results and workflow wiring."""

import copy
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ci  # noqa: E402 - Repository tools are outside the test import path.


class SelectionTest(unittest.TestCase):
    """Keep builds enabled unless the whole PR changes only approved documents."""

    def setUp(self):
        """Provide full-length PR revisions without relying on GitHub metadata."""
        self.event = {
            "pull_request": {"base": {"sha": "a" * 40}, "head": {"sha": "b" * 40}}
        }

    def select(self, paths, **kwargs):
        """Return the chosen scope for NUL-delimited Git paths."""
        with patch(
            "ci.subprocess.check_output",
            return_value="".join(f"M\0{path}\0" for path in paths).encode(),
        ):
            return ci.select_checks(ROOT, "pull_request", self.event, **kwargs)[0]

    def test_documentation_only(self):
        """Skip builds only for README and Markdown under docs."""
        self.assertFalse(self.select(["README.md", "docs/nested/usage.md"]))

    def test_code_configuration_release_and_empty_changes(self):
        """Retain all checks for every unapproved path and an empty diff."""
        for path in [
            "tools/x.py",
            "components/x.cpp",
            ".github/workflows/test.yml",
            "requirements-ci.txt",
            ".prettierrc.json",
            ".prettierignore",
            ".node-version",
            "tools/style/package.json",
            "tools/style/package-lock.json",
            "examples/esphome/x.yaml",
            "CHANGELOG.md",
            "docs/example.yaml",
            "docs/images/panel.png",
            "OTHER.md",
        ]:
            with self.subTest(path=path):
                self.assertTrue(self.select(["README.md", path]))
        self.assertTrue(self.select([]))

    def test_deletions_type_changes_and_malformed_diff(self):
        """Deleting or replacing a document may affect packaging and requires builds."""
        for data in [
            b"D\0README.md\0",
            b"T\0docs/guide.md\0",
            b"M\0README.md",
            b"R100\0old.md\0README.md\0",
        ]:
            with (
                self.subTest(data=data),
                patch("ci.subprocess.check_output", return_value=data),
            ):
                self.assertTrue(ci.select_checks(ROOT, "pull_request", self.event)[0])

    def test_non_pr_and_forced_runs(self):
        """Run everything for main, dispatch, reusable calls and unknown events."""
        for event in ["push", "workflow_dispatch", "workflow_call", "merge_group", ""]:
            self.assertTrue(ci.select_checks(ROOT, event, self.event)[0])
        self.assertTrue(self.select(["README.md"], force_full=True))

    def test_unavailable_or_malformed_metadata(self):
        """A missing diff or invalid revision cannot suppress tests."""
        for event in [
            {},
            None,
            {"pull_request": None},
            {"pull_request": {"base": {"sha": "--help"}, "head": {"sha": "b" * 40}}},
        ]:
            self.assertTrue(ci.select_checks(ROOT, "pull_request", event)[0])
        for error in [
            OSError(),
            subprocess.CalledProcessError(1, "git"),
            subprocess.TimeoutExpired("git", 30),
        ]:
            with patch("ci.subprocess.check_output", side_effect=error):
                self.assertTrue(ci.select_checks(ROOT, "pull_request", self.event)[0])

    def test_git_history_includes_earlier_code_and_renamed_sources(self):
        """A final prose commit or a code-to-doc rename cannot hide earlier code."""
        with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
            root = Path(directory)
            env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
            env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")

            def git(*args):
                return subprocess.check_output(
                    [
                        "git",
                        "-c",
                        "core.hooksPath=" + os.devnull,
                        "-C",
                        directory,
                        *args,
                    ],
                    env=env,
                    stderr=subprocess.DEVNULL,
                    text=True,
                ).strip()

            git("init")
            git("config", "user.name", "Fixture")
            git("config", "user.email", "fixture@example.invalid")
            (root / "README.md").write_text("Initial\n")
            (root / "code.py").write_text("print(1)\n")
            git("add", ".")
            git("commit", "-m", "base")
            base = git("rev-parse", "HEAD")
            (root / "README.md").write_text("Updated\n")
            git("commit", "-am", "docs")
            event = {
                "pull_request": {
                    "base": {"sha": base},
                    "head": {"sha": git("rev-parse", "HEAD")},
                }
            }
            self.assertFalse(ci.select_checks(root, "pull_request", event)[0])
            (root / "README.md").unlink()
            git("commit", "-am", "delete README")
            event["pull_request"]["head"]["sha"] = git("rev-parse", "HEAD")
            self.assertTrue(ci.select_checks(root, "pull_request", event)[0])
            (root / "README.md").write_text("Restored\n")
            git("add", "README.md")
            git("commit", "-m", "restore README")
            (root / "code.py").write_text("print(2)\n")
            git("commit", "-am", "code")
            (root / "README.md").write_text("Final docs\n")
            git("commit", "-am", "docs again")
            event["pull_request"]["head"]["sha"] = git("rev-parse", "HEAD")
            self.assertTrue(ci.select_checks(root, "pull_request", event)[0])
            event["pull_request"]["base"]["sha"] = git("rev-parse", "HEAD")
            (root / "docs").mkdir()
            git("mv", "code.py", "docs/source.md")
            git("commit", "-m", "rename")
            event["pull_request"]["head"]["sha"] = git("rev-parse", "HEAD")
            self.assertTrue(ci.select_checks(root, "pull_request", event)[0])


class ResultTest(unittest.TestCase):
    """Reject incomplete full runs and failed lightweight prerequisites."""

    def needs(self, full):
        """Create the aggregated job results passed by GitHub Actions."""
        return {
            "changes": {"result": "success", "outputs": {"full": str(full).lower()}},
            **{
                name: {"result": "success" if full else "skipped"}
                for name in ci.FULL_JOBS
            },
        }

    def test_complete_runs(self):
        """Accept both a complete suite and deliberate documentation-only skips."""
        ci.check_results(self.needs(True))
        ci.check_results(self.needs(False))

    def test_every_unsuccessful_full_job(self):
        """Every required job must succeed, including the selector."""
        for name in self.needs(True):
            for result in ["failure", "skipped", "cancelled", None]:
                with self.subTest(name=name, result=result):
                    needs = self.needs(True)
                    needs[name]["result"] = result
                    with self.assertRaises(ValueError):
                        ci.check_results(needs)

    def test_light_run_cannot_hide_failed_selection_or_unknown_scope(self):
        """Fail when selection did not complete or job metadata is incomplete."""
        for full in [True, False]:
            needs = self.needs(full)
            for change in [
                {"result": "failure"},
                {"outputs": {}},
                {"outputs": {"full": ""}},
            ]:
                broken = copy.deepcopy(needs)
                broken["changes"].update(change)
                with self.assertRaises(ValueError):
                    ci.check_results(broken)
            del needs["native"]
            with self.assertRaises(ValueError):
                ci.check_results(needs)


class WorkflowTest(unittest.TestCase):
    """Bind the workflow conditions to the tested selection and release contract."""

    def test_triggers_and_release_override(self):
        """Avoid duplicate branch pushes and always force a full release run."""
        workflow = yaml.safe_load((ROOT / ".github/workflows/test.yml").read_text())
        triggers = workflow[True]  # PyYAML's YAML 1.1 loader interprets `on` as True.
        self.assertEqual(triggers["push"], {"branches": ["main"]})
        self.assertIn("pull_request", triggers)
        self.assertIn("workflow_dispatch", triggers)
        self.assertTrue(triggers["workflow_call"]["inputs"]["full-suite"]["default"])
        release = yaml.safe_load((ROOT / ".github/workflows/release.yml").read_text())
        self.assertIs(release["jobs"]["tests"]["with"]["full-suite"], True)
        concurrency = workflow["concurrency"]
        self.assertIn("github.event.pull_request.number", concurrency["group"])
        self.assertIn("github.run_id", concurrency["group"])
        self.assertIn("!inputs.full-suite", concurrency["cancel-in-progress"])

    def test_markdown_check_runs_in_each_selected_path(self):
        """Light PRs check Markdown after selection; full runs check it in style."""
        jobs = yaml.safe_load((ROOT / ".github/workflows/test.yml").read_text())["jobs"]
        steps = jobs["changes"]["steps"]
        select = next(i for i, step in enumerate(steps) if step.get("id") == "select")
        commands = [
            "python tools/install_ci_node.py",
            "python tools/format_assets.py --check --group markdown",
        ]
        for command in commands:
            matches = [
                (i, step) for i, step in enumerate(steps) if step.get("run") == command
            ]
            self.assertEqual(len(matches), 1)
            index, step = matches[0]
            self.assertGreater(index, select)
            self.assertEqual(step["if"], "steps.select.outputs.full == 'false'")
        full = next(
            step
            for step in jobs["style"]["steps"]
            if step.get("name") == "Check asset formatting"
        )
        self.assertEqual(
            full["run"],
            "python tools/format_assets.py --check --group web --group data --group markdown",
        )
        self.assertNotIn("if", full)

    def test_job_dependencies_and_always_running_gate(self):
        """Heavy jobs use one selector and the final check sees every job result."""
        jobs = yaml.safe_load((ROOT / ".github/workflows/test.yml").read_text())["jobs"]
        self.assertEqual(set(jobs), ci.FULL_JOBS | {"changes", "ci-result"})
        for name in ci.FULL_JOBS:
            self.assertEqual(jobs[name]["needs"], "changes")
            self.assertEqual(jobs[name]["if"], "needs.changes.outputs.full == 'true'")
        gate = jobs["ci-result"]
        self.assertEqual(gate["if"], "${{ always() }}")
        self.assertEqual(set(gate["needs"]), ci.FULL_JOBS | {"changes"})
        self.assertEqual(gate["steps"][-1]["env"]["CI_NEEDS"], "${{ toJSON(needs) }}")


if __name__ == "__main__":
    unittest.main()
