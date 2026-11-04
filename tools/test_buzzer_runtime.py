#!/usr/bin/env python3
"""Exercise linked BZ01 code with explicit native and MMIO models, without hardware."""

import argparse
from pathlib import Path
import unittest

from unicorn.arm_const import UC_ARM_REG_PRIMASK, UC_ARM_REG_R0

from build_motion_trim_patch import BOOT_PROFILE
import build_platform_patch as platform
import build_tb01_patch as legacy
from compile_motion_trim import compile_patch
from test_platform_patch import PlatformDesk
from test_tb01_patch import FLASH, RAM, RTC_CR

LLVM_BIN = None


def profile_packet(operation=9, *, volume=50, on_ms=5, off_ms=5, count=3, minutes=2):
    """Encode one flat profile, including minutes only for a timer request."""
    body = bytes(
        [
            0xD7,
            3,
            operation,
            0xFA,
            0xFD,
            0x4E,
            0x20,
            volume,
            on_ms >> 8,
            on_ms & 255,
            off_ms >> 8,
            off_ms & 255,
            count,
            0,
            0,
            0,
            0,
            0,
            0,
        ]
    )
    return body + minutes.to_bytes(2, "big") if operation == 10 else body


class BuzzerDesk(PlatformDesk):
    """Reuse the native-helper models while executing the new audio hooks and code."""

    def __init__(self, sections, symbols, old_sections, old_symbols):
        """Initialize the shared RAM and install the timer replacement trampoline."""
        base = {name: sections[name] for name in (*platform.HOOKS, "candidate")}
        base["descriptor"] = platform.descriptor()
        self.mmio = []
        super().__init__(base, symbols, old_sections, old_symbols)
        self.uc.mem_write(FLASH + 0x67EC, sections["hook_buzzer_timer"])
        # Authored return instruction for a modeled native continuation.
        self.uc.mem_write(FLASH + 0x67F4, b"\x70\xbd")
        self.callbacks.pop(FLASH + 0x67EC)
        self.extra_project_ranges += [(FLASH + 0x67EC, FLASH + 0x67F6)]
        self.uc.ctl_remove_cache(FLASH, FLASH + 0x10000)
        self.timer_minutes = 0
        self.put(0xF4, 200)
        self.setreg(0x48001400, 0x4000)
        self.setreg(0x40021004, 0x00280008)
        self.setreg(0x40021014, 0x00400001)

    def _code(self, uc, address, size, data):
        if self.platform_loaded and address == FLASH + 0x67EC:
            self.timer_minutes = uc.reg_read(UC_ARM_REG_R0)
        if self.platform_loaded and address == FLASH + 0x67F4:
            uc.reg_write(UC_ARM_REG_R0, self.timer_minutes)
            self._rtc_set()
            return
        super()._code(uc, address, size, data)

    def _write(self, uc, access, address, size, value, data):
        if self.platform_loaded:
            if 0x40014000 <= address < 0x40014048 or 0x40020000 <= address < 0x40020068:
                assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
                self.mmio.append((address, value))
                return
            if address == 0x48001418:
                assert value in (0x80, 0x800000)
                self.pin = value == 0x80
                self.edges.append((self.get(0x75C, 4), self.pin))
                self.mmio.append((address, value))
                return
        super()._write(uc, access, address, size, value, data)

    def _send(self):
        length = self.get(0xE4)
        assert length in (6, 8)
        self.replies.append(bytes(self.uc.mem_read(RAM + 0x395, length)))

    def alarm(self):
        """Model RTC delivery, then execute the actual sound-entry interception hook."""
        self.setreg(RTC_CR, 0)
        self.put(0x8A8, 1)
        self.call(FLASH + 0x3408, 0xFF, 3)

    def short(self, operation):
        """Send a short v3 command with a matched nonce."""
        return self.send(bytes([0xD7, 3, operation, 0xFA, 0xFD]))


