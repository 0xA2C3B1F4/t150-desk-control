#!/usr/bin/env python3
"""Execute saved RS01 Cortex-M0 instructions, parser, hooks and UART offline.

EEPROM transport and stock SysTick physical sampling are fixture boundaries.
The lease, reset admission, actuator writes and command paths execute unchanged.
"""

import argparse
import json
from pathlib import Path
import struct

from unicorn.arm_const import (
    UC_ARM_REG_PC,
    UC_ARM_REG_PRIMASK,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R4,
    UC_ARM_REG_SP,
    UC_ARM_REG_XPSR,
)

from build_reset_speed_patch import build, sha256
from test_child_lock_patch import (
    FLASH,
    RAM,
    REGS,
    RETURN,
    RX_RING,
    SP,
    Machine,
    decode,
    frame,
    require,
)


class ResetMachine(Machine):
    """Extend the offline machine with RS01 state and modeled timer registers."""

    lowest_sp = SP

    def _code(self, uc, address, size, data):
        super()._code(uc, address, size, data)
        type(self).lowest_sp = min(type(self).lowest_sp, self.min_sp)

    def __init__(self, image, symbols, **kwargs):
        """Map timer registers, install stock-helper stubs and initialize RS01 state."""
        super().__init__(image, **kwargs)
        self.symbols = symbols
        self.uc.mem_map(0x40000000, 0x2000)
        self.stubs[0x080035E4] = lambda m: None
        self.stubs[0x08004798] = lambda m: None
        self.require_atomic_admission = False
        self.call(symbols["rs_init"])
        self.put(0x891, 0)

    def _write(self, uc, access, address, size, value, data):
        if self.require_atomic_admission and address in (
            RAM + 0x88D,
            RAM + 0x7AF,
            RAM + 0x187,
        ):
            require(
                uc.reg_read(UC_ARM_REG_PRIMASK) == 1,
                "reset admission/hold store was interruptible",
            )
        if 0x40000400 <= address < 0x40000450:
            require(
                address - 0x40000400
                in (0, 4, 0x14, 0x1C, 0x20, 0x28, 0x2C, 0x30, 0x3C, 0x40, 0x44),
                f"unexpected TIM3 store {address:#x}",
            )
            self.writes.append((address, size, value, uc.reg_read(UC_ARM_REG_PC)))
            return
        super()._write(uc, access, address, size, value, data)

    def send(self, op=0, nonce=1, token=None, arg=0):
        """Dispatch an RS01 request and return its validated diagnostic reply or None.

        Use the current lease token by default. Consume emitted TX bytes, then check
        reply length, correlation and admission flags.
        """
        if token is None:
            token = self.get(0xFD0, 4)
        start = self.get(0xF5)
        self.request(
            bytes([0xD5, 1, op])
            + nonce.to_bytes(2, "big")
            + token.to_bytes(4, "big")
            + bytes([arg])
        )
        wire = self.response(start)
        if not wire:
            return None
        self.put(0xF4, self.get(0xF5))  # fixture consumes the emitted UART bytes
        reply = decode(wire)
        require(len(reply) == 48, "D6 body length")
        require(reply[:3] == b"\xd6\x01\x1f", "RS01 admission diagnostics capability")
        require(
            reply[3] == op and int.from_bytes(reply[12:14], "big") == nonce,
            "response correlation",
        )
        require(reply[46] <= 6 and reply[47] <= 6, "admission result enum")
        require(
            bool(reply[8] & 1) == (reply[46] == 0)
            and bool(reply[8] & 2) == (reply[47] == 0),
            "admission reasons agree with flags",
        )
        return reply

    def get_reply(self, nonce=1):
        """Query RS01 capabilities with a zero lease token and the supplied nonce."""
        return self.send(0, nonce, 0)

    def state(self):
        """Read the emulated RS01 ownership-state byte from its reserved RAM slot."""
        return self.get(0xFE0)

    def clock(self, tick):
        """Set the emulated stock tick counter, wrapping to 32 bits."""
        self.put(0x75C, tick & 0xFFFFFFFF, 4)

    def systick(self):
        """Execute the image's hooked SysTick handler through its vector entry."""
        self.call(int.from_bytes(self.uc.mem_read(FLASH + 0x283C, 4), "little") & ~1)

    def step(self, stub=None):
        """Run the hooked stock motion step with an optional temporary helper stub."""
        if stub is not None:
            self.stubs[0x08005A58] = stub
        self.call(FLASH + 0x8688, stops={FLASH + 0x868C})
        self.stubs.pop(0x08005A58, None)

    def pwm(self, n, direction=1):
        """Call the image's PWM writer with a simulated speed and direction."""
        self.put(0x75A, n, 2)
        self.call(FLASH + 0x70D8, direction, n)

    def output(self):
        """Read both modeled TIM3 compare registers without modeling motor movement."""
        return tuple(
            int.from_bytes(self.uc.mem_read(a, 4), "little")
            for a in (0x4000043C, 0x40000440)
        )

    def interrupt_step(self, point):
        """Inject lease expiry at a chosen instruction in the stock motion step."""
        self.interrupt_call(FLASH + 0x8688, point, FLASH + 0x868C)

    def interrupt_call(self, entry, point, end):
        """Simulate an IRQ at a real stock instruction with hardware frame room.

        Unicorn executes the hooked SysTick and its original tick increment.
        Exception register stacking/restoration is modeled explicitly here.
        """
        self.call(entry, stops={point})
        saved = {
            r: self.uc.reg_read(r) for r in (*REGS, UC_ARM_REG_XPSR, UC_ARM_REG_PRIMASK)
        }
        self.uc.reg_write(UC_ARM_REG_SP, saved[UC_ARM_REG_SP] - 32)
        self.clock(self.get(0xFD4, 4) - 1)
        self.systick()
        require(
            self.state() == 3 and self.output() == (0, 0),
            "in-flight IRQ cuts and pauses",
        )
        for r, value in saved.items():
            self.uc.reg_write(r, value)
        self.stops = {end}
        self.stopped_at = None
        self.uc.emu_start(point | 1, FLASH + 0x10000, count=100000)
        require(self.stopped_at == end, "interrupted stock call returns")

    def start(self):
        """Obtain a fresh challenge and require an accepted reset START reply."""
        self.get_reply()
        r = self.send(1, 2)
        require(r[4] == 0 and r[5] == 1 and r[8] & 8, "eligible START")
        return r


