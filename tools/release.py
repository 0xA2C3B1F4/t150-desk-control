#!/usr/bin/env python3
"""Validate a clean release candidate and package its explicit binary build.

This tool never creates tags or publishes releases. It uses only the standard
library. GitHub draft handling lives in release_github.py and is checked as
part of the release infrastructure before either workflow stage runs.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import release_binary

REPOSITORY = "0xA2C3B1F4/t150-desk-control"
VERSION = r"[0-9]{4}\.(?:[1-9]|1[0-2])\.(?:0|[1-9][0-9]*)"
SHA = r"[0-9a-f]{40}"
VERSION_FILE = "components/desk_uart/project_version.json"
STYLE_INFRASTRUCTURE = {
    "tools/format_assets.py",
    "tools/install_ci_node.py",
    ".node-version",
    "tools/style/package.json",
    "tools/style/package-lock.json",
    ".prettierrc.json",
    ".prettierignore",
}
YAMLS = {
    name: "examples/esphome/" + name
    for name in (
        "stamp-c3.yaml",
        "esp32.yaml",
        "existing-device.yaml",
        "secrets.example.yaml",
        "stamp-c3-firmware-export.yaml",
        "stamp-c3-firmware-update.yaml",
        "stamp-c3-tb01.yaml",
    )
}
YAMLS.update(
    {
        name: "examples/esphome/packages/" + name
        for name in (
            "panel-lock.yaml",
            "reset-speed.yaml",
            "motion-trims.yaml",
            "custom-timer-buzzer.yaml",
            "firmware-export.yaml",
            "firmware-update.yaml",
            "stamp-c3-controls.yaml",
        )
    }
)
ASSETS = frozenset(YAMLS) | {
    "firmware-catalog.json",
    "release-notes.md",
    "INSTALL.md",
    "release-manifest.json",
    "SHA256SUMS",
    release_binary.REPORT,
}


def asset_names(version):
    """Return the exact release allowlist, including its single named binary."""
    return ASSETS | {release_binary.binary_name(version)}


def digest(data):
    """Return the SHA-256 hexadecimal digest used for release asset verification."""
    return hashlib.sha256(data).hexdigest()


def json_bytes(data):
    """Encode deterministic UTF-8 JSON with sorted keys and a final newline."""
    return (
        json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False) + "\n"
    ).encode()


def current_identity(catalog):
    """Resolve the release target from its own catalog, not workflow-side recipes.

    The compiled catalog check binds this metadata to the production recipe in
    the candidate test run. Release workflows can run from a later main commit.
    """
    current = catalog.get("current", {})
    target = current.get("id")
    images = [image for image in catalog.get("images", []) if image.get("id") == target]
    if (
        catalog.get("schema_version") != 2
        or not isinstance(target, str)
        or not target
        or not isinstance(current.get("profile"), str)
        or not current["profile"]
        or catalog.get("prepare", {}).get("target") != target
        or len(images) != 1
        or not isinstance(images[0].get("sha256"), str)
        or not re.fullmatch(r"[0-9a-f]{64}", images[0]["sha256"])
    ):
        raise ValueError("Invalid current controller firmware in release catalog")
    return {"id": target, "sha256": images[0]["sha256"]}


def git(root, *args):
    """Run Git at the source root with a clean Git environment and a 30-second limit."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0")
    return subprocess.check_output(
        ["git", "-C", str(root), *args], env=env, stderr=subprocess.PIPE, timeout=30
    )


def text(root, *args):
    """Return decoded Git output with surrounding whitespace removed."""
    return git(root, *args).decode().strip()


