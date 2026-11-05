#!/usr/bin/env python3
"""Run local panel-lock code and exact saved native paths without hardware.

Supply the private, hash-bound R7 full image. No manufacturer bytes are stored
in this repository. Native GPIO predicates, scanner, handlers, UART decoder,
CL01 apply and sleep wrapper execute; external EEPROM transport and sleep's
hardware boundary use explicit models. These checks do not prove physical
button labels, stopping distance, MCU timing or installed firmware behavior.
"""

import argparse
import hashlib
import json
from pathlib import Path
import unittest

from unicorn.arm_const import (
    UC_ARM_REG_APSR,
    UC_ARM_REG_PC,
    UC_ARM_REG_PRIMASK,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R4,
    UC_ARM_REG_R12,
    UC_ARM_REG_SP,
)

from build_local_lock_patch import OUTPUT_FULL_SHA256, descriptor
from build_motion_trim_patch import BOOT_PROFILE
import build_platform_patch as platform
from compile_motion_trim import STORAGE_HOOKS, TARGET_HOOKS, compile_patch
from test_child_lock_patch import FLASH, RAM, REGS, RETURN, Machine, decode, direct_body

LLVM_BIN = None
SOURCE_R7 = None
REPORT = None
LOCAL_SLOT = 0x1298
SCANNERS = tuple(range(0xA10, 0xA90, 16))
BUSY_OFFSETS = (
    0x88D,
    0x895,
    0x88C,
    0x754,
    *range(0x7AD, 0x7B5),
    *range(0x184, 0x18B),
    0x18D,
    0x88F,
    0x890,
    0x148,
    0x8B8,
    0x892,
    0x893,
    0x894,
    0x8B9,
    0xAA0,
    0xAA1,
    0x18E,
    0x8BA,
)


def linked_image(source, sections):
    """Place linked sections over the exact R7 input with no output hash shortcut."""
    if hashlib.sha256(source).hexdigest() != platform.SOURCE_FULL_SHA256:
        raise ValueError("Supply the exact R7 full image")
    spans = {name: value[0] for name, value in platform.HOOKS.items()}
    spans.update({name: value[0] for name, value in TARGET_HOOKS.items()})
    spans.update({name: value[0] for name, value in STORAGE_HOOKS.items()})
    spans.update(
        {
            "candidate": platform.CODE_OFFSET,
            "hook_buzzer_timer": 0x67EC,
            "hook_display_stop": 0x7D76,
            "hook_display_render": 0x8678,
        }
    )
    if set(sections) != set(spans):
        raise ValueError(f"Unaccounted linked sections: {set(sections) ^ set(spans)}")
    image = bytearray(source)
    for name, offset in spans.items():
        image[offset : offset + len(sections[name])] = sections[name]
    offset = platform.LAYOUT["descriptor_offset"]
    image[offset : offset + 32] = descriptor()
    return bytes(image)


