#!/usr/bin/env python3
"""Execute foreground exclusion and GPIO wrappers as Cortex-M0 machine code."""

import argparse
import hashlib
import json
import os
from pathlib import Path
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
CODE = FLASH + 0xC000
STACK = RAM + 0xE80
RETURN = FLASH + 0xFFF0
FIXTURE = RAM + 0x1500
IMAGE_SHA256 = "6a2ac33ba77713c9de9d675be4e2629af229e4110f78d7c35defbb1d13555992"
SELECTOR = bytes.fromhex("2178002914d0012922d00229f8d1")
MAIN = bytes.fromhex("1048fef788f9fdf7faf8fbf750fcfaf7acfefaf7aefefcf796fd00f0e6fed7e7")
FIXED = {
    "service": 0x6984,
    "uart": 0x586C,
    "display": 0x3F1C,
    "protect": 0x33D8,
    "rearm": 0x33E0,
    "physical": 0x51B4,
    "motion": 0x9458,
    "cl01": 0x8CA8,
}
HOOKS = {
    "hook_trim_gpio": 0x438C,
    "hook_trim_selector": 0x863E,
    "hook_trim_foreground": 0x8684,
}
LLVM_BIN = None
NATIVE_IMAGE = None
REPORT = None
REGISTERS = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(13)] + [
    arm.UC_ARM_REG_SP,
    arm.UC_ARM_REG_LR,
    arm.UC_ARM_REG_APSR,
    arm.UC_ARM_REG_PRIMASK,
]
FIXTURE_ASSEMBLY = """
.syntax unified
.cpu cortex-m0
.thumb
.section .text.fixture,"ax",%progbits
.global trim_controller_blocks_native
.thumb_func
trim_controller_blocks_native:
  ldr r0, =0x20001500
  ldr r0, [r0]
  b fixture_clobber
.global trim_controller_inputs_blocked
.thumb_func
trim_controller_inputs_blocked:
  ldr r0, =0x20001504
  ldr r0, [r0]
  b fixture_clobber
.global trim_controller_poll
.thumb_func
trim_controller_poll:
  ldr r0, =0x20001508
  ldr r1, [r0]
  cmp r1, #0
  beq fixture_clobber
  subs r1, #1
  ldr r0, =0x20001500
  str r1, [r0]
fixture_clobber:
  movs r1, #0xa5
  movs r2, #0x5a
  movs r3, #0xff
  mov r12, r3
  cmp r1, #0
  bx lr

.thumb_func
test_service_body:
.thumb_func
test_uart_body:
.thumb_func
test_display_body:
.thumb_func
test_physical_body:
.thumb_func
test_motion_body:
  bx lr
.thumb_func
test_protect_body:
  ldr r0, =0x20001510
  ldr r0, [r0]
  cmp r0, #0
  beq 1f
  ldr r0, =0x2000088d
  movs r1, #9
  strb r1, [r0]
  ldr r0, =0x4000043c
  movs r1, #0
  str r1, [r0]
  str r1, [r0,#4]
1: bx lr
.thumb_func
test_rearm_body:
  ldr r0, =0x20000760
  ldrh r0, [r0,#12]
  ldr r1, =0x200007ec
  ldrb r1, [r1]
  cmp r0, r1
  blo 2f
  ldr r0, =0x20000892
  movs r1, #0
  strb r1, [r0]
2: bx lr
.thumb_func
test_cl01_body:
  push {r4,r5,r6,lr}
  mov r4, r0
  mov r5, r1
  ldr r0, =0x2000150c
  ldr r0, [r0]
  ldrh r2, [r4,#16]
  cmp r0, #0
  beq 5f
  ldr r0, =0x48000000
  cmp r4, r0
  beq 3f
  ldr r0, =0x48000400
  cmp r4, r0
  bne 5f
  ldr r0, =0xc000
  b 4f
3: ldr r0, =0x1f00
4: orrs r2, r0
5: movs r0, #0
  tst r2, r5
  beq 6f
  movs r0, #1
6: mov r1, r5
  pop {r4,r5,r6,pc}
.balign 4
.ltorg
"""


