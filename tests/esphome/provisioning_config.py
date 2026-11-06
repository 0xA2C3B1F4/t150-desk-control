#!/usr/bin/env python3
"""Check the fixed distribution contract and preserve existing Stamp controls."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import esphome
import yaml

from desk_test_support import ROOT, parse_args, test_env


class ConfigLoader(yaml.SafeLoader):
    """Read normalized configuration while retaining unresolved secret names."""


ConfigLoader.add_constructor(
    "!secret", lambda loader, node: loader.construct_scalar(node)
)


def main() -> None:
    """Check rejected configurations, generated service wiring and unchanged controls."""
    subprocess.run(
        [sys.executable, str(ROOT / "tests/esphome/generated_patches_test.py")],
        check=True,
    )
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    pinned = json.loads((ROOT / "tests/fixtures/provisioning-esphome.json").read_text())
    package = Path(esphome.__file__).parent
    for name, digest in pinned["files"].items():
        assert hashlib.sha256((package / name).read_bytes()).hexdigest() == digest, name
    flag_files = sorted(
        str(p.relative_to(package))
        for p in package.rglob("*")
        if p.suffix in {".cpp", ".h", ".py"}
        and any(
            flag in p.read_text(errors="replace")
            for flag in ["USE_API_NOISE_PSK_FROM_YAML", "USE_API_PLAINTEXT"]
        )
    )
    assert flag_files == pinned["flag_files"], "API flag use changed"
    source = (
        (ROOT / "distribution/stamp-c3.yaml")
        .read_text()
        .replace(
            "../examples/esphome/packages/",
            str(ROOT / "examples/esphome/packages") + "/",
        )
        .replace("path: ../components", "path: " + str(ROOT / "components"))
    )
    cases = {
        "good": source,
        "improv-uart": source.replace(
            "improv_serial:", "improv_serial:\n  uart_id: desk_serial"
        ),
        "improv-url": source.replace(
            "improv_serial:", "improv_serial:\n  next_url: https://example.invalid/"
        ),
        "outgoing": source.replace(
            "  id: native_api", "  id: native_api\n  outgoing_connection: {}"
        ),
        "wizard": source.replace(
            "  id: native_api",
            "  id: native_api\n  wizard:\n    pages:\n      - entities:\n          - id: desk_height",
        ).replace("desk_uart:\n", "desk_uart:\n  height:\n    id: desk_height\n"),
        "provisioning": source + "\nprovisioning:\n  timeout: 5min\n",
        "build-flags": source.replace(
            "  name: t150-stamp-c3",
            "  build_flags: [-DUNREVIEWED]\n  name: t150-stamp-c3",
        ),
        "compression": source.replace(
            "  log: false", "  log: false\n  compression: br"
        ),
        "api-key": source.replace(
            "encryption: {}",
            "encryption:\n    key: AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=",
        ),
        "web-password": source.replace(
            "  log: false",
            "  log: false\n  auth:\n    username: test\n    password: invalid-shared-password",
        ),
        "ota": source + "\nota:\n  - platform: esphome\n",
        "fallback-ap": source.replace("wifi:\n", "wifi:\n  ap:\n    ssid: test-ap\n"),
        "plaintext": source.replace("  encryption: {}\n", ""),
        "uart": source.replace("uart_tx_pin: GPIO4", "uart_tx_pin: GPIO6"),
        "pin": source.replace(
            "  desk_id: desk", "  desk_id: desk\n  button_pin: GPIO9"
        ),
        "priority": source.replace(
            "  desk_id: desk", "  desk_id: desk\n  setup_priority: 100"
        ),
        "zero-priority": source.replace(
            "  desk_id: desk", "  desk_id: desk\n  setup_priority: 0"
        ),
        "api-priority": source.replace(
            "  id: native_api", "  id: native_api\n  setup_priority: 900"
        ),
        "automation": source.replace(
            "  name: t150-stamp-c3",
            "  on_boot:\n    then:\n      - logger.log: unexpected\n  name: t150-stamp-c3",
        ),
        "mqtt": source + "\nmqtt:\n  broker: example.invalid\n",
        "reboot": source.replace("reboot_timeout: 0s", "reboot_timeout: 15min"),
        "entity": source + "\nbutton:\n  - platform: restart\n    name: Restart\n",
        "flash": source.replace("flash_size: 4MB", "flash_size: 8MB"),
        "uart-debug": source + "\nuart:\n  debug: {}\n",
        "web-script": source.replace(
            "  log: false", "  log: false\n  js_url: https://example.invalid/test.js"
        ),
        "web-internal": source.replace(
            "  log: false", "  log: false\n  include_internal: true"
        ),
        "idf-options": source.replace(
            "    type: esp-idf",
            "    type: esp-idf\n    advanced:\n      loop_task_stack_size: 12288",
        ),
        "external-all": source.replace(
            "components: [desk_uart, desk_provisioning]", "components: all"
        ),
        "external-split": source.replace(
            "components: [desk_uart, desk_provisioning]",
            "components: [desk_provisioning]\n  - source:\n      type: local\n      path: "
            + str(ROOT / "components")
            + "\n    components: [desk_uart]",
        ),
        # These two cases exercise ESPHome's schema/loader before our final guard.
        "external-override": source.replace(
            "components: [desk_uart, desk_provisioning]",
            "components: [desk_uart, desk_provisioning, api]",
        ),
        "lambda-value": source.replace(
            "name: Source version", 'name: !lambda return "unsafe";'
        ),
        "version": source.replace('version: "2026.11.0"', 'version: "2026.11.1"'),
    }
    expected_errors = {
        "improv-uart": "used both by improv_serial and desk_uart",
        "improv-url": "unreviewed improv_serial",
        "outgoing": "API wizard and outgoing",
        "wizard": "API wizard and outgoing",
        "provisioning": "unexpected component(s)",
        "build-flags": "custom code/automation/priority",
        "compression": "requires gzip web assets",
        "api-key": "API encryption is owned",
        "web-password": "web auth is owned",
        "ota": "unexpected component(s)",
        "fallback-ap": "Wi-Fi is configured only",
        "plaintext": "API encryption is owned",
        "uart": "requires desk UART",
        "pin": "G3 user button",
        "priority": "custom code/automation/priority",
        "zero-priority": "custom code/automation/priority",
        "api-priority": "custom code/automation/priority",
        "automation": "custom code/automation/priority",
        "mqtt": "unexpected component(s)",
        "reboot": "network reboot timeouts",
        "entity": "separate entity platforms",
        "flash": "requires 4MB flash",
        "version": "project version must match",
        "uart-debug": "custom code/automation/priority",
        "web-script": "reviewed local web assets",
        "web-internal": "reviewed local web assets",
        "idf-options": "reviewed ESP-IDF defaults",
        "external-all": "local distribution components only",
        "external-split": "local distribution components only",
        "external-override": "Could not find __init__.py file for component api",
        "lambda-value": "This option is not templatable",
    }
    failures = []
    for name, text in cases.items():
        cfg = work / (name + ".yaml")
        cfg.write_text(text)
        args = [
            sys.executable,
            "-m",
            "esphome",
            "-s",
            "build_directory",
            str(work / "build"),
            "config",
            str(cfg),
        ]
        result = subprocess.run(args, env=env, capture_output=True, text=True)
        (work / (name + ".log")).write_text(result.stdout + result.stderr)
        assert (result.returncode == 0) == (name == "good"), (
            name + ": " + result.stdout[-2000:] + result.stderr[-1000:]
        )
        if name != "good":
            if expected_errors[name] not in result.stdout + result.stderr:
                failures.append(name + ": " + result.stdout + result.stderr)
        if name == "good":
            dist = yaml.load(result.stdout, Loader=ConfigLoader)
    assert not failures, "\n".join(failures)
    override = subprocess.run(
        [
            sys.executable,
            "-m",
            "esphome",
            "--toolchain",
            "platformio",
            "config",
            str(work / "good.yaml"),
        ],
        env=env,
        capture_output=True,
        text=True,
    )
    assert (
        override.returncode != 0
        and "requires native ESP-IDF toolchain" in override.stdout + override.stderr
    )

    # Compare normalized configs against the pre-refactor Stamp example at the approved main baseline.
    baseline_path = ROOT / "tests/fixtures/stamp-c3-before-distribution.yaml"
    assert (
        hashlib.sha256(baseline_path.read_bytes()).hexdigest()
        == pinned["baseline"]["sha256"]
    )
    baseline = baseline_path.read_text()
    current = (
        (ROOT / "examples/esphome/stamp-c3.yaml")
        .read_text()
        .replace(
            "!include packages/",
            "!include " + str(ROOT / "examples/esphome/packages") + "/",
        )
    )
    (work / "secrets.yaml").write_text(
        "wifi_ssid: offline-test\nwifi_password: dummy-password\napi_encryption_key: AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\nota_password: dummy-password\nweb_username: test\nweb_password: dummy-password\n"
    )
    configs = []
    for name, text in (("baseline", baseline), ("shared", current)):
        text = text.replace(
            "path: ../../components", "path: " + str(ROOT / "components")
        )
        cfg = work / (name + ".yaml")
        cfg.write_text(text)
        result = subprocess.run(
            [sys.executable, "-m", "esphome", "config", str(cfg)],
            env=env,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        configs.append(yaml.load(result.stdout, Loader=ConfigLoader))
    for key in ("uart", "desk_uart"):
        assert configs[0][key] == configs[1][key], key
    assert (
        configs[0]["web_server"]["sorting_groups"]
        == configs[1]["web_server"]["sorting_groups"]
    )
    for key in ("source_version", "source_revision"):
        dist["desk_uart"][0].pop(key)
    assert dist["desk_uart"] == configs[1]["desk_uart"]
    assert dist["uart"] == configs[1]["uart"]
    assert (
        dist["web_server"]["sorting_groups"]
        == configs[1]["web_server"]["sorting_groups"]
    )
    assert configs[1]["ota"], "Normal YAML must retain OTA"
    for key in (
        "panel_lock",
        "motion_speed",
        "display_mode",
        "display_timeout",
        "normal_up_trim",
        "normal_down_trim",
        "slow_up_trim",
        "slow_down_trim",
        "experimental_custom_timer_buzzer",
    ):
        assert key not in dist["desk_uart"][0] or dist["desk_uart"][0][key] is False, (
            key
        )
    # Generate the good image and inspect flags and service setup wiring.
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "esphome",
            "-s",
            "build_directory",
            str(work / "build"),
            "compile",
            str(work / "good.yaml"),
            "--only-generate",
        ],
        env=env,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    defines = (work / "build/src/esphome/core/defines.h").read_text()
    for flag in (
        "USE_API_PLAINTEXT",
        "USE_API_NOISE",
        "USE_API_NOISE_PSK_FROM_YAML",
        "USE_WEBSERVER_AUTH",
        "USE_IMPROV_SERIAL",
    ):
        assert "#define " + flag in defines, flag
    for flag in ("USE_OTA", "USE_CAPTIVE_PORTAL", "USE_WIFI_AP"):
        assert "#define " + flag not in defines, flag
    main = (work / "build/src/main.cpp").read_text()
    assert "->set_noise_psk(" not in main
    assert "->set_auth_password(" not in main
    assert "->set_base(" in main and "->set_api(native_api)" in main
    assert "t150_wifi_patch_guard();" in main
    assert (work / "build/desk_apply_wifi_patch.py").is_file()
    assert (work / "build/desk_upstream_wifi.cpp").is_file()
    # The normal YAML keeps upstream Wi-Fi and its existing OTA path.
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "esphome",
            "-s",
            "build_directory",
            str(work / "shared-build"),
            "compile",
            str(work / "shared.yaml"),
            "--only-generate",
        ],
        env=env,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    normal_build = work / "shared-build"
    assert (
        normal_build / "src/esphome/components/wifi/wifi_component.cpp"
    ).read_bytes() == (package / "components/wifi/wifi_component.cpp").read_bytes()
    assert not (normal_build / "desk_apply_wifi_patch.py").exists()
    assert "t150_wifi_patch_guard" not in (normal_build / "src/main.cpp").read_text()
    assert (
        "#define USE_OTA" in (normal_build / "src/esphome/core/defines.h").read_text()
    )
    (work / "summary.json").write_text(
        json.dumps(
            {
                "configuration_cases": len(cases) + 1,
                "framework_files_checked": len(pinned["files"]),
                "original_controls_preserved": True,
                "generated_flags_checked": True,
                "normal_yaml_ota_and_upstream_wifi_preserved": True,
                "hardware_access": False,
            },
            indent=2,
        )
        + "\n"
    )
    print("Distribution configuration and source contract passed")


if __name__ == "__main__":
    main()
