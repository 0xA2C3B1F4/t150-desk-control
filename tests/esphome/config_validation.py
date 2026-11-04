#!/usr/bin/env python3
"""Validate defaults, opt-in and UART constraints using installed ESPHome."""

import json
import subprocess
import sys

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
        "display_controls",
        base
        + "  display_mode:\n    name: Display mode\n  display_timeout:\n    name: Display timeout\n  display_status:\n    name: Display status\n",
        True,
    ),
    (
        "display_custom_slider",
        base + "  display_timeout:\n    name: Display timeout\n    mode: slider\n",
        True,
    ),
    (
        "display_rejects_restore",
        base
        + "  display_timeout:\n    name: Display timeout\n    restore_value: true\n",
        False,
    ),
    (
        "motion_trim_boxes",
        base
        + "  normal_up_trim:\n    name: Normal up trim\n  normal_down_trim:\n    name: Normal down trim\n  slow_up_trim:\n    name: Slow up trim\n  slow_down_trim:\n    name: Slow down trim\n  motion_trim_status:\n    name: Motion trim status\n",
        True,
    ),
    (
        "motion_trim_rejects_slider",
        base + "  normal_up_trim:\n    name: Trim\n    mode: slider\n",
        False,
    ),
    (
        "motion_trim_rejects_unit",
        base + "  normal_up_trim:\n    name: Trim\n    unit_of_measurement: mm\n",
        False,
    ),
    (
        "motion_trim_rejects_restore",
        base + "  normal_up_trim:\n    name: Trim\n    restore_value: true\n",
        False,
    ),
    (
        "recovery_without_motion",
        base
        + "  restore_preset_3:\n    name: Restore preset\n  accept_current_preset_3:\n    name: Accept current preset\n  preset_restore_pending:\n    name: Backup pending\n",
        True,
    ),
    ("keep_awake_requires_backup", base + "  keep_awake: true\n", False),
    (
        "keep_awake_opt_in",
        base + "  keep_awake: true\n  experimental_target_height: true\n",
        True,
    ),
    ("default_monitoring", base, True),
    ("reset_requires_opt_in", base + "  reset_desk:\n    name: Reset desk\n", False),
    (
        "reset_requires_stop_and_status",
        base
        + "  experimental_reset_controls: true\n  reset_desk:\n    name: Reset desk\n",
        False,
    ),
    (
        "reset_controls",
        base
        + "  experimental_reset_controls: true\n  stop:\n    name: Stop\n  control_status:\n    name: Control status\n  reset_desk:\n    name: Reset\n",
        True,
    ),
    (
        "reset_without_stop",
        base
        + "  experimental_reset_controls: true\n  control_status:\n    name: Control status\n  reset_desk:\n    name: Reset\n",
        False,
    ),
    (
        "reset_status_without_availability",
        base
        + "  experimental_reset_controls: true\n  stop:\n    name: Stop\n  reset_status:\n    name: Reset status\n  reset_desk:\n    name: Reset\n",
        False,
    ),
    (
        "reset_without_any_status",
        base
        + "  experimental_reset_controls: true\n  stop:\n    name: Stop\n  reset_desk:\n    name: Reset\n",
        False,
    ),
    (
        "legacy_reset_controls",
        base
        + "  experimental_reset_controls: true\n  stop:\n    name: Stop\n  reset_status:\n    name: Reset status\n  reset_available:\n    name: Reset available\n  reset_desk:\n    name: Reset desk\n  continue_reset:\n    name: Continue reset\n",
        True,
    ),
    (
        "legacy_continue_reset",
        base
        + "  experimental_reset_controls: true\n  stop:\n    name: Stop\n  reset_status:\n    name: Reset status\n  reset_available:\n    name: Reset available\n  continue_reset:\n    name: Continue reset\n",
        True,
    ),
    (
        "speed_and_status_only",
        base
        + "  reset_status:\n    name: Reset status\n  motion_speed:\n    name: Motion speed\n  motion_speed_available:\n    name: Motion speed available\n",
        True,
    ),
    (
        "child_lock",
        base
        + "  child_lock:\n    name: Child lock\n  child_lock_status:\n    name: Child lock status\n  child_lock_available:\n    name: Child lock available\n",
        True,
    ),
    (
        "concise_panel_lock",
        base
        + "  child_lock:\n    name: Panel lock\n  control_status:\n    name: Control status\n",
        True,
    ),
    (
        "child_lock_diagnostics_only",
        base
        + "  child_lock_status:\n    name: Child lock status\n  child_lock_available:\n    name: Child lock available\n",
        True,
    ),
    (
        "child_lock_restore_disabled",
        base + "  child_lock:\n    name: Child lock\n    restore_mode: DISABLED\n",
        True,
    ),
    (
        "child_lock_rejects_restore",
        base + "  child_lock:\n    name: Child lock\n    restore_mode: ALWAYS_OFF\n",
        False,
    ),
    (
        "child_lock_rejects_inversion",
        base + "  child_lock:\n    name: Child lock\n    inverted: true\n",
        False,
    ),
    ("direct_requires_opt_in", base + "  target_height_mode: direct\n", False),
    (
        "direct_rejects_legacy_keep_awake",
        base
        + "  target_height_mode: direct\n  experimental_target_height: true\n  keep_awake: true\n",
        False,
    ),
    (
        "direct_auto",
        base
        + "  target_height_mode: direct\n  experimental_target_height: true\n  keep_awake: auto\n",
        True,
    ),
    (
        "preset_auto",
        base
        + "  target_height_mode: preset\n  experimental_target_height: true\n  keep_awake: auto\n",
        True,
    ),
    (
        "auto_target",
        base
        + "  target_height_mode: auto\n  experimental_target_height: true\n  keep_awake: auto\n  target_height_backend:\n    name: Backend\n",
        True,
    ),
    ("auto_monitor_only", base + "  target_height_mode: auto\n", True),
    (
        "auto_legacy_keep_awake",
        base
        + "  target_height_mode: auto\n  experimental_target_height: true\n  keep_awake: true\n",
        True,
    ),
    ("unknown_target_mode", base + "  target_height_mode: automatic\n", False),
    ("uart_observation_only", base + "  uart_observation: true\n", True),
    ("raw_load_monitoring", base + "  load_raw:\n    name: Raw load\n", True),
    (
        "raw_load_inch_monitoring",
        base + "  display_unit: in\n  load_raw:\n    name: Raw load\n",
        True,
    ),
    ("inch_monitoring", base + "  display_unit: in\n", True),
    ("invalid_display_unit", base + "  display_unit: metres\n", False),
    (
        "conflicting_sensor_unit",
        base.replace(
            "    name: Height", "    name: Height\n    unit_of_measurement: cm"
        )
        + "  display_unit: in\n",
        False,
    ),
    (
        "inch_controls",
        base
        + "  display_unit: in\n  experimental_target_height: true\n  target_height:\n    name: Target\n  preset_1_height:\n    name: Preset\n",
        True,
    ),
    (
        "conflicting_number_unit",
        base
        + "  display_unit: in\n  preset_1_height:\n    name: Preset\n    unit_of_measurement: cm\n",
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
    (
        "preset_opt_in",
        base + "  experimental_preset_buttons: true\n  preset_3:\n    name: Preset\n",
        True,
    ),
    ("stop_without_motion_opt_in", base + "  stop:\n    name: Stop\n", True),
    ("custom_default", base + "  experimental_custom_timer_buzzer: false\n", True),
    ("custom_opt_in", base + "  experimental_custom_timer_buzzer: true\n", True),
    (
        "custom_available_without_opt_in",
        base + "  custom_available:\n    name: Available\n",
        False,
    ),
    (
        "custom_result_without_opt_in",
        base + "  custom_command_result:\n    name: Result\n",
        False,
    ),
    (
        "custom_diagnostics",
        base
        + "  experimental_custom_timer_buzzer: true\n  custom_available:\n    name: Available\n  custom_command_result:\n    name: Result\n",
        True,
    ),
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
    (
        "restore_without_limit",
        base + "  restore_downward_load_limit:\n    name: Restore down\n",
        False,
    ),
    ("wrong_baud", base.replace("baud_rate: 115200", "baud_rate: 9600"), False),
    ("missing_rx", base.replace("  rx_pin: GPIO16\n", ""), False),
    ("missing_tx", base.replace("  tx_pin: GPIO17\n", ""), False),
]
results = []
for name, text, expected in cases:
    cfg = work / (name + ".yaml")
    cfg.write_text(text)
    result = subprocess.run(
        ["esphome", "config", str(cfg)], env=env, text=True, capture_output=True
    )
    (work / (name + ".log")).write_text(result.stdout + result.stderr)
    passed = result.returncode == 0
    assert passed == expected, f"{name}: unexpected result, see log"
    if name == "child_lock":
        lock = yaml.safe_load(result.stdout)["desk_uart"][0]["child_lock"]
        assert lock["restore_mode"] == "DISABLED", lock
    if name == "motion_trim_boxes":
        desk = yaml.safe_load(result.stdout)["desk_uart"][0]
        for key in (
            "normal_up_trim",
            "normal_down_trim",
            "slow_up_trim",
            "slow_down_trim",
        ):
            assert desk[key]["mode"] == "BOX", desk[key]
            assert desk[key]["entity_category"] == "config", desk[key]
            assert not desk[key].get("unit_of_measurement"), desk[key]
    if name in ("raw_load_monitoring", "raw_load_inch_monitoring"):
        raw = yaml.safe_load(result.stdout)["desk_uart"][0]["load_raw"]
        assert raw["accuracy_decimals"] == 0, raw
        assert raw["state_class"] == "measurement", raw
        assert raw["entity_category"] == "diagnostic", raw
        assert not raw.get("unit_of_measurement") and not raw.get("device_class"), raw
    results.append({"case": name, "accepted": passed, "expected": expected})
# Exercise the schema's version guard without installing a second ESPHome.
legacy_cfg = work / "older_esphome.yaml"
legacy_cfg.write_text(
    base.replace(
        "  name: desk-config-test\n",
        "  name: desk-config-test\n  min_version: 2026.9.0\n",
    )
)
legacy_result = subprocess.run(
    [
        sys.executable,
        "-c",
        "import runpy; import esphome.config_validation as cv; "
        "cv.ESPHOME_VERSION = '2026.9.0'; "
        "runpy.run_module('esphome', run_name='__main__')",
        "config",
        str(legacy_cfg),
    ],
    env=env,
    text=True,
    capture_output=True,
)
legacy_output = legacy_result.stdout + legacy_result.stderr
(work / "older_esphome.log").write_text(legacy_output)
assert legacy_result.returncode != 0
assert "requires at least ESPHome version 2026.10.1" in legacy_output
results.append({"case": "older_esphome", "accepted": False, "expected": False})
(work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