def compile_foreground():
    """Link the wrappers with explicit controller and CL01 test doubles."""
    tools = {
        name: find_tool(name, LLVM_BIN)
        for name in ("clang", "ld.lld", "llvm-objcopy", "llvm-nm")
    }
    versions = {
        name: subprocess.check_output([tool, "--version"], text=True)
        for name, tool in tools.items()
    }
    if any(not re.search(r"\b23\.1\.0\b", version) for version in versions.values()):
        raise ValueError("Foreground tests require LLVM 23.1.0")
    with tempfile.TemporaryDirectory(
        prefix="t150-trim-foreground-", dir=os.environ["TMPDIR"]
    ) as directory:
        work = Path(directory)
        fixture = FIXTURE_ASSEMBLY
        for name in FIXED:
            fixture += f'\n.section .stub_{name},"ax",%progbits\n ldr r3, 1f\n bx r3\n .balign 4\n1: .word test_{name}_body\n'
        (work / "fixture.S").write_text(fixture)
        layout = ["SECTIONS {"]
        for name, offset in {
            **HOOKS,
            **{"stub_" + name: offset for name, offset in FIXED.items()},
        }.items():
            layout.append(f" .{name} {FLASH + offset:#x} : {{ *(.{name}) }}")
        layout += [
            f" .candidate {CODE:#x} : {{ *(.text*) *(.rodata*) }}",
            " /DISCARD/ : { *(.ARM.attributes) *(.comment) }",
            "}",
        ]
        for name in HOOKS:
            layout.append(f'ASSERT(SIZEOF(.{name}) == 4, "{name} size")')
        (work / "layout.ld").write_text("\n".join(layout) + "\n")
        objects = []
        for source in (PATCHES / "motion_trim_foreground.S", work / "fixture.S"):
            output = work / (source.name + ".o")
            subprocess.run(
                [
                    tools["clang"],
                    "--target=arm-none-eabi",
                    "-mcpu=cortex-m0",
                    "-mthumb",
                    "-c",
                    source,
                    "-o",
                    output,
                ],
                check=True,
            )
            objects.append(output)
        elf = work / "foreground.elf"
        subprocess.run(
            [tools["ld.lld"], "-T", work / "layout.ld", *objects, "-o", elf],
            check=True,
            capture_output=True,
        )
        if subprocess.check_output(
            [tools["llvm-nm"], "--undefined-only", elf], text=True
        ).strip():
            raise ValueError("Unresolved foreground symbol")
        listing = subprocess.check_output(
            [tools["llvm-nm"], "--defined-only", elf], text=True
        )
        symbols = {
            fields[2]: int(fields[0], 16)
            for line in listing.splitlines()
            if len(fields := line.split()) == 3
        }
        sections = {}
        for name in (*HOOKS, *("stub_" + name for name in FIXED), "candidate"):
            output = work / (name + ".bin")
            subprocess.run(
                [
                    tools["llvm-objcopy"],
                    "-O",
                    "binary",
                    "--only-section=." + name,
                    elf,
                    output,
                ],
                check=True,
            )
            sections[name] = output.read_bytes()
    manifest = {
        "toolchain": versions,
        "section_sizes": {name: len(value) for name, value in sections.items()},
        "source_sha256": hashlib.sha256(
            (PATCHES / "motion_trim_foreground.S").read_bytes()
        ).hexdigest(),
    }
    return sections, symbols, manifest


