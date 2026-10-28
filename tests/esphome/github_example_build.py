#!/usr/bin/env python3
"""Build the GitHub example in a fresh cache with dummy device credentials.

Fetches the current checkout commit, including a PR merge revision in Actions.
Requires that commit to be available on GitHub. Does not upload firmware.
"""

import argparse
import hashlib
import json
import re
import subprocess

import yaml

from desk_test_support import ROOT, parse_args, test_env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--example", choices=("stamp-c3", "esp32"),
                        default="stamp-c3")
    args = parse_args(parser=parser)
    work = args.work_dir
    env = test_env(work)
    env["GIT_TERMINAL_PROMPT"] = "0"
    assert not (work / "data").exists(), "Use a fresh work directory to verify a real GitHub fetch"
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    assert re.fullmatch(r"[0-9a-f]{40}", revision), revision
    # actions/checkout stores its read-only job credential in Git configuration.
    # Forward it only through the child process environment so ESPHome's fresh
    # clone can access this private repo. Never print or persist the header.
    auth = subprocess.run(
        ["git", "config", "--get-urlmatch", "http.extraheader",
         "https://github.com/0xA2C3B1F4/t150-desk-control.git"],
        cwd=ROOT, text=True, capture_output=True,
    )
    if auth.returncode == 0 and auth.stdout.strip():
        count = int(env.get("GIT_CONFIG_COUNT", "0"))
        env[f"GIT_CONFIG_KEY_{count}"] = "http.https://github.com/0xA2C3B1F4/t150-desk-control.git.extraheader"
        env[f"GIT_CONFIG_VALUE_{count}"] = auth.stdout.strip()
        env["GIT_CONFIG_COUNT"] = str(count + 1)
    source = ROOT / f"examples/esphome/{args.example}.yaml"
    text = source.read_text()
    example_config = yaml.load(text, Loader=yaml.BaseLoader)
    web_version = int(example_config["web_server"]["version"])
    assert web_version in (2, 3), web_version
    github_source = f"github://0xA2C3B1F4/t150-desk-control@{revision}"
    text, replacements = re.subn(
        r"^    # source: github://0xA2C3B1F4/t150-desk-control@[^\s]+$",
        f"    source: {github_source}", text, flags=re.MULTILINE,
    )
    assert replacements == 1, "Expected one documented GitHub override"
    # Activate the documented GitHub override and relocate generated data.
    text = re.sub(r"  build_directory: .*", f"  build_directory: {work}/build", text)
    cfg = work / f"{args.example}.yaml"
    cfg.write_text(text)
    (work / "secrets.yaml").write_text(
        'wifi_ssid: "isolated-test-network"\n'
        'wifi_password: "dummy-not-a-device-password"\n'
        'api_encryption_key: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="\n'
        'ota_password: "dummy-not-a-device-password"\n'
        'web_username: "desk-test"\n'
        'web_password: "dummy-not-a-device-password"\n'
    )
    for action in ("config", "compile"):
        with (work / f"{action}.log").open("w") as log:
            subprocess.run(["esphome", action, str(cfg)], cwd=work, env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    clones = list((work / "data/external_components").glob("*/.git"))
    assert len(clones) == 1, clones
    clone = clones[0].parent
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=clone, text=True).strip()
    assert commit == revision, f"Fetched {commit}, expected checkout {revision}"
    remote = subprocess.check_output(["git", "remote", "get-url", "origin"], cwd=clone, text=True).strip()
    assert remote in ("https://github.com/0xA2C3B1F4/t150-desk-control",
                      "https://github.com/0xA2C3B1F4/t150-desk-control.git")
    checks = []
    for source_dir, generated_dir, names in (
        (clone / "lib/desk_protocol/include/desk_protocol", work / "build/src/desk_protocol",
         sorted(p.name for p in (ROOT / "lib/desk_protocol/include/desk_protocol").glob("*.h"))),
        (clone / "components/desk_uart", work / "build/src/esphome/components/desk_uart",
         ["desk_uart.cpp", "desk_uart.h"]),
    ):
        for name in names:
            original, generated = source_dir / name, generated_dir / name
            checkout = ROOT / original.relative_to(clone)
            assert checkout.read_bytes() == original.read_bytes() == generated.read_bytes(), name
            checks.append({"file": str(original.relative_to(clone)),
                           "sha256": hashlib.sha256(generated.read_bytes()).hexdigest()})
    device_name = re.search(r"^  device_name: (.+)$", text, re.MULTILINE).group(1)
    firmware = work / f"build/.pioenvs/{device_name}/firmware.ota.bin"
    assert firmware.is_file()
    defines = (work / "build/src/esphome/core/defines.h").read_text()
    for flag in ("USE_WEBSERVER_LOCAL", "USE_WEBSERVER_AUTH", f"USE_WEBSERVER_VERSION {web_version}"):
        assert f"#define {flag}\n" in defines, flag
    assert "#define USE_WEBSERVER_OTA" not in defines
    summary = {"passed": True, "source": github_source, "checkout_commit": revision,
               "resolved_commit": commit, "example": source.name, "example_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
               "credentials": "dummy device values; Git authentication inherited without credential output",
               "matching_compiled_files": checks, "firmware_bytes": firmware.stat().st_size,
               "web_server": {"version": web_version, "local_assets": True, "authentication": True,
                              "browser_tested": False},
               "uploaded": False}
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
