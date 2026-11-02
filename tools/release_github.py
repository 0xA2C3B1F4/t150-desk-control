#!/usr/bin/env python3
"""Read verified Actions results and maintain draft releases. Never publish."""

import argparse
import ast
import io
import json
import os
from pathlib import Path
import platform
import re
import urllib.parse
import urllib.request
import zipfile

import release


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Prevent authenticated API requests from following any redirect."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        """Decline redirects so urllib exposes the original redirect response."""
        return None


class API:
    """Access GitHub release and Actions endpoints with a caller-supplied token."""

    def __init__(self, token):
        """Store the API token without making a network request."""
        self.token = token

    def request(self, path, method="GET", data=None, raw=False, upload=False):
        """Send an authenticated JSON or binary request to a fixed GitHub API host.

        Require an absolute API path and disable redirects. Decode a nonempty
        response as JSON and propagate HTTP errors to the caller.
        """
        host = "https://uploads.github.com" if upload else "https://api.github.com"
        if not path.startswith("/") or path.startswith("//"):
            raise ValueError("Invalid API path")
        body = data if raw else release.json_bytes(data) if data is not None else None
        req = urllib.request.Request(
            host + path,
            data=body,
            method=method,
            headers={
                "Authorization": "Bearer " + self.token,
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "t150-release",
                "Content-Type": "application/octet-stream"
                if raw
                else "application/json",
            },
        )
        with urllib.request.build_opener(NoRedirect).open(req, timeout=60) as response:
            content = response.read()
        return json.loads(content) if content else None

    def pages(self, path, key=None):
        """Collect paginated API results, failing after 1,000 pages without an end."""
        result = []
        for page in range(1, 1001):
            data = self.request(
                path + ("&" if "?" in path else "?") + f"per_page=100&page={page}"
            )
            batch = data[key] if key else data
            result.extend(batch)
            if len(batch) < 100:
                return result
        raise ValueError("API pagination exceeded limit")

    def archive(self, artifact_id):
        """Download a bounded artifact ZIP through its HTTPS redirect.

        Follow the signed download URL separately so it receives no GitHub token.
        """
        try:
            self.request(
                f"/repos/{release.REPOSITORY}/actions/artifacts/{int(artifact_id)}/zip"
            )
        except urllib.error.HTTPError as error:
            if error.code != 302:
                raise
            location = error.headers["Location"]
            if urllib.parse.urlparse(location).scheme != "https":
                raise ValueError("Artifact redirect is not HTTPS")
            # Signed blob download receives no GitHub Authorization header.
            with urllib.request.urlopen(location, timeout=60) as response:
                data = response.read(10_000_001)
            if len(data) > 10_000_000:
                raise ValueError("Release artifact too large")
            return data
        raise ValueError("Artifact API did not return a download redirect")


def required_jobs(root):
    """Return required CI job names, expanding ESPHome jobs from run_all.TESTS."""
    tree = ast.parse((Path(root) / "tests/esphome/run_all.py").read_text())
    tests = next(
        ast.literal_eval(n.value)
        for n in tree.body
        if isinstance(n, ast.Assign)
        and any(isinstance(t, ast.Name) and t.id == "TESTS" for t in n.targets)
    )
    return (
        {
            "style",
            "native (Debug)",
            "native (Release)",
            "release-checks",
            "release-examples",
        }
        | {"esphome (" + name + ")" for name in tests if name != "github_example_build"}
        | {
            "github-example (" + name + ")"
            for name in (
                "stamp-c3",
                "esp32",
                "desk",
                "stamp-c3-firmware-export",
                "stamp-c3-firmware-update",
            )
        }
    )


def validate_report(report, version, commit, stage=None):
    """Reject report metadata with mismatched identity, stage or failed job results."""
    if (
        report.get("schema_version") != 1
        or report.get("passed") is not True
        or report.get("repository") != release.REPOSITORY
        or report.get("version") != version
        or report.get("candidate_sha") != commit
        or not re.fullmatch(release.SHA, report.get("workflow_sha", ""))
        or not isinstance(report.get("run_id"), int)
        or report["run_id"] < 1
        or not report.get("jobs")
        or any(v != "success" for v in report["jobs"].values())
        or stage is not None
        and report.get("stage") != stage
    ):
        raise ValueError("Invalid validation report")


def verify_run(run, workflow_id, successful):
    """Require the expected repository, workflow and main-branch dispatch context.

    Also require successful completion when requested by the caller.
    """
    if (
        run.get("repository", {}).get("full_name") != release.REPOSITORY
        or run.get("workflow_id") != workflow_id
        or run.get("event") != "workflow_dispatch"
        or run.get("head_branch") != "main"
        or successful
        and (run.get("status") != "completed" or run.get("conclusion") != "success")
    ):
        raise ValueError("Wrong repository, workflow, event, branch or run result")


def build_report(
    api, root, version, commit, workflow_sha, run_id, stage, candidate_run_id=None
):
    """Require exactly one successful latest job per required CI check.

    Return a validation report for the requested stage with source infrastructure
    and tool metadata. Artifact contents are checked separately.
    """
    if stage == "tagged-draft" and (
        not isinstance(candidate_run_id, int) or candidate_run_id < 1
    ):
        raise ValueError("Tagged report requires its candidate-check run ID")
    base = f"/repos/{release.REPOSITORY}"
    workflow_id = api.request(base + "/actions/workflows/release.yml")["id"]
    run = api.request(base + f"/actions/runs/{run_id}")
    verify_run(run, workflow_id, False)
    if run["head_sha"] != workflow_sha:
        raise ValueError("Dispatch SHA differs")
    all_jobs = api.pages(base + f"/actions/runs/{run_id}/jobs?filter=latest", "jobs")
    required = required_jobs(root)
    matched = {
        name: [j for j in all_jobs if j["name"].split(" / ")[-1] == name]
        for name in required
    }
    if any(
        len(jobs) != 1 or jobs[0]["conclusion"] != "success"
        for jobs in matched.values()
    ):
        raise ValueError("Required CI jobs are missing, skipped or failed")
    return {
        "schema_version": 1,
        "passed": True,
        "repository": release.REPOSITORY,
        "version": version,
        "candidate_sha": commit,
        "workflow_sha": workflow_sha,
        "run_id": int(run_id),
        "workflow_id": workflow_id,
        "stage": stage,
        "candidate_run_id": candidate_run_id,
        "jobs": {k: v[0]["conclusion"] for k, v in sorted(matched.items())},
        "tools": {
            "python": platform.python_version(),
            "runner": platform.platform(),
            "patch_llvm_pin": "23.1.0",
            "esphome_pin": release.esphome_pin(root),
        },
        "infrastructure": release.infrastructure(root, commit, workflow_sha),
    }


def unpack(data, directory):
    """Extract artifact files whose names exactly match the allowlist.

    Enforce the expanded-size limit and require a new output directory.
    """
    expected = {"package/" + name for name in release.ASSETS} | {
        "validation-report.json"
    }
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = [x for x in archive.infolist() if not x.is_dir()]
        if len(entries) != len(expected) or {x.filename for x in entries} != expected:
            raise ValueError("Unexpected artifact entries")
        if sum(x.file_size for x in entries) > 10_000_000:
            raise ValueError("Expanded artifact too large")
        directory.mkdir(parents=True, exist_ok=False)
        for entry in entries:
            target = directory / entry.filename
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(entry))


def previous_candidate(api, run_id, directory, root, version, commit):
    """Download and revalidate a successful candidate package against current source.

    Create the output directory and reject stale reports, changed job results
    or assets that differ from the deterministic package.
    """
    base = f"/repos/{release.REPOSITORY}"
    workflow_id = api.request(base + "/actions/workflows/release.yml")["id"]
    run = api.request(base + f"/actions/runs/{run_id}")
    verify_run(run, workflow_id, True)
    artifacts = [
        x
        for x in api.pages(base + f"/actions/runs/{run_id}/artifacts", "artifacts")
        if x["name"] == "release-package" and not x["expired"]
    ]
    if len(artifacts) != 1:
        raise ValueError(
            "Candidate artifact missing or ambiguous; rerun candidate-check"
        )
    unpack(api.archive(artifacts[0]["id"]), directory)
    report = json.loads((directory / "validation-report.json").read_text())
    validate_report(report, version, commit, "candidate-check")
    if (
        report["run_id"] != run_id
        or report["workflow_sha"] != run["head_sha"]
        or report["workflow_id"] != workflow_id
        or set(report["jobs"]) != required_jobs(root)
    ):
        raise ValueError("Report does not describe the selected candidate run")
    expected = build_report(
        api, root, version, commit, run["head_sha"], run_id, "candidate-check"
    )
    if (
        report["infrastructure"] != expected["infrastructure"]
        or report["jobs"] != expected["jobs"]
    ):
        raise ValueError("Candidate report differs from actual job results")
    release.verify_package(directory / "package", version, commit)
    generated = release.package_bytes(root, version, commit)
    if any(
        (directory / "package" / name).read_bytes() != data
        for name, data in generated.items()
    ):
        raise ValueError(
            "Candidate artifact differs from current deterministic package"
        )


def remote_tag(api, version, commit):
    """Require an annotated remote version tag to resolve to the expected commit."""
    base = f"/repos/{release.REPOSITORY}"
    obj = api.request(base + "/git/ref/tags/v" + version)["object"]
    if obj["type"] != "tag":
        raise ValueError("Remote tag is not annotated")
    for _ in range(10):
        obj = api.request(base + "/git/tags/" + obj["sha"])["object"]
        if obj["type"] == "commit":
            if obj["sha"] != commit:
                raise ValueError("Remote tag points to another commit")
            return
        if obj["type"] != "tag":
            break
    raise ValueError("Remote tag does not resolve to a commit")


def draft(api, directory, root, version, commit, run_id, workflow_sha):
    """Create or update a verified release draft and replace its allowlisted assets.

    Recheck tags, source assets and Actions results before writing. Refuse
    published or ambiguous releases and verify each uploaded asset digest.
    """
    release.verify_tag(root, version, commit)
    remote_tag(api, version, commit)
    release.verify_package(directory / "package", version, commit)
    report = json.loads((directory / "validation-report.json").read_text())
    validate_report(report, version, commit, "tagged-draft")
    expected = build_report(
        api,
        root,
        version,
        commit,
        workflow_sha,
        run_id,
        "tagged-draft",
        report.get("candidate_run_id"),
    )
    if any(
        report.get(key) != value for key, value in expected.items() if key != "tools"
    ):
        raise ValueError("Draft report differs from verified job results")
    for name, data in release.package_bytes(root, version, commit).items():
        if (directory / "package" / name).read_bytes() != data:
            raise ValueError("Draft asset differs from source: " + name)
    base = f"/repos/{release.REPOSITORY}"
    matches = [
        x for x in api.pages(base + "/releases") if x["tag_name"] == "v" + version
    ]
    if len(matches) > 1 or any(
        not x["draft"] or x["target_commitish"] != commit for x in matches
    ):
        raise ValueError("Release is published, ambiguous or belongs to another commit")
    body = (directory / "package/release-notes.md").read_text()
    body += f"\nValidated source: `{commit}`.\nValidation report SHA-256: `{release.digest((directory / 'validation-report.json').read_bytes())}`.\n"
    payload = {
        "tag_name": "v" + version,
        "target_commitish": commit,
        "name": version,
        "body": body,
        "draft": True,
        "prerelease": False,
    }
    remote_tag(api, version, commit)
    item = (
        api.request(base + f"/releases/{matches[0]['id']}", "PATCH", payload)
        if matches
        else api.request(base + "/releases", "POST", payload)
    )
    if not item["draft"]:
        raise ValueError("Refusing to alter a published release")
    rid = item["id"]
    for asset in api.pages(base + f"/releases/{rid}/assets"):
        if asset["name"] not in release.ASSETS | {"validation-report.json"}:
            raise ValueError("Existing draft contains an unexpected asset")
        api.request(base + f"/releases/assets/{asset['id']}", "DELETE")
    for name in sorted(release.ASSETS | {"validation-report.json"}):
        path = (
            directory / name
            if name == "validation-report.json"
            else directory / "package" / name
        )
        data = path.read_bytes()
        asset = api.request(
            base + f"/releases/{rid}/assets?name=" + urllib.parse.quote(name),
            "POST",
            data,
            raw=True,
            upload=True,
        )
        if asset.get("digest") != "sha256:" + release.digest(data):
            raise ValueError("Uploaded asset digest mismatch")
    print(item["html_url"])


def main():
    """Validate a candidate and dispatch report, artifact download or draft handling.

    Use GH_TOKEN for GitHub requests. Report and candidate-download write to
    --output; draft reads the package there, validates it and never publishes.
    """
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("command", choices=["report", "candidate-download", "draft"])
    p.add_argument("--source-root", type=Path, required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--commit", required=True)
    p.add_argument("--workflow-commit", required=True)
    p.add_argument("--run-id", type=int, required=True)
    p.add_argument("--candidate-run", type=int)
    p.add_argument("--stage", choices=["candidate-check", "tagged-draft"])
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    release.validate(args.source_root, args.version, args.commit)
    api = API(os.environ["GH_TOKEN"])
    if args.command == "report":
        if not args.stage:
            raise ValueError("Report stage required")
        args.output.write_bytes(
            release.json_bytes(
                build_report(
                    api,
                    args.source_root,
                    args.version,
                    args.commit,
                    args.workflow_commit,
                    args.run_id,
                    args.stage,
                    args.candidate_run,
                )
            )
        )
    elif args.command == "candidate-download":
        previous_candidate(
            api, args.run_id, args.output, args.source_root, args.version, args.commit
        )
    else:
        draft(
            api,
            args.output,
            args.source_root,
            args.version,
            args.commit,
            args.run_id,
            args.workflow_commit,
        )


if __name__ == "__main__":
    main()
