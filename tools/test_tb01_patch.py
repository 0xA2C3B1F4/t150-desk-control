#!/usr/bin/env python3
"""Execute TB01 instructions with synthetic images and modeled native helpers.

No vendor image or stock function body is included. The patch's generated
Cortex-M0 code and entry hooks execute in Unicorn; Python models native RTC,
UART, initialization, SysTick and sleep helpers. This checks patch behavior and
ABI boundaries, not the stock helpers, timing, physical sound or hardware.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_THUMB, Uc
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_PRIMASK,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_SP,
    UC_ARM_REG_XPSR,
    UC_CPU_ARM_CORTEX_M0,
)

import build_tb01_patch as builder
import generate_tb01_core as generator

FLASH = 0x08000000
RAM = 0x20000000
RETURN = FLASH + 0x1000
STACK = RAM + 0xFB8
RTC_CR = 0x40002808
RTC_ISR = 0x4000280C
RTC_ALARM = 0x4000281C
EXTI_PR = 0x40010414
CALLEE_SAVED = (
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
)
LLVM_BIN = None


def _bcd(number):
    return (number // 10) * 16 + number % 10


def _synthetic_source():
    source = bytearray(b"\xff" * builder.FULL_SIZE)
    source[builder.APP_OFFSET : builder.APP_OFFSET + 4] = (RAM + 0xFC8).to_bytes(
        4, "little"
    )
    source[0x34D0:0x34D4] = (RAM + 0x8A8).to_bytes(4, "little")
    for offset, original in builder.HOOKS.values():
        source[offset : offset + len(original)] = original
    return bytes(source)


class SyntheticDesk:
    """Run only the project-owned TB01 instructions with explicit helper models."""

    def __init__(self, sections, symbols):
        """Map synthetic flash, RAM and MMIO with no vendor function bodies."""
        source = _synthetic_source()
        image, self.spans = builder._apply_sections(source, 0, sections, symbols)
        self.source, self.image, self.symbols = source, image, symbols
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
        for address, size in (
            (FLASH, 0x10000),
            (RAM, 0x1000),
            (0x48000000, 0x2000),
            (0x40000000, 0x3000),
            (0x40010000, 0x14000),
        ):
            self.uc.mem_map(address, size)
        self.uc.mem_write(FLASH, image)
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_XPSR, 0x01000000)
        self.pin = False
        self.edges = []
        self.replies = []
        self.tick_atomic = False
        self.timer_stuck = False
        self.delivery_stuck = False
        self.expiry_on_disable = 0
        self.rollover_on_set = False
        self.route_result = 0
        self.init_count = 0
        self.sleep_count = 0
        self.lowest_sp = STACK
        self.stopped = None
        self.stops = {RETURN}
        self.callbacks = {
            0x080094A0: self._init,
            0x080094C0: self._tick,
            0x080097E0: self._route,
            0x08007568: self._sleep,
            0x08006984: self._rtc_read,
            0x080067EC: self._rtc_set,
            0x08006AA0: self._rtc_irq,
            0x0800670C: self._rtc_enable,
            0x08006870: self._rtc_clear,
            0x08004064: self._exti_clear,
            0x08003610: self._send,
            0x080028F4: self._divide,
        }
        # Authored synthetic "bx lr" return slots, not extracted stock code.
        for address in (*self.callbacks, RETURN, 0x08003410):
            self.uc.mem_write(address, b"\x70\x47")
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        self.setreg(0x48000010, 0xFFFF)
        self.setreg(0x48000410, 0xFFFF)
        self.setreg(0x40002800, 0x00120037)
        self.put(0xE8, RAM + 0x414, 4)
        self.put(0xE0, RAM + 0x394, 4)
        self.uc.mem_write(RAM + 0xFF0, b"\xff" * 12)
        self.call(FLASH + 0x9402, stops={FLASH + 0x9406})

    def reg(self, address):
        """Read one modeled 32-bit register."""
        return int.from_bytes(self.uc.mem_read(address, 4), "little")

    def setreg(self, address, value):
        """Set one modeled 32-bit register outside CPU execution."""
        self.uc.mem_write(address, (value & 0xFFFFFFFF).to_bytes(4, "little"))

    def put(self, offset, value, width=1):
        """Write a stock-RAM field in the synthetic model."""
        self.uc.mem_write(
            RAM + offset, (value & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
        )

    def get(self, offset, width=1):
        """Read a stock-RAM field in the synthetic model."""
        return int.from_bytes(self.uc.mem_read(RAM + offset, width), "little")

    def _code(self, uc, address, size, data):
        self.lowest_sp = min(self.lowest_sp, uc.reg_read(UC_ARM_REG_SP))
        if address in self.stops:
            self.stopped = address
            uc.emu_stop()
        elif address in self.callbacks:
            self.callbacks[address]()
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif not FLASH + builder.CODE_OFFSET <= address < FLASH + builder.PAYLOAD_END:
            if not any(
                FLASH + offset <= address < FLASH + offset + len(original)
                for offset, original in builder.HOOKS.values()
            ):
                raise AssertionError(f"Unexpected unmodeled native entry {address:#x}")

    def _write(self, uc, access, address, size, value, data):
        if RAM <= address and address + size <= RAM + 0x1000:
            if address < RAM + 0xFF0 and address + size > RAM + 0xFC8:
                raise AssertionError("TB01 wrote another patch's reserved RAM")
        elif address not in (0x48001418, 0x48001428, 0x48000418, 0x48000428):
            raise AssertionError(f"Unexpected patch MMIO write {address:#x}")
        if self.tick_atomic and (
            RAM + 0xFF0 <= address < RAM + 0xFFC or address in (0x48001418, 0x48001428)
        ):
            assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
        if address in (0x48001418, 0x48001428):
            assert value == 0x80
            self.pin = address == 0x48001418
            self.edges.append((self.get(0x75C, 4), self.pin))

    def _init(self):
        assert bytes(self.uc.mem_read(RAM + 0xFF0, 10)) == b"\0" * 10
        self.init_count += 1

    def _tick(self):
        self.put(0x75C, self.get(0x75C, 4) + 1, 4)

    def _route(self):
        self.uc.reg_write(UC_ARM_REG_R0, self.route_result)

    def _sleep(self):
        assert not self.pin and self.get(0xFF8) == 0
        self.sleep_count += 1

    def _rtc_read(self):
        assert self.uc.reg_read(UC_ARM_REG_PRIMASK) == 0
        value = self.reg(0x40002800)
        for offset, shift in ((0xDC, 16), (0xDD, 8)):
            part = (value >> shift) & 0xFF
            self.put(offset, (part >> 4) * 10 + (part & 15))

    def _rtc_set(self):
        assert self.uc.reg_read(UC_ARM_REG_PRIMASK) == 0
        if self.rollover_on_set:
            self.setreg(0x40002800, 0x00120100)
        if not self.timer_stuck:
            minutes = (
                self.get(0xDC) * 60 + self.get(0xDD) + self.uc.reg_read(UC_ARM_REG_R0)
            )
            hour, minute = divmod(minutes % (24 * 60), 60)
            self.setreg(RTC_ALARM, 0x81000000 | _bcd(hour) << 16 | _bcd(minute) << 8)
            self.setreg(RTC_CR, self.reg(RTC_CR) | 0x1100)

    def _rtc_irq(self):
        assert self.uc.reg_read(UC_ARM_REG_PRIMASK) == 0
        if self.expiry_on_disable:
            self.put(0x8A8, 1)
            if self.expiry_on_disable == 2:
                self.put(0x14D, 1)
            self.expiry_on_disable = 0
        value = self.reg(RTC_CR)
        mask = self.uc.reg_read(UC_ARM_REG_R0)
        assert mask == 0x1000
        enabled = self.uc.reg_read(UC_ARM_REG_R1)
        if not (enabled and self.delivery_stuck):
            self.setreg(RTC_CR, value | mask if enabled else value & ~mask)

    def _rtc_enable(self):
        assert self.uc.reg_read(UC_ARM_REG_PRIMASK) == 0
        assert self.uc.reg_read(UC_ARM_REG_R0) == 0x100
        if not self.timer_stuck:
            self.setreg(RTC_CR, self.reg(RTC_CR) & ~0x100)
        self.uc.reg_write(UC_ARM_REG_R0, int(not self.timer_stuck))

    def _rtc_clear(self):
        assert self.uc.reg_read(UC_ARM_REG_R0) == 0x1000
        self.setreg(RTC_ISR, self.reg(RTC_ISR) & ~0x100)

    def _exti_clear(self):
        assert self.uc.reg_read(UC_ARM_REG_R0) == 1 << 17
        self.setreg(EXTI_PR, self.reg(EXTI_PR) & ~(1 << 17))

    def _divide(self):
        numerator = self.uc.reg_read(UC_ARM_REG_R0)
        denominator = self.uc.reg_read(UC_ARM_REG_R1)
        quotient, remainder = divmod(numerator, denominator)
        self.uc.reg_write(UC_ARM_REG_R0, quotient)
        self.uc.reg_write(UC_ARM_REG_R1, remainder)

    def _send(self):
        assert self.uc.reg_read(UC_ARM_REG_R0) == RAM + 0xF0
        assert self.uc.reg_read(UC_ARM_REG_R1) == RAM + 0xE0
        assert self.get(0xE4) == 6
        assert self.get(0x394) == 0xFA
        self.replies.append(bytes(self.uc.mem_read(RAM + 0x395, 6)))

    def call(self, address, *args, stops=None):
        """Call patch instructions and check callee-saved registers and stack return."""
        initial_sp = self.uc.reg_read(UC_ARM_REG_SP)
        saved = {register: self.uc.reg_read(register) for register in CALLEE_SAVED}
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1), args):
            self.uc.reg_write(register, value)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
        self.stops, self.stopped = {RETURN} if stops is None else stops, None
        self.uc.emu_start(address | 1, FLASH + 0x10000, count=100000)
        assert self.stopped in self.stops, (
            "Patch did not return to its expected boundary"
        )
        if stops is None:
            assert self.uc.reg_read(UC_ARM_REG_SP) == initial_sp
            assert all(
                self.uc.reg_read(register) == value for register, value in saved.items()
            )
        assert self.lowest_sp >= RAM + 0xAC4, (
            "Observed stack entered native persistent data"
        )
        return self.uc.reg_read(UC_ARM_REG_R0)

    def send(self, body):
        """Enter the decoded-body route hook with a modeled already-validated frame."""
        previous = len(self.replies)
        self.uc.mem_write(RAM + 0x414, bytes(body))
        self.put(0x124, len(body) + 1)
        assert self.call(self.symbols["tb01_route"]) == 1
        return self.replies[-1] if len(self.replies) > previous else None

    def command(self, op, first=0, second=0, count=0):
        """Send a correlated D7 request and return the result byte."""
        body = bytes(
            [
                0xD7,
                1,
                op,
                0xFA,
                0xFD,
                first >> 8,
                first & 255,
                second >> 8,
                second & 255,
                count,
            ]
        )
        reply = self.send(body)
        assert reply and reply[:5] == bytes([0xD8, 1, op, 0xFA, 0xFD])
        return reply[5]

    def tick(self, now):
        """Execute the tick hook with an incrementing modeled native millisecond tick."""
        self.put(0x75C, now - 1, 4)
        self.tick_atomic = True
        try:
            self.call(self.symbols["tb01_watchdog"])
        finally:
            self.tick_atomic = False
        assert self.get(0x75C, 4) == now


class PatchTests(unittest.TestCase):
    """Check patch instructions and image layout using no vendor firmware."""

    @classmethod
    def setUpClass(cls):
        """Compile the generated project-owned assembly once for the test suite."""
        cls.sections, cls.symbols = builder.compile_patch(LLVM_BIN)
        if builder.sections_sha256(cls.sections) != builder.PATCH_SECTIONS_SHA256:
            raise AssertionError(
                "Synthetic tests must execute the pinned TB01 sections"
            )

    def desk(self):
        """Return a freshly initialized synthetic desk."""
        return SyntheticDesk(self.sections, self.symbols)

    def test_hash_binding_and_inverse(self):
        """Reject synthetic input identities and confine reversible edits to declared spans."""
        desk = self.desk()
        for source in (
            desk.source,
            desk.image,
            desk.source[:-1],
            desk.source[builder.APP_OFFSET :],
        ):
            with self.assertRaises(builder.ImageError):
                builder.build(source, LLVM_BIN)
        restored, allowed = bytearray(desk.image), set()
        for span in desk.spans:
            offset = span["offset"]
            before = bytes.fromhex(span["before"])
            restored[offset : offset + len(before)] = before
            allowed.update(range(offset, offset + len(before)))
        self.assertEqual(bytes(restored), desk.source)
        self.assertTrue(
            all(
                a == b or index in allowed
                for index, (a, b) in enumerate(zip(desk.source, desk.image))
            )
        )
        self.assertEqual(
            desk.image[builder.PAYLOAD_END :], desk.source[builder.PAYLOAD_END :]
        )
        app, _ = builder._apply_sections(
            desk.source[builder.APP_OFFSET :],
            builder.APP_OFFSET,
            self.sections,
            self.symbols,
        )
        self.assertEqual(app, desk.image[builder.APP_OFFSET :])

    def test_layout_rejection(self):
        """Reject changed hooks, occupied code, stack changes and oversized or missing sections."""
        source = _synthetic_source()
        for offset in (0x2800, 0x3408, 0x34D0, 0x865E, 0x9402, 0x9434, 0x9452, 0xA010):
            changed = bytearray(source)
            changed[offset] ^= 1
            with self.assertRaises(builder.ImageError):
                builder._apply_sections(bytes(changed), 0, self.sections, self.symbols)
        for replacement in (b"", b"\0" * (builder.CODE_CAPACITY + 1)):
            sections = dict(self.sections, candidate=replacement)
            with self.assertRaises(builder.ImageError):
                builder._apply_sections(source, 0, sections, self.symbols)
        for sections in (dict(self.sections, hook_tick=b"\0"), {"candidate": b"\0"}):
            with self.assertRaises(builder.ImageError):
                builder._apply_sections(source, 0, sections, self.symbols)
        with self.assertRaises(builder.ImageError):
            builder._apply_sections(
                source, 0, self.sections, dict(self.symbols, __aeabi_uidiv=0x080028F6)
            )

    def test_pinned_section_identity(self):
        """Bind every synthetic execution to the same project-owned sections as the builder."""
        self.assertEqual(
            builder.sections_sha256(self.sections), builder.PATCH_SECTIONS_SHA256
        )
        for name in builder.SECTION_ORDER:
            changed = dict(self.sections)
            data = changed[name]
            changed[name] = bytes([data[0] ^ 1]) + data[1:]
            with self.assertRaises(builder.ImageError):
                builder._check_section_identity(changed)
        original_read = Path.read_bytes

        def tampered_read(path):
            data = original_read(path)
            if path.name == "candidate.bin":
                return bytes([data[0] ^ 1]) + data[1:]
            return data

        with patch.object(Path, "read_bytes", tampered_read):
            with self.assertRaises(builder.ImageError):
                builder.compile_patch(LLVM_BIN)

    def test_transactional_output_cleanup(self):
        """Keep write errors through cleanup failures and preserve existing directories."""
        desk = self.desk()
        original_open = Path.open
        with tempfile.TemporaryDirectory(
            prefix="t150-tb01-output-", dir=os.environ["TMPDIR"]
        ) as directory:
            root = Path(directory)
            existing = root / "existing"
            existing.mkdir()
            sentinel = existing / "user.txt"
            sentinel.write_text("preserve")
            with self.assertRaises(FileExistsError):
                builder._write_outputs(existing, desk.image, 0, {})
            self.assertEqual(sentinel.read_text(), "preserve")
            for failure_name in ("t150-tb01.payload.bin", "manifest.json"):
                destination = root / failure_name

                def failing_open(path, mode="r", *args, **kwargs):
                    if path.name == failure_name and mode == "xb":
                        if failure_name == "manifest.json":
                            raise OSError("Simulated manifest-open failure")
                        stream = original_open(path, mode, *args, **kwargs)

                        class PartialWriter:
                            def __enter__(self):
                                return self

                            def __exit__(self, *exc):
                                stream.close()

                            def write(self, data):
                                stream.write(data[:5])
                                raise OSError("Simulated partial binary-write failure")

                        return PartialWriter()
                    return original_open(path, mode, *args, **kwargs)

                with patch.object(Path, "open", failing_open):
                    with self.assertRaises(OSError):
                        builder._write_outputs(destination, desk.image, 0, {})
                self.assertFalse(destination.exists())
            for operation in ("unlink", "rmdir"):
                destination = root / f"cleanup-failure-{operation}"
                write_error = OSError("Original manifest-write failure")

                def fail_manifest(path, mode="r", *args, **kwargs):
                    if path.name == "manifest.json" and mode == "xb":
                        raise write_error
                    return original_open(path, mode, *args, **kwargs)

                with (
                    patch.object(Path, "open", fail_manifest),
                    patch.object(
                        Path, operation, side_effect=PermissionError("Cleanup failed")
                    ),
                ):
                    with self.assertRaises(OSError) as failure:
                        builder._write_outputs(destination, desk.image, 0, {})
                self.assertIs(failure.exception, write_error)
                self.assertTrue(
                    any("Cleanup failed" in note for note in write_error.__notes__)
                )
                self.assertEqual(sentinel.read_text(), "preserve")
            destination = root / "application"
            builder._write_outputs(
                destination, desk.image[builder.APP_OFFSET :], builder.APP_OFFSET, {}
            )
            self.assertFalse((destination / "t150-tb01.full.bin").exists())
            self.assertEqual(
                (destination / "t150-tb01.application.bin").read_bytes(),
                desk.image[builder.APP_OFFSET :],
            )
            self.assertEqual(
                (destination / "t150-tb01.payload.bin").read_bytes(),
                desk.image[builder.APP_OFFSET : builder.PAYLOAD_END],
            )

    def test_saved_file_reader(self):
        """Refuse symlinks and FIFOs before reading saved input files."""
        with tempfile.TemporaryDirectory(
            prefix="t150-tb01-reader-", dir=os.environ["TMPDIR"]
        ) as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(_synthetic_source())
            self.assertEqual(builder.read_saved_image(source), source.read_bytes())
            link = source.with_suffix(".link")
            link.symlink_to(source)
            with self.assertRaises(builder.ImageError):
                builder.read_saved_image(link)
            fifo = source.with_suffix(".fifo")
            os.mkfifo(fifo)
            with self.assertRaises(builder.ImageError):
                builder.read_saved_image(fifo)

    def test_timer_contract_and_bounds(self):
        """Use native helper contracts for snapshot-based timer setup, cancellation and bounds."""
        desk = self.desk()
        self.assertEqual(desk.init_count, 1)
        self.assertEqual(desk.command(0), 0)
        self.assertEqual(desk.command(1, 15), 0)
        self.assertEqual(desk.reg(RTC_ALARM), 0x81121500)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0x1100)
        self.assertEqual(desk.get(0x8B4, 4), 15)
        self.assertEqual(desk.command(1, 45), 2)
        self.assertEqual(desk.command(2), 0)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0)
        self.assertEqual(desk.command(1, 45), 0)
        self.assertEqual(desk.reg(RTC_ALARM), 0x81124500)
        for duration in (0, 1, 721, 65535):
            self.assertEqual(self.desk().command(1, duration), 1)
        for hour, minute, duration, expected in (
            (23, 59, 15, 0x81001400),
            (23, 0, 120, 0x81010000),
            (12, 0, 720, 0x81000000),
            (0, 59, 2, 0x81010100),
        ):
            desk = self.desk()
            desk.setreg(0x40002800, _bcd(hour) << 16 | _bcd(minute) << 8 | 0x59)
            self.assertEqual(desk.command(1, duration), 0)
            self.assertEqual(desk.reg(RTC_ALARM), expected)
        desk = self.desk()
        desk.rollover_on_set = True
        self.assertEqual(desk.command(1, 15), 0)
        self.assertEqual(desk.reg(RTC_ALARM), 0x81121500)

    def test_cancel_after_native_timer_selection(self):
        """Keep T selection busy, then cancel the shared timer after native selection settles."""
        desk = self.desk()
        desk.setreg(RTC_CR, 0x1100)
        desk.setreg(RTC_ALARM, 0x81123000)
        desk.put(0x8B4, 30, 4)
        desk.put(0x18C, 1)
        self.assertEqual(desk.command(2), 2)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0x1100)
        self.assertEqual(desk.get(0x8B4, 4), 30)
        # The native T timeout stores the selected duration and clears this counter.
        desk.put(0x18C, 0)
        self.assertEqual(desk.command(2), 0)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0)
        self.assertEqual(desk.get(0x8B4, 4), 0)
        self.assertEqual(desk.get(0x18C), 0)

    def test_timer_failure_and_expiry_race(self):
        """Preserve delivered expiry and restore delivery when modeled native disable fails."""
        for variant in (1, 2):
            desk = self.desk()
            self.assertEqual(desk.command(1, 15), 0)
            desk.expiry_on_disable = variant
            self.assertEqual(desk.command(2), 3)
            self.assertEqual(desk.get(0x8A8), 1)
            self.assertEqual(desk.get(0x8B4, 4), 15)
        desk = self.desk()
        desk.timer_stuck = True
        self.assertEqual(desk.command(1, 15), 3)
        self.assertEqual(desk.get(0x8B4, 4), 0)
        desk = self.desk()
        self.assertEqual(desk.command(1, 15), 0)
        desk.timer_stuck = True
        self.assertEqual(desk.command(2), 3)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0x1100)
        desk.delivery_stuck = True
        self.assertEqual(desk.command(2), 3)
        self.assertEqual(desk.get(0x8B4, 4), 0)
        desk.timer_stuck = False
        desk.setreg(RTC_ISR, 0x100)
        desk.setreg(EXTI_PR, 1 << 17)
        self.assertEqual(desk.command(2), 0)
        self.assertEqual(desk.reg(RTC_ISR) & 0x100, 0)
        self.assertEqual(desk.reg(EXTI_PR) & (1 << 17), 0)

    def test_admission_and_preexisting_route(self):
        """Block native activity, motion, menu, reset, panel lock and inherited IRQ masks."""
        for offset, width, value in (
            (0x88D, 1, 7),
            (0x75A, 2, 1),
            (0x18C, 1, 2),
            (0x8A8, 1, 1),
            (0x7AD, 1, 1),
            (0xA14, 1, 1),
            (0xFC8, 1, 1),
            (0x88F, 1, 1),
        ):
            desk = self.desk()
            desk.put(offset, value, width)
            self.assertEqual(desk.command(1, 15), 2)
            self.assertEqual(desk.command(3, 100, 200, 2), 2)
            self.assertFalse(desk.pin)
        desk = self.desk()
        desk.route_result = 1
        self.assertIsNone(desk.send([0xD7, 1, 1, 0, 1, 0, 15, 0, 0, 0]))
        self.assertEqual(desk.get(0x8B4, 4), 0)
        desk = self.desk()
        desk.uc.reg_write(UC_ARM_REG_PRIMASK, 1)
        self.assertIsNone(desk.send([0xD7, 1, 1, 0, 1, 0, 15, 0, 0, 0]))
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), 1)
        desk = self.desk()
        desk.put(0xF4, 8)
        self.assertIsNone(desk.send([0xD7, 1, 3, 0, 1, 0, 100, 0, 200, 2]))
        self.assertFalse(desk.pin)

    def test_buzzer_sequence_and_preemption(self):
        """Execute atomic pulse transitions, native priority, physical-key abort and sleep release."""
        desk = self.desk()
        self.assertEqual(desk.command(3, 100, 200, 3), 0)
        self.assertTrue(desk.pin)
        for now, expected in (
            (99, True),
            (100, False),
            (300, True),
            (400, False),
            (600, True),
            (700, False),
        ):
            desk.tick(now)
            self.assertEqual(desk.pin, expected)
        self.assertEqual(desk.get(0xFF8), 0)
        desk = self.desk()
        self.assertEqual(desk.command(3, 100, 200, 2), 0)
        desk.put(0x88F, 1)
        desk.tick(1)
        self.assertFalse(desk.pin)
        desk = self.desk()
        self.assertEqual(desk.command(3, 100, 200, 2), 0)
        desk.put(0x14E, 2)
        desk.call(self.symbols["tb01_native_priority"], 0xAA, 2)
        self.assertTrue(desk.pin)
        desk.call(self.symbols["tb01_native_priority"], 0xFF, 3)
        self.assertFalse(desk.pin)
        edges = len(desk.edges)
        desk.tick(1)
        self.assertEqual(len(desk.edges), edges)
        desk = self.desk()
        self.assertEqual(desk.command(3, 100, 200, 2), 0)
        desk.call(self.symbols["tb01_sleep"])
        self.assertFalse(desk.pin)
        self.assertEqual(desk.sleep_count, 1)

    def test_hook_abi(self):
        """Preserve the native sound wrapper arguments, flags and displaced prologue contract."""
        desk = self.desk()
        self.assertEqual(desk.command(3, 100, 200, 2), 0)
        initial_sp = desk.uc.reg_read(UC_ARM_REG_SP)
        for register in CALLEE_SAVED:
            desk.uc.reg_write(register, register * 0x10203)
        desk.uc.reg_write(UC_ARM_REG_XPSR, 0xA1000000)
        desk.call(FLASH + 0x3408, 0xFF, 3, stops={FLASH + 0x3410})
        self.assertFalse(desk.pin)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_R0), 0xFF)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_R1), 3)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_R4), 0xFF)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_R5), RAM + 0x8A8)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_R7), 3)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_SP), initial_sp - 24)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_XPSR) & 0xF0000000, 0xA0000000)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_LR), RETURN | 1)

    def test_corruption_and_malformed_body(self):
        """Abort corrupt or overdue pulse state and reject malformed decoded D7 bodies."""
        for offset, value in ((0xFF8, 99), (0xFF9, 0), (0xFF9, 21)):
            desk = self.desk()
            self.assertEqual(desk.command(3, 100, 200, 2), 0)
            desk.put(offset, value)
            desk.tick(1)
            self.assertFalse(desk.pin)
            self.assertEqual(desk.get(0xFF8), 0)
        desk = self.desk()
        self.assertEqual(desk.command(3, 100, 200, 2), 0)
        desk.tick(351)
        self.assertFalse(desk.pin)
        for body in (
            [0xD7],
            [0xD7, 1, 3],
            [0xD7, 1, 3, 0, 1],
            [0xD7, 2, 3, 0, 1, 0, 100, 0, 200, 2],
            [0xD7, 1, 99, 0, 1, 0, 100, 0, 200, 2],
        ):
            desk = self.desk()
            reply = desk.send(body)
            self.assertTrue(reply is None or reply[-1] == 1)
            self.assertFalse(desk.pin)
            self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0)


class GeneratorTests(unittest.TestCase):
    """Keep source provenance checks strict without requiring a C compiler."""

    def test_markers_and_assembly_comparison(self):
        """Check source markers and ignore only a valid compiler identity string."""
        generator.check_source()
        assembly = 'movs r0, #1\n.ident "clang version 23.1.0"\n'
        generator.check_assembly(
            assembly, assembly.replace("clang version 23.1.0", "other build")
        )
        for changed in (
            assembly.replace("#1", "#2"),
            assembly + '.ident "duplicate"\n',
            assembly.replace('.ident "clang version 23.1.0"', ".ident invalid"),
        ):
            with self.assertRaises(ValueError):
                generator.check_assembly(assembly, changed)
        with tempfile.TemporaryDirectory(
            prefix="t150-tb01-markers-", dir=os.environ["TMPDIR"]
        ) as directory:
            patches = Path(directory)
            for source, output in generator.SOURCES.items():
                (patches / source).write_bytes(
                    (generator.PATCHES / source).read_bytes()
                )
                (patches / output).write_bytes(
                    (generator.PATCHES / output).read_bytes()
                )
            (patches / generator.HEADER).write_bytes(
                (generator.PATCHES / generator.HEADER).read_bytes()
            )
            with patch.object(generator, "PATCHES", patches):
                generator.check_source()
                (patches / generator.HEADER).write_text("changed header")
                with self.assertRaises(ValueError):
                    generator.check_source()
                (patches / generator.HEADER).write_bytes(
                    (builder.PATCHES / generator.HEADER).read_bytes()
                )
                generator.check_source()
                original_source = (patches / "tb01_core.c").read_text()
                (patches / "tb01_core.c").write_text(original_source + "\n")
                with self.assertRaises(ValueError):
                    generator.check_source()
                (patches / "tb01_core.c").write_text(original_source)
                (patches / "tb01_core.S").write_text("")
                with self.assertRaises(ValueError):
                    generator.check_source()

    def test_compiler_version_rejection(self):
        """Reject another version, a version suffix and an empty compiler banner."""
        for banner in ("clang version 23.1.4\n", "clang version 23.1.0bad\n", ""):
            result = subprocess.CompletedProcess([], 0, stdout=banner)
            with patch.object(generator.subprocess, "run", return_value=result):
                with self.assertRaises(ValueError):
                    generator.generate("unused-compiler")


def main():
    """Run synthetic TB01 instruction, image-layout and provenance tests."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    args = parser.parse_args()
    LLVM_BIN = args.llvm_bin
    unittest.main(argv=[__file__], verbosity=2)


if __name__ == "__main__":
    main()
