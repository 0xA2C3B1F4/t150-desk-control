#!/usr/bin/env python3
"""Check TR01 GET probes against exact supported pre-TR01 application images."""

import argparse
import hashlib
import itertools
import json
from pathlib import Path

from unicorn import UC_HOOK_MEM_READ

import build_child_lock_patch as child_lock
import build_platform_patch as platform
import build_reset_speed_patch as reset_speed
import build_tb01_patch as timer_buzzer
from test_child_lock_patch import RAM, REQUEST, SP, Machine, frame

APPLICATIONS = {
    "stock": "bf52d596c81d6d499480a9dd3173e6991de5ee0c72364e0b53bc91ea89053faf",
    "dt02": child_lock.SOURCE_APP_SHA256,
    "cl01": child_lock.OUTPUT_APP_SHA256,
    "rs01-r7": reset_speed.OUTPUT_APP_SHA256,
    "tb01": timer_buzzer.OUTPUT_APP_SHA256,
    "fp01": platform.OUTPUT_APP_SHA256,
}


def check_image(path):
    """Execute actual parser/dispatch code while allowing only receive bookkeeping."""
    data = path.read_bytes()
    if len(data) not in (platform.FULL_SIZE, platform.APP_SIZE):
        raise ValueError("Expected a full or application image")
    application = data[-platform.APP_SIZE :]
    digest = hashlib.sha256(application).hexdigest()
    names = [name for name, expected in APPLICATIONS.items() if expected == digest]
    if len(names) != 1:
        raise ValueError("Image is not a pinned supported pre-TR01 application")
    # Fill the unused loader region with erased bytes; no loader code is supplied.
    image = b"\xff" * platform.APP_OFFSET + application
    cases = 0
    nonce_bytes = (0, 1, 0xFA, 0xFD, 0xFE, 0xFF)
    for high, low, moving in itertools.product(nonce_bytes, nonce_bytes, (0, 1)):
        machine = Machine(image)
        machine.put(0x88D, moving)
        machine.put(0x895, moving)
        machine.put(0x75A, moving, 2)
        machine.put(0xFC8, moving)
        dispatching = False

        def instruction(_machine, pc):
            nonlocal dispatching
            if pc == 0x08007BAC:
                dispatching = True

        def read(_uc, _access, address, size, _value, _context):
            if (
                dispatching
                and address < RAM + REQUEST + 18
                and address + size > RAM + REQUEST + 1
            ):
                raise AssertionError(
                    "Legacy dispatcher examined a TR01 field after its opcode"
                )

        machine.on_instruction = instruction
        machine.uc.hook_add(UC_HOOK_MEM_READ, read)
        request = bytes((0xD9, 1, 0, high, low)) + bytes(12)
        length = machine.request(request, wire=b"\xfa" + frame(request))
        if length != 18 or machine.response():
            raise AssertionError("Probe was not parsed once without a reply")
        if any(
            pc < platform.FLASH_BASE + platform.APP_OFFSET for pc in machine.visited
        ):
            raise AssertionError("Probe entered the bootloader region")
        for address, size, value, pc in machine.writes:
            # The stock parser writes FA at body[-1] and FD at body[128].
            legacy_marker = (
                size == 1
                and pc == 0x0800454C
                and (
                    (address == RAM + REQUEST - 1 and value == 0xFA)
                    or (address == RAM + REQUEST + 128 and value == 0xFD)
                )
            )
            permitted = legacy_marker or all(
                RAM + 0xAC4 <= byte < SP
                or byte in (RAM + 0xFC, RAM + 0x122, RAM + 0x123)
                or RAM + REQUEST <= byte < RAM + REQUEST + 18
                or RAM + 0xFEC <= byte < RAM + 0xFF0
                for byte in range(address, address + size)
            )
            if not permitted:
                raise AssertionError(
                    f"Probe changed non-parser state at {address:#x} from {pc:#x}: {value:#x}"
                )
        cases += 1
    return {
        "image": names[0],
        "application_sha256": digest,
        "cases": cases,
        "result": "passed",
        "dispatcher_reads_only_opcode": True,
        "scope": "Exact parser and dispatcher; valid double-FA GET probes, synthetic idle/moving RAM; no hardware timing claim",
    }


def main():
    """Accept private images and optionally save a source-bound compatibility report."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", action="append", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = {
        "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "machine_sha256": hashlib.sha256(
            Path(__file__).with_name("test_child_lock_patch.py").read_bytes()
        ).hexdigest(),
        "results": [check_image(path) for path in args.image],
    }
    text = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
