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
    "config_validation",
    "identity_integration",
    "github_example_build",
    "firmware_export_config",
    "firmware_export_integration",
    "display_units_integration",
    "integration",
    "stop_integration",
    "load_limits_integration",
    "target_integration",
    "retarget_integration",
    "preset_settings_integration",
    "timer_integration",
    "keep_awake_integration",
    "preset_persistence_integration",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", nargs="+", choices=TESTS, metavar="TEST", help="Run only these tests")
    args = parse_args(parser=ap)
    failed = []
    for name in args.only or TESTS:
        out = args.work_dir / f"{name}.out"
        start = time.monotonic()
        with out.open("w") as log:
            result = subprocess.run(
                [sys.executable, str(Path(__file__).parent / f"{name}.py"), "--work-dir", str(args.work_dir / name)],
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