class Machine:
    """Run linked code with explicit fixtures or the pinned native image."""

    def __init__(self, sections, symbols, patched, image=None):
        """Map firmware and install only the selected hook sections."""
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_M0)
        self.uc.reg_write(arm.UC_ARM_REG_XPSR, 0x01000000)
        for address, size in (
            (FLASH, 65536),
            (RAM, 8192),
            (0x40000000, 0x30000),
            (0x48000000, 0x10000),
        ):
            self.uc.mem_map(address, size)
        self.uc.mem_write(FLASH, image or bytes(65536))
        self.uc.mem_write(CODE, sections["candidate"])
        if image is None:
            self.uc.mem_write(FLASH + 0x863E, SELECTOR)
            self.uc.mem_write(FLASH + 0x866E, MAIN)
            self.uc.mem_write(FLASH + 0x86B0, struct.pack("<I", RAM + 0xD8))
            self.uc.mem_write(FLASH + 0x4388, bytes.fromhex("004a1047a98c0008"))
            for name, offset in FIXED.items():
                self.uc.mem_write(FLASH + offset, sections["stub_" + name])
        if patched:
            for name, offset in HOOKS.items():
                self.uc.mem_write(FLASH + offset, sections[name])
        self.symbols = symbols
        self.trace = []
        self.writes = []
        self.stops = set()
        self.skip_first = None
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.store)

    def code(self, uc, address, size, context):
        """Observe calls and stop at explicit joins without replacing bodies."""
        if address == self.skip_first:
            self.skip_first = None
        elif address in self.stops:
            uc.emu_stop()
            return
        self.trace.append(address)
        if address in (
            FLASH + value for value in (0x488C, 0x4938, 0x37DC, 0x3BAC, 0x84F8, 0x7568)
        ):
            raise RuntimeError("Excluded native storage/sleep path executed")

    def store(self, uc, access, address, size, value, context):
        """Keep visible native writes and exclude the wrapper stack/test state."""
        if RAM + 0xAC4 <= address < RAM + 0x1000 or FIXTURE <= address < FIXTURE + 0x20:
            return
        self.writes.append((address, size, value))

    def reset(self, blocked=0, inputs=0, locked=0, mask=0):
        """Initialize synthetic state and recognizable register values."""
        self.uc.mem_write(RAM, bytes(8192))
        for address, value in (
            (FIXTURE, blocked),
            (FIXTURE + 4, inputs),
            (FIXTURE + 12, locked),
            (RAM + 0xFC8, locked),
        ):
            self.uc.mem_write(address, struct.pack("<I", value))
        self.uc.mem_write(RAM + 0x7EC, b"\x14")
        self.uc.mem_write(RAM + 0x76C, struct.pack("<H", 25))
        self.uc.mem_write(RAM + 0x892, b"\x01")
        for index, register in enumerate(REGISTERS[:13]):
            self.uc.reg_write(register, 0xABCD0000 + index)
        self.uc.reg_write(arm.UC_ARM_REG_SP, STACK)
        self.uc.reg_write(arm.UC_ARM_REG_LR, RETURN | 1)
        self.uc.reg_write(arm.UC_ARM_REG_APSR, 0xB0000000)
        self.uc.reg_write(arm.UC_ARM_REG_PRIMASK, mask)
        self.trace.clear()
        self.writes.clear()

    def run(self, start, stops):
        """Run to a named join, guarding loop-back stops and an instruction cap."""
        self.stops = set(stops)
        self.skip_first = start if start in self.stops else None
        self.uc.emu_start(start | 1, RETURN, count=50000)
        if self.uc.reg_read(arm.UC_ARM_REG_PC) not in self.stops:
            raise RuntimeError("Foreground wrapper did not reach a named join")
        return [self.uc.reg_read(register) for register in REGISTERS]

    def main_registers(self):
        """Set native persistent main-loop values."""
        for register, value in (
            (arm.UC_ARM_REG_R4, RAM + 0x754),
            (arm.UC_ARM_REG_R5, 0x48001400),
            (arm.UC_ARM_REG_R6, 0),
        ):
            self.uc.reg_write(register, value)