class BuzzerRuntimeTests(unittest.TestCase):
    """Check ownership, terminal states and inherited protocol compatibility."""

    @classmethod
    def setUpClass(cls):
        """Compile the production objects once with the pinned Cortex-M0 toolchain."""
        cls.sections, cls.symbols, cls.build = compile_patch(
            LLVM_BIN, boot_profile=BOOT_PROFILE, buzzer=True
        )
        cls.old_sections, cls.old_symbols = legacy.compile_patch(LLVM_BIN)

    def desk(self):
        """Return a fresh initialized controller model."""
        return BuzzerDesk(
            self.sections, self.symbols, self.old_sections, self.old_symbols
        )

    def assert_released(self, desk):
        """Require the pin, DMA, timer and playback state to be inactive."""
        self.assertFalse(desk.pin)
        self.assertEqual(desk.reg(0x40020058) & 1, 0)
        self.assertEqual(desk.reg(0x40021018) & (1 << 16), 0)
        self.assertEqual(desk.get(0x1028), 0)

    def test_short_pulses_and_full_sequence(self):
        """Five-millisecond pulses finish once, without leaving PWM enabled."""
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet())[5], 0)
        for now in (5, 10, 15, 20, 25):
            desk.tick(now)
        self.assert_released(desk)
        self.assertEqual(
            [edge for edge in desk.edges if edge[1]],
            [(0, True), (10, True), (20, True)],
        )

    def test_owner_refusal_and_no_loud_fallback(self):
        """A foreign timer, DMA owner or lost PWM claim never becomes full-volume output."""
        for address, value in ((0x40021018, 1 << 16), (0x40020058, 1), (0x40021004, 0)):
            desk = self.desk()
            desk.setreg(address, value)
            self.assertEqual(desk.send(profile_packet())[5], 3)
            self.assertFalse(desk.pin)
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet())[5], 0)
        desk.tick(5)
        desk.setreg(0x40014038, 0)
        desk.tick(10)
        self.assertFalse(desk.pin)
        self.assertEqual(desk.get(0x1028), 0)

    def test_alarm_profile_survives_sleep_and_finishes(self):
        """An armed profile survives sleep, then clears the native alarm on completion."""
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet(10))[5], 0)
        self.assertEqual(desk.get(0x10AC), 1)
        desk.call(self.symbols["tb01_sleep"])
        self.assertEqual(desk.get(0x10AC), 1)
        desk.alarm()
        self.assertEqual(desk.get(0x10AC), 2)
        for now in (5, 10, 15, 20, 25):
            desk.tick(now)
        self.assert_released(desk)
        self.assertEqual(desk.get(0x8A8), 0)
        self.assertEqual(desk.get(0x8B4, 4), 0)
        self.assertEqual(desk.get(0x10AC), 0)

    def test_silent_alarm_and_start_failure_are_terminal(self):
        """Mute and peripheral refusal consume the own alarm without native fallback."""
        for silent in (False, True):
            desk = self.desk()
            self.assertEqual(
                desk.send(profile_packet(10, volume=0 if silent else 50))[5], 0
            )
            if not silent:
                desk.setreg(0x40021004, 0)
            desk.alarm()
            self.assertFalse(desk.pin)
            self.assertEqual(desk.get(0x8A8), 0)
            self.assertEqual(desk.get(0x10AC), 0)

    def test_alarm_cancel_ack_and_sleep(self):
        """Both cancel commands, panel acknowledgement and sleep terminate an own alarm."""
        for ending in ("cancel", "stop", "key", "sleep"):
            desk = self.desk()
            self.assertEqual(desk.send(profile_packet(10))[5], 0)
            desk.alarm()
            if ending in ("cancel", "stop"):
                self.assertEqual(desk.short(2 if ending == "cancel" else 4)[5], 0)
            elif ending == "key":
                desk.put(0x8A8, 0)
                desk.tick(1)
            else:
                desk.call(self.symbols["tb01_sleep"])
            self.assert_released(desk)
            self.assertEqual(desk.get(0x8A8), 0)

    def test_native_replacement_and_pending_cancel(self):
        """Native timer replacement and explicit cancellation cannot retain the old sound."""
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet(10))[5], 0)
        self.assertEqual(desk.send(profile_packet(10))[5], 2)
        desk.call(FLASH + 0x67EC, 2)
        self.assertEqual(desk.get(0x10AC), 0)
        self.assertEqual(desk.short(2)[5], 0)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0)

    def test_guard_failure_releases_alarm_even_with_bad_profile(self):
        """A peripheral ownership tag permits cleanup despite a corrupt profile or guard."""
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet(10))[5], 0)
        desk.alarm()
        desk.put(0x10A4, 0xFF)
        desk.put(0x13F0, 0, 4)
        desk.tick(1)
        self.assert_released(desk)
        self.assertEqual(desk.get(0x8A8), 0)
        self.assertEqual(desk.get(0x10AC), 0)

    def test_dma_error_and_direct_alarm_guard_failure(self):
        """DMA transfer errors and direct-output guard faults release the owned alarm."""
        for volume in (50, 100):
            desk = self.desk()
            self.assertEqual(desk.send(profile_packet(10, volume=volume))[5], 0)
            desk.alarm()
            if volume == 50:
                desk.setreg(0x40020000, 0x80000)
            else:
                desk.put(0x13F0, 0, 4)
            desk.tick(1)
            self.assert_released(desk)
            self.assertEqual(desk.get(0x8A8), 0)

    def test_alarm_preempts_beep_and_native_tail_does_not(self):
        """An alarm replaces a current beep, while an exhausted AA callback leaves it playing."""
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet(10, on_ms=25))[5], 0)
        self.assertEqual(desk.send(profile_packet(on_ms=100))[5], 0)
        desk.put(0x14E, 3)
        desk.call(self.symbols["tb01_native_priority"], 0xAA, 3)
        self.assertTrue(desk.pin)
        desk.alarm()
        self.assertEqual(desk.get(0x1024, 2), 25)
        self.assertEqual(desk.get(0x10AC), 2)
        self.assertEqual(desk.short(4)[5], 0)
        self.assert_released(desk)

    def test_cancel_beep_preserves_armed_timer(self):
        """Stopping immediate playback does not remove an alarm that has not fired."""
        desk = self.desk()
        self.assertEqual(desk.send(profile_packet(10))[5], 0)
        self.assertEqual(desk.send(profile_packet())[5], 0)
        self.assertEqual(desk.short(4)[5], 0)
        self.assert_released(desk)
        self.assertEqual(desk.get(0x10AC), 1)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0x1100)
        desk.alarm()
        self.assertEqual(desk.get(0x10AC), 2)

    def test_inherited_irq_mask_rejects_without_effect(self):
        """A caller with interrupts disabled cannot enter the RTC setup wait."""
        desk = self.desk()
        desk.uc.reg_write(UC_ARM_REG_PRIMASK, 1)
        self.assertIsNone(desk.send(profile_packet(10)))
        self.assertEqual(desk.get(0x10AC), 0)
        self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0)
        self.assertEqual(desk.uc.reg_read(UC_ARM_REG_PRIMASK), 1)

    def test_cancel_waits_for_idle_unless_own_alarm_is_playing(self):
        """ARMED and native timers keep motion, menu, key and lock cancellation gates."""
        for own_profile in (False, True):
            for offset, width in (
                (0x88D, 1),
                (0x75A, 2),
                (0x18C, 1),
                (0xFC8, 1),
                (0x7AD, 1),
            ):
                desk = self.desk()
                if own_profile:
                    self.assertEqual(desk.send(profile_packet(10))[5], 0)
                else:
                    desk.setreg(RTC_CR, 0x1100)
                desk.put(offset, 1, width)
                before = desk.reg(RTC_CR)
                self.assertEqual(desk.short(2)[5], 2)
                self.assertEqual(desk.reg(RTC_CR), before)
                self.assertEqual(desk.get(0x10AC), int(own_profile))

    def test_failed_profile_publication_cancels_new_timer(self):
        """A fault or delivered event before publication cannot leave a native alarm behind."""
        for fault in (False, True):
            desk = self.desk()
            setter = desk._rtc_set

            def fail_after_set():
                setter()
                desk.put(
                    0x13F0 if fault else 0x8A8, 0 if fault else 1, 4 if fault else 1
                )

            desk._rtc_set = fail_after_set
            self.assertEqual(desk.send(profile_packet(10))[5], 3)
            self.assertEqual(desk.reg(RTC_CR) & 0x1100, 0)
            self.assertEqual(desk.get(0x8A8), 0)
            self.assertEqual(desk.get(0x10AC), 0)
            self.assertFalse(desk.pin)

    def test_bad_packets_and_tx_space_do_not_mutate(self):
        """Malformed profiles, unsupported old ops and TX exhaustion leave sound idle."""
        desk = self.desk()
        for packet in (
            profile_packet(10)[:-1],
            profile_packet(10) + b"\0",
            profile_packet(7),
            profile_packet(10, minutes=1),
            profile_packet(10, minutes=721),
        ):
            reply = desk.send(packet)
            self.assertTrue(reply is None or reply[5] == 1)
            self.assertEqual(desk.get(0x1028), 0)
            self.assertEqual(desk.get(0x10AC), 0)
        desk.put(0xF4, 20)
        self.assertIsNone(desk.send(profile_packet()))
        self.assertFalse(desk.pin)

    def test_legacy_beep_and_clock_wrap(self):
        """The old v1 beep stays at 50 ms minimum and new deadlines cross uint32 wrap."""
        desk = self.desk()
        self.assertEqual(desk.command(3, 49, 50, 1), 1)
        self.assertEqual(desk.command(3, 50, 50, 1), 0)
        desk.tick(50)
        self.assert_released(desk)
        desk.put(0x75C, 0xFFFFFFFD, 4)
        self.assertEqual(desk.send(profile_packet(count=1))[5], 0)
        desk.tick(1)
        self.assertTrue(desk.pin)
        desk.tick(2)
        self.assert_released(desk)


def main():
    """Run patch instruction tests with an explicitly selected compiler directory."""
    global LLVM_BIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    args = parser.parse_args()
    LLVM_BIN = args.llvm_bin
    unittest.main(argv=[__file__], verbosity=2)


if __name__ == "__main__":
    main()
