#!/usr/bin/env python3
"""Compare four linked trim wrappers with exact native arithmetic spans offline."""

import argparse
from pathlib import Path
import random
import struct
import unittest

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_WRITE, UC_MODE_THUMB, Uc, arm_const as arm

from compile_motion_trim import TARGET_HOOKS, compile_patch

LLVM_BIN = None
FLASH = 0x08000000
RAM = 0x20000000
STACK = RAM + 0xE80
CODE = FLASH + 0xA010
MOTION = RAM + 0x1050
REGISTERS = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(13)] + [
    arm.UC_ARM_REG_SP,
    arm.UC_ARM_REG_LR,
    arm.UC_ARM_REG_APSR,
    arm.UC_ARM_REG_PRIMASK,
]
# Exact-image anchors are the source contract, not a complete manufacturer image.
PRESET = bytes.fromhex("49890a1d904201ddc01d03e0091f884201da801fe060")
DIRECT = bytes.fromhex("009804990a1d904201dd073003e00439884200da0638")


def reference(requested, current, values, speed):
    """Apply signed native branch ordering and the documented trim-only clamp."""

    def signed(value):
        value &= 0xFFFFFFFF
        return value if value < 0x80000000 else value - 0x100000000

    up = signed(requested) > signed(current + 4)
    down = not up and signed(requested) < signed(current - 4)
    base = (requested + (7 if up else -6 if down else 0)) & 0xFFFFFFFF
    if not (up or down) or not 6 <= base <= 747:
        return base
    correction = values[2 * speed + (0 if up else 1)]
    return min(747, max(6, base + (correction if up else -correction)))


class TargetMachine:
    """Execute the actual linked C/assembly with synthetic RAM and MCU registers."""

    def __init__(self, sections, patched):
        """Map the platform code and either the original or replacement spans."""
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_M0)
        self.uc.reg_write(arm.UC_ARM_REG_XPSR, 0x01000000)
        for address, size in (
            (FLASH, 0x10000),
            (RAM, 0x2000),
            (0x40021000, 0x1000),
            (0x40015000, 0x1000),
            (0x1FFFF000, 0x1000),
            (0x48001000, 0x1000),
        ):
            self.uc.mem_map(address, size)
        self.uc.mem_write(CODE, sections["candidate"])
        for name, (start, _) in TARGET_HOOKS.items():
            data = (
                sections[name]
                if patched
                else DIRECT
                if name.endswith("direct")
                else PRESET
            )
            self.uc.mem_write(FLASH + start, data)
        self.writes = []
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.record_write)

    def record_write(self, uc, access, address, size, value, context):
        """Keep externally visible writes and reject stack spill into native data."""
        if RAM + 0xAC4 <= address < STACK:
            return
        self.writes.append((address, size, value))

    def run(self, name, requested, current, values=(0, 0, 0, 0), speed=0, changes=None):
        """Reset each case and return registers and non-stack writes at the join."""
        self.uc.mem_write(RAM, bytes(8192))

        def word(address, value):
            self.uc.mem_write(address, struct.pack("<I", value))

        word(0x40021018, 0x00400000)
        word(0x40015800, 0x440)
        word(0x1FFFF7CC, 64)
        for address, value in (
            (RAM + 0x1000, 0x46503031),
            (RAM + 0x1004, 1),
            (RAM + 0x1008, 1),
            (RAM + 0x100C, 1),
            (MOTION + 4, 0x54523031),
            (RAM + 0xFE4, 0x52533031),
            (RAM + 0x850, 24),
            (RAM + 0x854, 39),
            (RAM + 0x160, 6),
            (RAM + 0x15C, 747),
            (RAM + 0xA9C, requested),
        ):
            word(address, value)
        for address in range(RAM + 0x13F0, RAM + 0x1400, 4):
            word(address, 0x51A7C38D)
        self.uc.mem_write(MOTION, bytes(value & 0xFF for value in values))
        self.uc.mem_write(RAM + 0xFE2, bytes([speed]))
        self.uc.mem_write(RAM + 0x88D, b"\x01" if "preset" in name else b"\x00")
        self.uc.mem_write(RAM + 0x76A, struct.pack("<H", current & 0xFFFF))
        word(STACK, requested)
        word(STACK + 16, current)
        for address, data in (changes or {}).items():
            self.uc.mem_write(address, data)
        for i, register in enumerate(REGISTERS[:13]):
            self.uc.reg_write(register, 0xABCD0000 + i)
        self.uc.reg_write(arm.UC_ARM_REG_R0, requested)
        self.uc.reg_write(arm.UC_ARM_REG_R1, RAM + 0x760)
        self.uc.reg_write(arm.UC_ARM_REG_R4, RAM + 0xA90)
        self.uc.reg_write(arm.UC_ARM_REG_SP, STACK)
        self.uc.reg_write(arm.UC_ARM_REG_LR, FLASH + 0x1235)
        self.uc.reg_write(arm.UC_ARM_REG_APSR, 0xA8000000)
        self.uc.reg_write(arm.UC_ARM_REG_PRIMASK, requested % 2)
        self.writes.clear()
        start, end = TARGET_HOOKS[name]
        self.uc.emu_start(FLASH + start + 1, FLASH + end, count=20000)
        if self.uc.reg_read(arm.UC_ARM_REG_PC) != FLASH + end:
            raise RuntimeError("Target hook did not reach its native continuation")
        return [self.uc.reg_read(register) for register in REGISTERS], list(self.writes)


