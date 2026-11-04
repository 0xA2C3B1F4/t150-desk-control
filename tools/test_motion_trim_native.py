#!/usr/bin/env python3
"""Execute the native-save shim and snapshot on Cortex-M0 machine code."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile
import unittest

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_CODE,
    UC_HOOK_MEM_WRITE,
    UC_MODE_THUMB,
    Uc,
    arm_const as arm,
)

from build_reset_speed_patch import find_tool

PATCHES = Path(__file__).resolve().parent / "patches"
FLASH = 0x08000000
RAM = 0x20000000
CODE = FLASH + 0xB800
STACK = RAM + 0xE80
RETURN = FLASH + 0xFFF0
PROLOGUE = bytes.fromhex("08b5012080f3108820480068")
IMAGE_SHA256 = "6a2ac33ba77713c9de9d675be4e2629af229e4110f78d7c35defbb1d13555992"
LLVM_BIN = None
NATIVE_IMAGE = None
REPORT = None
REGISTERS = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(13)] + [
    arm.UC_ARM_REG_SP,
    arm.UC_ARM_REG_LR,
    arm.UC_ARM_REG_APSR,
    arm.UC_ARM_REG_PRIMASK,
]
FIXTURE_C = """
#include "motion_trim_native.h"
volatile uint8_t test_ready, test_result, test_bytes[TRIM_NATIVE_BYTES];
volatile uint32_t test_calls, test_mask, test_adapter;
uint8_t trim_adapter_capture_native(TrimAdapterState* state, const uint8_t* bytes) {
  uint32_t mask;
  __asm volatile("mrs %0, PRIMASK" : "=r"(mask));
  test_mask = mask;
  test_adapter = (uint32_t)(uintptr_t)state;
  ++test_calls;
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    test_bytes[i] = bytes[i];
  }
  return test_result;
}
"""
FIXTURE_S = """
.syntax unified
.cpu cortex-m0
.thumb
.section .text.fixture,"ax",%progbits
.global trim_controller_capture_native
.thumb_func
trim_controller_capture_native:
  push {r4,lr}
  ldr r0, =test_ready
  ldrb r0, [r0]
  cmp r0, #0
  beq 1f
  ldr r0, =0x20001000
  bl trim_native_capture
  b 2f
1: movs r0, #1
2: movs r1, #0xa5
  movs r2, #0x5a
  movs r3, #0xff
  mov r12, r3
  cmp r1, #0
  pop {r4,pc}