def run(image, symbols):
    """Execute RS01 admission, lease, parser and actuator-write regressions offline.

    Return completed case names and stack usage. Peripheral sampling, EEPROM
    transport and selected stock transitions use fixtures, so these checks do
    not establish physical behavior.
    """
    cases = []

    def done(name):
        cases.append(name)

    m = ResetMachine(image, symbols)
    r = m.get_reply(0xFAFD)
    require(
        r[4] == 0 and r[5] == 0 and int.from_bytes(r[14:18], "big") != 0,
        "fresh capability token",
    )
    require(
        r[8] & 1 and m.send(1, 2)[4] == 0 and m.state() == 1,
        "idle START does not need a stock reset flag",
    )
    for mode in (0, 8, 10, 11, 12):
        for position in (0.0, 14.5, 15.0, 170.0, 707.0):
            m = ResetMachine(image, symbols)
            m.put(0x88D, mode)
            m.put(0x7D4, struct.unpack("<I", struct.pack("<f", position))[0], 4)
            require(
                m.get(0x891) == 0 and m.get(0x8B9) == 0,
                "no forced stock eligibility flags",
            )
            r = m.get_reply()
            require(
                r[8] & 1,
                "idle/recoverable mode advertises START at every tested height",
            )
            r = m.send(1, 2)
            require(
                r[4] == 0 and r[5] == 1 and m.get(0x7D4, 4) == 0,
                "START establishes calibration coordinates",
            )
    done(
        "explicit reset from idle and recoverable modes at lower, boundary and elevated coordinates"
    )
    for mode in (1, 2, 3, 4, 5, 6, 7, 9, 13):
        m = ResetMachine(image, symbols)
        m.put(0x88D, mode)
        r = m.get_reply()
        expected = 4 if mode in (1, 2) else 6
        require(
            not r[8] & 1
            and r[46] == expected
            and m.send(1, 2)[4] == expected
            and m.state() == 0,
            "unsupported/busy mode still rejects START",
        )
    done("busy and unsupported controller modes still refuse reset")
    for offset in (
        0x88F,
        0x890,
        0x148,
        0x8B8,
        0x892,
        0x894,
        0xAA0,
        0xAA1,
        0x18E,
        0x8BA,
        0x754,
    ):
        m = ResetMachine(image, symbols)
        m.put(offset, 1)
        require(m.get_reply()[46] == 6, "GET reports START guard")
        r = m.send(1, 2)
        require(r[4] == 6 and m.state() == 0, f"guard {offset:#x}")
    done("all independent reset guards")
    for offset in (0xFC8, 0xFCA):
        m = ResetMachine(image, symbols)
        m.put(offset, 1)
        require(m.get_reply()[46] == 5, "GET reports START lock")
        require(m.send(1, 2)[4] == 5, "CL01 locked admission")
    m = ResetMachine(image, symbols)
    m.pins(0x7E)
    m.put(0xFC9, 1)
    m.get_reply()
    require(m.send(1, 2)[4] == 5, "CL01 release gate admission")
    done("CL01 lock, fault and held release gate reject")
    for primask in (0, 1):
        m = ResetMachine(image, symbols, primask=primask)
        m.require_atomic_admission = True
        m.start()
        require(m.get(0x7AF) == 1 and m.get(0x187) == 1, "controller owns hold")
        old = m.get(0xFD0, 4)
        m.clock(100)
        m.send(3, 3)
        deadline = m.get(0xFD4, 4)
        require(
            m.send(3, 3, old)[4] == 2 and m.get(0xFD4, 4) == deadline,
            "replayed renewal cannot extend",
        )
        require(
            m.uc.reg_read(UC_ARM_REG_PRIMASK) == primask, "incoming mask preservation"
        )
    done("start and rotating-token renewal, duplicate rejection, PRIMASK")
    m = ResetMachine(image, symbols)
    m.start()
    saved = {r: m.uc.reg_read(r) for r in REGS[4:12]}
    m.systick()
    require(
        all(m.uc.reg_read(r) == v for r, v in saved.items()),
        "SysTick wrapper preserves exception callee registers",
    )
    require(m.uc.reg_read(UC_ARM_REG_SP) == SP, "SysTick wrapper stack unwinds")
    done("actual SysTick callee register ABI and stack unwind")
    for rebound in (False, True):
        m = ResetMachine(image, symbols)
        m.start()
        if rebound:
            m.put(0x88D, 1)
            m.put(0x88F, 1)
            m.step(lambda n: None)
            require(m.state() == 2, "rebound observation")
        m.pwm(600)
        r = m.send(4, 0xFDFE, 0)
        require(
            r[5] == 3 and r[6] == 1 and r[8] & 12 == 4 and m.output() == (0, 0),
            "cancel must cut both phases",
        )
        require(
            m.get(0x7AF) == 0 and m.get(0x187) == 0 and not m.get(0xFD0, 4),
            "cancel invalidates lease/hold",
        )
        m.pwm(900)
        require(m.output() == (0, 0), "stale output computation cannot re-enable PWM")
        before = m.get(0x88D)
        m.step(lambda n: n.put(0x88D, 99))
        require(m.get(0x88D) == before, "paused main freezes stock machine")
        m.get_reply(4)
        require(m.send(3, 5)[4] == 3 and m.state() == 3, "renew never resumes")
        r = m.send(2, 6)
        require(
            r[4] == 0 and r[5] == (2 if rebound else 1),
            "explicit resume preserves phase",
        )
    done("cancel descent/rebound, stale actuator block, freeze, explicit resume")
    # Execute the actual scanner/Down dispatcher as well as the motion loop.
    # A held synthetic Down sets AA0 after 120 ticks, including in reset mode.
    # Earlier pause fixtures skipped this path and missed the R4 resume guard.
    for rebound in (False, True):
        for stop_kind in ("cancel", "ordinary", "lease", "panel"):
            m = ResetMachine(image, symbols)
            m.initialize_scanners()
            m.put(0x170, RAM + 0xB00, 4)
            m.put(0x860, 500, 4)
            m.start()
            for tick in range(1, 602, 30):
                m.clock(tick)
                m.call(FLASH + 0x51B4)
                m.step()
            require(
                m.get(0xAA0) == 1 and m.state() == 1,
                "real held Down establishes AA0 during descent",
            )
            require(
                any(
                    a == RAM + 0xAA0 and v == 1 and pc == FLASH + 0x4BFA
                    for a, _, v, pc in m.writes
                ),
                "AA0 came from original Down handler",
            )
            if rebound:
                m.put(0x762, 501, 2)
                m.clock(631)
                m.step()
                require(
                    m.state() == 2, "real bottom contact reaches rebound before pause"
                )
                m.visited.clear()
                m.clock(661)
                m.call(FLASH + 0x51B4)
                require(
                    FLASH + 0x4B70 not in m.visited,
                    "rebound 88F gate skips the Down handler, so no AA0 store IRQ there",
                )
            m.pwm(600)
            position = m.get(0x7D4, 4)
            target = m.get(0xA9C, 4)
            if stop_kind == "cancel":
                m.send(4, 20, 0)
            elif stop_kind == "ordinary":
                m.request(bytes([0x17, 1, 0, 0]))
                m.put(0xF4, m.get(0xF5))
            elif stop_kind == "lease":
                m.clock(m.get(0xFD4, 4))
                m.systick()
            else:
                m.pins(0x7E)
                m.systick()
            require(
                m.state() == 3 and m.output() == (0, 0), "pause cuts actual held reset"
            )
            require(m.get(0xAA0) == 0, "pause releases the owned Down action marker")
            require(
                m.get(0x7D4, 4) == position and m.get(0xA9C, 4) == target,
                "pause preserves calibration coordinates",
            )
            if stop_kind == "panel":
                require(
                    m.get_reply(21)[47] == 5, "held physical key still blocks resume"
                )
                m.pins(0x7F)
            m.clock(m.get(0x75C, 4) + 35)
            m.call(FLASH + 0x51B4)
            m.step()
            r = m.get_reply(22)
            require(
                r[47] == 0 and r[8] & 2 and m.state() == 3,
                "released held-reset pause advertises resume without motion",
            )
            require(
                m.send(3, 23)[4] == 3 and m.state() == 3,
                "renew cannot resume a released hold",
            )
            r = m.send(2, 24)
            require(
                r[4] == 0 and r[5] == (2 if rebound else 1),
                "explicit resume after real held reset",
            )
            require(
                m.get(0x7D4, 4) == position and m.get(0xA9C, 4) == target,
                "resume retains original phase coordinates",
            )
    done(
        "real held-Down scanner pause/resume in both phases via Cancel, Stop, lease and panel"
    )
    for offset in (
        0x890,
        0x148,
        0x8B8,
        0x892,
        0x894,
        0xAA0,
        0xAA1,
        0x18E,
        0x8BA,
        0x754,
    ):
        m = ResetMachine(image, symbols)
        m.start()
        m.send(4, 3, 0)
        m.step()
        m.put(offset, 1)
        # Existing scanner release consumes pending M-save event 18E. Check
        # that event at admission, before another scanner-release boundary.
        if offset != 0x18E:
            m.step()
        require(
            m.get_reply(4)[47] == 6 and m.send(2, 5)[4] == 6,
            "new independent guard still blocks resume",
        )
        require(
            m.get(offset) == 1 and m.state() == 3 and m.output() == (0, 0),
            "resume does not clear guards to proceed",
        )
    for mode, direction, marker in ((2, 3, 2), (2, 0, 1), (1, 0, 1), (10, 0, 1)):
        m = ResetMachine(image, symbols)
        m.start()
        m.put(0x88D, mode)
        m.put(0x88C, direction)
        m.put(0xAA0, marker)
        m.send(4, 3, 0)
        require(
            m.get(0xAA0) == marker,
            "pause preserves unowned or unexpected action marker",
        )
    m = ResetMachine(image, symbols)
    m.put(0xAA0, 1)
    m.send(4, 3, 0)
    require(m.get(0xAA0) == 1, "idle cancellation does not clear another action marker")
    done(
        "owned-hold cleanup preserves unrelated guards, unexpected markers and unowned phases"
    )
    for point in (
        FLASH + 0x4BFA,
        FLASH + 0x59B8,
        FLASH + 0x59BE,
        FLASH + 0x59C0,
        FLASH + 0x59C2,
        FLASH + 0x59C4,
    ):
        m = ResetMachine(image, symbols)
        m.initialize_scanners()
        m.put(0x170, RAM + 0xB00, 4)
        m.put(0x860, 500, 4)
        m.start()
        for tick in range(1, 602, 30):
            m.clock(tick)
            m.call(FLASH + 0x51B4)
            m.step()
        require(m.get(0xAA0) == 1, "real scanner established hold before interrupt")
        m.clock(631)
        if point == FLASH + 0x4BFA:
            m.interrupt_call(FLASH + 0x51B4, point, RETURN)
            require(
                m.get(0xAA0) == 1,
                "interrupted stock scanner reasserts AA0 after the ISR cut",
            )
            m.step()
        else:
            m.put(0x762, 501, 2)
            m.interrupt_step(point)
        require(
            m.state() == 3 and m.get(0xAA0) == 0 and m.output() == (0, 0),
            "main boundary finishes owned hold cleanup",
        )
        require(m.get(0xFE8, 4) == 0, "deferred cleanup consumed once")
        m.clock(m.get(0x75C, 4) + 35)
        m.call(FLASH + 0x51B4)
        m.step()
        require(
            m.state() == 3 and m.get(0xAA0) == 0 and m.output() == (0, 0),
            "next real scanner pass remains paused after the IRQ race",
        )
        m.get_reply(31)
        r = m.send(2, 32)
        require(
            r[4] == 0 and r[5] == (1 if point == FLASH + 0x4BFA else 2),
            "explicit resume after interrupted held scanner or phase transition",
        )
    done(
        "SysTick inside held scanner and each bottom-transition store completes cleanup before explicit resume"
    )
    for offset, value, expected in (
        (0x890, 1, 6),
        (0x7B0, 1, 4),
        (0x7AE, 1, 4),
        (0xFC9, 1, 5),
        (0x88C, 0, 3),
    ):
        m = ResetMachine(image, symbols)
        m.start()
        m.send(4, 3, 0)
        if offset == 0xFC9:
            m.pins(0x7E)
        m.put(offset, value)
        r = m.get_reply(4)
        require(
            r[5] == 3 and r[47] == expected and not r[8] & 2,
            "paused GET reports exact RESUME refusal category",
        )
        require(
            m.send(2, 5)[4] == expected and m.state() == 3 and m.output() == (0, 0),
            "refused RESUME leaves outputs off",
        )
    m = ResetMachine(image, symbols)
    m.start()
    m.put(0x88D, 1)
    m.put(0x88F, 1)
    m.step(lambda n: None)
    m.send(4, 3, 0)
    require(m.get_reply(4)[47] == 0, "rebound-owned 88F does not block RESUME")
    done(
        "paused admission diagnostics distinguish guard, keys, release gate, phase and rebound"
    )
    for start in (0, 0xFFFFFC00):
        m = ResetMachine(image, symbols)
        m.clock(start)
        r = m.start()
        t = m.get(0xFD0, 4)
        m.clock(start + 1498)
        m.systick()
        require(m.state() == 1, "lease remains valid before deadline")
        m.pwm(400)
        m.systick()
        require(
            m.state() == 3 and m.get(0xFE1) == 2 and m.output() == (0, 0),
            "lease expires at exact tick including wrap",
        )
        require(
            m.send(3, 3, t)[4] == 2 and m.state() == 3,
            "old renewal after expiry cannot resume",
        )
    done("SysTick lease boundary and uint32 wrap expiry")
    m = ResetMachine(image, symbols)
    m.put(0x76A, 747, 2)
    m.start()
    for tick in range(300, 180001, 300):
        m.clock(tick)
        r = m.send(3, (tick // 300 + 2) & 0xFFFF)
        require(
            r[4] == 0 and r[5] == 1,
            "renewable lease must not impose a short whole-operation deadline",
        )
    m.clock(181499)
    m.systick()
    require(
        m.state() == 3 and m.get(0xFE1) == 2,
        "long operation still stops when final lease expires",
    )
    done(
        "synthetic 180-second ownership with repeated renewals, then independent lease expiry"
    )
    m = ResetMachine(image, symbols)
    m.start()
    m.pwm(500)
    m.put(0xFC8, 1)
    m.pins(0x7E)
    m.systick()
    require(
        m.state() == 3 and m.get(0xFE1) == 3 and m.output() == (0, 0),
        "raw local key interrupts even if CL01 locks after start",
    )
    done("local key interruption through CL01")
    m = ResetMachine(image, symbols)
    m.start()
    m.clock(1500)
    m.send(3, 3)
    require(m.state() == 3, "expired renewal observed before command admission")
    done("atomic expiry before request")
    m = ResetMachine(image, symbols)
    r = m.get_reply()
    old = m.get(0xFD0, 4)
    m.clock(1500)
    require(
        m.send(1, 2, old)[4] == 2 and m.state() == 0, "delayed START challenge expires"
    )
    done("short idle START challenge")
    m = ResetMachine(image, symbols)
    m.start()
    old = m.get(0xFD0, 4)
    m.call(symbols["rs_init"])
    require(
        m.state() == 0 and m.get(0xFE2) == 0 and not m.get(0xFD0, 4), "RAM state reset"
    )
    require(
        m.send(1, 3, old)[4] == 2 and m.send(3, 4, old)[4] == 2,
        "preboot token rejected after reboot",
    )
    done("reboot token/ownership/speed defaults")
    m = ResetMachine(image, symbols)
    m.start()
    deadline = m.get(0xFD4, 4)
    token = m.get(0xFD0, 4)
    m.clock(500)
    m.get_reply(4)
    require(
        m.get(0xFD4, 4) == deadline and m.get(0xFD0, 4) == token,
        "GET never renews or changes active token",
    )
    done("GET is not a heartbeat")
    m = ResetMachine(image, symbols)
    m.get_reply()
    token = m.get(0xFD0, 4)
    m.clock(1490)
    m.get_reply(2)
    require(
        m.get(0xFD0, 4) == token, "unexpired inactive GET preserves the admission token"
    )
    m.clock(1600)
    require(
        m.send(1, 3, token)[4] == 0,
        "fresh inactive GET refreshes unused admission challenge",
    )
    done("fresh inactive GET has a full admission window")
    m = ResetMachine(image, symbols)
    m.start()
    for body in (
        bytes([0x17, 4, 1]),
        bytes([0x17, 2, 1]),
        bytes([0x19, 0, 0, 1]),
        bytes([0x13, 1, 0]),
        bytes([0xEF]),
        bytes([0xD3, 1, 1, 0, 1]),
    ):
        start = len(m.writes)
        m.request(body)
        require(
            m.state() == 1 and m.get(0x7AF) == 1 and m.get(0x754) == 0,
            "competing mutation blocked",
        )
        require(
            not any(a in (RAM + 0x824, RAM + 0x754) for a, *_ in m.writes[start:]),
            "no stock mutation store",
        )
    m.request(bytes([0x17, 1, 0, 0]))
    require(m.state() == 3 and m.output() == (0, 0), "ordinary Stop maps cancellation")
    done("competing UART writes blocked, ordinary Stop preempts")
    m = ResetMachine(image, symbols)
    m.start()
    m.put(0x88D, 10)
    m.step(lambda n: None)
    require(m.state() == 5 and m.output() == (0, 0), "controller fault detected")
    m.pwm(700)
    require(m.output() == (0, 0), "fault actuator remains cut")
    m.step(lambda n: n.put(0x88D, 1))
    require(m.get(0x88D) == 10, "fault freeze")
    done("controller fault freezes and cannot restore output")
    m = ResetMachine(image, symbols)
    m.start()
    m.put(0x88D, 0)
    m.put(0x88C, 0)
    m.step(lambda n: None)
    require(m.state() == 5, "descent standstill is not completion")
    m = ResetMachine(image, symbols)
    m.start()
    m.put(0x88D, 1)
    m.put(0x88F, 1)
    m.step(lambda n: None)
    m.put(0x88D, 0)
    m.put(0x88F, 0)
    m.step(lambda n: None)
    require(
        m.state() == 5 and not m.get(0xFD0, 4),
        "arbitrary mode0 after rebound is not proof of success",
    )
    done("standstill and arbitrary idle transitions cannot claim completion")
    for mode, rebound, state, value, expected in (
        (1, 0, 0, 7, 2),
        (1, 0, 0, 1, 1),
        (1, 0, 0, 0, 0),
        (1, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF),
        (2, 0, 1, 7, 7),
        (1, 1, 2, 7, 7),
        (0, 0, 0, 7, 7),
    ):
        m = ResetMachine(image, symbols)
        m.get_reply()
        require(m.send(5, 2, arg=1)[4] == 0, "speed set")
        m.put(0x88D, mode)
        m.put(0x88F, rebound)
        m.put(0xFE0, state)
        m.put(0x9E8, value, 4)
        m.put(0x9CC, 33, 4)
        m.uc.reg_write(UC_ARM_REG_R4, RAM + 0x9C8)
        saved = {reg: m.uc.reg_read(reg) for reg in REGS[2:13]}
        m.uc.reg_write(UC_ARM_REG_XPSR, 0xA1000000)
        m.call(FLASH + 0x63C8, stops={FLASH + 0x63CC})
        require(m.get(0x9E8, 4) == expected, "ordinary-only reducing cap")
        require(
            m.uc.reg_read(UC_ARM_REG_R0) == 33
            and m.uc.reg_read(UC_ARM_REG_R1) == expected,
            "replaced original loads",
        )
        require(
            all(m.uc.reg_read(reg) == v for reg, v in saved.items()),
            "speed hook preserves registers",
        )
        require(
            m.uc.reg_read(UC_ARM_REG_XPSR) & 0xF8000000 == 0xA0000000,
            "speed hook preserves APSR",
        )
    done("actual speed hook preserves ABI and only reduces ordinary setpoint")
    # The original state machine and stock homing/rebound helpers execute.
    # A bounded average buffer and EEPROM transport are injected fixtures.
    m = ResetMachine(image, symbols)
    m.put(0x170, RAM + 0xB00, 4)
    m.put(0x860, 500, 4)
    m.start()
    for tick in (1, 30, 60, 90):
        m.clock(tick)
        m.step()
    require(
        m.state() == 1 and m.get(0x158, 4) == 2 and m.get(0xA9C, 4) == 0xFFFFF830,
        "original homing path and limit",
    )
    m.put(0x762, 501, 2)
    m.clock(120)
    m.step()
    require(
        m.state() == 2
        and m.get(0x88D) == 1
        and m.get(0x88F) == 1
        and m.get(0xA9C, 4) == 12,
        "original bottom contact transitions to L+6 rebound",
    )
    writes = []

    def saved_byte(n):
        address = n.uc.reg_read(UC_ARM_REG_R1)
        value = n.uc.reg_read(UC_ARM_REG_R0)
        require(
            address in (0x40, 0x41, 0x4A, 0x4B, 0x2A, 0x2B) and value < 256,
            "original homing save stays in position/state record",
        )
        writes.append((address, value))

    m.stubs[0x08004938] = saved_byte
    m.stubs[0x08007B18] = lambda n: None
    m.put(0x7D4, struct.unpack("<I", struct.pack("<f", 12.0))[0], 4)
    m.put(0x88C, 0)
    m.put(0x76A, 12, 2)
    m.clock(150)
    m.step()
    require(
        m.state() == 4 and m.output() == (0, 0) and len(writes) == 6,
        "actual stock completion and original save execute",
    )
    done(
        "original homing descent, bottom contact, rebound and completion EEPROM call sites"
    )
    m = ResetMachine(image, symbols)
    m.put(0x170, RAM + 0xB00, 4)
    m.put(0x860, 500, 4)
    m.start()
    m.put(0x762, 501, 2)
    m.clock(1)
    m.interrupt_step(0x080059BE)
    require(
        m.state() == 3 and m.get(0xFE3) == 2 and m.output() == (0, 0),
        "paused bottom transition reconciles to rebound without motion",
    )
    m.get_reply(10)
    r = m.send(2, 11)
    require(
        r[4] == 0 and r[5] == 2, "explicit resume after interrupted bottom transition"
    )
    done("IRQ during stock bottom-contact transition preserves explicit rebound resume")
    m = ResetMachine(image, symbols)
    m.put(0x170, RAM + 0xB00, 4)
    m.put(0x860, 500, 4)
    m.start()
    m.put(0x762, 501, 2)
    m.clock(1)
    m.step()
    m.put(0x7D4, struct.unpack("<I", struct.pack("<f", 12.0))[0], 4)
    m.put(0x88C, 0)
    m.put(0x76A, 12, 2)
    m.clock(30)
    calls = []
    m.stubs[0x08004938] = lambda n: calls.append(n.uc.reg_read(UC_ARM_REG_R1))
    m.stubs[0x08007B18] = lambda n: None
    m.interrupt_step(0x08005C08)
    require(
        m.state() == 4
        and m.output() == (0, 0)
        and len(calls) == 6
        and not m.get(0xFD0, 4),
        "exact completion hook observes successful stock save after ISR pause",
    )
    done(
        "IRQ during stock completion reports completed only after original save returns"
    )
    m = ResetMachine(image, symbols)
    calls = []
    m.stubs[0x080058D0] = lambda n: calls.append(1)
    before = bytes(m.uc.mem_read(RAM + 0xFD0, 48))
    m.call(FLASH + 0x5C0C, stops={FLASH + 0x5C10})
    require(
        calls == [1] and bytes(m.uc.mem_read(RAM + 0xFD0, 48)) == before,
        "unowned stock homing completion remains unchanged",
    )
    done(
        "non-owned physical homing completion still calls original save without RS01 state changes"
    )
    for mode in (8, 10, 11, 12):
        m = ResetMachine(image, symbols)
        m.put(0x170, RAM + 0xB00, 4)
        m.put(0x860, 500, 4)
        m.start()
        m.put(0x762, 501, 2)
        m.clock(1)
        m.step()
        m.put(0x7D4, struct.unpack("<I", struct.pack("<f", 12.0))[0], 4)
        m.put(0x88C, 0)
        m.put(0x76A, 12, 2)
        m.stubs[0x08004938] = lambda n: None
        m.stubs[0x08007B18] = lambda n: None
        m.clock(30)
        m.step()
        require(m.state() == 4, "stock completion before later fault")
        m.put(0x88D, mode)
        m.clock(60)
        m.step()
        r = m.get_reply(12)
        require(
            r[5] == 0 and r[9] == mode and r[8] & 1,
            "later fault clears sticky completion and advertises reset eligibility",
        )
        require(
            m.send(1, 13)[4] == 0,
            "fresh explicit START after completed reset and new fault",
        )
    m = ResetMachine(image, symbols)
    m.put(0xFE0, 4)
    m.put(0x895, 8)
    m.call(symbols["rs_after_main"])
    require(m.state() == 0, "nonzero previous mode also clears completed marker")
    done(
        "completed calibration followed by fault 8/10/11/12 allows a fresh explicit reset"
    )
    for phase in ("descent", "rebound", "paused", "fault"):
        m = ResetMachine(image, symbols)
        m.start()
        m.pwm(600)
        if phase == "rebound":
            m.put(0x88D, 1)
            m.put(0x88F, 1)
            m.step(lambda n: None)
        elif phase == "paused":
            m.send(4, 14, 0)
        elif phase == "fault":
            m.put(0x88D, 10)
            m.step(lambda n: None)
        start = m.get(0xF5)
        sequence = m.get(0x120, 2)
        m.request(bytes([0x17, 1, 0, 0]))
        r = decode(m.response(start))
        require(
            r == bytes([0x18, 1, 0, 0]) + sequence.to_bytes(2, "big"),
            "consumed Stop emits real stock ACK with shared sequence",
        )
        require(m.output() == (0, 0) and m.state() in (3, 5), "ACK follows output cut")
    done(
        "ordinary Stop before adapter discovery gets stock ACK during every owned or frozen phase"
    )
    m = ResetMachine(image, symbols)
    m.start()
    m.pwm(600)
    m.put(0xF4, 0)
    m.put(0xF5, 250)
    m.request(bytes([0x17, 1, 0, 0]))
    require(
        m.state() == 3 and m.output() == (0, 0) and m.get(0xF5) == 250,
        "full TX still cuts ordinary Stop without overwriting queue",
    )
    done("ordinary Stop still cuts with a full TX queue")
    m = ResetMachine(image, symbols)
    m.uc.mem_write(RAM + 0xFD0, b"\xa5" * 48)
    start = len(m.writes)
    m.call(symbols["rs_init"])
    require(
        bytes(m.uc.mem_read(RAM + 0xFD0, 20)) == b"\0" * 20
        and m.get(0xFE4, 4) == 0x52533031
        and m.get(0xFE8, 4) == 0,
        "all 28 bytes of state are initialized",
    )
    require(
        bytes(m.uc.mem_read(RAM + 0xFEC, 20)) == b"\xa5" * 20,
        "init does not touch adjacent unused reservation",
    )
    stores = [w for w in m.writes[start:] if RAM + 0xFD0 <= w[0] < RAM + 0x1000]
    require(
        stores[0][0] == RAM + 0xFE4
        and stores[0][2] == 0
        and stores[-1][0] == RAM + 0xFE4
        and stores[-1][2] == 0x52533031,
        "magic is invalidated first and published last",
    )
    done(
        "boot initializes every state field with magic invalidated first, within the 28-byte state"
    )
    m = ResetMachine(image, symbols)
    m.start()
    m.pwm(600)
    m.send(4, 15, 0)
    m.call(FLASH + 0x6090)
    require(
        m.output() == (0, 0),
        "stock timer re-enable after ISR cut cannot restore cleared compare values",
    )
    m.pwm(900)
    require(m.output() == (0, 0), "later stale writer remains gated")
    m.call(FLASH + 0x7814, 0x40000400, RAM + 0xB20)
    m.call(FLASH + 0x78A8, 0x40000400, RAM + 0xB20)
    require(
        m.output() == (0, 0),
        "actual generic startup writers preserve zero compare values with the proven zero parameter",
    )
    done(
        "stock timer-enable and zero-valued startup compare writers cannot bypass the paused output cut"
    )
    m = ResetMachine(image, symbols)
    m.stubs[0x08006548] = lambda n: None
    m.stubs[0x08006F24] = lambda n: None
    m.call(FLASH + 0x4A98)
    require(
        bytes(m.uc.mem_read(RAM, 0xC0)) == image[0x2800:0x28C0],
        "real startup vector-copy loop includes patched SysTick word",
    )
    done("actual startup copy installs the patched SysTick vector in SRAM")
    m = ResetMachine(image, symbols)
    m.uc.mem_write(RAM + 0x194, b"\xa5" * 0xE34)
    m.uc.mem_write(RAM + 0xFC8, b"\xa5" * 0x38)
    m.call(FLASH + 0x8294, 0, RAM + 0x194, 0xE34)
    require(
        bytes(m.uc.mem_read(RAM + 0x194, 0xE34)) == b"\0" * 0xE34,
        "real scatter zero helper clears its declared range",
    )
    require(
        bytes(m.uc.mem_read(RAM + 0xFC8, 0x38)) == b"\xa5" * 0x38,
        "scatter zero stops before CL01 and RS01 reservations",
    )
    done("actual startup zero loop ends at FC8 and preserves the upper reservations")
    m = ResetMachine(image, symbols)
    m.start()
    m.send(4, 16, 0)
    # Run the actual main-loop communication call and the frozen motion call.
    # Only unrelated RTC/display/input sampling is replaced by fixture stubs.
    for address in (0x08006984, 0x08003F1C, 0x080033D8, 0x080033E0, 0x080051B4):
        m.stubs[address] = lambda n: None
    m.clock(100)
    m.put(0x788, 1)
    m.put(0x78A, 500, 2)
    m.put(0x78C, 0, 4)
    wire = frame(bytes([0xD5, 1, 0, 0x12, 0x34, 0, 0, 0, 0, 0]))
    m.uc.mem_write(RAM + RX_RING, wire)
    m.put(0xFC, 0)
    m.put(0xFD, len(wire))
    start = m.get(0xF5)
    m.call(FLASH + 0x866E, stops={FLASH + 0x868C})
    replies = []
    begin = None
    escaped = False
    for index, byte in enumerate(m.response(start)):
        if begin is None:
            require(byte == 0xFA, "main-loop reply delimiter")
            begin = index
        elif escaped:
            escaped = False
        elif byte == 0xFE:
            escaped = True
        elif byte == 0xFD:
            replies.append(decode(m.response(start)[begin : index + 1]))
            begin = None
    require(
        begin is None and [r[0] for r in replies] == [0xD6, 0x05],
        "real main-loop poll delivers RS01 and periodic05 while paused",
    )
    require(
        m.state() == 3 and m.output() == (0, 0),
        "communication never resumes paused motion",
    )
    done(
        "real main-loop UART parser and periodic05 telemetry continue while motion is frozen"
    )
    m = ResetMachine(image, symbols)
    record = bytearray(b"\xff" * 256)
    record[0x60:0x62] = b"\x5a\xa5"
    m.eeprom = record
    m.attach_eeprom()
    m.stubs[0x08007620] = lambda n: None
    m.put(0xFD0, 0xFFFFFFFF, 4)
    m.put(0xFE0, 1)
    m.put(0xFE2, 1)
    m.put(0xFEC, 0xA5A5A5A5, 4)
    m.call(FLASH + 0x85F8, stops={FLASH + 0x85FC})
    require(
        m.state() == 0
        and not m.get(0xFD0, 4)
        and not m.get(0xFE2)
        and m.get(0xFC8) == 0,
        "actual boot hook clears ownership/speed before stock init and loads CL01",
    )
    require(
        m.get(0xFEC, 4) == 0xA6000000,
        "actual boot hook initializes parser state after dirty RAM",
    )
    done("actual boot hook and persistent CL01 reload")
    m = ResetMachine(image, symbols)
    m.put(0x88D, 1)
    m.put(0x9E8, 7, 4)
    m.call(symbols["rs_cap"])
    require(m.get(0x9E8, 4) == 7, "normal default leaves stock speed")
    m.get_reply()
    require(m.send(5, 2, arg=1)[4] == 4, "speed change refused during motion")
    done("default original speed and idle-only setting")
    m = ResetMachine(image, symbols)
    m.get_reply()
    token = m.get(0xFD0, 4)
    m.put(0xF4, 0)
    m.put(0xF5, 200)
    require(
        m.send(1, 2, token) is None and m.state() == 0,
        "no START if response cannot fit",
    )
    m = ResetMachine(image, symbols)
    m.start()
    m.pwm(800)
    m.put(0xF4, 0)
    m.put(0xF5, 200)
    require(
        m.send(4, 3, 0) is None and m.state() == 3 and m.output() == (0, 0),
        "CANCEL cuts despite full reply queue",
    )
    done("TX queue admission and fail-safe cancellation")
    return dict(
        passed=True,
        cases=cases,
        count=len(cases),
        hardware_tested=False,
        fixture_boundaries=[
            "EEPROM transport",
            "SysTick physical sampling",
            "selected stock transition fixtures",
        ],
        minimum_stack_observed=hex(ResetMachine.lowest_sp),
    )


def main():
    """Build RS01 from a saved CL01 image, then run and print the offline checks.

    Also write the report to --report when requested.
    """
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("--llvm-bin", type=Path)
    p.add_argument("--report", type=Path)
    a = p.parse_args()
    image, manifest = build(a.source.read_bytes(), a.llvm_bin)
    report = run(image, manifest["symbols"])
    report["output_sha256"] = sha256(image)
    if a.report:
        a.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
