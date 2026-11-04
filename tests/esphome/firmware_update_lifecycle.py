#!/usr/bin/env python3
"""Exercise the real updater with host UART/HTTP/heap/preferences stand-ins.

Optional private image arguments exercise the complete hash-bound workflow.
The repository contains no controller firmware. No network or device access.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

import esphome

from desk_test_support import ROOT, parse_args

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--original", type=Path)
parser.add_argument("--patched", type=Path)
parser.add_argument("--interrupted", type=Path)
parser.add_argument("--child-lock", type=Path)
parser.add_argument("--reset-speed", type=Path)
parser.add_argument("--timer-buzzer", type=Path)
parser.add_argument("--platform", type=Path)
parser.add_argument("--motion-trims", type=Path)
parser.add_argument("--current", type=Path)
parser.add_argument("--buzzer", type=Path)
parser.add_argument("--recovery-record", type=Path)
args = parse_args(parser=parser)
required_images = (
    "original",
    "patched",
    "child_lock",
    "reset_speed",
    "timer_buzzer",
    "platform",
    "motion_trims",
    "current",
    "buzzer",
)
if (
    args.interrupted
    or args.recovery_record
    or any(getattr(args, name) for name in required_images)
):
    missing = [
        "--" + name.replace("_", "-")
        for name in required_images
        if not getattr(args, name)
    ]
    if missing:
        parser.error(
            "Complete lifecycle requires all nine images; missing " + ", ".join(missing)
        )
work = args.work_dir
host = ROOT / "tests/esphome/firmware_export_host"
update = ROOT / "tests/esphome/firmware_update_host"
package = Path(esphome.__file__).resolve().parent
for name in ["desk_uart", "desk_firmware_export", "desk_firmware_update"]:
    link = work / "include/esphome/components" / name
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(ROOT / "components" / name, target_is_directory=True)
flags = [
    "-std=c++17",
    "-DUSE_HOST",
    "-pthread",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer",
    "-O1",
    "-g",
]
for path in [
    update / "include",
    host / "include",
    work / "include",
    ROOT / "components/desk_firmware_update",
    package.parent,
]:
    flags += ["-I" + str(path)]
openssl = shlex.split(
    subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True)
)
sources = [
    ROOT / "components/desk_firmware_export/desk_firmware_export.cpp",
    ROOT / "components/desk_firmware_update/desk_firmware_update.cpp",
    package / "components/sha256/sha256.cpp",
    update / "update_test.cpp",
]
command = [
    os.environ.get("CXX", "c++"),
    *flags,
    *map(str, sources),
    *openssl,
    "-o",
    str(work / "update-test"),
]
with (work / "build.log").open("w") as log:
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
command = [str(work / "update-test")]
if args.original:
    command += [str(args.original.resolve()), str(args.patched.resolve())]
if args.interrupted or args.child_lock:
    command += [str(args.interrupted.resolve()) if args.interrupted else "-"]
if args.child_lock:
    command += [str(args.child_lock.resolve())]
if args.reset_speed:
    command += [str(args.reset_speed.resolve())]
if args.timer_buzzer:
    command += [str(args.timer_buzzer.resolve())]
if args.platform:
    command += [str(args.platform.resolve())]
if args.motion_trims:
    command += [str(args.motion_trims.resolve())]
if args.current:
    command += [str(args.recovery_record.resolve()) if args.recovery_record else "-"]
    command += [str(args.current.resolve()), str(args.buzzer.resolve())]
with (work / "run.log").open("w") as log:
    subprocess.run(
        command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60
    )
run_output = (work / "run.log").read_text()
if (
    args.recovery_record
    and "Prior updater recovery record loaded unchanged and verified" not in run_output
):
    raise SystemExit("Prior updater recovery record was not exercised")
if args.platform:
    identities = "TB01/FP01/TR01/BZ01/DS01"
    pairs = 81
    expected = (
        f"{identities} exact preparation, {pairs} modeled transitions, 53-to-32 erase, final ACK loss, "
        "torn blocks 33/53 and repeated original recovery passed"
    )
    if expected not in run_output:
        raise SystemExit("Extended lifecycle suite was not exercised")
interrupted = None
if args.interrupted:
    # The test strips a 0x2800 loader prefix from full-flash dumps; hash the same bytes.
    image = args.interrupted.read_bytes()
    interrupted = hashlib.sha256(
        image[0x2800:] if len(image) == 65536 else image
    ).hexdigest()
    if f"Saved power-cut image {interrupted} recovery passed" not in run_output:
        raise SystemExit("Interrupted image was not exercised")
summary = dict(
    passed=True,
    private_images=bool(args.original),
    child_lock_image=bool(args.child_lock),
    reset_speed_image=bool(args.reset_speed),
    timer_buzzer_image=bool(args.timer_buzzer),
    platform_image=bool(args.platform),
    motion_trims_image=bool(args.motion_trims),
    current_image=bool(args.current),
    buzzer_image=bool(args.buzzer),
    prior_recovery_record=bool(args.recovery_record),
    interrupted_image=interrupted,
    hardware_access=False,
    boundary="Real adapter and SHA; host stand-ins for IDF, HTTP transport, UART, heap and preferences",
)
summary["application_sha256"] = {}
for name in (
    "original",
    "patched",
    "child_lock",
    "reset_speed",
    "timer_buzzer",
    "platform",
    "motion_trims",
    "current",
    "buzzer",
    "interrupted",
):
    path = getattr(args, name)
    if path:
        image = path.read_bytes()
        application = image[0x2800:] if len(image) == 65536 else image
        summary["application_sha256"][name] = hashlib.sha256(application).hexdigest()
(work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print((work / "run.log").read_text())
print(json.dumps(summary))
