#!/usr/bin/env python3
"""Test FP01 project instructions with synthetic flash and modeled native helpers.

The inherited TB01 fixture models RTC/UART helpers; no manufacturer code or
image is needed. Eight KiB RAM, profile MMIO and read-before-write tracking
extend that fixture. Actual startup/handoff tests use private exact-image
fixtures separately. These tests do not establish physical SRAM independence.
"""

import argparse
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, arm_const as arm_regs
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_PRIMASK,
    UC_ARM_REG_SP,
)

import build_platform_patch as builder
import build_reset_speed_patch as rs_builder
import build_tb01_patch as legacy_builder
import generate_platform_layout as layout_generator
from test_tb01_patch import FLASH, RAM, RETURN, SyntheticDesk

LLVM_BIN = None


def synthetic_source():
    """Construct only documented anchors and erased space, without vendor code."""
    source = bytearray(b"\xff" * builder.FULL_SIZE)
    for offset, contents in {**builder.SOURCE_ANCHORS, **builder.BOOT_ANCHORS}.items():
        source[offset : offset + len(contents)] = contents
    for offset, contents in builder.HOOKS.values():
        source[offset : offset + len(contents)] = contents
    return bytes(source)


class PlatformDesk(SyntheticDesk):
    """Extend modeled native helpers with FP01 RAM ownership and parity ordering."""

    def __init__(
        self, sections, symbols, legacy_sections, legacy_symbols, initialize=True
    ):
        """Load project instructions and mark upper SRAM dirty before startup."""
        self.platform_loaded = False
        super().__init__(legacy_sections, legacy_symbols)
        self.uc.mem_map(RAM + 0x1000, 0x1000)
        self.uc.mem_map(0x1FFFF000, 0x1000)
        self.sections, self.symbols = sections, symbols
        self.source = synthetic_source()
        self.image, self.spans = builder._apply_sections(
            self.source, 0, sections, symbols
        )
        self.uc.mem_write(FLASH, self.image)
        for address in self.callbacks:
            self.uc.mem_write(address, b"\x70\x47")
        self.callbacks[0x08004A98] = self._remap
        self.uc.mem_write(0x08004A98, b"\x70\x47")
        self.profile_identity = 0x10010440
        self.setreg(0x40015800, self.profile_identity)
        self.setreg(0x1FFFF7CC, 64)
        self.uc.mem_write(RAM + 0x1000, b"\xa5" * 0x1000)
        self.uc.mem_write(RAM + 0xFF0, b"\xc7" * 16)
        self.uc.ctl_remove_cache(FLASH, FLASH + builder.FULL_SIZE)
        self.platform_loaded = True
        self.written_words = set()
        self.upper_reads = []
        self.upper_writes = []
        self.remap_count = 0
        self.require_release_atomic = False
        self.clock_writes = []
        self.clock_events = []
        self.legacy_reads = []
        self.legacy_writes = []
        self.extra_project_ranges = []
        self.booting = True
        self.uc.hook_add(UC_HOOK_MEM_READ, self._read)
        self.call(FLASH + 0x85F4, stops={FLASH + 0x85F8})
        self.booting = False
        if initialize:
            self.call(FLASH + 0x9402, stops={FLASH + 0x9406})

    def setreg(self, address, value):
        """Set MMIO and retain the ID presented only while its clock is enabled."""
        if address == 0x40015800:
            self.profile_identity = value
        super().setreg(address, value)

    def _code(self, uc, address, size, data):
        if not self.platform_loaded:
            return super()._code(uc, address, size, data)
        self.lowest_sp = min(self.lowest_sp, uc.reg_read(UC_ARM_REG_SP))
        if address in self.stops:
            self.stopped = address
            uc.emu_stop()
        elif address in self.callbacks:
            self.callbacks[address]()
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif (
            not FLASH + builder.CODE_OFFSET
            <= address
            < FLASH + builder.LAYOUT["code_end"]
        ):
            if any(start <= address < end for start, end in self.extra_project_ranges):
                return
            if not any(
                FLASH + offset <= address < FLASH + offset + len(original)
                for offset, original in builder.HOOKS.values()
            ):
                raise AssertionError(f"Unexpected unmodeled native entry {address:#x}")

    def _read(self, uc, access, address, size, value, data):
        if RAM + 0xFC8 <= address < RAM + 0x1000:
            self.legacy_reads.append((address, size))
        if address in (0x40021018, 0x40015800, 0x1FFFF7CC):
            assert size == 4
            self.clock_events.append(("read", address))
        if address == 0x40015800:
            value = self.profile_identity if self.reg(0x40021018) & 0x00400000 else 0
            uc.mem_write(address, value.to_bytes(4, "little"))
        if RAM + 0x1000 <= address < RAM + 0x2000:
            if len(self.written_words) != 256:
                raise AssertionError(
                    "Upper SRAM read before complete word initialization"
                )
            if address + size > RAM + 0x1400:
                raise AssertionError("Upper read leaves the shared reservation")
            self.upper_reads.append((address, size))

    def _write(self, uc, access, address, size, value, data):
        if not self.platform_loaded:
            return super()._write(uc, access, address, size, value, data)
        if address == 0x40021018:
            assert size == 4 and uc.reg_read(UC_ARM_REG_PRIMASK) == 1
            self.clock_writes.append(value)
            self.clock_events.append(("write", address))
            return
        if RAM + 0x1000 <= address < RAM + 0x2000:
            if address + size > RAM + 0x1400:
                raise AssertionError("Upper write leaves the shared reservation")
            if len(self.written_words) != 256:
                assert size == 4 and address % 4 == 0 and value == 0
                assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
                self.written_words.add(address)
            if self.tick_atomic:
                assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
            self.upper_writes.append((address, size, value))
            return
        if RAM + 0xFC8 <= address < RAM + 0x1000:
            assert self.booting and address == RAM + 0xFE4 and size == 4 and value == 0
            assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
            assert not self.written_words
            self.legacy_writes.append((address, size, value))
            return
        if self.require_release_atomic and address == 0x48001428:
            assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
        super()._write(uc, access, address, size, value, data)

    def _init(self):
        if not self.platform_loaded:
            return super()._init()
        assert bytes(self.uc.mem_read(RAM + 0x1020, 12)) == b"\0" * 12
        self.init_count += 1

    def _remap(self):
        assert len(self.written_words) == 256
        assert self.get(0x1000, 4) == 0x46503031
        assert self.get(0x100C, 4) == 0
        self.remap_count += 1

    def _sleep(self):
        assert not self.pin and self.get(0x1028) == 0
        self.sleep_count += 1


