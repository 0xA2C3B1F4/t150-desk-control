#!/usr/bin/env python3
"""Verify canonical panel-lock YAML and deprecated aliases through ESPHome."""

import json
import re
import subprocess
import sys

import yaml

from desk_test_support import ROOT, parse_args, test_env

KEYS = ("panel_lock", "panel_lock_status", "panel_lock_available")
ALIASES = dict(zip(("child_lock", "child_lock_status", "child_lock_available"), KEYS))
NAMES = dict(zip(KEYS, ("Panel lock", "Panel lock status", "Panel lock available")))


def warning(old, new):
    """Return the migration instruction promised for one deprecated YAML key."""
    return f"`{old}` has been renamed to `{new}`. Update your YAML configuration."


def main():
    """Check validation, warnings, entity identity and generated setter equivalence."""
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    base = f"""esphome:
  name: desk-panel-lock-config-test
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
    results = []

    def entities(legacy=False):
        text = ""
        for key in KEYS:
            configured = key.replace("panel_lock", "child_lock") if legacy else key
            text += f"  {configured}:\n    id: {key}_entity\n    name: {NAMES[key]}\n"
        return text

    def run(name, body, valid=True, deprecated=(), generate=False):
        config = work / (name + ".yaml")
        config.write_text(base + body)
        command = [sys.executable, "-m", "esphome"]
        command += ["compile" if generate else "config", str(config)]
        if generate:
            command += ["--only-generate"]
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        output = result.stdout + result.stderr
        (work / (name + ".log")).write_text(output)
        assert (result.returncode == 0) == valid, (name, output)
        for old, new in ALIASES.items():
            message = warning(old, new)
            assert (message in output) == (old in deprecated), (name, message, output)
        results.append(
            {"case": name, "accepted": valid, "deprecated": list(deprecated)}
        )
        if not valid:
            return None
        if generate:
            return (work / "build/src/main.cpp").read_text()
        return yaml.safe_load(result.stdout)["desk_uart"][0]

    canonical = run("canonical", entities())
    legacy = run("legacy_all", entities(True), deprecated=tuple(ALIASES))
    for old, new in ALIASES.items():
        assert old not in legacy and old not in canonical
        assert legacy[new] == canonical[new], (old, legacy[new], canonical[new])
        assert canonical[new]["name"] == NAMES[new]
        if new == "panel_lock":
            assert canonical[new]["restore_mode"] == "DISABLED"
        body = f"  {old}:\n    name: {NAMES[new]}\n"
        normalized = run("legacy_" + old, body, deprecated=(old,))
        assert old not in normalized and new in normalized
        duplicate = body + f"  {new}:\n    name: {NAMES[new]}\n"
        run("duplicate_" + old, duplicate, valid=False)
    for property_name, value in (("restore_mode", "ALWAYS_OFF"), ("inverted", "true")):
        body = f"  child_lock:\n    name: Panel lock\n    {property_name}: {value}\n"
        run(
            "invalid_legacy_" + property_name,
            body,
            valid=False,
            deprecated=("child_lock",),
        )
    generated = []
    for legacy_keys in (False, True):
        source = run(
            "generated_legacy" if legacy_keys else "generated_canonical",
            entities(legacy_keys),
            deprecated=tuple(ALIASES) if legacy_keys else (),
            generate=True,
        )
        for setter in (
            "set_panel_lock_switch",
            "set_panel_lock_status",
            "set_panel_lock_available",
        ):
            assert setter in source
        assert "set_child_lock" not in source
        assert "SWITCH_RESTORE_DISABLED" in source
        source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
        source = re.sub(r"//[^\n]*", "", source)
        generated.append(
            "\n".join(line.strip() for line in source.splitlines() if line.strip())
        )
    assert generated[0] == generated[1], (
        "Aliases changed generated C++ or entity identity"
    )
    (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(
        json.dumps(
            {"cases": len(results), "generated_code_equal": True, "passed": True}
        )
    )


if __name__ == "__main__":
    main()