.balign 4
.ltorg
"""


def compile_native():
    """Link only the owned shim/capture and explicit test doubles in TMPDIR."""
    tools = {
        name: find_tool(name, LLVM_BIN)
        for name in ("clang", "ld.lld", "llvm-objcopy", "llvm-nm")
    }
    versions = {
        name: subprocess.check_output([tool, "--version"], text=True)
        for name, tool in tools.items()
    }
    if any(not re.search(r"\b23\.1\.0\b", version) for version in versions.values()):
        raise ValueError("Native-save target tests require LLVM 23.1.0")
    with tempfile.TemporaryDirectory(
        prefix="t150-trim-native-", dir=os.environ["TMPDIR"]
    ) as directory:
        work = Path(directory)
        (work / "fixture.c").write_text(FIXTURE_C)
        (work / "fixture.S").write_text(FIXTURE_S)
        (work / "layout.ld").write_text(
            "SECTIONS {\n"
            " .hook_trim_native 0x080058d0 : { *(.hook_trim_native) }\n"
            " .candidate 0x0800b800 : { *(.text*) *(.rodata*) }\n"
            " .fixture 0x20001400 : { *(.bss*) *(.data*) }\n"
            " /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }\n"
            '}\nASSERT(SIZEOF(.hook_trim_native) == 12, "native hook size")\n'
        )
        objects = []
        frames = {}
        for source in (
            PATCHES / "motion_trim_native.c",
            PATCHES / "motion_trim_native.S",
            work / "fixture.c",
            work / "fixture.S",
        ):
            output = work / (source.name + ".o")
            flags = [
                "--target=arm-none-eabi",
                "-mcpu=cortex-m0",
                "-mthumb",
                "-I",
                str(PATCHES),
            ]
            if source.suffix == ".c":
                flags += [
                    "-std=c11",
                    "-Oz",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    "-fstack-usage",
                ]
            subprocess.run(
                [tools["clang"], *flags, "-c", source, "-o", output], check=True
            )
            objects.append(output)
            if output.with_suffix(".su").exists():
                frames[source.name] = (
                    output.with_suffix(".su")
                    .read_text()
                    .replace(str(PATCHES) + "/", "")
                )
        elf = work / "native.elf"
        subprocess.run(
            [tools["ld.lld"], "-T", work / "layout.ld", *objects, "-o", elf],
            check=True,
            capture_output=True,
        )
        if subprocess.check_output(
            [tools["llvm-nm"], "--undefined-only", elf], text=True
        ).strip():
            raise ValueError("Native shim test has unresolved symbols")
        listing = subprocess.check_output(
            [tools["llvm-nm"], "--defined-only", elf], text=True
        )
        symbols = {
            fields[2]: int(fields[0], 16)
            for line in listing.splitlines()
            if len(fields := line.split()) == 3
        }
        sections = {}
        for section in ("hook_trim_native", "candidate"):
            output = work / (section + ".bin")
            subprocess.run(
                [
                    tools["llvm-objcopy"],
                    "-O",
                    "binary",
                    "--only-section=." + section,
                    elf,
                    output,
                ],
                check=True,
            )
            sections[section] = output.read_bytes()
    manifest = {
        "toolchain": versions,
        "frames": frames,
        "section_sizes": {name: len(value) for name, value in sections.items()},
        "source_sha256": {
            name: hashlib.sha256((PATCHES / name).read_bytes()).hexdigest()
            for name in (
                "motion_trim_native.c",
                "motion_trim_native.h",
                "motion_trim_native.S",
            )
        },
    }
    return sections, symbols, manifest


def expected_snapshot(bits, height, mode):
    """Model the reviewed unsigned float-bit conversion and ordered bytes."""
    exponent = (bits >> 23) & 255
    significand = (bits & 0x7FFFFF) | 0x800000
    if exponent < 127:
        position = 0
    elif exponent <= 150:
        position = significand >> (150 - exponent)
    elif exponent - 150 < 32:
        position = (significand << (exponent - 150)) & 0xFFFFFFFF
    else:
        position = 0
    return bytes(
        (
            (position >> 8) & 255,
            position & 255,
            (height >> 8) & 255,
            height & 255,
            0,
            10 if mode == 2 else mode,
        )
    )


class Machine:
    """Execute the linked target with optional exact private native firmware."""

    def __init__(self, sections, symbols, patched, image):
        """Map code and record modeled electrical calls and visible stores."""
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_M0)
        self.uc.reg_write(arm.UC_ARM_REG_XPSR, 0x01000000)
        self.symbols = symbols
        self.patched = patched
        self.uc.mem_map(FLASH, 65536)
        self.uc.mem_write(FLASH, image or bytes(65536))
        self.uc.mem_write(CODE, sections["candidate"])
        self.uc.mem_write(
            FLASH + 0x58D0, sections["hook_trim_native"] if patched else PROLOGUE
        )
        self.uc.mem_write(FLASH + 0x595C, struct.pack("<I", RAM + 0x7D4))
        self.uc.mem_map(RAM, 8192)
        self.uc.mem_map(0x40000000, 0x30000)
        self.uc.mem_map(0x48000000, 0x10000)
        self.electrical = []
        self.writes = []
        self.stop = RETURN
        self.uc.hook_add(UC_HOOK_CODE, self.helper)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.store)

    def helper(self, uc, address, size, context):
        """Substitute only native electrical write/delay helpers when reached."""
        if address == self.stop:
            uc.emu_stop()
            return
        if address not in (FLASH + 0x4938, FLASH + 0x7B18):
            return
        if uc.reg_read(arm.UC_ARM_REG_PRIMASK) != 1:
            raise RuntimeError("Native save helper reached without PRIMASK")
        self.electrical.append(
            (address, uc.reg_read(arm.UC_ARM_REG_R0), uc.reg_read(arm.UC_ARM_REG_R1))
        )
        uc.reg_write(arm.UC_ARM_REG_PC, uc.reg_read(arm.UC_ARM_REG_LR))

    def store(self, uc, access, address, size, value, context):
        """Keep caller-visible stores, excluding stack and explicit test state."""
        if (
            RAM + 0xAC4 <= address < RAM + 0x1000
            or RAM + 0x1400 <= address < RAM + 0x1500
        ):
            return
        self.writes.append((address, size, value))

    def run(
        self, bits, height, mode, mask, disposition=0, ready=1, entry=0x58D0, join=False
    ):
        """Reset synthetic state, invoke one entry and check its execution bound."""
        self.uc.mem_write(RAM, bytes(8192))
        for address, value in ((RAM + 0x7D4, bits), (RAM + 0x878, height)):
            self.uc.mem_write(address, struct.pack("<I", value))
        self.uc.mem_write(RAM + 0x88D, bytes([mode]))
        self.uc.mem_write(RAM + 0x106, struct.pack("<H", 1040))
        self.uc.mem_write(RAM + 0x7EB, bytes((15, 20)))
        self.uc.mem_write(self.symbols["test_result"], bytes([disposition]))
        self.uc.mem_write(self.symbols["test_ready"], bytes([ready]))
        for index, register in enumerate(REGISTERS[:13]):
            self.uc.reg_write(register, 0xABCD0000 + index)
        self.uc.reg_write(arm.UC_ARM_REG_SP, STACK)
        self.uc.reg_write(arm.UC_ARM_REG_LR, RETURN | 1)
        self.uc.reg_write(arm.UC_ARM_REG_APSR, 0xB0000000)
        self.uc.reg_write(arm.UC_ARM_REG_PRIMASK, mask)
        self.electrical.clear()
        self.writes.clear()
        stop = FLASH + 0x58DC if join else RETURN
        self.stop = stop
        self.uc.emu_start(FLASH + entry + 1, stop, count=20000)
        if self.uc.reg_read(arm.UC_ARM_REG_PC) != stop:
            raise RuntimeError("Native save did not reach its expected continuation")
        return [self.uc.reg_read(register) for register in REGISTERS], list(self.writes)

    def capture(self):
        """Read the test adapter's received snapshot and mask."""
        return bytes(self.uc.mem_read(self.symbols["test_bytes"], 6)), int.from_bytes(
            self.uc.mem_read(self.symbols["test_mask"], 4), "little"
        )


