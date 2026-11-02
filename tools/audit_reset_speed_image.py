#!/usr/bin/env python3
"""Inventory RS01 static-proof inputs from the exact saved CL01 full image.

This records literal and direct-call coverage. It is not an alias-analysis proof
or a physical test. The companion REA evidence explains each reachable writer.
"""

import argparse
import json
from pathlib import Path
import struct

from build_reset_speed_patch import (
    FLASH_BASE,
    FULL_SIZE,
    identify,
    read_saved_image,
    sha256,
)

TARGETS = (
    0x0800606C,
    0x08006090,
    0x080070D8,
    0x08007814,
    0x080078A8,
    0x08006434,
    0x08007732,
    0x08007746,
)


def audit(data):
    if len(data) != FULL_SIZE or identify(data) != 0:
        raise ValueError(
            "The static inventory requires the exact saved full CL01 R3 image"
        )

    def word(offset):
        return struct.unpack_from("<I", data, offset)[0]

    literals = {
        hex(v): [hex(FLASH_BASE + i) for i in range(len(data) - 3) if word(i) == v]
        for v in (0x40000400, 0x4000043C, 0x40000440)
    }
    calls = {hex(v): [] for v in TARGETS}
    for offset in range(0, len(data) - 3, 2):
        first, second = struct.unpack_from("<HH", data, offset)
        if first & 0xF800 != 0xF000 or second & 0xD000 != 0xD000:
            continue
        sign = (first >> 10) & 1
        i1 = 1 ^ (((second >> 13) & 1) ^ sign)
        i2 = 1 ^ (((second >> 11) & 1) ^ sign)
        delta = (
            (sign << 24)
            | (i1 << 23)
            | (i2 << 22)
            | ((first & 0x3FF) << 12)
            | ((second & 0x7FF) << 1)
        )
        if sign:
            delta -= 1 << 25
        target = FLASH_BASE + offset + 4 + delta
        if target in TARGETS:
            calls[hex(target)].append(hex(FLASH_BASE + offset))
    scatter = []
    for offset in range(word(0x3190) - FLASH_BASE, word(0x3194) - FLASH_BASE, 16):
        source, destination, size, helper = struct.unpack_from("<IIII", data, offset)
        scatter.append(
            dict(
                table_address=hex(FLASH_BASE + offset),
                source=hex(source),
                destination=hex(destination),
                size=size,
                end_exclusive=hex(destination + size),
                helper=hex(helper),
            )
        )
    return dict(
        source_sha256=sha256(data),
        coverage_bytes=len(data),
        tim3_literal_locations=literals,
        direct_thumb_bl_call_sites=calls,
        writer_function_pointer_literals={
            hex(v | 1): [
                hex(FLASH_BASE + i) for i in range(len(data) - 3) if word(i) == (v | 1)
            ]
            for v in TARGETS
        },
        upper_sram_literal_locations=[
            dict(address=hex(FLASH_BASE + i), value=hex(word(i)))
            for i in range(len(data) - 3)
            if 0x20000FC8 <= word(i) <= 0x20001000
        ],
        boot_stack=hex(word(0)),
        application_stack=hex(word(0x2800)),
        runtime_stack=hex(word(0x28D4)),
        application_reset=hex(word(0x2804)),
        scatter=scatter,
        vector_copy=dict(
            helper="0x08004a98",
            source_literal=hex(word(0x4AC0)),
            destination_literal=hex(word(0x4AC4)),
            words=48,
        ),
        limitations=[
            "Literal inventory examines every byte offset, direct BL scan every halfword, including data",
            "Computed aliases and indirect calls require the companion instruction/caller evidence",
            "No device access or physical measurement",
        ],
    )


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("--report", type=Path)
    a = p.parse_args()
    result = audit(read_saved_image(a.source))
    output = json.dumps(result, indent=2) + "\n"
    if a.report:
        with a.report.open("x") as f:
            f.write(output)
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
