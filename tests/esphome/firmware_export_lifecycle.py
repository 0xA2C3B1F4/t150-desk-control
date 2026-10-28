#!/usr/bin/env python3
"""Run the unchanged IDF export adapter with synthetic host platform edges.

Real ESPHome SHA256/OpenSSL is used. UART, heap reporting and synchronous HTTP
send are test stand-ins; this does not execute ESP-IDF or access a device.
"""
import argparse
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import shlex
import subprocess

import esphome
from desk_test_support import ROOT, parse_args

CASES = ("crc_failure", "partial_timeout", "allocation_failure", "boot_timeout", "partial_cancel", "download_clear", "successful_replacement")
IMAGES = {
    "A": bytes((i * 37 + 11) & 255 for i in range(55296)),
    "B": bytes((i * 19 + 7) & 255 for i in range(1025)),
}
HASHES = {name: hashlib.sha256(image).hexdigest() for name, image in IMAGES.items()}
SCALARS = dict(lower_seed=4, upper_seed=460, display_offset=725, high_test_addend=-1182, high_display=1188)
VALUES = "Lower seed 4; upper seed 460; offset 72.5 cm; high addend -1182; high display 118.8 cm"
STATUS_TEXT = {
    "idle": "Idle", "complete": "Complete", "waiting_boot": "Waiting for desk power cycle",
    "receiving": "Reading firmware", "error": "Failed", "cancelled": "Cancelled",
}


def check(condition, description):
    if not condition:
        raise AssertionError(description)


def validate_snapshot(row):
    case, point, fixture = row["case"], row["checkpoint"], row["profile_fixture"]
    http, entities = row["http"], row["entities"]
    context = f"{case}/{point}"
    for field in ("received", "size", "image_size", "max_size", "bad_packets", "duplicates", "free_heap", "largest_block"):
        check(type(http[field]) is int and http[field] >= 0, f"{context}: invalid integer {field}")
    for field in ("busy", "ready", "closing_complete"):
        check(type(http[field]) is bool, f"{context}: invalid boolean {field}")
    for field in ("state", "error", "sha256"):
        check(type(http[field]) is str, f"{context}: invalid string {field}")
    check(http["max_size"] == 131072 and http["duplicates"] == 0, f"{context}: size bound or duplicates changed")
    profile = http["image_profile"]
    check(type(profile["extraction_supported"]) is bool and profile["control_approved"] is False,
          f"{context}: invalid research-only profile flags")
    profile_status = {
        "known": "Known T150 profile", "unrecognized": "Unrecognized firmware",
        "unsupported": "Unsupported image size", "empty": "No firmware image",
    }[fixture]
    check(profile["status"] == profile_status, f"{context}: wrong profile status")
    check(profile["extraction_supported"] == (fixture == "known"), f"{context}: extraction flag changed")
    if fixture == "known":
        check(profile.get("scalars") == SCALARS, f"{context}: retained scalars changed")
        check(all(type(v) is int for v in profile["scalars"].values()), f"{context}: scalar types changed")
    else:
        check("scalars" not in profile, f"{context}: unavailable profile contains scalars")
    check(entities["profile"] == profile_status, f"{context}: profile entity and HTTP disagree")
    check(entities["values"] == (VALUES if fixture == "known" else "Unavailable"), f"{context}: profile values entity differs")
    check(type(entities["ready"]) is bool and entities["ready"] == http["ready"], f"{context}: ready entity differs")
    check(entities["sha256"] == http["sha256"], f"{context}: hash entity differs")
    check(entities["status"] == STATUS_TEXT[http["state"]], f"{context}: status entity differs")
    check(entities["error"] == (http["error"] or "None"), f"{context}: error entity differs")
    expected_progress = 100.0 * http["received"] / http["size"] if http["size"] else 0.0
    check(math.isclose(entities["progress"], expected_progress, rel_tol=0, abs_tol=0.0001),
          f"{context}: progress entity differs")
    image = "B" if fixture == "unsupported" else "A"
    check(http["ready"] == (fixture != "empty"), f"{context}: completed slot readiness changed")
    check(http["image_size"] == (0 if fixture == "empty" else len(IMAGES[image])), f"{context}: retained image length changed")
    check(http["sha256"] == ("" if fixture == "empty" else HASHES[image]), f"{context}: whole image hash differs from hashlib")

    if point in ("complete", "baseline", "replacement"):
        check(http["state"] == "complete" and not http["busy"] and http["closing_complete"], f"{context}: incomplete success")
        check(http["received"] == http["size"] == len(IMAGES[image]) and not http["error"], f"{context}: successful byte counts differ")
    elif point == "armed":
        check(http["state"] == "waiting_boot" and http["busy"] and not http["closing_complete"], f"{context}: not armed")
        check(http["received"] == http["size"] == 0, f"{context}: armed counts not reset")
    elif point == "partial":
        check(http["state"] == "receiving" and http["busy"] and not http["closing_complete"], f"{context}: not receiving")
        check(http["received"] == 1024 and http["size"] == 2049, f"{context}: partial byte counts differ")
    elif point in ("finished", "released"):
        check(http["state"] == ("cancelled" if case == "partial_cancel" else "error"), f"{context}: wrong failure state")
        check(http["busy"] == (point == "finished") and not http["closing_complete"], f"{context}: UART grace/release differs")
        expected_size = 0 if case == "boot_timeout" else 2049
        expected_received = 0 if case in ("allocation_failure", "boot_timeout") else 1024
        check(http["size"] == expected_size and http["received"] == expected_received and bool(http["error"]),
              f"{context}: failed attempt counts/error differ")
        check(http["bad_packets"] == (4 if case == "crc_failure" else 0), f"{context}: failure packet count differs")
    elif point == "cleared":
        check(http["state"] == "idle" and not http["busy"] and not http["closing_complete"], f"{context}: clear left reader active")
        check(http["received"] == http["size"] == http["bad_packets"] == 0 and not http["error"], f"{context}: clear left attempt metadata")
    else:
        raise AssertionError(f"Unexpected checkpoint {context}")