class ForegroundTest(unittest.TestCase):
    """Check selector routing, continued service and seven-contact masking."""

    @classmethod
    def setUpClass(cls):
        """Compile the production assembly with explicit dependency fixtures."""
        cls.sections, cls.symbols, cls.manifest = compile_foreground()
        cls.image = NATIVE_IMAGE.read_bytes() if NATIVE_IMAGE else None
        if (
            cls.image is not None
            and hashlib.sha256(cls.image).hexdigest() != IMAGE_SHA256
        ):
            raise ValueError("Unexpected native image")
        cls.native = Machine(cls.sections, cls.symbols, False)
        cls.patched = Machine(cls.sections, cls.symbols, True)

    def test_selector_contract(self):
        """Blocked selectors route normally; unblocked loads and CMP flags match."""
        for selector in (0, 1, 2, 3, 255):
            for blocked in (0, 1):
                for mask in (0, 1):
                    self.patched.reset(blocked=blocked, mask=mask)
                    self.patched.main_registers()
                    self.patched.uc.mem_write(RAM + 0x754, bytes([selector]))
                    actual = self.patched.run(FLASH + 0x863E, (FLASH + 0x8642,))
                    self.native.reset(mask=mask)
                    self.native.main_registers()
                    self.native.uc.mem_write(
                        RAM + 0x754, bytes([0 if blocked else selector])
                    )
                    original = self.native.run(FLASH + 0x863E, (FLASH + 0x8642,))
                    self.assertEqual(actual[:14], original[:14])
                    self.assertEqual(actual[15:], original[15:])
                    expected = 0 if blocked else selector
                    target = (
                        0x866E
                        if expected == 0
                        else 0x868E
                        if expected == 1
                        else 0x864C
                        if expected == 2
                        else 0x863E
                    )
                    self.patched.run(FLASH + 0x8642, (FLASH + target,))
                    self.assertEqual(self.patched.uc.reg_read(arm.UC_ARM_REG_SP), STACK)
                    self.assertEqual(
                        self.patched.uc.mem_read(RAM + 0x754, 1)[0], selector
                    )

    def test_foreground_service_and_poll_transition(self):
        """Service/safety run, while the post-poll ownership decision gates actions."""
        for blocked, change, final in ((0, 0, 0), (1, 0, 1), (1, 1, 0), (0, 2, 1)):
            self.patched.reset(blocked=blocked)
            self.patched.main_registers()
            self.patched.uc.mem_write(FIXTURE + 8, struct.pack("<I", change))
            self.patched.uc.mem_write(FIXTURE + 16, struct.pack("<I", 1))
            registers = self.patched.run(FLASH + 0x866E, (FLASH + 0x863E,))
            for name in ("service", "uart", "display", "protect", "rearm"):
                self.assertIn(FLASH + FIXED[name], self.patched.trace)
            self.assertEqual(FLASH + FIXED["physical"] in self.patched.trace, not final)
            self.assertEqual(FLASH + FIXED["motion"] in self.patched.trace, not final)
            self.assertLess(
                self.patched.trace.index(self.symbols["trim_controller_poll"] & ~1),
                self.patched.trace.index(
                    self.symbols["trim_controller_blocks_native"] & ~1
                ),
            )
            self.assertEqual(registers[4:7], [RAM + 0x754, 0x48001400, 0])
            self.assertEqual(
                registers[7:14], [0xABCD0000 + i for i in range(7, 13)] + [STACK]
            )
            self.assertEqual(registers[-1], 0)
            self.assertEqual(self.patched.uc.mem_read(RAM + 0x88D, 1)[0], 9)
            self.assertEqual(self.patched.uc.mem_read(RAM + 0x892, 1)[0], 0)

    def test_gpio_contacts_and_safety(self):
        """Only seven contacts hide, independently of ownership and CL01 lock."""
        for locked in (0, 1):
            for inputs in (0, 1):
                for blocked in (0, 1):
                    for port, contact in (
                        (0x48000000, 0x1F00),
                        (0x48000400, 0xC000),
                        (0x48000800, 0),
                    ):
                        for raw in (0, 0xFFFF, 0x5555):
                            for predicate in (
                                *[1 << bit for bit in range(16)],
                                0x1F03,
                                0xC00F,
                            ):
                                self.patched.reset(blocked, inputs, locked)
                                self.patched.uc.mem_write(
                                    port + 16, struct.pack("<H", raw)
                                )
                                self.patched.uc.reg_write(arm.UC_ARM_REG_R0, port)
                                self.patched.uc.reg_write(arm.UC_ARM_REG_R1, predicate)
                                registers = self.patched.run(FLASH + 0x4388, (RETURN,))
                                observed = raw | (contact if locked or inputs else 0)
                                self.assertEqual(
                                    registers[:3],
                                    [
                                        int(bool(observed & predicate)),
                                        predicate,
                                        observed,
                                    ],
                                )
                                self.assertEqual(
                                    registers[4:14],
                                    [0xABCD0000 + i for i in range(4, 13)] + [STACK],
                                )
                                self.assertIn(FLASH + 0x8CA8, self.patched.trace)
                                self.assertEqual(
                                    self.patched.uc.mem_read(RAM + 0xFC8, 1)[0], locked
                                )

    def test_exact_native_service_prefix(self):
        """The owned real FP01 loop retains UART, display and protection effects."""
        if self.image is None:
            self.skipTest("Pass --native-image for exact native service paths")
        native = Machine(self.sections, self.symbols, False, self.image)
        patched = Machine(self.sections, self.symbols, True, self.image)
        for dma_status in (0, 2):
            for machine in (native, patched):
                machine.reset(blocked=1, inputs=1)
                machine.main_registers()
                machine.uc.mem_write(0x40020000, struct.pack("<I", dma_status))
            native.run(FLASH + 0x866E, (FLASH + 0x8684,))
            patched.run(FLASH + 0x866E, (FLASH + 0x863E,))
            self.assertEqual(patched.writes, native.writes)
            for address in (0x6984, 0x586C, 0x3F1C, 0x33D8, 0x33E0):
                self.assertIn(FLASH + address, patched.trace)
            self.assertNotIn(FLASH + 0x51B4, patched.trace)
            self.assertNotIn(FLASH + 0x9458, patched.trace)
            if dma_status:
                for address in (0x319C, 0x3A50, 0x39B8):
                    self.assertIn(FLASH + address, patched.trace)


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
                    "build": getattr(ForegroundTest, "manifest", None),
                    "scope": "Machine-code wrapper contracts; explicit controller/CL01 fixtures, optional exact native service prefix; no hardware timing or production profile.",
                },
                indent=2,
            )
            + "\n"
        )
    raise SystemExit(not program.result.wasSuccessful())
