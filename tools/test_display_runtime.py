#!/usr/bin/env python3
"""Execute linked display code with modeled native rendering and UART storage."""

import argparse
from pathlib import Path
import unittest

from unicorn.arm_const import UC_ARM_REG_PRIMASK

from build_motion_trim_patch import BOOT_PROFILE
import build_tb01_patch as legacy
from compile_motion_trim import compile_patch
from test_buzzer_runtime import BuzzerDesk, profile_packet
from test_tb01_patch import FLASH, RAM

LLVM_BIN = None


def request(operation=0, mode=0, seconds=0):
    """Encode the v4 GET or atomic mode/timeout SET with escapable nonce bytes."""
    return bytes(
        [0xD7, 4, operation, 0xFA, 0xFD, 0, mode, seconds >> 8, seconds & 255, 0, 0, 0]
    )


class DisplayDesk(BuzzerDesk):
    """Run actual project instructions while modeling the two native renderers."""

    def __init__(self, *args):
        """Initialize the platform and a circular TX buffer with visible render stubs."""
        super().__init__(*args)
        self.rendered = []
        self.callbacks[FLASH + 0x3F1C] = lambda: self.rendered.append("native")
        self.callbacks[FLASH + 0x36AC] = lambda: self.rendered.append("blank")
        for address in (FLASH + 0x3F1C, FLASH + 0x36AC):
            self.uc.mem_write(address, b"\x70\x47")
        self.put(0xF0, RAM + 0x200, 4)
        self.put(0xF4, 0)
        self.put(0xF5, 0)

    def display_request(self, body, room=255):
        """Enter the early route and decode bytes published by the real TX encoder."""
        head = self.get(0xF5)
        self.put(0xF4, (head + room + 1) & 255)
        self.uc.mem_write(RAM + 0x414, bytes(body))
        self.put(0x124, len(body) + 1)
        self.call(self.symbols["trim_controller_route"])
        end = self.get(0xF5)
        if head == end:
            return None
        wire = bytes(
            self.get(0x200 + (i & 255))
            for i in range(head, head + ((end - head) & 255))
        )
        assert wire[0] == 0xFA and wire[-1] == 0xFD
        decoded = bytearray()
        escape = False
        for byte in wire[1:-1]:
            if byte == 0xFE and not escape:
                escape = True
            else:
                decoded.append(byte)
                escape = False
        assert not escape
        checksum = 0
        for byte in decoded:
            checksum ^= byte
        assert checksum == 0
        return bytes(decoded[:-1])

    def render(self, now):
        """Call the foreground renderer at an explicit native tick value."""
        self.put(0x75C, now, 4)
        self.call(self.symbols["display_controller_render"])
        return self.rendered[-1]

    def display_state(self):
        """Snapshot the complete private display slot."""
        return bytes(self.uc.mem_read(RAM + 0x1030, 16))