class LocalLockDesk(Machine):
    """Execute native code with 8 KiB SRAM and masked byte EEPROM transport."""

    def __init__(self, image, symbols, *, locked=False):
        """Initialize platform modules, native scanners and a complementary record."""
        eeprom = bytearray(range(256))
        eeprom[0x60:0x62] = b"\xa5\x5a" if locked else b"\x5a\xa5"
        super().__init__(image, eeprom=eeprom)
        self.symbols = symbols
        self.instructions = 0
        self.peripheral_writes = []
        self.uc.mem_map(RAM + 0x1000, 0x1000)
        self.uc.mem_map(0x1FFFF000, 0x1000)
        self.uc.mem_map(0x40000000, 0x24000)
        self.uc.mem_map(0xE000E000, 0x2000)
        self.setreg(0x40015800, 0x10010440)
        self.setreg(0x1FFFF7CC, 64)
        self.attach_eeprom()
        self.stubs[FLASH + 0x94A0] = lambda machine: None
        self.call(symbols["platform_bootstrap"])
        self.call(symbols["tb01_init"])
        self.put(0xFC8, int(locked))
        self.put(0xFC9, 1)
        self.put(0xFCA, 0)
        self.initialize_scanners()
        self.foreground(0, 0)
        self.foreground(20, 0)
        self.foreground(40, 0)
        self.eeprom_writes.clear()
        self.initial_eeprom = bytes(self.eeprom)

    def _code(self, uc, address, size, data):
        self.instructions += 1
        super()._code(uc, address, size, data)

    def _write(self, uc, access, address, size, value, data):
        if not (RAM <= address and address + size <= RAM + 0x1400):
            self.peripheral_writes.append((address, size, value))
        self.writes.append((address, size, value, uc.reg_read(UC_ARM_REG_PC)))

    def setreg(self, address, value):
        """Seed a modeled register without executing a device write."""
        self.uc.mem_write(address, value.to_bytes(4, "little"))

    def reg(self, address):
        """Read one modeled little-endian MMIO register."""
        return int.from_bytes(self.uc.mem_read(address, 4), "little")

    def raw(self, pressed):
        """Set raw contacts using the controller's T/up/down/P1/P2/P3/M bits."""
        native = 0
        for source, target in ((0, 0), (1, 4), (2, 3), (3, 2), (4, 1), (5, 5), (6, 6)):
            native |= ((pressed >> source) & 1) << target
        self.pins(127 ^ native)

    def foreground(self, now, pressed, *, native=True):
        """Execute the linked foreground hook before the native dispatcher."""
        self.put(0x75C, now & 0xFFFFFFFF, 4)
        self.raw(pressed)
        entry = (
            self.symbols["trim_foreground"]
            if native
            else self.symbols["local_lock_controller_poll"]
        )
        return self.call(entry, stops={RETURN, FLASH + 0x868C})

    def advance(self, start, end, pressed, *, native=True):
        """Poll at 20 ms intervals, retaining native scanner and handler activity."""
        for now in range(start + 20, end + 1, 20):
            self.foreground(now, pressed, native=native)
        if (end - start) % 20:
            self.foreground(end, pressed, native=native)

    def blocked(self, kind="motion"):
        """Read the production latch predicates."""
        return bool(self.call(self.symbols[f"local_lock_controller_{kind}_blocked"]))

    def events(self):
        """Read all native click, long-release and double-click event counters."""
        return bytes(
            self.get(base + offset) for base in SCANNERS for offset in (12, 13, 14)
        )

    def d3(self, desired=None):
        """Run decoded D3 through the real parser and linked early UART route."""
        start = self.get(0xF5)
        body = bytes(
            (
                0xD3,
                1,
                0 if desired is None else 2 - desired,
                0xFA,
                0xFD,
            )
        )
        self.request(body)
        wire = self.response(start)
        return decode(wire) if wire else None

    def allow_native_storage(self):
        """Model ordinary native writes only for comparisons that expect those writes."""

        def write(machine):
            address = machine.uc.reg_read(UC_ARM_REG_R1)
            value = machine.uc.reg_read(UC_ARM_REG_R0)
            if address not in range(256) or value not in range(256):
                raise AssertionError(
                    "Native EEPROM write leaves the byte-addressed model"
                )
            machine.eeprom_writes.append((address, value))
            machine.eeprom[address] = value

        self.stubs[FLASH + 0x4938] = write

    def assert_only_record_changed(self):
        """Reject changes to unit, preset and every other EEPROM byte."""
        for offset, (before, after) in enumerate(zip(self.initial_eeprom, self.eeprom)):
            if offset not in (0x60, 0x61) and before != after:
                raise AssertionError(f"Protected EEPROM changed at {offset:#x}")