class NativeSaveTest(unittest.TestCase):
    """Verify request-time bytes, native pass-through and caller safety stores."""

    @classmethod
    def setUpClass(cls):
        """Compile the target and optionally bind the exact FP01 image."""
        cls.sections, cls.symbols, cls.manifest = compile_native()
        cls.image = NATIVE_IMAGE.read_bytes() if NATIVE_IMAGE else None
        if cls.image is not None and (
            hashlib.sha256(cls.image).hexdigest() != IMAGE_SHA256
            or cls.image[0x58D0:0x58DC] != PROLOGUE
        ):
            raise ValueError("Native image is not the pinned exact FP01")
        cls.native = Machine(cls.sections, cls.symbols, False, cls.image)
        cls.patched = Machine(cls.sections, cls.symbols, True, cls.image)

    def test_capture_snapshot_and_dispositions(self):
        """Both captured dispositions preserve all exponents, signs and mode bytes."""
        cases = [
            (sign | (exponent << 23) | mantissa, 0xFEDCBA98, exponent)
            for exponent in range(256)
            for sign in (0, 0x80000000)
            for mantissa in (0, 0x7FFFFF)
        ]
        generator = random.Random(0x58D0)
        cases += [
            (
                generator.getrandbits(32),
                generator.getrandbits(32),
                generator.randrange(256),
            )
            for _ in range(512)
        ]
        for bits, height, mode in cases:
            for mask in (0, 1):
                for disposition in (0, 2):
                    registers, _ = self.patched.run(
                        bits, height, mode, mask, disposition
                    )
                    self.assertEqual(
                        self.patched.capture(),
                        (expected_snapshot(bits, height, mode), 1),
                    )
                    self.assertEqual(self.patched.electrical, [])
                    self.assertEqual(
                        registers[3], 0xABCD0000 | (10 if mode == 2 else mode)
                    )
                    self.assertEqual(
                        registers[4:13], [0xABCD0000 + index for index in range(4, 13)]
                    )
                    self.assertEqual(registers[13], STACK)
                    self.assertEqual(registers[-1], 0)
                    self.assertEqual(self.patched.uc.mem_read(RAM + 0x88D, 1)[0], mode)

    def test_passthrough_native_join(self):
        """The displaced prologue's registers, flags, mask and frame match exactly."""
        for mask in (0, 1):
            for ready in (0, 1):
                for bits in (0, 0x3F800000, 0xFFFFFFFF, 0x80000000):
                    original = self.native.run(
                        bits, 0x1234, 2, mask, 1, ready, join=True
                    )
                    actual = self.patched.run(
                        bits, 0x1234, 2, mask, 1, ready, join=True
                    )
                    self.assertEqual(actual, original)
                    self.assertEqual(
                        self.patched.uc.mem_read(STACK - 8, 8),
                        self.native.uc.mem_read(STACK - 8, 8),
                    )
                    calls = int.from_bytes(
                        self.patched.uc.mem_read(self.symbols["test_calls"], 4),
                        "little",
                    )
                    self.assertEqual(calls, ready)

    def test_exact_native_passthrough(self):
        """Complete native saves keep registers, writes and helper order unchanged."""
        if self.image is None:
            self.skipTest("Pass --native-image for private exact-image comparison")
        for mode in range(256):
            bits = (mode << 23) | 0x807FFFFF
            for mask in (0, 1):
                original = self.native.run(bits, 0xFEDCBA98, mode, mask, 1)
                actual = self.patched.run(bits, 0xFEDCBA98, mode, mask, 1)
                self.assertEqual(actual, original)
                self.assertEqual(self.patched.electrical, self.native.electrical)
                writes = [
                    (address, value)
                    for helper, value, address in self.native.electrical
                    if helper == FLASH + 0x4938
                ]
                self.assertEqual(
                    bytes(value for _, value in writes),
                    expected_snapshot(bits, 0xFEDCBA98, mode),
                )

    def test_exact_native_callers(self):
        """Capture retains low-supply output cuts, rearm latch and rs_complete effects."""
        if self.image is None:
            self.skipTest("Pass --native-image for private exact-image callers")
        for entry in (0x3538, 0x946C):
            for mask in (0, 1):
                original, original_writes = self.native.run(
                    0x43FA0000, 0x1234, 0, mask, 1, entry=entry
                )
                for disposition in (0, 2):
                    actual, actual_writes = self.patched.run(
                        0x43FA0000, 0x1234, 0, mask, disposition, entry=entry
                    )
                    self.assertEqual(actual_writes, original_writes)
                    self.assertEqual(actual[3:14], original[3:14])
                    self.assertEqual(actual[-1], 0)
                    self.assertEqual(self.patched.electrical, [])
                    self.assertEqual(
                        self.patched.capture(),
                        (expected_snapshot(0x43FA0000, 0x1234, 0), 1),
                    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--native-image", type=Path)
    parser.add_argument("--report", type=Path)
    arguments, remaining = parser.parse_known_args()
    LLVM_BIN, NATIVE_IMAGE, REPORT = (
        arguments.llvm_bin,
        arguments.native_image,
        arguments.report,
    )
    program = unittest.main(argv=[__file__, *remaining], exit=False)
    if REPORT:
        REPORT.write_text(
            json.dumps(
                {
                    "result": "passed" if program.result.wasSuccessful() else "failed",
                    "tests": program.result.testsRun,
                    "skipped": len(program.result.skipped),
                    "native_image_sha256": IMAGE_SHA256 if NATIVE_IMAGE else None,
                    "test_sha256": hashlib.sha256(
                        Path(__file__).read_bytes()
                    ).hexdigest(),
                    "build": getattr(NativeSaveTest, "manifest", None),
                },
                indent=2,
            )
            + "\n"
        )
    raise SystemExit(not program.result.wasSuccessful())
