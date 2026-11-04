#!/usr/bin/env python3
"""Link trim modules for Cortex-M0 and execute their runtime helpers.

This is a synthetic instruction test. It installs no firmware and exercises no
EEPROM. Native division symbols are pinned addresses, not emulated substitutes.
"""

import argparse
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

from unicorn import UC_ARCH_ARM, UC_MODE_THUMB, Uc, arm_const as arm

from build_reset_speed_patch import find_tool

PATCHES = Path(__file__).resolve().parent / "patches"
LLVM_BIN = None
FLASH = 0x08000000
CODE = FLASH + 0xA010
RAM = 0x20000000
STACK = RAM + 0xF00
RETURN = FLASH + 0xFBE0
MODULES = ("core", "storage", "owner", "i2c", "adapter", "runtime")
FLAGS = (
    "--target=arm-none-eabi",
    "-mcpu=cortex-m0",
    "-mthumb",
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
)


class TrimRuntimeTest(unittest.TestCase):
    """Check compiler helper ABI, memory boundaries and complete module linkage."""

    @classmethod
    def setUpClass(cls):
        """Build the actual trim objects with the pinned production compiler."""
        cls.directory = tempfile.TemporaryDirectory(
            prefix="t150-trim-runtime-", dir=os.environ["TMPDIR"]
        )
        cls.addClassCleanup(cls.directory.cleanup)
        work = Path(cls.directory.name)
        tools = {
            name: find_tool(name, LLVM_BIN)
            for name in ("clang", "ld.lld", "llvm-nm", "llvm-objcopy")
        }
        for tool in tools.values():
            version = subprocess.check_output([tool, "--version"], text=True)
            if "23.1.0" not in version:
                raise RuntimeError("Trim runtime tests require LLVM 23.1.0")
        objects = []
        cls.frames = {}
        for module in MODULES:
            source = PATCHES / f"motion_trim_{module}.c"
            obj = work / f"{module}.o"
            subprocess.run(
                [tools["clang"], *FLAGS, "-c", source, "-o", obj], check=True
            )
            objects.append(obj)
            cls.frames[module] = obj.with_suffix(".su").read_text()
        cls.runtime_unresolved = subprocess.check_output(
            [tools["llvm-nm"], "--undefined-only", objects[-1]], text=True
        )
        linker = work / "trim.ld"
        linker.write_text(
            "ENTRY(trim_adapter_poll)\n"
            "__aeabi_uidiv = 0x080028f5;\n"
            "__aeabi_uidivmod = __aeabi_uidiv;\n"
            "SECTIONS {\n"
            f".text {CODE} : {{ *(.text*) *(.rodata*) }}\n"
            "/DISCARD/ : { *(.ARM.attributes) *(.comment) "
            "*(.ARM.exidx*) *(.ARM.extab*) }\n"
            "}\n"
            f'ASSERT(ADDR(.text) + SIZEOF(.text) <= {RETURN}, "flash overflow")\n'
            'ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "unexpected RAM")\n'
        )
        elf = work / "trim.elf"
        subprocess.run([tools["ld.lld"], "-T", linker, *objects, "-o", elf], check=True)
        cls.unresolved = subprocess.check_output(
            [tools["llvm-nm"], "--undefined-only", elf], text=True
        )
        listing = subprocess.check_output(
            [tools["llvm-nm"], "--defined-only", elf], text=True
        )
        cls.symbols = {
            fields[2]: int(fields[0], 16)
            for line in listing.splitlines()
            if len(fields := line.split()) == 3
        }
        binary = work / "trim.bin"
        subprocess.run([tools["llvm-objcopy"], "-O", "binary", elf, binary], check=True)
        cls.code = binary.read_bytes()
        # Include the existing platform in a separate size/link check. This
        # does not connect the new trim code to its live hooks.
        platform_objects = []
        for source_name in (
            "tb01_core.c",
            "platform_runtime.c",
            "platform_tb01.c",
            "tb01_hooks.S",
            "platform_boot.S",
        ):
            obj = work / (source_name + ".o")
            flags = FLAGS if source_name.endswith(".c") else FLAGS[:3]
            subprocess.run(
                [tools["clang"], *flags, "-c", PATCHES / source_name, "-o", obj],
                check=True,
            )
            platform_objects.append(obj)
        combined = work / "platform-trim.elf"
        subprocess.run(
            [
                tools["ld.lld"],
                "-L",
                PATCHES,
                "-T",
                PATCHES / "platform_patch.ld",
                *platform_objects,
                *objects,
                "-o",
                combined,
            ],
            check=True,
        )
        cls.combined_unresolved = subprocess.check_output(
            [tools["llvm-nm"], "--undefined-only", combined], text=True
        )
        combined_binary = work / "platform-trim-code.bin"
        subprocess.run(
            [
                tools["llvm-objcopy"],
                "-O",
                "binary",
                "--only-section=.candidate",
                combined,
                combined_binary,
            ],
            check=True,
        )
        cls.combined_code_size = combined_binary.stat().st_size

    def setUp(self):
        """Give each test fresh synthetic flash and eight KiB of SRAM."""
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_M0)
        self.uc.reg_write(arm.UC_ARM_REG_XPSR, 0x01000000)
        self.uc.mem_map(FLASH, 0x10000)
        self.uc.mem_map(RAM, 0x2000)
        self.uc.mem_write(CODE, self.code)

    def call(self, name, arguments, mask=0):
        """Call real Thumb code and check preserved registers and bounded return."""
        preserved = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(4, 12)]
        values = [0x12340000 + i for i in range(len(preserved))]
        for register, value in zip(preserved, values, strict=True):
            self.uc.reg_write(register, value)
        for i, value in enumerate(arguments):
            self.uc.reg_write(getattr(arm, f"UC_ARM_REG_R{i}"), value)
        self.uc.reg_write(arm.UC_ARM_REG_SP, STACK)
        self.uc.reg_write(arm.UC_ARM_REG_LR, RETURN | 1)
        self.uc.reg_write(arm.UC_ARM_REG_PRIMASK, mask)
        self.uc.emu_start(self.symbols[name] | 1, RETURN, count=10000)
        self.assertEqual(self.uc.reg_read(arm.UC_ARM_REG_PC), RETURN)
        self.assertEqual(self.uc.reg_read(arm.UC_ARM_REG_SP), STACK)
        self.assertEqual(self.uc.reg_read(arm.UC_ARM_REG_PRIMASK), mask)
        for register, value in zip(preserved, values, strict=True):
            self.assertEqual(self.uc.reg_read(register), value)

    def test_complete_link(self):
        """All six trim modules link without implicit runtime dependencies."""
        self.assertEqual(self.runtime_unresolved.strip(), "")
        self.assertEqual(self.unresolved.strip(), "")
        self.assertEqual(self.combined_unresolved.strip(), "")
        self.assertLessEqual(CODE + self.combined_code_size, RETURN)
        absolute_helpers = {
            name: value
            for name, value in self.symbols.items()
            if name.startswith("__aeabi_") and not CODE <= value < RETURN
        }
        self.assertEqual(
            absolute_helpers,
            {"__aeabi_uidiv": 0x080028F5, "__aeabi_uidivmod": 0x080028F5},
        )
        for module in MODULES:
            self.assertIn("\tstatic", self.frames[module])

    def test_memory_helpers(self):
        """Copy and clear preserve surrounding bytes at each alignment and length."""
        original = bytes((i * 37 + 11) % 256 for i in range(1024))
        for name in (
            "__aeabi_memcpy",
            "__aeabi_memcpy4",
            "__aeabi_memclr4",
            "__aeabi_memclr8",
        ):
            for destination_offset in range(4):
                for source_offset in range(4):
                    for length in (0, 1, 2, 3, 4, 7, 16, 36, 72, 92, 132, 160):
                        self.uc.mem_write(RAM, original)
                        destination = 512 + destination_offset
                        source = 64 + source_offset
                        expected = bytearray(original)
                        if name.startswith("__aeabi_memclr"):
                            expected[destination : destination + length] = bytes(length)
                            arguments = [RAM + destination, length]
                        else:
                            expected[destination : destination + length] = original[
                                source : source + length
                            ]
                            arguments = [RAM + destination, RAM + source, length]
                        self.call(name, arguments, mask=length % 2)
                        self.assertEqual(self.uc.mem_read(RAM, 1024), expected)
        # Size zero must not dereference either pointer.
        self.call("__aeabi_memcpy", [0, 0, 0])
        self.call("__aeabi_memcpy4", [0, 0, 0])
        self.call("__aeabi_memclr4", [0, 0])
        self.call("__aeabi_memclr8", [0, 0])

    def test_multiply(self):
        """The real helper returns the low 64 product bits across carries and signs."""
        mask = (1 << 64) - 1
        edges = [0, 1, 2, 0xFFFF, 0xFFFFFFFF, 1 << 32, 1 << 63, mask - 1, mask]
        cases = [(left, right) for left in edges for right in edges]
        generator = random.Random(0x545231)
        cases.extend(
            (generator.getrandbits(64), generator.getrandbits(64)) for _ in range(512)
        )
        for left, right in cases:
            self.call(
                "__aeabi_lmul",
                [left & 0xFFFFFFFF, left >> 32, right & 0xFFFFFFFF, right >> 32],
                mask=left % 2,
            )
            result = self.uc.reg_read(arm.UC_ARM_REG_R0) | (
                self.uc.reg_read(arm.UC_ARM_REG_R1) << 32
            )
            self.assertEqual(result, (left * right) & mask)


def main():
    """Run runtime instruction tests with an explicitly selected LLVM directory."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    args, remaining = parser.parse_known_args()
    LLVM_BIN = args.llvm_bin
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