class LocalLockRuntimeTests(unittest.TestCase):
    """Check actual scanner races, admission, storage and motion entry behavior."""

    @classmethod
    def setUpClass(cls):
        """Compile once and require the external image's exact source identity."""
        cls.sections, cls.symbols, cls.build = compile_patch(
            LLVM_BIN,
            boot_profile=BOOT_PROFILE,
            buzzer=True,
            display=True,
            local_lock=True,
        )
        cls.image = linked_image(SOURCE_R7.read_bytes(), cls.sections)
        if hashlib.sha256(cls.image).hexdigest() != OUTPUT_FULL_SHA256:
            raise ValueError("Executed image differs from the pinned LK01 full image")
        cls.measurements = {}

    def desk(self, **kwargs):
        """Return a fresh exact native machine."""
        desk = LocalLockDesk(self.image, self.symbols, **kwargs)
        self.created_desks = getattr(self, "created_desks", []) + [desk]
        return desk

    def tearDown(self):
        """Retain the lowest observed model stack without implying hardware measurement."""
        for desk in getattr(self, "created_desks", []):
            previous = self.measurements.get("observed_minimum_sp", RAM + 0xFB8)
            self.measurements["observed_minimum_sp"] = min(previous, desk.min_sp)

    def complete_pair(self, desk, start=60):
        """Hold a captured pair through stability and the full three-second hold."""
        desk.foreground(start, 65)
        desk.advance(start, start + 3040, 65)

    def test_exact_hold_once_and_protected_storage(self):
        """Stable forty milliseconds precede the hold; a held pair writes once."""
        for locked in (False, True):
            desk = self.desk(locked=locked)
            desk.foreground(60, 65)
            desk.advance(60, 3080, 65)
            self.assertEqual(desk.eeprom_writes, [])
            desk.foreground(3099, 65)
            self.assertEqual(desk.eeprom_writes, [])
            desk.foreground(3100, 65)
            self.assertEqual(len(desk.eeprom_writes), 3)
            self.assertEqual(desk.get(0xFC8), int(not locked))
            self.assertEqual(desk.events(), bytes(24))
            self.assertEqual(desk.get(0xAAC, 4), 0)
            self.assertEqual(desk.get(0xABC, 4), 0)
            desk.advance(3100, 7100, 65)
            self.assertEqual(len(desk.eeprom_writes), 3)
            desk.assert_only_record_changed()

    def test_first_key_stays_native_before_capture(self):
        """Both first-key orders remain states zero through three for 1500 ms."""
        for key, base in ((64, 0xA60), (1, 0xA70)):
            desk = self.desk()
            desk.foreground(60, key)
            for now in range(80, 1561, 20):
                desk.foreground(now, key)
                self.assertIn(desk.get(base + 4), range(4))
                self.assertEqual(desk.events(), bytes(24))
                self.assertEqual(desk.get(0xAAC, 4), 0)
                self.assertEqual(desk.get(0xABC, 4), 0)
            desk.foreground(1560, 65)
            desk.advance(1560, 4600, 65)
            self.assertEqual(len(desk.eeprom_writes), 3)
            desk.assert_only_record_changed()

    def test_second_contact_between_poll_and_native_scanner(self):
        """An edge after detection cannot publish a click before the next capture."""
        for first in (0, 64, 1):
            desk = self.desk()
            desk.foreground(60, first)
            armed = [False]

            def inject(machine, address):
                if address == FLASH + 0x51B4 and not armed[0]:
                    machine.raw(65)
                    armed[0] = True

            desk.on_instruction = inject
            desk.foreground(80, first)
            desk.on_instruction = None
            self.assertTrue(armed[0])
            self.assertEqual(desk.events(), bytes(24))
            desk.foreground(100, 65)
            desk.advance(100, 3140, 65)
            self.assertEqual(len(desk.eeprom_writes), 3)
            self.assertEqual(desk.events(), bytes(24))
            self.assertEqual((desk.get(0xAAC, 4), desk.get(0xABC, 4)), (0, 0))

    def test_pending_clicks_and_long_release_cannot_be_captured(self):
        """A T or preset tap and an M long release retain their native pending work."""
        for key, duration in ((1, 60), (8, 60), (64, 600)):
            desk = self.desk()
            desk.foreground(60, key)
            desk.advance(60, 60 + duration, key)
            desk.foreground(80 + duration, 0)
            desk.foreground(100 + duration, 0)
            pending = bytes(desk.uc.mem_read(RAM + 0xA10, 128))
            self.assertTrue(
                any(pending[index] for index in range(4, 128, 16))
                or desk.get(0xAAC, 4) > 100 + duration
                or desk.get(0xABC, 4) > 100 + duration
            )
            desk.foreground(120 + duration, 65)
            desk.advance(120 + duration, 3400 + duration, 65)
            self.assertEqual(desk.eeprom_writes, [])
            desk.assert_only_record_changed()

    def test_admission_rechecks_every_guard_and_no_release_gate(self):
        """All shared busy flags reject D3 SET without changing EEPROM or trim gate."""
        for offset in (*BUSY_OFFSETS, 0x75A):
            desk = self.desk()
            desk.put(offset, 1)
            self.assertEqual(desk.call(self.symbols["local_lock_request"], 1), 2)
            self.assertEqual(desk.eeprom_writes, [])
            self.assertEqual(desk.d3(1)[3], 2)
            self.assertEqual(desk.eeprom_writes, [])
            self.assertEqual(desk.get(0x106C), 0)
        for address in (0x4000043C, 0x40000440):
            desk = self.desk()
            desk.setreg(address, 1)
            self.assertEqual(desk.call(self.symbols["local_lock_request"], 1), 2)
            self.assertEqual(desk.eeprom_writes, [])
        for state in (1, 2, 3, 5, 255):
            desk = self.desk()
            desk.put(0xFE0, state)
            self.assertEqual(desk.call(self.symbols["local_lock_request"], 1), 2)

    def test_captured_hold_consumes_guards_and_trim_ownership(self):
        """A guard appearing mid-hold cannot queue a toggle for later admission."""
        for offset in BUSY_OFFSETS:
            desk = self.desk()
            desk.foreground(60, 65)
            desk.foreground(100, 65)
            desk.put(offset, 1)
            desk.foreground(120, 65, native=False)
            desk.put(offset, 0)
            desk.advance(120, 3400, 65)
            self.assertEqual(desk.eeprom_writes, [])
        desk = self.desk()
        desk.call(self.symbols["trim_controller_boot"])
        self.assertEqual(desk.call(self.symbols["trim_controller_blocks_native"]), 1)
        release_gate = desk.get(0x106C)
        self.assertEqual(desk.call(self.symbols["local_lock_request"], 1), 2)
        self.assertEqual(desk.get(0x106C), release_gate)
        desk.foreground(60, 65, native=False)
        desk.advance(60, 3400, 65, native=False)
        self.assertEqual(desk.eeprom_writes, [])

    def test_write_failures_stay_locked_and_never_repeat(self):
        """Dropped invalidation, data and commit writes fail closed without a retry."""
        for drop_at in (0, 1, 2):
            desk = self.desk()
            write = desk.stubs[FLASH + 0x4938]

            def fail(machine):
                machine.drop_writes = len(machine.eeprom_writes) == drop_at
                write(machine)

            desk.stubs[FLASH + 0x4938] = fail
            self.complete_pair(desk)
            self.assertEqual(desk.get(0xFC8), 1)
            self.assertEqual(desk.get(0xFCA), 1)
            count = len(desk.eeprom_writes)
            desk.advance(3100, 7100, 65)
            self.assertEqual(len(desk.eeprom_writes), count)
            desk.assert_only_record_changed()

    def test_uart_override_cancels_and_cannot_form_physical_pair(self):
        """UART M/T activity remains distinct from raw physical recognition."""
        desk = self.desk()
        desk.put(0x7B0, 1)
        desk.put(0x7B4, 1)
        desk.advance(40, 4000, 0, native=False)
        self.assertEqual(desk.eeprom_writes, [])
        desk = self.desk()
        desk.foreground(60, 65)
        desk.put(0x7AE, 1)
        desk.foreground(80, 65)
        desk.put(0x7AE, 0)
        desk.advance(80, 4000, 65)
        self.assertEqual(desk.eeprom_writes, [])

    def test_ordinary_short_sequences_match_native_dispatcher(self):
        """T double taps and M then preset retain the native scanner outcome."""
        sequences = (
            (
                (60, 1),
                (80, 1),
                (100, 1),
                (120, 0),
                (140, 0),
                (180, 1),
                (200, 1),
                (220, 0),
                (240, 0),
                (600, 0),
            ),
            (
                (60, 64),
                (80, 64),
                (100, 64),
                (120, 0),
                (140, 0),
                (440, 0),
                (460, 8),
                (480, 8),
                (500, 0),
                (520, 0),
                (840, 0),
            ),
        )
        for sequence in sequences:
            actual, native = self.desk(), self.desk()

            def native_write(machine):
                address = machine.uc.reg_read(UC_ARM_REG_R1)
                value = machine.uc.reg_read(UC_ARM_REG_R0)
                self.assertIn(address, range(256))
                self.assertIn(value, range(256))
                machine.eeprom_writes.append((address, value))
                machine.eeprom[address] = value

            for machine in (actual, native):
                machine.stubs[FLASH + 0x4938] = native_write
            previous = 40
            for end, pressed in sequence:
                for now in range(previous + 20, end + 1, 20):
                    actual.foreground(now, pressed)
                    native.put(0x75C, now, 4)
                    native.raw(pressed)
                    native.call(FLASH + 0x51B4)
                previous = end
            self.assertEqual(
                bytes(actual.uc.mem_read(RAM, 0xAC4)),
                bytes(native.uc.mem_read(RAM, 0xAC4)),
            )
            self.assertEqual(actual.eeprom_writes, native.eeprom_writes)

    def test_pre_capture_other_contacts_match_native_dispatcher(self):
        """M then Up+Down and T then another key keep their exact native behavior."""
        for first, combined in ((64, 64 | 2 | 4), (1, 1 | 8)):
            for second_at in (100, 1600):
                with self.subTest(first=first, combined=combined, second_at=second_at):
                    actual, native = self.desk(), self.desk()
                    for desk in (actual, native):
                        desk.allow_native_storage()
                    for now in range(60, second_at + 821, 20):
                        pressed = first if now < second_at else combined
                        if now >= second_at + 400:
                            pressed = 0
                        actual.foreground(now, pressed)
                        native.put(0x75C, now, 4)
                        native.raw(pressed)
                        native.call(FLASH + 0x51B4)
                        self.assertFalse(actual.blocked("inputs"), f"masked at {now}")
                        self.assertEqual(
                            bytes(actual.uc.mem_read(RAM, 0xAC4)),
                            bytes(native.uc.mem_read(RAM, 0xAC4)),
                            f"native state differs at {now}",
                        )
                        self.assertEqual(actual.eeprom_writes, native.eeprom_writes)

    def test_pre_capture_interruptions_do_not_release_native_keys(self):
        """Gap and rejected remote SETs cannot create a release of held M or T."""
        for key in (64, 1):
            for origin in ("gap", "d3", "d9"):
                with self.subTest(key=key, origin=origin):
                    actual, native = self.desk(), self.desk()
                    for now in (60, 80, 100):
                        actual.foreground(now, key)
                        native.foreground(now, key)
                    if origin == "d3":
                        self.assertEqual(actual.d3(1)[3], 2)
                    elif origin == "d9":
                        body = bytes((0xD9, 1, 1, 0xFA, 0xFD)) + bytes(12)
                        actual.uc.mem_write(RAM + 0x414, body)
                        actual.put(0x124, len(body) + 1)
                        self.assertEqual(
                            actual.call(self.symbols["local_lock_controller_route"]), 0
                        )
                    start = 240 if origin == "gap" else 120
                    for now in range(start, 701, 20):
                        actual.foreground(now, key)
                        native.put(0x75C, now, 4)
                        native.raw(key)
                        native.call(FLASH + 0x51B4)
                        self.assertFalse(actual.blocked("inputs"))
                        self.assertEqual(
                            bytes(actual.uc.mem_read(RAM + 0x754, 0xAC4 - 0x754)),
                            bytes(native.uc.mem_read(RAM + 0x754, 0xAC4 - 0x754)),
                            f"native control state differs at {now}",
                        )
                        self.assertEqual(actual.eeprom_writes, [])
                    actual.foreground(720, 65)
                    actual.advance(720, 3900, 65)
                    self.assertEqual(actual.eeprom_writes, [])

    def test_stop_and_all_motion_entries_survive_remote_unlock(self):
        """A held locked key stops once and blocks every start after D3 unlock."""
        starts = [bytes((0x17, subcommand, 1)) for subcommand in range(3, 10)]
        starts += [bytes((0x17, 1, 0, 1)), direct_body()]
        starts += [bytes((0xD5, 1, op, 0xFA, 0xFD, 0, 0, 0, 0, 0)) for op in (1, 2, 3)]
        for key in (1, 2, 4, 8, 16, 32, 64):
            desk = self.desk(locked=True)
            desk.put(0x148, 1)
            desk.put(0x88D, 1)
            desk.foreground(500, key, native=False)
            self.assertTrue(desk.blocked())
            self.assertEqual(desk.get(0x148), 0)
            self.assertIn(FLASH + 0x34E0, desk.visited)
            desk.put(0x88D, 0)
            desk.put(0x75A, 0, 2)
            desk.setreg(0x4000043C, 0)
            desk.setreg(0x40000440, 0)
            reply = desk.d3(0)
            self.assertIsNotNone(reply)
            self.assertEqual(desk.get(0xFC8), 0)
            self.assertTrue(desk.blocked())
            for body in starts:
                before = bytes(desk.uc.mem_read(RAM + 0x7AD, 8))
                desk.uc.mem_write(RAM + 0x414, body)
                desk.put(0x124, len(body) + 1)
                self.assertEqual(
                    desk.call(self.symbols["local_lock_controller_route"]), 1
                )
                self.assertEqual(bytes(desk.uc.mem_read(RAM + 0x7AD, 8)), before)
            desk.foreground(520, 0, native=False)
            desk.foreground(559, 0, native=False)
            self.assertTrue(desk.blocked())
            desk.foreground(560, 0, native=False)
            self.assertFalse(desk.blocked())

    def test_pending_native_motion_cannot_start_after_stop(self):
        """Accepted alt motion or UART keys cannot start after either Stop entry."""
        for pending in (0x895, 0x148, 0x7AE, 0x7AF, 0x7B1, 0x7B2, 0x7B3):
            for origin in ("local", "remote"):
                with self.subTest(pending=hex(pending), origin=origin):
                    desk = self.desk(locked=True)
                    desk.put(pending, 1)
                    if pending in (0x7AE, 0x7AF, 0x7B1, 0x7B2, 0x7B3):
                        scanner = {
                            0x7AE: 0xA10,
                            0x7AF: 0xA20,
                            0x7B1: 0xA30,
                            0x7B2: 0xA40,
                            0x7B3: 0xA50,
                        }[pending]
                        desk.put(scanner + 4, 2)
                        desk.put(scanner + 8, 240, 4)
                        desk.put(scanner + 12, 1)
                    if origin == "local":
                        desk.foreground(60, 1)
                        self.assertTrue(desk.blocked())
                    else:
                        desk.request(bytes((0x17, 1, 0, 0)))
                    desk.call(FLASH + 0x9458)
                    desk.advance(60, 600, 1)
                    self.assertEqual(desk.get(0x88D), 0)
                    self.assertEqual(desk.reg(0x4000043C), 0)
                    self.assertEqual(desk.reg(0x40000440), 0)
                    self.assertEqual(desk.get(pending), 0)
                    self.assertEqual(desk.events(), bytes(24))
                    desk.foreground(620, 0)
                    desk.advance(620, 1000, 0)
                    self.assertEqual(desk.get(0x88D), 0)
                    self.assertEqual(desk.reg(0x4000043C), 0)
                    self.assertEqual(desk.reg(0x40000440), 0)

    def test_reset_watchdog_remains_the_raw_key_pause_path(self):
        """RS01 pauses via its existing watchdog without a parallel native brake."""
        desk = self.desk(locked=True)
        desk.put(0xFE4, 0x52533031, 4)
        desk.put(0xFE0, 1)
        desk.put(0xFD0, 0x12345678, 4)
        desk.put(0xFD4, 100000, 4)
        desk.put(0x88D, 2)
        desk.put(0x88C, 3)
        desk.setreg(0x4000043C, 100)
        desk.foreground(60, 1, native=False)
        self.assertNotIn(FLASH + 0x34E0, desk.visited)
        desk.call(FLASH + 0x944C)
        self.assertEqual(desk.get(0xFE0), 3)
        self.assertEqual(desk.get(0xFE1), 3)
        self.assertEqual(desk.get(0xFD0, 4), 0)
        self.assertEqual(desk.reg(0x4000043C), 0)
        self.assertEqual(desk.reg(0x40000440), 0)

    def test_sleep_selector_and_real_sleep_return(self):
        """The real linked sleep wrapper cancels a hold and retains its motion latch."""
        desk = self.desk(locked=True)
        desk.foreground(60, 65)
        desk.put(0x754, 2)
        desk.uc.reg_write(UC_ARM_REG_R4, RAM + 0x754)
        desk.call(self.symbols["trim_selector"])
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_R1), 0)
        boundaries = []

        def wake(machine):
            boundaries.append(machine.get(LOCAL_SLOT + 24))
            machine.put(0x75C, 500000, 4)
            machine.raw(65)

        desk.stubs[FLASH + 0x7568] = wake
        desk.call(self.symbols["tb01_sleep"])
        self.assertEqual(boundaries, [6])
        self.assertEqual(desk.get(LOCAL_SLOT + 16, 4), 500000)
        self.assertTrue(desk.blocked())
        desk.put(0x754, 0)
        desk.advance(500000, 503200, 65)
        self.assertEqual(desk.eeprom_writes, [])
        desk.foreground(503220, 0)
        desk.foreground(503260, 0)
        self.assertFalse(desk.blocked())

    def test_masked_apply_clears_uart_overrun(self):
        """ORE raised during the EEPROM delay is cleared before mask restoration."""
        desk = self.desk()
        delay = desk.stubs[FLASH + 0x7B18]

        def inject(machine):
            delay(machine)
            self.assertEqual(machine.uc.reg_read(UC_ARM_REG_PRIMASK), 1)
            machine.setreg(0x4001381C, 8)

        desk.stubs[FLASH + 0x7B18] = inject
        self.assertEqual(desk.call(self.symbols["local_lock_request"], 1), 0)
        self.assertIn((0x40013820, 4, 8), desk.peripheral_writes)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), 0)
        writes = [write for write in desk.writes if write[0] == 0x40013820]
        self.assertEqual(len(writes), 1)

    def test_reregister_resets_slot_and_all_hook_abi(self):
        """Module init resets stale local state; ordinary hooks restore saved registers."""
        desk = self.desk()
        desk.uc.mem_write(RAM + LOCAL_SLOT, bytes([255]) * 32)
        desk.call(self.symbols["local_lock_controller_init"])
        self.assertEqual(desk.get(LOCAL_SLOT, 4), 0x4C4B3031)
        self.assertEqual(desk.get(LOCAL_SLOT + 24), 0)
        self.assertEqual(desk.get(LOCAL_SLOT + 27), 0)
        for mask in (0, 1):
            for name in (
                "local_lock_controller_poll",
                "local_lock_controller_before_sleep",
                "local_lock_controller_after_wake",
                "local_lock_request",
            ):
                desk.uc.reg_write(UC_ARM_REG_PRIMASK, mask)
                saved = {
                    register: desk.uc.reg_read(register) for register in REGS[4:12]
                }
                stack = desk.uc.reg_read(UC_ARM_REG_SP)
                desk.call(self.symbols[name], 0)
                self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), mask)
                self.assertEqual(desk.uc.reg_read(UC_ARM_REG_SP), stack)
                self.assertEqual(
                    {register: desk.uc.reg_read(register) for register in REGS[4:12]},
                    saved,
                )

    def test_native_uart_isr_overrun_paths(self):
        """The exact USART ISR needs its CR3 error enable to clear ORE itself."""
        for cr3 in (0, 1 << 8):
            desk = self.desk()
            desk.setreg(0x40013808, cr3)
            desk.setreg(0x4001381C, 8)
            desk.peripheral_writes.clear()
            desk.call(FLASH + 0x8114)
            clears = [
                write for write in desk.peripheral_writes if write[0] == 0x40013820
            ]
            self.assertEqual(clears, [] if cr3 == 0 else [(0x40013820, 4, 8)])

    def test_native_stop_nonstop_hook_registers_and_flags(self):
        """Nonzero native suboperations preserve the displaced comparison ABI."""
        for value in (1, 2, 255):
            original, hooked = self.desk(), self.desk()
            original.uc.mem_write(FLASH + 0x7D76, bytes.fromhex("d0780128"))
            original.uc.ctl_remove_cache(FLASH + 0x7D76, FLASH + 0x7D7A)
            for desk in (original, hooked):
                desk.uc.mem_write(RAM + 0x414, bytes((0x17, 1, 0, value)))
                for register, seeded in (
                    (UC_ARM_REG_R0, 0x111),
                    (UC_ARM_REG_R1, 0x222),
                    (UC_ARM_REG_R2, RAM + 0x414),
                    (UC_ARM_REG_R3, 0x333),
                    (UC_ARM_REG_R4, 0x444),
                    (UC_ARM_REG_R12, 0xCCC),
                ):
                    desk.uc.reg_write(register, seeded)
                desk.call(FLASH + 0x7D76, stops={FLASH + 0x7D7A})
            for register in (*REGS[:13], UC_ARM_REG_SP, UC_ARM_REG_APSR):
                self.assertEqual(
                    original.uc.reg_read(register), hooked.uc.reg_read(register)
                )

    def test_expired_deadlines_fault_recovery_and_get_remains_readonly(self):
        """Expired private deadlines and a storage fault allow repair; GET never writes."""
        desk = self.desk(locked=True)
        desk.eeprom[0x60:0x62] = b"\0\0"
        desk.put(0xFCA, 1)
        desk.put(0xAAC, 1, 4)
        desk.put(0xABC, 2, 4)
        desk.foreground(60, 0)
        self.complete_pair(desk, 80)
        self.assertEqual(desk.get(0xFC8), 0)
        self.assertEqual(desk.get(0xFCA), 0)
        count = len(desk.eeprom_writes)
        desk.put(0x88D, 1)
        self.assertEqual(desk.d3()[3], 0)
        self.assertEqual(len(desk.eeprom_writes), count)

    def test_feedback_segments_and_native_priority(self):
        """Loc, UnL, Err and busy shift exact glyphs without changing PB2."""
        glyphs = (
            (0x38, 0x5C, 0x58),
            (0x3E, 0x54, 0x38),
            (0x79, 0x50, 0x50),
            (0x7C, 0x6D, 0x6E),
        )
        for feedback, expected in enumerate(glyphs, 1):
            desk = self.desk()
            desk.put(LOCAL_SLOT + 22, 1, 2)
            desk.put(LOCAL_SLOT + 30, feedback)
            captured = []

            def capture(machine, address):
                if address == FLASH + 0x458C:
                    captured.append(machine.uc.reg_read(UC_ARM_REG_R0))

            desk.on_instruction = capture
            for _ in range(3):
                self.assertEqual(
                    desk.call(self.symbols["local_lock_controller_render"]), 1
                )
            self.assertEqual(tuple(captured), expected)
            self.assertFalse(
                any(
                    address == 0x48000428 and value & 4
                    for address, _, value in desk.peripheral_writes
                )
            )
            desk.put(0x88D, 1)
            self.assertEqual(desk.call(self.symbols["local_lock_controller_render"]), 0)
            desk.put(0x88D, 0)
            desk.put(0x75C, 0x10000, 4)
            self.assertEqual(desk.call(self.symbols["local_lock_controller_render"]), 0)

    def test_idle_poll_gap_keeps_ordinary_physical_input_available(self):
        """A long idle gap consumes no ordinary unlocked key press."""
        desk = self.desk()
        desk.foreground(500, 64)
        self.assertFalse(desk.blocked("inputs"))
        desk.foreground(520, 64)
        self.assertIn(desk.get(0xA64), (1, 2, 3))
        self.assertEqual(desk.eeprom_writes, [])

    def test_poll_instruction_budget(self):
        """An idle hold poll has a bounded instruction count well below 100 ms."""
        desk = self.desk()
        desk.foreground(60, 65)
        maximum = 0
        for now in range(80, 3081, 20):
            previous = desk.instructions
            desk.put(0x75C, now, 4)
            desk.raw(65)
            desk.call(FLASH + 0x866E, stops={FLASH + 0x868C})
            maximum = max(maximum, desk.instructions - previous)
        self.assertLess(maximum, 24000)
        self.measurements["maximum_idle_hold_poll_instructions"] = maximum
        self.measurements["conservative_20_cycles_per_instruction_at_8mhz_ms"] = (
            maximum * 20 / 8000
        )


def main():
    """Run exact-image tests only when their explicit private fixture is supplied."""
    global LLVM_BIN, SOURCE_R7, REPORT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--source-r7", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args, remaining = parser.parse_known_args()
    LLVM_BIN, SOURCE_R7, REPORT = args.llvm_bin, args.source_r7, args.report
    suite = unittest.main(argv=[__file__, *remaining], verbosity=2, exit=False)
    if REPORT is not None:
        REPORT.write_text(
            json.dumps(
                {
                    "source_image_sha256": hashlib.sha256(
                        SOURCE_R7.read_bytes()
                    ).hexdigest(),
                    "linked_sha256": hashlib.sha256(
                        LocalLockRuntimeTests.image
                    ).hexdigest(),
                    "tests": suite.result.testsRun,
                    "successful": suite.result.wasSuccessful(),
                    "measurements": LocalLockRuntimeTests.measurements,
                    "source_sha256": LocalLockRuntimeTests.build["source_sha256"],
                    "section_sha256": LocalLockRuntimeTests.build["section_sha256"],
                    "hardware": False,
                },
                indent=2,
            )
            + "\n"
        )
    raise SystemExit(not suite.result.wasSuccessful())


if __name__ == "__main__":
    main()