def validate_download(row, run):
    expected, context = row["expected_image"], f"{row['case']}/{row['checkpoint']}"
    path = (run / row["file"]).resolve()
    check(path.parent == run.resolve(), f"{context}: output path escapes run directory")
    if expected == "none":
        check(row["status"] == 409 and not path.exists(), f"{context}: empty image remains downloadable")
        return
    check(expected in IMAGES and row["status"] == 200, f"{context}: download failed")
    check(row["content_type"] == "application/octet-stream", f"{context}: wrong binary content type")
    headers = row["headers"]
    check(headers.get("Content-Disposition") == "attachment; filename=UploadedFlashImage.bin", f"{context}: wrong download filename")
    check(headers.get("Cache-Control") == "no-store", f"{context}: missing no-store header")
    downloaded = path.read_bytes()
    check(downloaded == IMAGES[expected], f"{context}: downloaded bytes differ")
    check(hashlib.sha256(downloaded).hexdigest() == HASHES[expected], f"{context}: download hash differs")


def validate_output(run, output, cases):
    rows = [json.loads(line) for line in output.splitlines() if line.strip()]
    check(rows and rows[-1] == {"kind": "result", "passed": True, "cases": len(cases)}, "Missing complete successful result")
    for row in rows[:-1]:
        check(row.get("case") in cases, "Unexpected lifecycle case")
        if row["kind"] == "snapshot":
            validate_snapshot(row)
        elif row["kind"] == "download":
            validate_download(row, run)
        else:
            raise AssertionError("Unexpected fixture record kind")
    for case in cases:
        snapshots = [r["checkpoint"] for r in rows if r.get("case") == case and r["kind"] == "snapshot"]
        downloads = [r["checkpoint"] for r in rows if r.get("case") == case and r["kind"] == "download"]
        if case == "download_clear":
            expected_snapshots = ["complete", "baseline", "cleared", "replacement", "cleared"]
            expected_downloads = ["complete", "cleared", "in_flight", "replacement", "cleared"]
        elif case == "successful_replacement":
            expected_snapshots = ["complete", "baseline", "replacement", "cleared"]
            expected_downloads = ["complete", "replacement", "cleared"]
        else:
            partial = [] if case in ("allocation_failure", "boot_timeout") else ["partial"]
            expected_snapshots = ["complete", "baseline", "armed"] + partial + ["finished", "released", "cleared"]
            expected_downloads = ["complete", "armed"] + partial + ["finished", "cleared"]
        check(snapshots == expected_snapshots and downloads == expected_downloads, f"{case}: missing or reordered checkpoints")
    return {"snapshots": sum(r["kind"] == "snapshot" for r in rows),
            "downloads": sum(r["kind"] == "download" for r in rows)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT, help="Checkout to test, including a temporary mutation copy")
    parser.add_argument("--case", choices=CASES, help="Run one case, mainly for bounded mutation checks")
    parser.add_argument("--build-type", choices=("Debug", "Release"), action="append", help="Defaults to both Debug and Release")
    args = parse_args(parser=parser)
    source = args.source_root.resolve()
    host = source / "tests/esphome/firmware_export_host"
    package = Path(esphome.__file__).resolve().parent
    pkg_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True))
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    cases = (args.case,) if args.case else CASES
    builds = []
    for build_type in args.build_type or ("Debug", "Release"):
        run = args.work_dir / build_type.lower()
        run.mkdir(parents=True, exist_ok=False)
        flags = ["-std=c++17", "-DUSE_HOST", "-pthread", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        flags += ["-O0"] if build_type == "Debug" else ["-O2", "-DNDEBUG"]
        flags += ["-I" + str(host / "include"), "-I" + str(source / "components/desk_firmware_export"),
                  "-I" + str(source / "lib/desk_protocol/include"), "-I" + str(package.parent)]
        # OpenSSL's include flags are needed while compiling the actual SHA source;
        # the matching link flags are passed only to the linker below.
        include_flags = [flag for flag in pkg_flags if flag.startswith(("-I", "-D"))]
        sources = [source / "components/desk_firmware_export/desk_firmware_export.cpp",
                   package / "components/sha256/sha256.cpp", host / "lifecycle_test.cpp"]
        commands, objects = [], []
        with (run / "build.log").open("w") as log:
            for index, unit in enumerate(sources):
                obj = run / f"unit-{index}.o"
                command = compiler + flags + include_flags + ["-c", str(unit), "-o", str(obj)]
                commands.append(command)
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
                objects.append(str(obj))
            executable = run / "lifecycle-test"
            command = compiler + flags + objects + pkg_flags + ["-o", str(executable)]
            commands.append(command)
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        (run / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        command = [str(executable)] + ([args.case] if args.case else [])
        env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        with (run / "runtime.log").open("w") as log:
            result = subprocess.run(command, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        if result.returncode:
            raise RuntimeError(f"Lifecycle fixture failed with {result.returncode}; see {run / 'runtime.log'}")
        counts = validate_output(run, (run / "runtime.log").read_text(), cases)
        builds.append(dict(build_type=build_type, passed=True, cases=list(cases), **counts))
    summary = {
        "passed": True, "hardware_access": False,
        "esphome_version": importlib.metadata.version("esphome"),
        "sha256_implementation": "Installed ESPHome SHA256 with OpenSSL",
        "production_source_sha256": hashlib.sha256(sources[0].read_bytes()).hexdigest(),
        "builds": builds,
        "checks": ["Failed and cancelled new reads retain the previous image, hash and profile",
                   "HTTP JSON agrees with native accessors and independent image hashes",
                   "Synchronous in-flight download survives Clear and releases its allocation after return",
                   "Clear removes new-download availability and image/profile metadata",
                   "A successful read directly replaces the previous image, exact length, hash and profile",
                   "A successful read after Clear publishes its exact length, hash and profile"],
        "limits": ["Platform boundaries are host stand-ins; this is not ESP-IDF or network validation",
                   "Known-profile metadata is an explicit lifecycle fixture, not recognition of synthetic firmware",
                   "Authentication is outside the handler fixture and remains covered by separate device checks"],
    }
    (args.work_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