class MotionHookTest(unittest.TestCase):
    """Verify target stores, zero equivalence and the ordinary-motion gates."""

    @classmethod
    def setUpClass(cls):
        """Compile all integration code once with its actual linker layout."""
        cls.sections, cls.symbols, cls.manifest = compile_patch(LLVM_BIN)

    def setUp(self):
        """Use independent original and patched machines for each property."""
        self.native = TargetMachine(self.sections, False)
        self.patched = TargetMachine(self.sections, True)

    def test_zero_trim_equivalence(self):
        """Zero trims preserve every register, flag and non-stack write at all joins."""
        edges = (0, 4, 6, 743, 747, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
        cases = [(request, current) for request in edges for current in edges]
        generator = random.Random(0x54523031)
        cases += [
            (generator.getrandbits(32), generator.getrandbits(32)) for _ in range(128)
        ]
        for name in TARGET_HOOKS:
            for requested, current in cases:
                with self.subTest(hook=name, request=requested, current=current):
                    self.assertEqual(
                        self.patched.run(name, requested, current),
                        self.native.run(name, requested, current),
                    )

    def test_four_trims_and_native_scratch_state(self):
        """Every trim value uses its own speed/direction and keeps native scratch state."""
        for name in TARGET_HOOKS:
            for speed in (0, 1):
                for trim in range(-3, 4):
                    values = (trim, -trim, -trim, trim)
                    for requested, current in (
                        (500, 200),
                        (200, 500),
                        (6, 200),
                        (740, 200),
                    ):
                        actual, writes = self.patched.run(
                            name, requested, current, values, speed
                        )
                        original, original_writes = self.native.run(
                            name, requested, current
                        )
                        expected = reference(requested, current, values, speed)
                        self.assertEqual(actual[0], expected)
                        self.assertEqual(actual[1:], original[1:])
                        self.assertEqual(
                            writes,
                            [
                                (address, size, expected)
                                for address, size, _ in original_writes
                            ],
                        )

    def test_dead_zone_skips_preset_store(self):
        """A nonzero trim cannot create the preset's skipped second target write."""
        for name in TARGET_HOOKS:
            for delta in range(-4, 5):
                requested = 300 + delta
                actual, writes = self.patched.run(name, requested, 300, (3, -3, 3, -3))
                self.assertEqual(actual[0], requested)
                self.assertEqual(writes, [])

    def test_ineligible_states_keep_original_target(self):
        """Invalid magic/profile/speed and calibration or protection states bypass trims."""
        changes = [
            {MOTION + 4: bytes(4)},
            {RAM + 0xFE4: bytes(4)},
            {RAM + 0xFE2: b"\x02"},
            {RAM + 0x88D: b"\x02"},
            {RAM + 0x850: struct.pack("<I", 25)},
            {RAM + 0x854: struct.pack("<I", 40)},
            {RAM + 0x160: struct.pack("<I", 7)},
            {RAM + 0x15C: struct.pack("<I", 748)},
        ]
        changes += [{RAM + 0xFE0: bytes([state])} for state in (1, 2, 3, 5)]
        changes += [{RAM + offset: b"\x01"} for offset in (0x88F, 0x890, 0x8B8, 0x148)]
        changes += [{MOTION: b"\x04\x00\x00\x00"}]
        for name in TARGET_HOOKS:
            for change in changes:
                actual = self.patched.run(name, 500, 200, (3, 3, 3, 3), changes=change)
                self.assertEqual(
                    actual, self.native.run(name, 500, 200, changes=change)
                )

    def test_tuple_publication(self):
        """Publish one aligned tuple word and reject invalid input unchanged."""
        machine = self.patched
        machine.run("hook_trim_direct", 500, 200)
        for values in ((-3, 3, -2, 2), (0, 0, 0, 0), (-4, 0, 0, 0), (0, 0, 0, 4)):
            valid = all(-3 <= value <= 3 for value in values)
            source = RAM + 0x600
            encoded = bytes(value & 0xFF for value in values)
            machine.uc.mem_write(source, encoded)
            before = bytes(machine.uc.mem_read(MOTION, 8))
            machine.uc.reg_write(arm.UC_ARM_REG_R0, source)
            machine.uc.reg_write(arm.UC_ARM_REG_SP, STACK)
            machine.uc.reg_write(arm.UC_ARM_REG_LR, FLASH + 0xF801)
            machine.uc.reg_write(arm.UC_ARM_REG_PRIMASK, 1)
            machine.writes.clear()
            machine.uc.emu_start(
                self.symbols["trim_motion_publish"] | 1, FLASH + 0xF800, count=2000
            )
            self.assertEqual(machine.uc.reg_read(arm.UC_ARM_REG_PC), FLASH + 0xF800)
            self.assertEqual(machine.uc.reg_read(arm.UC_ARM_REG_PRIMASK), 1)
            self.assertEqual(machine.uc.reg_read(arm.UC_ARM_REG_R0), int(valid))
            if valid:
                self.assertEqual(
                    machine.writes,
                    [
                        (MOTION, 4, int.from_bytes(encoded, "little")),
                        (MOTION + 4, 4, 0x54523031),
                    ],
                )
            else:
                self.assertEqual(machine.writes, [])
                self.assertEqual(bytes(machine.uc.mem_read(MOTION, 8)), before)


def main():
    """Run target-wrapper instruction checks with the selected pinned toolchain."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    args, remaining = parser.parse_known_args()
    LLVM_BIN = args.llvm_bin
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