class PlatformTests(unittest.TestCase):
    """Exercise layout, gates, moved TB01 behavior and inverse byte restoration."""

    @classmethod
    def setUpClass(cls):
        """Compile FP01 and the byte-exact legacy implementation once."""
        cls.sections, cls.symbols, cls.toolchain = builder._compile_patch(LLVM_BIN)
        cls.legacy_sections, cls.legacy_symbols = legacy_builder.compile_patch(LLVM_BIN)
        cls.rs_sections, cls.rs_symbols = rs_builder.compile_patch(LLVM_BIN)

    def desk(self, initialize=True):
        """Create a fresh modeled desk with dirty upper RAM."""
        return PlatformDesk(
            self.sections,
            self.symbols,
            self.legacy_sections,
            self.legacy_symbols,
            initialize,
        )

    def test_legacy_sections_unchanged(self):
        """Macro defaults preserve the pinned legacy executable sections exactly."""
        self.assertEqual(
            legacy_builder.sections_sha256(self.legacy_sections),
            legacy_builder.PATCH_SECTIONS_SHA256,
        )

    def test_descriptor_and_generated_layout(self):
        """Flash and RAM describe the reviewed 53-block payload and 54-block app."""
        layout_generator.check_source()
        self.assertEqual(
            struct.unpack("<8s6I", builder.descriptor()),
            (b"T150FP01", 1, FLASH + 0xA010, FLASH + 0xFBE0, RAM + 0x1000, 1024, 1),
        )
        self.assertEqual(builder.PAYLOAD_SIZE, 53 * 1024)
        self.assertEqual(builder.APP_SIZE, 54 * 1024)
        self.assertEqual(self.symbols["PLATFORM_RAM_ABI"], 1)
        self.assertEqual(self.symbols["PLATFORM_LAYOUT_VERSION"], 1)
        self.assertEqual(
            self.symbols["__aeabi_uidivmod"], self.symbols["__aeabi_uidiv"]
        )

    def test_layout_rejects_overlap(self):
        """The generator rejects overlapping RAM slots before generating constants."""
        layout = copy.deepcopy(builder.LAYOUT)
        layout["slots"][1]["offset"] = 16
        with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
            path = Path(directory) / "layout.json"
            path.write_text(json.dumps(layout))
            with self.assertRaises(ValueError):
                layout_generator.load_layout(path)

    def test_layout_preserves_existing_abi_slots(self):
        """Reject contiguous reallocations that silently change an ABI 1 consumer."""
        for index, current in enumerate(builder.LAYOUT["slots"][:-1]):
            if current["name"] not in layout_generator.ABI1_SLOTS:
                continue
            with self.subTest(slot=current["name"]):
                layout = copy.deepcopy(builder.LAYOUT)
                slot, following = layout["slots"][index : index + 2]
                slot["size"] += 4
                following["offset"] += 4
                following["size"] -= 4
                with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
                    path = Path(directory) / "layout.json"
                    path.write_text(json.dumps(layout))
                    with self.assertRaisesRegex(ValueError, "Existing RAM ABI"):
                        layout_generator.load_layout(path)

    def test_trim_allocations_fit_cortex_m0(self):
        """Check actual target sizes and alignments as well as the host build."""
        subprocess.run(
            [
                self.toolchain["clang"]["path"],
                "--target=arm-none-eabi",
                "-mcpu=cortex-m0",
                "-mthumb",
                "-std=c11",
                "-ffreestanding",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(builder.PATCHES),
                "-x",
                "c",
                "-fsyntax-only",
                "-",
            ],
            input='#include "motion_trim_layout.h"\n',
            text=True,
            capture_output=True,
            check=True,
        )

    def test_inverse_and_protected_bytes(self):
        """Only named hooks, code and descriptor change; the last page stays erased."""
        source = synthetic_source()
        output, spans = builder._apply_sections(source, 0, self.sections, self.symbols)
        restored = bytearray(output)
        for span in spans:
            before, offset = bytes.fromhex(span["before"]), span["image_offset"]
            restored[offset : offset + len(before)] = before
        self.assertEqual(bytes(restored), source)
        self.assertEqual(output[:0x2800], source[:0x2800])
        self.assertEqual(output[0xFC00:], b"\xff" * 1024)
        self.assertEqual(output[0x2800:0x2804], (RAM + 0xFC8).to_bytes(4, "little"))

    def test_source_anchors_and_erased_space(self):
        """Each RAM/boot anchor and the protected final page fails closed."""
        for offset in (*builder.SOURCE_ANCHORS, *builder.BOOT_ANCHORS, 0xFC00):
            with self.subTest(offset=offset):
                source = bytearray(synthetic_source())
                source[offset] ^= 1
                with self.assertRaises(builder.ImageError):
                    builder._apply_sections(
                        bytes(source), 0, self.sections, self.symbols
                    )

    def test_unrecognized_input(self):
        """Synthetic or altered firmware cannot pass the exact-image builder gate."""
        with self.assertRaises(builder.ImageError):
            builder.build(synthetic_source(), LLVM_BIN)

    def test_word_initialization_before_remap(self):
        """Dirty RAM receives all 256 zero-word writes before any header reads."""
        desk = self.desk(initialize=False)
        self.assertEqual(desk.remap_count, 1)
        self.assertEqual(len(desk.written_words), 256)
        self.assertEqual(desk.upper_writes[255], (RAM + 0x13FC, 4, 0))
        self.assertEqual(desk.upper_writes[-1], (RAM + 0x1000, 4, 0x46503031))
        self.assertEqual(desk.get(0x100C, 4), 0)
        self.assertEqual(desk.get(0xFF0, 16), int.from_bytes(b"\xc7" * 16, "little"))
        self.assertEqual(bytes(desk.uc.mem_read(RAM + 0x1400, 0xC00)), b"\xa5" * 0xC00)

    def test_startup_invalidates_only_the_owned_rs01_magic(self):
        """The pre-remap write is one aligned FE4 zero and changes no other owner."""
        desk = self.desk(initialize=False)
        desk.uc.mem_write(RAM + 0xFC8, b"\xa5" * 0x38)
        desk.written_words.clear()
        desk.legacy_writes.clear()
        desk.booting = True
        desk.call(FLASH + 0x85F4, stops={FLASH + 0x85F8})
        desk.booting = False
        self.assertEqual(desk.legacy_writes, [(RAM + 0xFE4, 4, 0)])
        expected = bytearray(b"\xa5" * 0x38)
        expected[0x1C:0x20] = bytes(4)
        self.assertEqual(bytes(desk.uc.mem_read(RAM + 0xFC8, 0x38)), bytes(expected))

    def test_wrong_profile_startup_zeroes_magic_without_upper_access(self):
        """The legacy magic invalidation preserves the wrapper ABI on rejected chips."""
        for identity in (0, 0x444):
            desk = self.desk(initialize=False)
            desk.uc.mem_unmap(RAM + 0x1000, 0x1000)
            desk.setreg(0x40015800, identity)
            desk.callbacks[0x08004A98] = lambda: None
            desk.written_words.clear()
            desk.legacy_writes.clear()
            desk.put(0xFE4, 0x52533031, 4)
            registers = [getattr(arm_regs, "UC_ARM_REG_R" + str(i)) for i in range(13)]
            for i, register in enumerate(registers):
                desk.uc.reg_write(register, (i + 1) * 0x1020304)
            desk.uc.reg_write(arm_regs.UC_ARM_REG_XPSR, 0xA1000000)
            desk.uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
            observed = registers + [
                UC_ARM_REG_SP,
                UC_ARM_REG_LR,
                arm_regs.UC_ARM_REG_XPSR,
                UC_ARM_REG_PRIMASK,
            ]
            before = [desk.uc.reg_read(register) for register in observed]
            desk.booting = True
            desk.call(self.symbols["platform_startup"])
            desk.booting = False
            self.assertEqual(
                [desk.uc.reg_read(register) for register in observed], before
            )
            self.assertEqual(desk.legacy_writes, [(RAM + 0xFE4, 4, 0)])

    def test_preinit_watchdog_cannot_pause_from_stale_expired_or_raw_key_state(self):
        """Actual project RS01 watchdog reads only zero magic after early startup."""
        for expired, raw_key in (
            (False, False),
            (True, False),
            (False, True),
            (True, True),
        ):
            with self.subTest(expired=expired, raw_key=raw_key):
                desk = self.desk(initialize=False)
                rs_code = self.rs_sections["reset_speed"]
                desk.uc.mem_write(FLASH + 0x9400, rs_code)
                for name, (offset, _) in builder.HOOKS.items():
                    desk.uc.mem_write(FLASH + offset, self.sections[name])
                desk.extra_project_ranges.append(
                    (FLASH + 0x9400, FLASH + 0x9400 + len(rs_code))
                )
                desk.callbacks.pop(self.rs_symbols["rs_watchdog"], None)
                desk.uc.ctl_remove_cache(FLASH, FLASH + builder.FULL_SIZE)
                desk.put(0xFE4, 0x52533031, 4)
                desk.put(0xFE0, 1)
                desk.put(0xFE3, 1)
                desk.put(0xFD0, 0x87654321, 4)
                desk.put(0xFD4, 20 if expired else 200, 4)
                desk.put(0x75C, 100, 4)
                desk.setreg(0x48000010, 0 if raw_key else 0xFFFF)
                desk.put(0x7AD, 1)
                desk.put(0x7AF, 1)
                desk.put(0x187, 1)
                desk.put(0xFC9, 0)
                desk.written_words.clear()
                desk.booting = True
                desk.call(FLASH + 0x85F4, stops={FLASH + 0x85F8})
                desk.booting = False
                before = bytes(desk.uc.mem_read(RAM + 0xFC8, 0x38))
                desk.legacy_reads.clear()
                desk.legacy_writes.clear()
                desk.edges.clear()
                desk.call(self.rs_symbols["rs_watchdog"])
                self.assertEqual(desk.legacy_reads, [(RAM + 0xFE4, 4)])
                self.assertEqual(desk.legacy_writes, [])
                self.assertEqual(bytes(desk.uc.mem_read(RAM + 0xFC8, 0x38)), before)
                self.assertEqual(
                    (desk.get(0x7AD), desk.get(0x7AF), desk.get(0x187)), (1, 1, 1)
                )
                self.assertEqual(desk.edges, [])

    def test_tick_before_module_ready(self):
        """Published tick and abort hooks cannot start effects before TB01 init."""
        desk = self.desk(initialize=False)
        desk.tick(123)
        desk.call(self.symbols["tb01_abort"])
        self.assertFalse(desk.pin)
        self.assertEqual(desk.edges, [])
        self.assertEqual(desk.get(0x100C, 4), 0)
        self.assertEqual(desk.command(3, 50, 50, 1), 3)
        self.assertEqual(desk.get(0x1028), 0)

    def test_moved_deadline_wrap_and_sleep(self):
        """The maintained 32-bit TB01 deadline works after the RAM move."""
        desk = self.desk()
        desk.put(0x75C, 0xFFFFFFF0, 4)
        self.assertEqual(desk.command(3, 50, 50, 2), 0)
        self.assertEqual(desk.get(0x1020, 4), 34)
        desk.tick(33)
        self.assertTrue(desk.pin)
        desk.tick(34)
        self.assertFalse(desk.pin)
        desk.tick(84)
        self.assertTrue(desk.pin)
        desk.call(self.symbols["tb01_sleep"])
        self.assertFalse(desk.pin)
        self.assertEqual(desk.get(0x1028), 0)

    def test_unknown_version_silent(self):
        """Unknown D7 versions are consumed without replies or buzzer effects."""
        desk = self.desk()
        for version in (0, 2, 3, 4, 255):
            self.assertIsNone(
                desk.send(bytes([0xD7, version, 3, 1, 2, 0, 50, 0, 50, 1]))
            )
        self.assertFalse(desk.pin)

    def test_native_priority_and_timer(self):
        """Native routing and RTC service retain priority over the extension."""
        desk = self.desk()
        desk.route_result = 7
        self.assertEqual(desk.call(self.symbols["tb01_route"]), 7)
        desk.route_result = 0
        self.assertEqual(desk.command(1, 2), 0)
        self.assertEqual(desk.command(2), 0)
        self.assertEqual(desk.command(3, 50, 50, 1), 0)
        desk.call(self.symbols["tb01_native_priority"], 0xFF, 0)
        self.assertFalse(desk.pin)

    def test_guard_fault_disables_and_reports(self):
        """Guard failure stops owned effects and returns backend without reinit."""
        desk = self.desk()
        self.assertEqual(desk.command(3, 50, 50, 1), 0)
        desk.put(0x13F4, 0, 4)
        desk.tick(1)
        self.assertFalse(desk.pin)
        self.assertEqual(desk.get(0x1010, 4), 1)
        self.assertEqual(desk.get(0x100C, 4), 0)
        self.assertEqual(desk.command(3, 50, 50, 1), 3)
        self.assertEqual(desk.get(0x13F4, 4), 0)

    def test_guard_fault_preserves_native_output(self):
        """Fault recovery leaves a native-owned sound output alone."""
        desk = self.desk()
        desk.put(0x8A8, 1)
        desk.put(0x13F0, 0, 4)
        desk.pin = True
        desk.call(self.symbols["platform_ready"])
        self.assertTrue(desk.pin)

    def test_clock_aware_profile_gate_preserves_clock_and_mask(self):
        """Clock-disabled ID reads need a balanced debug enable and readback."""
        for enabled in (False, True):
            for mask in (0, 1):
                with self.subTest(enabled=enabled, mask=mask):
                    desk = self.desk()
                    previous = 0x00020A01 | (0x00400000 if enabled else 0)
                    desk.setreg(0x40021018, previous)
                    desk.clock_writes.clear()
                    desk.clock_events.clear()
                    desk.edges.clear()
                    desk.uc.reg_write(UC_ARM_REG_PRIMASK, mask)
                    self.assertEqual(
                        desk.call(self.symbols["platform_profile_matches"]), 1
                    )
                    self.assertEqual(desk.reg(0x40021018), previous)
                    self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), mask)
                    self.assertEqual(desk.edges, [])
                    if enabled:
                        self.assertEqual(desk.clock_writes, [])
                    else:
                        self.assertEqual(
                            desk.clock_writes, [previous | 0x00400000, previous]
                        )
                        write = desk.clock_events.index(("write", 0x40021018))
                        delay = desk.clock_events.index(("read", 0x40021018), write + 1)
                        identity = desk.clock_events.index(("read", 0x40015800))
                        self.assertLess(write, delay)
                        self.assertLess(delay, identity)

    def test_clock_gate_wrong_profiles_leave_upper_unmapped(self):
        """Clock handling cannot admit an unsupported chip or touch its upper RAM."""
        for identity, flash in (
            (0, 64),
            (0x444, 64),
            (0x440, 32),
            (0xFFFFFFFF, 0xFFFFFFFF),
        ):
            for enabled in (False, True):
                for mask in (0, 1):
                    with self.subTest(identity=identity, enabled=enabled, mask=mask):
                        desk = self.desk()
                        desk.uc.mem_unmap(RAM + 0x1000, 0x1000)
                        previous = 0x00020A01 | (0x00400000 if enabled else 0)
                        desk.setreg(0x40021018, previous)
                        desk.setreg(0x40015800, identity)
                        desk.setreg(0x1FFFF7CC, flash)
                        desk.clock_writes.clear()
                        desk.edges.clear()
                        desk.uc.reg_write(UC_ARM_REG_PRIMASK, mask)
                        self.assertEqual(
                            desk.call(self.symbols["platform_profile_matches"]), 0
                        )
                        self.assertEqual(
                            desk.call(self.symbols["platform_initialized"]), 0
                        )
                        self.assertEqual(desk.reg(0x40021018), previous)
                        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), mask)
                        self.assertEqual(desk.edges, [])
                        self.assertEqual(
                            desk.clock_writes,
                            [] if enabled else [previous | 0x00400000, previous] * 2,
                        )

    def test_profile_mismatch_without_upper_mapping(self):
        """Every exposed hook rejects a 4 KiB or unknown profile before upper RAM."""
        for identity, flash in ((0x444, 64), (0x440, 32), (0xFFFFFFFF, 0xFFFFFFFF)):
            with self.subTest(identity=identity, flash=flash):
                desk = self.desk()
                desk.uc.mem_unmap(RAM + 0x1000, 0x1000)
                desk.setreg(0x40015800, identity)
                desk.setreg(0x1FFFF7CC, flash)
                for name in (
                    "platform_bootstrap",
                    "platform_initialized",
                    "platform_ready",
                    "platform_modules_ready",
                    "tb01_init",
                    "tb01_watchdog",
                    "tb01_abort",
                    "tb01_sleep",
                ):
                    if name == "tb01_init":
                        desk.callbacks[0x080094A0] = lambda: None
                    if name == "tb01_sleep":
                        desk.callbacks[0x08007568] = lambda: None
                    desk.call(self.symbols[name])
                self.assertEqual(desk.command(0), 3)

    def test_not_ready_releases_owned_pulse_atomically(self):
        """Loss of module readiness cannot leave an owned output asserted."""
        for ready in (0, 2, 0xFFFFFFFF):
            for mask in (0, 1):
                with self.subTest(ready=ready, mask=mask):
                    desk = self.desk()
                    self.assertEqual(desk.command(3, 50, 50, 1), 0)
                    desk.put(0x100C, ready, 4)
                    desk.require_release_atomic = True
                    desk.uc.reg_write(UC_ARM_REG_PRIMASK, mask)
                    desk.call(self.symbols["tb01_abort"])
                    self.assertFalse(desk.pin)
                    self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), mask)
                    self.assertEqual(desk.get(0x1010, 4), int(ready != 0))
                    if ready == 0:
                        desk.call(self.symbols["tb01_init"])
                        self.assertEqual(desk.get(0x100C, 4), 1)

    def test_fault_route_release_preserves_mask_and_native_owner(self):
        """Unmasked route faults serialize owner checks and leave native audio on."""
        for native in (0, 1):
            desk = self.desk()
            self.assertEqual(desk.command(3, 50, 50, 1), 0)
            desk.put(0x13F0, 0, 4)
            desk.put(0x8A8, native)
            desk.require_release_atomic = True
            self.assertEqual(desk.command(3, 50, 50, 1), 3)
            self.assertEqual(desk.pin, bool(native))
            self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), 0)

    def test_fault_after_route_gate_blocks_buzzer_admission(self):
        """Locked beep admission rechecks a fault latched after the route gate."""
        desk = self.desk()
        state = {"return": None, "injected": False}

        def inject(uc, address, size, data):
            if address == self.symbols["platform_ready"] and not state["injected"]:
                state["return"] = uc.reg_read(UC_ARM_REG_LR) & ~1
            elif address == state["return"] and not state["injected"]:
                self.assertEqual(uc.reg_read(UC_ARM_REG_PRIMASK), 0)
                state["injected"] = True
                # Model an IRQ which latched a guard fault after the first gate.
                desk.put(0x100C, 0, 4)
                desk.put(0x1010, 1, 4)

        desk.uc.hook_add(UC_HOOK_CODE, inject)
        self.assertEqual(desk.command(3, 50, 50, 1), 2)
        self.assertTrue(state["injected"])
        self.assertFalse(desk.pin)
        self.assertEqual(desk.get(0x1028), 0)

    def test_compiler_frames_keep_native_router_thin(self):
        """Native routes do not retain extension request arrays on their stack."""
        report = self.toolchain["compiler_frames"]["platform_tb01.c"]
        self.assertIn("tb01_route\t8\tstatic", report)
        self.assertIn("tb01_extension_route\t56\tstatic", report)

    def test_hash_pins_cannot_be_empty_or_malformed(self):
        """A missing output pin never disables exact-image verification."""
        for name in (
            "OUTPUT_FULL_SHA256",
            "OUTPUT_APP_SHA256",
            "OUTPUT_PAYLOAD_SHA256",
            "PATCH_SECTIONS_SHA256",
        ):
            for value in (None, 123, "", "a" * 63, "g" * 64, "A" * 64):
                with self.subTest(name=name, value=value):
                    with patch.object(builder, name, value):
                        with self.assertRaises(builder.ImageError):
                            builder._validate_hash_pins()

    def test_source_dirty_and_release_gate(self):
        """Manifests record dirty state and release mode rejects uncommitted input."""
        for dirty in (False, True):
            outputs = [
                SimpleNamespace(stdout="a" * 40 + "\n"),
                SimpleNamespace(
                    stdout=" M tools/patches/platform_runtime.c\n" if dirty else ""
                ),
            ]
            with patch.object(builder.subprocess, "run", side_effect=outputs):
                provenance = builder._source_provenance()
            self.assertEqual(provenance["source_commit"], "a" * 40)
            self.assertEqual(provenance["source_dirty"], dirty)
            with patch.object(builder.subprocess, "run", side_effect=outputs):
                if dirty:
                    with self.assertRaises(builder.ImageError):
                        builder._source_provenance(require_clean_source=True)
                else:
                    self.assertFalse(builder._source_provenance(True)["source_dirty"])

    def test_layout_rejects_changed_flash_and_loader_binding(self):
        """Readback, payload and loader erase lengths retain separate fixed bounds."""
        for name, value in (
            ("flash_base", 0),
            ("full_size", 55296),
            ("application_offset", 0),
            ("loader_erase_size", 55296),
        ):
            layout = copy.deepcopy(builder.LAYOUT)
            layout[name] = value
            with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
                path = Path(directory) / "layout.json"
                path.write_text(json.dumps(layout))
                with self.assertRaises(ValueError):
                    layout_generator.load_layout(path)
        self.assertEqual(builder.LAYOUT["loader_erase_size"], 54272)
        self.assertEqual(builder.APP_SIZE, 55296)

    def test_pinned_project_sections(self):
        """All project-owned executable and descriptor bytes match the pinned build."""
        self.assertTrue(builder.PATCH_SECTIONS_SHA256)
        self.assertEqual(
            builder.sections_sha256(self.sections), builder.PATCH_SECTIONS_SHA256
        )


def main():
    """Run synthetic instruction tests using the selected pinned LLVM directory."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    args, remaining = parser.parse_known_args()
    LLVM_BIN = args.llvm_bin
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