class DisplayRuntimeTests(unittest.TestCase):
    """Check v4 validation, ownership, native attention and display-only writes."""

    @classmethod
    def setUpClass(cls):
        """Compile the cumulative display candidate and the inherited model support."""
        cls.sections, cls.symbols, _ = compile_patch(
            LLVM_BIN, boot_profile=BOOT_PROFILE, buzzer=True, display=True
        )
        cls.old_sections, cls.old_symbols = legacy.compile_patch(LLVM_BIN)

    def desk(self):
        """Return a fresh initialized display candidate."""
        return DisplayDesk(
            self.sections, self.symbols, self.old_sections, self.old_symbols
        )

    def test_defaults_boundaries_and_no_poll_restart(self):
        """GET and an identical SET leave an expired display expired."""
        desk = self.desk()
        reply = desk.display_request(request())
        self.assertEqual(reply, bytes([0xD8, 4, 0, 0xFA, 0xFD, 0, 3, 5, 0, 60]))
        self.assertEqual(desk.render(59999), "native")
        self.assertEqual(desk.render(60000), "blank")
        state = desk.display_state()
        for _ in range(10):
            self.assertEqual(desk.display_request(request())[7], 9)
            self.assertEqual(desk.display_request(request(1, 0, 60))[7], 9)
        self.assertEqual(desk.display_state(), state)
        self.assertEqual(desk.display_request(request(1, 0, 1))[5], 0)
        self.assertEqual(desk.render(60999), "native")
        self.assertEqual(desk.render(61000), "blank")
        self.assertEqual(desk.display_request(request(1, 1, 65535))[8:], b"\xff\xff")
        self.assertEqual(desk.render(70000000), "native")

    def test_elapsed_display_stays_blank_across_half_range(self):
        """A long idle period cannot wrap an expired display back to visible."""
        desk = self.desk()
        self.assertEqual(desk.render(60000), "blank")
        for now in ((60000 + 0x80000000) & 0xFFFFFFFF, 59999):
            self.assertEqual(desk.render(now), "blank")
            self.assertEqual(desk.display_request(request())[7] & 4, 0)
        self.assertEqual(desk.display_request(request(1, 0, 1))[5], 0)
        self.assertEqual(desk.render(59999), "native")
        self.assertEqual(desk.render(60999), "blank")

    def test_bad_lengths_versions_and_reserved_fields(self):
        """Old private frames and malformed input never change display state."""
        desk = self.desk()
        state = desk.display_state()
        for length in range(2, 30):
            if length == 12:
                continue
            body = (request(1, 1, 1) + bytes(30))[:length]
            self.assertIsNone(desk.display_request(body))
        for field in (2, 5, 6, 9, 10, 11):
            body = bytearray(request(1, 1, 1))
            body[field] = 255
            self.assertEqual(desk.display_request(body)[5:], bytes([1, 3, 16, 0, 0]))
        self.assertEqual(desk.display_request(request(1, 0, 0))[5], 1)
        self.assertEqual(desk.display_request(request(0, 0, 1))[5], 1)
        self.assertEqual(desk.display_state(), state)

    def test_tx_capacity_mask_and_corrupt_slot(self):
        """No space means no mutation; invalid readiness returns only sentinels."""
        desk = self.desk()
        state = desk.display_state()
        for room in range(25):
            self.assertIsNone(desk.display_request(request(1, 1, 1), room))
            self.assertEqual(desk.display_state(), state)
        desk.uc.reg_write(UC_ARM_REG_PRIMASK, 1)
        self.assertEqual(
            desk.display_request(request(1, 1, 1), 25)[5:], bytes([2, 3, 16, 0, 0])
        )
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), 1)
        self.assertEqual(desk.display_state(), state)
        desk.uc.reg_write(UC_ARM_REG_PRIMASK, 0)
        for field in (0x1030, 0x13F0):
            desk.put(field, 0, 4)
            self.assertEqual(
                desk.display_request(request())[5:], bytes([2, 3, 16, 0, 0])
            )
            self.assertEqual(desk.render(100000), "native")

    def test_trim_reservation_keeps_both_responses_possible(self):
        """v4 remains usable during trim ownership without taking its reserved bytes."""
        desk = self.desk()
        desk.put(0x1060, 0x54524331, 4)
        desk.put(0x106A, 2)
        desk.put(0x106B, 1)
        state = desk.display_state()
        self.assertIsNone(desk.display_request(request(1, 0, 1), 61))
        self.assertEqual(desk.display_state(), state)
        self.assertEqual(desk.display_request(request(1, 0, 1), 62)[5], 0)
        self.assertEqual(desk.get(0x106B), 1)

    def test_attention_and_pending_keys_leave_native_ram_unchanged(self):
        """Every observed attention flag and pending scanner forces native rendering."""
        offsets = [
            0x88D,
            0x75A,
            0x754,
            0x148,
            0x88F,
            0x890,
            0x892,
            0x893,
            0x894,
            0x8B8,
            0x8B9,
            0x8BA,
            0xAA0,
            0xAA1,
            0x18E,
            0x18C,
            0x8A8,
            0xFC9,
            0xFCA,
            *range(0x7AD, 0x7B5),
        ]
        offsets += [
            base + offset for base in range(0xA10, 0xA81, 16) for offset in (4, 12, 14)
        ]
        for offset in offsets:
            with self.subTest(offset=hex(offset)):
                desk = self.desk()
                self.assertEqual(desk.render(60000), "blank")
                desk.put(offset, 1)
                before = bytes(desk.uc.mem_read(RAM, 0xAC4))
                self.assertEqual(desk.render(60000), "native")
                self.assertEqual(bytes(desk.uc.mem_read(RAM, 0xAC4)), before)
                desk.put(offset, 0)
                self.assertEqual(desk.render(100000), "native")
                self.assertEqual(desk.render(159999), "native")
                self.assertEqual(desk.render(160000), "blank")
        desk = self.desk()
        desk.put(0xFC8, 1)
        desk.put(0x899, 3)
        self.assertEqual(desk.render(60000), "blank")

    def test_raw_keys_alarm_and_motor_outputs(self):
        """Raw key inputs, RTC pending flags and motor outputs bypass blanking."""
        for address, value in [
            (0x48000010, 0xFEFF),
            (0x48000410, 0xBFFF),
            (0x4000280C, 0x100),
            (0x40010414, 1 << 17),
            (0x4000043C, 1),
            (0x40000440, 1),
        ]:
            desk = self.desk()
            desk.setreg(address, value)
            self.assertEqual(desk.render(60000), "native")
            self.assertEqual(desk.reg(address), value)

    def test_custom_sound_does_not_extend_display(self):
        """An ordinary profile beep can play while the height digits remain blank."""
        desk = self.desk()
        self.assertEqual(desk.render(60000), "blank")
        self.assertEqual(desk.send(profile_packet(on_ms=200))[5], 0)
        self.assertEqual(desk.render(60001), "blank")
        desk.put(0x8A8, 1)
        self.assertEqual(desk.render(60002), "native")

    def test_accepted_motion_and_stop_restart_display(self):
        """Accepted target entry and handled Stop restart; rejected requests do not."""
        desk = self.desk()
        self.assertEqual(desk.render(60000), "blank")
        desk.call(desk.symbols["trim_motion_target"], 800, 780)
        self.assertEqual(desk.render(60001), "native")
        self.assertEqual(desk.render(120000), "blank")
        cases = (
            (bytes([0x17, 1, 0, 0]), 1, True),
            (bytes([0x17, 1, 0, 0]), 0, False),
            (bytes([0x17, 1, 0, 1]), 1, False),
            (bytes([0x17, 1, 0]), 1, False),
            (bytes([0xD5, 1, 4, 0, 0, 0, 0, 0, 0, 0]), 1, True),
            (bytes([0xD5, 1, 4, 0, 0, 0, 0, 0, 0, 0]), 0, False),
            (bytes([0xD5, 1, 3, 0, 0, 0, 0, 0, 0, 0]), 1, False),
            (bytes([0xD5, 1, 4, 0, 0, 0, 0, 0, 0, 1]), 1, False),
        )
        for body, handled, restarts in cases:
            desk = self.desk()
            self.assertEqual(desk.render(60000), "blank")
            desk.uc.mem_write(RAM + 0x414, body)
            desk.put(0x124, len(body) + 1)
            desk.call(desk.symbols["display_controller_reset_stop"], handled)
            self.assertEqual(desk.render(60001), "native" if restarts else "blank")
            self.assertEqual(desk.render(120000), "blank")

    def test_sleep_restarts_with_a_stopped_tick(self):
        """The sleep marker restores a full visible interval without inferring elapsed sleep."""
        desk = self.desk()
        self.assertEqual(desk.render(60000), "blank")
        desk.call(desk.symbols["tb01_sleep"])
        self.assertEqual(desk.render(60000), "native")
        self.assertEqual(desk.render(119999), "native")
        self.assertEqual(desk.render(120000), "blank")


def main():
    """Run linked-code tests with the selected pinned compiler directory."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    args, remaining = parser.parse_known_args()
    LLVM_BIN = args.llvm_bin
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
