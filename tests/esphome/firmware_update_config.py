#!/usr/bin/env python3
"""Check updater opt-in and mutual exclusion without accessing a device."""

import json
import os
import shutil
import subprocess
import sys

from desk_test_support import ROOT, parse_args

subprocess.run(
    [sys.executable, str(ROOT / "tests/esphome/firmware_web_layout_test.py")],
    check=True,
)
subprocess.run(
    [sys.executable, str(ROOT / "tests/esphome/child_lock_web_test.py")], check=True
)
subprocess.run(
    [sys.executable, str(ROOT / "tools/embed_firmware_ui.py"), "--check"], check=True
)
subprocess.run(
    [sys.executable, str(ROOT / "tests/esphome/generated_patches_test.py")], check=True
)
args = parse_args()
w = args.work_dir
(w / "packages").mkdir(exist_ok=True)
for name in [
    "firmware-export",
    "firmware-update",
    "child-lock",
    "reset-speed",
    "display",
]:
    shutil.copy2(
        ROOT / f"examples/esphome/packages/{name}.yaml", w / f"packages/{name}.yaml"
    )
base = (
    (ROOT / "examples/esphome/stamp-c3.yaml")
    .read_text()
    .replace("path: ../../components", f"path: {ROOT}/components")
    .replace(
        "build_directory: .esphome/build/t150-desk-control-stamp-c3",
        f"build_directory: {w}/build",
    )
)
(w / "base.yaml").write_text(base)
(w / "secrets.yaml").write_text(
    (ROOT / "examples/esphome/secrets.example.yaml")
    .read_text()
    .replace(
        "REPLACE_WITH_GENERATED_BASE64_KEY",
        "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=",
    )
)
env = dict(os.environ, ESPHOME_DATA_DIR=str(w / "data"), PYTHONDONTWRITEBYTECODE="1")
results = []
for name, packages, valid in [
    ("normal", [], True),
    ("reader", ["firmware-export"], True),
    ("updater", ["firmware-update"], True),
    ("updater_with_lock", ["firmware-update", "child-lock"], True),
    ("panel_lock", ["child-lock"], True),
    ("display", ["display"], True),
    ("updater_with_display", ["firmware-update", "display"], True),
    ("reset_speed", ["child-lock", "reset-speed"], True),
    ("reset_speed_without_lock_entity", ["reset-speed"], True),
    ("updater_with_reset", ["firmware-update", "child-lock", "reset-speed"], True),
    ("both", ["firmware-export", "firmware-update"], False),
    ("no_auth", ["firmware-update"], False),
    ("with_ota", ["firmware-update"], True),
    ("no_ota", ["firmware-update"], True),
    ("web_ota", ["firmware-update"], False),
    ("http_ota", ["firmware-update"], False),
    ("platformio", ["firmware-update"], False),
]:
    yaml = (
        "packages:\n  base: !include base.yaml\n"
        + "".join(
            f"  p{i}: !include packages/{p}.yaml\n" for i, p in enumerate(packages)
        )
        + f"substitutions:\n  firmware_components_path: {ROOT}/components\n"
    )
    if name == "no_auth":
        yaml += "web_server:\n  auth: !remove\n"
    if name == "with_ota":
        yaml += "ota:\n  - platform: esphome\n"
    if name == "no_ota":
        yaml += "ota: !remove\n"
    if name == "web_ota":
        yaml += "ota:\n  - platform: web_server\n"
    if name == "http_ota":
        yaml += "http_request:\nota:\n  - platform: http_request\n"
    path = w / f"{name}.yaml"
    path.write_text(yaml)
    command = [sys.executable, "-m", "esphome"]
    if name == "platformio":
        command += ["--toolchain", "platformio"]
    result = subprocess.run(
        command + ["compile", str(path), "--only-generate"],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (w / f"{name}.log").write_text(result.stdout)
    assert (result.returncode == 0) == valid, (name, result.stdout)
    if valid:
        generated = (w / "build/src/main.cpp").read_text()
        assert ("desk_firmware_update::FirmwareUpdate" in generated) == (
            "firmware-update" in packages
        )
        assert ("set_child_lock_switch" in generated) == ("child-lock" in packages)
        assert ("set_display_mode_select" in generated) == ("display" in packages)
        assert ("set_display_timeout_number" in generated) == ("display" in packages)
        controls = any(p in packages for p in ("child-lock", "reset-speed", "display"))
        patched = controls or "firmware-update" in packages
        assert ("t150_web_patch_guard();" in generated) == patched
        if patched or (name != "no_ota" and "firmware-export" in packages):
            manifest = json.loads((w / "build/desk-patches.json").read_text())
            assert manifest["controls"] == controls
            assert manifest["maintenance"] == ("firmware-update" in packages)
            assert manifest["ota"] == (
                name != "no_ota"
                and any(p in packages for p in ("firmware-update", "firmware-export"))
            )
            assert (
                "t150_build_patches:" in (w / "build/src/idf_component.yml").read_text()
            )
        assert ("t150_ota_patch_guard();" in generated) == (
            name != "no_ota"
            and any(p in packages for p in ("firmware-update", "firmware-export"))
        )
        if "firmware-update" in packages:
            version = json.loads(
                (ROOT / "components/desk_uart/project_version.json").read_text()
            )["version"]
            assert f'set_target_version("{version}")' in generated
            defines = (w / "build/src/esphome/core/defines.h").read_text()
            assert "#define USE_WEBSERVER_GZIP" in defines
            assert "#define USE_WEBSERVER_LOCAL" in defines
    results.append(dict(configuration=name, expected_valid=valid, passed=True))
(w / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results))