def validate(root, version, commit):
    """Return the resolved root of a clean candidate matching its version and SHA.

    Reject substituted files, symlinks and metadata that differ from the
    committed source used to build release assets.
    """
    if not re.fullmatch(VERSION, version) or not re.fullmatch(SHA, commit):
        raise ValueError("Expected YYYY.M.N version and full lowercase commit SHA")
    root = Path(root).resolve()
    if Path(text(root, "rev-parse", "--show-toplevel")).resolve() != root:
        raise ValueError("Source root must be the repository root")
    if text(root, "rev-parse", "HEAD") != commit:
        raise ValueError("Candidate HEAD differs from expected commit")
    if text(root, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("Candidate source tree must be clean")
    data = json.loads((root / VERSION_FILE).read_text())
    if (
        data != {"schema_version": 1, "version": version}
        or type(data["schema_version"]) is not int
    ):
        raise ValueError("Candidate version metadata differs")
    # Read committed bytes, not ignored local substitutes.
    for path in [
        VERSION_FILE,
        "metadata/firmware-catalog.json",
        "CHANGELOG.md",
        "requirements-dev.txt",
        release_binary.CONFIG,
        *YAMLS.values(),
    ]:
        if (root / path).is_symlink() or (root / path).read_bytes() != git(
            root, "show", f"{commit}:{path}"
        ):
            raise ValueError("Source differs from committed regular file: " + path)
    return root


def verify_tag(root, version, commit):
    """Require the local version tag to be annotated and resolve to the candidate."""
    if not re.fullmatch(VERSION, version) or not re.fullmatch(SHA, commit):
        raise ValueError("Invalid version or commit")
    ref = "refs/tags/v" + version
    if text(root, "cat-file", "-t", ref) != "tag":
        raise ValueError("Release tag must be annotated")
    if text(root, "rev-parse", ref + "^{commit}") != commit:
        raise ValueError("Release tag points to another commit")


def infrastructure(root, candidate, workflow):
    """Verify unchanged release infrastructure between a candidate and descendant.

    Return the infrastructure paths and blob IDs, rejecting missing, added or
    changed workflow and release-tool files.
    """
    if not re.fullmatch(SHA, candidate) or not re.fullmatch(SHA, workflow):
        raise ValueError("Infrastructure comparison requires full SHAs")
    git(root, "merge-base", "--is-ancestor", candidate, workflow)

    def files(ref):
        return {
            p
            for p in text(root, "ls-tree", "-r", "--name-only", ref).splitlines()
            if (
                p.startswith(".github/workflows/")
                or p in STYLE_INFRASTRUCTURE
                or p in {"tools/ci.py", "tools/check_docs.py", "requirements-ci.txt"}
                or p.startswith("tools/release")
                and p.endswith(".py")
            )
        }

    paths = files(candidate)
    if paths != files(workflow):
        raise ValueError("Release infrastructure file set changed")
    result = {}
    for path in sorted(paths):
        a = text(root, "rev-parse", candidate + ":" + path)
        if a != text(root, "rev-parse", workflow + ":" + path):
            raise ValueError("Release infrastructure changed: " + path)
        result[path] = a
    if (
        not {
            "tools/release.py",
            ".github/workflows/test.yml",
            ".github/workflows/release.yml",
        }
        <= paths
    ):
        raise ValueError("Release infrastructure missing")
    return result


def tagged_yaml(name, original, ref):
    """Render a flat release YAML asset with the requested GitHub source ref.

    Require known source anchors before replacing them and remove checkout-only
    comments and package-directory prefixes.
    """
    source = f"github://{REPOSITORY}@{ref}"
    if name in ("stamp-c3.yaml", "esp32.yaml"):
        pattern = (
            r"  - <<:\n      source:\n        type: local\n        path: ../../components[^\n]*\n"
            r"(?:    #[^\n]*\n)*"
        )
        original, count = re.subn(pattern, "  - source: " + source + "\n", original)
        if count != 1:
            raise ValueError("Expected documented component source in " + name)
    elif name == "existing-device.yaml":
        pattern = r"source: github://" + re.escape(REPOSITORY) + r"@[A-Za-z0-9._/-]+"
        original, count = re.subn(pattern, "source: " + source, original)
        if count != 1:
            raise ValueError("Expected existing-device source")
    elif name in ("firmware-export.yaml", "firmware-update.yaml"):
        old = "  - source:\n      type: local\n      path: ${firmware_components_path}"
        if original.count(old) != 1:
            raise ValueError("Expected maintenance source in " + name)
        original = original.replace(old, "  - source: " + source)
    # Assets are flat GitHub release attachments, kept together after download.
    original = original.replace("!include packages/", "!include ")
    # Keep executable YAML and meaningful comments; remove checkout-only advice.
    lines = [line for line in original.splitlines() if not line.startswith("#")]
    return (
        "# Release v"
        + ref.removeprefix("v")
        + ". Keep downloaded YAML files together.\n"
        + "\n".join(lines).lstrip()
        + "\n"
    ).encode()


def yaml_bytes(root, version, commit):
    """Render only source configurations for the independent YAML build checks."""
    root = validate(root, version, commit)
    return {
        name: tagged_yaml(name, (root / path).read_text(), "v" + version)
        for name, path in YAMLS.items()
    }


def package_bytes(root, version, commit, binary_build, *, existing_package=False):
    """Return deterministic release assets for a validated candidate.

    Generate tagged YAML, release notes, a manifest and checksums without
    creating tags or writing the package to disk.
    """
    root = validate(root, version, commit)
    tag = "v" + version
    data = yaml_bytes(root, version, commit)
    binary, build_report = release_binary.read_build(
        binary_build, version, commit, esphome_pin(root), exact=not existing_package
    )
    for path, expected in build_report["configuration"].items():
        if digest((root / path).read_bytes()) != expected:
            raise ValueError("Binary configuration differs from source: " + path)
    data[release_binary.binary_name(version)] = binary
    data[release_binary.REPORT] = json_bytes(build_report)
    data["firmware-catalog.json"] = (
        root / "metadata/firmware-catalog.json"
    ).read_bytes()
    changelog = (root / "CHANGELOG.md").read_text()
    match = re.search(
        r"^## " + re.escape(version) + r"\n(.*?)(?=^## |\Z)", changelog, re.M | re.S
    )
    if not match:
        raise ValueError("Candidate changelog section missing")
    notes = re.sub(
        r"\]\((docs/[^)]+)\)",
        lambda m: (
            "](" + f"https://github.com/{REPOSITORY}/blob/{tag}/" + m.group(1) + ")"
        ),
        match.group(1).rstrip(),
    )
    data["release-notes.md"] = ("# " + version + "\n" + notes + "\n").encode()
    data["INSTALL.md"] = (
        f"# Install {tag}\n\n"
        "Download the YAML files into one directory and create your own secrets.yaml.\n"
        "Use stamp-c3.yaml or esp32.yaml for a new device. Merge existing-device.yaml\n"
        "into an existing configuration while keeping its network and OTA settings.\n"
        "Adjust UART pins for your board before compiling.\n\n"
        "The maintenance variants are separate builds. Read the wiring, motor\n"
        "isolation, backup and recovery instructions before using them:\n\n"
        f"https://github.com/{REPOSITORY}/blob/{tag}/docs/firmware-update.md\n\n"
        f"The USB image {release_binary.binary_name(version)} is for M5Stack\n"
        "Stamp-C3 C056-B with 4 MiB flash. Install it with ESPHome Web, configure\n"
        "Wi-Fi over USB, then hold G3 to read the device credentials in USB logs.\n"
        "Read the complete setup and erasing-update instructions before use:\n\n"
        f"https://github.com/{REPOSITORY}/blob/{tag}/docs/binary-install.md\n\n"
        "No manufacturer controller firmware image is included.\n"
    ).encode()
    manifest = {
        "schema_version": 2,
        "version": version,
        "tag": tag,
        "commit": commit,
        "esphome": esphome_pin(root),
        "binary": build_report["binary"],
        "catalog_sha256": digest(data["firmware-catalog.json"]),
        "controller_firmware": current_identity(
            json.loads(data["firmware-catalog.json"])
        ),
        "assets": {name: digest(content) for name, content in sorted(data.items())},
    }
    data["release-manifest.json"] = json_bytes(manifest)
    data["SHA256SUMS"] = "".join(
        f"{digest(content)}  {name}\n" for name, content in sorted(data.items())
    ).encode()
    if set(data) != asset_names(version):
        raise ValueError("Unexpected package contents")
    return data


def esphome_pin(root):
    """Read the exact ESPHome version requirement, rejecting a missing version pin."""
    match = re.search(
        r"^esphome==([^\s]+)$", (Path(root) / "requirements-dev.txt").read_text(), re.M
    )
    if not match:
        raise ValueError("ESPHome version pin missing")
    return match.group(1)


def verify_package(directory, version, commit):
    """Verify regular release assets, identity and checksums and return the manifest."""
    directory = Path(directory)
    assets = asset_names(version)
    if {p.name for p in directory.iterdir()} != assets:
        raise ValueError("Unexpected or missing release asset")
    if any(p.is_symlink() or not p.is_file() for p in directory.iterdir()):
        raise ValueError("Assets must be regular files")
    manifest = json.loads((directory / "release-manifest.json").read_text())
    if manifest.get("schema_version") != 2 or (
        manifest.get("version"),
        manifest.get("tag"),
        manifest.get("commit"),
    ) != (
        version,
        "v" + version,
        commit,
    ):
        raise ValueError("Package identity mismatch")
    if set(manifest["assets"]) != assets - {"release-manifest.json", "SHA256SUMS"}:
        raise ValueError("Manifest asset allowlist differs")
    for name, expected in manifest["assets"].items():
        if digest((directory / name).read_bytes()) != expected:
            raise ValueError("Asset digest mismatch: " + name)
    catalog_data = (directory / "firmware-catalog.json").read_bytes()
    if manifest.get("controller_firmware") != current_identity(
        json.loads(catalog_data)
    ):
        raise ValueError("Controller firmware identity differs from the catalog")
    if manifest.get("catalog_sha256") != digest(catalog_data):
        raise ValueError("Catalog digest differs from the release manifest")
    binary, report = release_binary.read_build(
        directory, version, commit, manifest["esphome"], exact=False
    )
    if manifest.get("binary") != report["binary"]:
        raise ValueError("Binary identity differs from the release manifest")
    expected = "".join(
        f"{digest((directory / name).read_bytes())}  {name}\n"
        for name in sorted(assets - {"SHA256SUMS"})
    )
    if (directory / "SHA256SUMS").read_text() != expected:
        raise ValueError("SHA256SUMS differs")
    return manifest


def main():
    """Validate release inputs or write a package into a new output directory.

    Dispatch local tag, package and infrastructure checks without publishing
    a release or creating a tag.
    """
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "command",
        choices=[
            "validate",
            "package",
            "verify-tag",
            "verify-package",
            "infrastructure",
        ],
    )
    p.add_argument("--source-root", type=Path, required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--commit", required=True)
    p.add_argument("--output", type=Path)
    p.add_argument("--workflow-commit")
    p.add_argument("--binary-build", type=Path)
    args = p.parse_args()
    if args.command == "validate":
        validate(args.source_root, args.version, args.commit)
    elif args.command == "verify-tag":
        verify_tag(args.source_root, args.version, args.commit)
    elif args.command == "infrastructure":
        print(
            json.dumps(
                infrastructure(args.source_root, args.commit, args.workflow_commit)
            )
        )
    elif args.command == "verify-package":
        verify_package(args.output, args.version, args.commit)
    else:
        if not args.binary_build:
            raise ValueError("Package requires an explicit --binary-build directory")
        data = package_bytes(
            args.source_root, args.version, args.commit, args.binary_build
        )
        if not args.output or args.output.exists():
            raise ValueError("Package output must be a new directory")
        args.output.mkdir(parents=True)
        for name, content in data.items():
            (args.output / name).write_bytes(content)
        verify_package(args.output, args.version, args.commit)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
