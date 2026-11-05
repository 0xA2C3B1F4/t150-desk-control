#!/usr/bin/env python3
"""Validate and build the published GitHub YAMLs in an isolated user directory.

Only the chosen YAML and dummy secrets are copied. Packages and components must
resolve to the same candidate commit. No firmware is uploaded.
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys

import yaml

from desk_test_support import ROOT, parse_args, test_env

EXAMPLES = (
    "stamp-c3",
    "esp32",
    "desk",
    "stamp-c3-firmware-export",
    "stamp-c3-firmware-update",
)
REPOSITORY = "0xA2C3B1F4/t150-desk-control"


def main():
    """Validate and optionally compile an example; GitHub mode checks its clones."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--example", choices=EXAMPLES, default="stamp-c3")
    parser.add_argument(
        "--maintenance",
        action="store_true",
        help="Alias for the Stamp-C3 maintenance example",
    )
    parser.add_argument("--source-mode", choices=("github", "local"), default="github")
    parser.add_argument(
        "--ref", help="Published ref used by both packages and components"
    )
    parser.add_argument("--expected-commit", help="Expected peeled commit for --ref")
    parser.add_argument("--device-name", help="Test an existing device name override")
    parser.add_argument(
        "--check-only", action="store_true", help="Validate without compiling"
    )
    args = parse_args(parser=parser)
    if args.maintenance:
        assert args.example == "stamp-c3"
        args.example = "stamp-c3-firmware-update"
    work = args.work_dir
    env = test_env(work)
    env["GIT_TERMINAL_PROMPT"] = "0"
    assert not (work / "data").exists(), (
        "Use a fresh work directory to verify GitHub fetches"
    )
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    expected = args.expected_commit or revision
    assert re.fullmatch(r"[0-9a-f]{40}", expected) and expected == revision
    assert not args.ref or args.source_mode == "github"
    source_ref = args.ref or revision
    assert re.fullmatch(r"[A-Za-z0-9._/-]+", source_ref)
    source = ROOT / f"examples/esphome/github/{args.example}.yaml"
    original = source.read_text()
    assert "!include" not in original and "type: local" not in original
    wrapper = yaml.safe_load(original)
    assert isinstance(wrapper["external_components"], dict), (
        "A list would retain the packages' local sources"
    )
    assert wrapper["packages"] == {
        "desk": f"github://{REPOSITORY}/examples/esphome/{args.example}.yaml@${{t150_ref}}"
    }
    assert (
        wrapper["external_components"]["source"]
        == f"github://{REPOSITORY}@${{t150_ref}}"
    )
    required = ["desk_uart"]
    if "firmware" in args.example:
        required.append("desk_firmware_export")
    if args.example.endswith("update"):
        required.append("desk_firmware_update")
    assert wrapper["external_components"]["components"] == required
    text, count = re.subn(
        r"^  t150_ref: .*", f"  t150_ref: {source_ref}", original, flags=re.M
    )
    assert count == 1
    text, count = re.subn(
        r"^  build_directory: .*", f"  build_directory: {work}/build", text, flags=re.M
    )
    assert count == 1, "Expected the isolated build-directory substitution"
    if args.device_name:
        assert re.fullmatch(r"[a-z0-9-]+", args.device_name)
        text, count = re.subn(
            r"^  device_name: .*",
            f"  device_name: {args.device_name}",
            text,
            flags=re.M,
        )
        assert count == 1, "Expected the existing device-name substitution"
    if args.source_mode == "local":
        assert text.count(wrapper["packages"]["desk"]) == 1, (
            "Expected the GitHub package source"
        )
        assert text.count(wrapper["external_components"]["source"]) == 1, (
            "Expected the GitHub component source"
        )
        text = text.replace(
            wrapper["packages"]["desk"],
            f"!include {ROOT}/examples/esphome/{args.example}.yaml",
        )
        text = text.replace(
            wrapper["external_components"]["source"],
            f"\n    type: local\n    path: {ROOT}/components",
        )
    cfg = work / f"{args.example}.yaml"
    cfg.write_text(text)
    (work / "secrets.yaml").write_text(
        'wifi_ssid: "isolated-test-network"\n'
        'wifi_password: "dummy-not-a-device-password"\n'
        'api_encryption_key: "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="\n'
        'ota_password: "dummy-not-a-device-password"\n'
        'web_username: "desk-test"\n'
        'web_password: "dummy-not-a-device-password"\n'
    )
    assert {p.name for p in work.iterdir()} == {cfg.name, "secrets.yaml"}
    result = subprocess.run(
        [sys.executable, "-m", "esphome", "config", str(cfg), "--show-secrets"],
        cwd=work,
        env=env,
        capture_output=True,
        text=True,
    )
    (work / "config.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    config = yaml.load(result.stdout, Loader=yaml.BaseLoader)
    external = config["external_components"]
    assert len(external) == 1 and external[0]["components"] == required
    assert config["esphome"]["name"] == (
        args.device_name or wrapper["substitutions"]["device_name"]
    )
    assert config["wifi"]["networks"][0]["ssid"] == "isolated-test-network"
    assert config["wifi"]["networks"][0]["password"] == "dummy-not-a-device-password"
    assert (
        config["api"]["encryption"]["key"]
        == "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="
    )
    assert config["web_server"]["auth"] == {
        "username": "desk-test",
        "password": "dummy-not-a-device-password",
    }
    assert any(ota["platform"] == "esphome" for ota in config["ota"])
    if "firmware" in args.example:
        assert int(config["uart"][0]["rx_buffer_size"]) == 4096
    if args.example.endswith("update"):
        assert (
            "panel_lock" in config["desk_uart"][0]
            and "motion_speed" in config["desk_uart"][0]
        )
        assert "desk_firmware_update" in config
    elif args.example.endswith("export"):
        assert "desk_firmware_export" in config
    clones = {}
    resolved = {}
    if args.source_mode == "github":
        assert external[0]["source"]["type"] == "git"
        assert external[0]["source"]["ref"] == source_ref
        for domain in ("packages", "external_components"):
            matches = list((work / "data" / domain).glob("*/.git"))
            assert len(matches) == 1, (domain, matches)
            clone = matches[0].parent
            commit = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=clone, text=True
            ).strip()
            assert commit == expected, (domain, commit, expected)
            remote = subprocess.check_output(
                ["git", "remote", "get-url", "origin"], cwd=clone, text=True
            ).strip()
            assert remote.removesuffix(".git") == "https://github.com/" + REPOSITORY
            clones[domain] = clone
            resolved[domain] = commit
    summary = {
        "passed": True,
        "example": source.name,
        "source_mode": args.source_mode,
        "ref": source_ref,
        "expected_commit": expected,
        "resolved_commits": resolved,
        "example_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "components": required,
        "device_name": config["esphome"]["name"],
        "compiled": not args.check_only,
        "uploaded": False,
    }
    if not args.check_only:
        with (work / "compile.log").open("w") as log:
            subprocess.run(
                [sys.executable, "-m", "esphome", "compile", str(cfg)],
                cwd=work,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
            )
        clone = clones.get("external_components", ROOT)
        checks = []
        for domain in required:
            originals = clone / "components" / domain
            generated = work / "build/src/esphome/components" / domain
            names = {p.name for p in originals.iterdir() if p.suffix in (".h", ".cpp")}
            assert names == {
                p.name for p in generated.iterdir() if p.suffix in (".h", ".cpp")
            }
            for name in sorted(names):
                original_file = originals / name
                data = original_file.read_bytes()
                assert data == (ROOT / original_file.relative_to(clone)).read_bytes()
                assert data == (generated / name).read_bytes()
                checks.append(
                    {
                        "file": str(original_file.relative_to(clone)),
                        "sha256": hashlib.sha256(data).hexdigest(),
                    }
                )
        assert not (work / "build/src/desk_protocol").exists()
        defines = (work / "build/src/esphome/core/defines.h").read_text()
        if args.source_mode == "github":
            assert f'#define DESK_SOURCE_REVISION "{expected}"' in defines
            assert '#define DESK_SOURCE_STATE "clean"' in defines
        for flag in (
            "USE_WEBSERVER_LOCAL",
            "USE_WEBSERVER_AUTH",
            "USE_WEBSERVER_VERSION 3",
            "USE_OTA",
        ):
            assert f"#define {flag}\n" in defines, flag
        assert "#define USE_WEBSERVER_OTA" not in defines
        if "firmware" in args.example:
            ota = (
                work / "build/src/esphome/components/esphome/ota/ota_esphome.cpp"
            ).read_text()
            assert "desk_firmware_export::OTAOperation desk_ota;" in ota
            assert (
                "t150_ota_patch_guard();" in (work / "build/src/main.cpp").read_text()
            )
        firmware = work / "build/build/firmware.ota.bin"
        assert firmware.is_file()
        summary.update(
            firmware_bytes=firmware.stat().st_size, matching_compiled_files=checks
        )
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
