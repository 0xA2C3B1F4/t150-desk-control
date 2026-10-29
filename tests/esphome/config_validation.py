#!/usr/bin/env python3
"""Validate defaults, opt-in and UART constraints using installed ESPHome."""

import json
import subprocess

import yaml

from desk_test_support import ROOT, parse_args, test_env

work = parse_args(__doc__).work_dir
env = test_env(work)
base = f"""esphome:
  name: desk-config-test
  build_path: {work}/build
esp32:
  board: esp32dev
  framework:
    type: esp-idf
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
logger:
  baud_rate: 0
uart:
  id: serial
  baud_rate: 115200
  tx_pin: GPIO17
  rx_pin: GPIO16
desk_uart:
  uart_id: serial
  height:
    name: Height
"""
cases = [
    (
        "recovery_without_motion",
        base
        + "  restore_preset_3:\n    name: Restore preset\n  accept_current_preset_3:\n    name: Accept current preset\n  preset_restore_pending:\n    name: Backup pending\n",
        True,
    ),
    ("keep_awake_requires_backup", base + "  keep_awake: true\n", False),
    ("keep_awake_opt_in", base + "  keep_awake: true\n  experimental_target_height: true\n", True),
    ("default_monitoring", base, True),
    ("raw_load_monitoring", base + "  load_raw:\n    name: Raw load\n", True),
    ("raw_load_inch_monitoring", base + "  display_unit: in\n  load_raw:\n    name: Raw load\n", True),
    ("inch_monitoring", base + "  display_unit: in\n", True),
    ("invalid_display_unit", base + "  display_unit: metres\n", False),
    (
        "conflicting_sensor_unit",
        base.replace("    name: Height", "    name: Height\n    unit_of_measurement: cm") + "  display_unit: in\n",
        False,
    ),
    (
        "inch_controls",
        base + "  display_unit: in\n  experimental_target_height: true\n  target_height:\n    name: Target\n  preset_1_height:\n    name: Preset\n",
        True,
    ),
    (
        "conflicting_number_unit",
        base + "  display_unit: in\n  preset_1_height:\n    name: Preset\n    unit_of_measurement: cm\n",
        False,
    ),
    (
        "inch_range_too_narrow",
        base + "  display_unit: in\n  min_height: 80\n  max_height: 80.1\n",
        False,
    ),
    (
        "preset_height_fields",
        base
        + "  preset_1_height:\n    name: Preset 1 height\n    mode: box\n  preset_2_height:\n    name: Preset 2 height\n    mode: box\n  preset_3_height:\n    name: Preset 3 height\n    mode: box\n",
        True,
    ),
    ("preset_without_opt_in", base + "  preset_3:\n    name: Preset\n", False),
    ("preset_opt_in", base + "  experimental_preset_buttons: true\n  preset_3:\n    name: Preset\n", True),
    ("stop_without_motion_opt_in", base + "  stop:\n    name: Stop\n", True),
    ("timer_without_opt_in", base + "  cancel_timer:\n    name: Cancel timer\n", False),
    (
        "timer_opt_in",
        base
        + "  experimental_timer_buttons: true\n  timer_30min:\n    name: Timer 30 min\n  timer_60min:\n    name: Timer 60 min\n  timer_90min:\n    name: Timer 90 min\n  timer_120min:\n    name: Timer 120 min\n  cancel_timer:\n    name: Cancel timer\n  timer_command_pending:\n    name: Timer command pending\n",
        True,
    ),
    (
        "load_limits",
        base
        + "  upward_load_limit:\n    name: Up limit\n  downward_load_limit:\n    name: Down base - affects homing\n  restore_downward_load_limit:\n    name: Restore down\n",
        True,
    ),
    ("restore_without_limit", base + "  restore_downward_load_limit:\n    name: Restore down\n", False),
    ("wrong_baud", base.replace("baud_rate: 115200", "baud_rate: 9600"), False),
    ("missing_rx", base.replace("  rx_pin: GPIO16\n", ""), False),
    ("missing_tx", base.replace("  tx_pin: GPIO17\n", ""), False),
]
results = []
for name, text, expected in cases:
    cfg = work / (name + ".yaml")
    cfg.write_text(text)
    result = subprocess.run(["esphome", "config", str(cfg)], env=env, text=True, capture_output=True)
    (work / (name + ".log")).write_text(result.stdout + result.stderr)
    passed = result.returncode == 0
    assert passed == expected, f"{name}: unexpected result, see log"
    if name in ("raw_load_monitoring", "raw_load_inch_monitoring"):
        raw = yaml.safe_load(result.stdout)["desk_uart"][0]["load_raw"]
        assert raw["accuracy_decimals"] == 0, raw
        assert raw["state_class"] == "measurement", raw
        assert raw["entity_category"] == "diagnostic", raw
        assert not raw.get("unit_of_measurement") and not raw.get("device_class"), raw
    results.append({"case": name, "accepted": passed, "expected": expected})
(work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
