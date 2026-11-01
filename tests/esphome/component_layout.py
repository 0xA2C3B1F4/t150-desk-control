#!/usr/bin/env python3
"""Build isolated component copies without the repository's portable-library tree."""
import hashlib
import json
import shutil
import subprocess
import sys

from desk_test_support import ROOT, parse_args, test_env


def main():
    work = parse_args(__doc__).work_dir
    source = work / "source"
    shutil.copytree(ROOT / "components", source / "components",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".DS_Store"))
    assert not (source / "lib").exists()
    assert not (source / ".git").exists()
    headers = sorted((ROOT / "lib/desk_protocol/include/desk_protocol").glob("*.h"))
    results = []
    for name in ("host", "esp32-no-web", "web", "reader", "updater"):
        case = work / name
        case.mkdir()
        is_host = name == "host"
        has_web = name in ("web", "reader", "updater")
        maintenance = name in ("reader", "updater")
        components = ["desk_uart"]
        if maintenance:
            components += ["desk_firmware_export", "desk_firmware_update"]
        platform = ("host:\n" if is_host else
                    "esp32:\n  board: esp32dev\n  framework:\n    type: esp-idf\n")
        uart = "  port: /dev/null\n" if is_host else "  tx_pin: GPIO17\n  rx_pin: GPIO16\n"
        config = (
            f"esphome:\n  name: layout-{name}\n  build_path: {case}/build\n"
            + platform
            + f"external_components:\n  - source:\n      type: local\n"
              f"      path: {source}/components\n    components: {components}\n"
              "logger:\n  baud_rate: 0\n"
              "uart:\n  id: serial\n  baud_rate: 115200\n"
            + uart
            + "desk_uart:\n  id: desk\n  uart_id: serial\n  height:\n    name: Height\n"
              "  child_lock:\n    name: Panel lock\n"
              "  motion_speed:\n    name: Motion speed\n"
              "  control_status:\n    name: Control status\n"
        )
        if has_web:
            config += (
                'wifi:\n  ssid: "isolated-test-network"\n  password: "dummy-password"\n'
                'web_server:\n  version: 3\n  local: true\n'
                '  auth:\n    username: test\n    password: dummy-password\n'
            )
        if maintenance:
            domain = "desk_firmware_export" if name == "reader" else "desk_firmware_update"
            config += f"{domain}:\n  desk_id: desk\n"
        yaml = case / "test.yaml"
        yaml.write_text(config)
        env = dict(test_env(work), ESPHOME_DATA_DIR=str(case / "data"),
                   PLATFORMIO_BUILD_DIR=str(case / "build/.pioenvs"),
                   PLATFORMIO_SETTING_ENABLE_TELEMETRY="No")
        with (case / "generate.log").open("w") as log:
            subprocess.run([sys.executable, "-m", "esphome", "compile", str(yaml),
                            "--only-generate"], env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        generated = case / "build/src/esphome/components/desk_uart"
        component_files = {p.name for p in (source / "components/desk_uart").iterdir()
                           if p.suffix in (".h", ".cpp")}
        assert component_files == {p.name for p in generated.iterdir() if p.suffix in (".h", ".cpp")}
        for filename in component_files:
            assert (generated / filename).read_bytes() == (source / "components/desk_uart" / filename).read_bytes()
        assert not (case / "build/src/desk_protocol").exists()
        if has_web:
            manifest = json.loads((case / "build/desk-patches.json").read_text())
            assert manifest["controls"]
            assert manifest["maintenance"] == (name == "updater")
            assert not manifest["ota"]
        assert ("t150_web_patch_guard();" in (case / "build/src/main.cpp").read_text()) == has_web
        if maintenance:
            assert (case / "build/src/esphome/components/desk_firmware_export/firmware_profile.h").is_file()
            assert (generated / "firmware_profile.h").is_file()
            combined = (case / "build/src/esphome.h").read_text()
            for domain in ("desk_uart", "desk_firmware_export"):
                assert f'#include "esphome/components/{domain}/firmware_profile.h"' in combined
        compile_case = name in ("host", "esp32-no-web", "updater")
        if compile_case:
            with (case / "compile.log").open("w") as log:
                subprocess.run([sys.executable, "-m", "esphome", "compile", str(yaml)],
                               env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        results.append({"case": name, "generated": True, "compiled": compile_case,
                        "web_hook": has_web, "canonical_headers": len(headers)})
        print(f"{name}: passed", flush=True)
    summary = {"passed": True, "source": "isolated components-only copy",
               "cases": results, "uploaded": False,
               "canonical_sha256": {p.name: hashlib.sha256((source / "components/desk_uart" / p.name).read_bytes()).hexdigest() for p in headers}}
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
