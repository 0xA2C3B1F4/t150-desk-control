#!/usr/bin/env python3
"""Run ESPHome configuration, host integration and GitHub example tests in sequence.

Each test gets <work-dir>/<test> as its own --work-dir, so the tests share the
PlatformIO caches in <work-dir>. Output of each test goes to <work-dir>/<test>.out.
Remove <work-dir> when finished. CI runs the same scripts as separate jobs.
The GitHub example test requires repository read access and a published HEAD.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import time

from desk_test_support import parse_args

TESTS = [
    "existing_device_config",
    "source_metadata_integration",
    "component_packaging",
    "component_layout",
    "config_validation",
    "panel_lock_config",
    "provisioning_config",
    "wifi_reconfiguration",
    "identity_integration",
    "github_example_build",
    "distribution_build",
    "firmware_export_config",
    "firmware_export_integration",
    "firmware_export_lifecycle",
    "firmware_update_lifecycle",
    "firmware_update_config",
    "firmware_profile_integration",
    "display_units_integration",
    "display_integration",
    "integration",
    "stop_integration",
    "load_limits_integration",
    "load_raw_integration",
    "target_integration",
    "retarget_integration",
    "retarget_phase_integration",
    "auto_target_integration",
    "backend_journal_integration",
    "panel_lock_integration",
    "reset_speed_integration",
    "motion_trim_integration",
    "motion_trim_target_integration",
    "direct_target_integration",
    "direct_target_review_integration",
    "preset_settings_integration",
    "timer_integration",
    "custom_timer_buzzer_integration",
    "custom_timer_buzzer_api_integration",
    "custom_timer_buzzer_controls",
    "custom_timer_buzzer_arbitration",
    "keep_awake_integration",
    "preset_persistence_integration",
    "profile_journal_integration",
]


def main():
    """Run selected tests with separate logs and exit nonzero on any failure."""
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--only", nargs="+", choices=TESTS, metavar="TEST", help="Run only these tests"
    )
    args = parse_args(parser=ap)
    failed = []
    for name in args.only or TESTS:
        out = args.work_dir / f"{name}.out"
        start = time.monotonic()
        with out.open("w") as log:
            commands = [
                [
                    sys.executable,
                    str(Path(__file__).parent / f"{name}.py"),
                    "--work-dir",
                    str(args.work_dir / name),
                ]
            ]
            if name == "firmware_update_lifecycle":
                commands.insert(
                    0,
                    [
                        sys.executable,
                        str(Path(__file__).parent / "firmware_update_wrapper_test.py"),
                    ],
                )
            for command in commands:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
                if result.returncode:
                    break
            if name == "reset_speed_integration" and result.returncode == 0:
                result = subprocess.run(
                    [
                        sys.executable,
                        str(Path(__file__).parent / "reset_speed_integration.py"),
                        "--work-dir",
                        str(args.work_dir / "reset_speed_concise"),
                        "--concise-ui",
                        "--only",
                        "resume",
                        "paused_idle",
                    ],
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
        status = "passed" if result.returncode == 0 else f"FAILED, see {out}"
        print(f"{name}: {status} ({time.monotonic() - start:.0f} s)", flush=True)
        if result.returncode != 0:
            failed.append(name)
    print(f"{len(args.only or TESTS) - len(failed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
