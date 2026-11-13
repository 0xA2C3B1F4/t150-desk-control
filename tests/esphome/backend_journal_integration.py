#!/usr/bin/env python3
"""Real ESPHome host + PTY backend/journal isolation tests; no desk or network I/O."""

import argparse
import json
import os
import re
import select
import struct
import subprocess
import time
import zlib

from auto_target_integration import IDENTITY
from desk_test_support import (
    ROOT,
    build_host,
    encode,
    frames,
    open_pty,
    parse_args,
    test_env,
)

ACCESS = r"""#include <cstdio>
#include <cstring>
#include "esphome.h"
class BackendJournalAccess : public esphome::desk_uart::DeskUART {
 public:
  static bool ready(esphome::desk_uart::DeskUART *d) {
    auto mode = &BackendJournalAccess::direct_mode_;
    auto known = &BackendJournalAccess::direct_;
    auto detected = &BackendJournalAccess::target_detected_;
    auto access = &BackendJournalAccess::direct_access_allowed_;
    return (d->*mode) && (d->*known).known() && (d->*detected)() &&
        (d->*access)(esphome::millis());
  }
  static void inspect(esphome::desk_uart::DeskUART *d, const char *label) {
    auto backend = &BackendJournalAccess::target_backend_name_;
    auto move = &BackendJournalAccess::target_move_;
    auto journal = &BackendJournalAccess::journal_error_;
    auto active = &BackendJournalAccess::journal_active_;
    auto target = &BackendJournalAccess::target_number_;
    ESP_LOGI("fixture", "STATE=%s|%s|%d|%d|%d|%d", label, (d->*backend)(),
        (d->*move).dirty(), (d->*journal)() != nullptr, d->*active, (d->*target) != nullptr);
  }
  static void inert(esphome::desk_uart::DeskUART *d) {
    auto move = &BackendJournalAccess::target_move_;
    const auto phase = (d->*move).phase();
    const bool dirty = (d->*move).dirty();
    const bool restore = d->restore_temporary_preset();
    const bool accept = d->accept_current_preset();
    ESP_LOGI("fixture", "RECOVERY=%d|%d|%d", restore, accept,
        (d->*move).phase() == phase && (d->*move).dirty() == dirty);
  }
  static void read3(esphome::desk_uart::DeskUART *d) {
    age(d);
    auto settings = &BackendJournalAccess::preset_settings_;
    ESP_LOGI("fixture", "READ3=%d", (d->*settings).begin_read(3, esphome::millis()));
  }
  static void age(esphome::desk_uart::DeskUART *d) {
    auto age = &BackendJournalAccess::last_keep_awake_;
    d->*age = esphome::millis() - 31000;
  }
};
"""

PROFILE = bytes.fromhex(
    "0100010004000000cc010000d502000062fbffffa4040000"
    "18002700da029c040800e4020600e40208000700faff0100"
)
NAME = "backend-journal-test"
# An identity read blocks maintenance for at most 700 ms and the next read
# starts at least 1000 ms after it, so a request must be accepted within 1 s.
MAINTENANCE_WAIT_LIMIT_MS = 1000


def journal(kind):
    """Seed production v4 bytes, retaining malformed/conflicting bytes verbatim."""
    data = bytearray(64)
    struct.pack_into("<IHB", data, 0, 0x44534B34, 64, 1)
    data[8:56] = PROFILE
    struct.pack_into("<HH", data, 56, 456, 122)
    if kind == "profile":
        data[8] ^= 2
    struct.pack_into("<I", data, 60, zlib.crc32(data[:60]))
    if kind == "corrupt":
        data[63] ^= 0x80
    result = struct.pack("=IB", zlib.crc32(b"desk-preset-journal-v4:desk"), 64) + data
    if kind == "conflict":
        legacy = struct.pack("<IHH", 0x44534B33, 400, 150)
        result += (
            struct.pack("=IB", zlib.crc32(b"desk-preset-journal-v1:desk"), 8) + legacy
        )
    return result


def configuration(work):
    """Use one generated program, switching only public config setters before setup."""
    return f"""esphome:
  name: {NAME}
  build_path: {work}/build
  on_boot:
    - priority: 800
      then:
        - lambda: |-
            const std::string mode(getenv("BACKEND_MODE"));
            id(desk)->set_auto_target_mode(mode == "auto");
            id(desk)->set_direct_mode(mode == "direct");
interval:
  - interval: 20ms
    then:
      - lambda: |-
          FILE *f = fopen("action", "r");
          if (!f) return;
          char action[24] = {{0}};
          const bool read = fgets(action, sizeof(action), f) != nullptr;
          fclose(f);
          if (!read) return;
          if ((!strcmp(action, "direct") || !strcmp(action, "read")) && !BackendJournalAccess::ready(id(desk))) return;
          if (!strcmp(action, "maintenance")) {{
            // The adapter refuses maintenance while a background identity read
            // is in flight (at most 700 ms), and firmware export reports that
            // refusal as "retry shortly". Keep the request until it is accepted
            // and log the wait in ms.
            static const uint32_t first_attempt = esphome::millis();
            BackendJournalAccess::inspect(id(desk), action);
            const bool accepted = id(desk)->begin_firmware_export(false);
            ESP_LOGI("fixture", "MAINTENANCE=%d|%u", accepted,
                static_cast<unsigned>(esphome::millis() - first_attempt));
            if (!accepted) return;
            id(desk)->end_firmware_export();
            remove("action");
            return;
          }}
          remove("action");
          BackendJournalAccess::inspect(id(desk), action);
          if (!strcmp(action, "direct")) {{
            BackendJournalAccess::inert(id(desk));
            BackendJournalAccess::age(id(desk));
            ESP_LOGI("fixture", "MOVE=%d", id(desk)->move_to_height(78));
          }} else if (!strcmp(action, "blocked")) {{
            ESP_LOGI("fixture", "BLOCKED=%d|%d|%d", !id(desk)->move_to_height(78),
                !id(desk)->recall_preset(1), !id(desk)->set_preset_height(3, 80));
          }} else if (!strcmp(action, "inert")) {{
            BackendJournalAccess::inert(id(desk));
            ESP_LOGI("fixture", "MOVE=%d", id(desk)->move_to_height(78));
          }} else if (!strcmp(action, "read")) {{
            BackendJournalAccess::read3(id(desk));
          }} else if (!strcmp(action, "stop")) {{
            ESP_LOGI("fixture", "STOP=%d", id(desk)->stop());
          }}
host:
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
logger:
  level: DEBUG
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  id: desk
  uart_id: serial
  experimental_target_height: true
  experimental_preset_buttons: true
  target_height_mode: auto
  keep_awake: auto
  height:
    name: Height
  target_height_backend:
    name: Backend
  preset_3_height:
    name: Preset 3 height
  controller_model:
    name: Controller model
    on_value:
      - lambda: ESP_LOGI("fixture", "IDENTITY_MODEL=%s", x.c_str());
  controller_manufacturer:
    name: Controller manufacturer
  controller_identifiers:
    name: Controller identifiers
  controller_date:
    name: Controller date
  command_result:
    name: Result
    on_value:
      - lambda: ESP_LOGI("fixture", "RESULT=%s", x.c_str());
  control_status:
    name: Control status
"""


def run_case(work, env, binary, name):
    """Run real adapter discovery against a journal-bearing synthetic controller."""
    mode = (
        "direct"
        if name.startswith("direct_")
        else "preset"
        if name.startswith("preset_")
        else "auto"
    )
    kind = next(
        (k for k in ("corrupt", "profile", "conflict") if k in name), "retained"
    )
    reconnect = "reconnect" in name
    stock = ("stock" in name and not reconnect) or mode == "preset"
    unresolved = any(
        part in name
        for part in ("silent", "malformed", "no_telemetry", "identity_mismatch")
    )
    direct = not stock and not unresolved and name != "auto_stock_reconnect"
    folder = work / name
    folder.mkdir(exist_ok=True)
    (folder / "action").unlink(missing_ok=True)
    preferences = folder / "prefs/.esphome/prefs" / (NAME + ".prefs")
    preferences.parent.mkdir(parents=True, exist_ok=True)
    before = journal(kind)
    preferences.write_bytes(before)
    master, slave = open_pty()
    run_env = dict(
        env,
        BACKEND_MODE=mode,
        DESK_TEST_SERIAL=os.ttyname(slave),
        DESK_TEST_PREFS_HOME=str(folder / "prefs"),
    )
    wire = bytearray()
    events = []
    sequence = processed = sets = 0
    q = goal = 14
    next_height = 0
    scheduled = set()
    duration = 15 if reconnect else 10 if direct else 9
    proc = None

    def reply(body):
        nonlocal sequence
        os.write(master, encode(bytes(body) + sequence.to_bytes(2, "big")))
        sequence = (sequence + 1) & 65535

    try:
        with (folder / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(binary)],
                cwd=folder,
                env=run_env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < duration:
                elapsed = time.monotonic() - start
                assert proc.poll() is None, (name, "host exited")
                gap = reconnect and 7 <= elapsed < 9.5
                peer_stock = stock or (
                    name == "auto_stock_reconnect" and elapsed >= 9.5
                )
                if elapsed >= next_height:
                    q += max(-2, min(2, goal - q))
                    if not gap and "no_telemetry" not in name:
                        body = bytearray(30)
                        body[0] = 5
                        body[19:21] = q.to_bytes(2, "big")
                        reply(body)
                    next_height = elapsed + 0.1
                actions = [
                    (
                        4.5,
                        "direct"
                        if direct or reconnect
                        else "inert"
                        if mode == "direct"
                        else "blocked",
                    ),
                    (6.2, "stop"),
                ]
                if name in ("auto_silent", "auto_identity_mismatch"):
                    actions += [(5.5, "maintenance")]
                if direct and not reconnect:
                    actions += [(8.1, "read"), (9.3, "inspect")]
                if reconnect:
                    actions += [
                        (
                            12.5,
                            "blocked" if name == "auto_stock_reconnect" else "direct",
                        ),
                        (14, "stop"),
                    ]
                for index, (deadline, action) in enumerate(actions):
                    if (
                        elapsed >= deadline
                        and index not in scheduled
                        and not (folder / "action").exists()
                    ):
                        (folder / "action").write_text(action)
                        scheduled.add(index)
                if not select.select([master], [], [], 0.01)[0]:
                    continue
                wire.extend(os.read(master, 65536))
                found = list(frames(wire))
                for frame in found[processed:]:
                    events.append({"seconds": round(elapsed, 3), "body": frame.hex()})
                    assert frame[0] not in (0x19, 0x1B), (
                        name,
                        "preset/storage write",
                        frame.hex(),
                    )
                    assert not (frame[0] == 0x17 and frame[1] != 1), (
                        name,
                        "preset recall",
                        frame.hex(),
                    )
                    if gap or "no_telemetry" in name:
                        continue
                    if frame[0] == 0x13:
                        continue
                    if frame[0] == 0x11:
                        width, value = IDENTITY[frame[1]]
                        if "identity_mismatch" in name and frame[1] == 3:
                            value = "Other controller"
                        reply(
                            bytes([0x12, frame[1]]) + value.encode().ljust(width, b"\0")
                        )
                    elif frame[0] == 0x15:
                        selector = frame[1]
                        value = {11: 24, 12: 39, 23: (q * 39 + 23) // 24}.get(
                            selector, 500
                        )
                        reply(bytes([0x16, selector]) + value.to_bytes(2, "big"))
                    elif frame[0] == 0xD1:
                        op, nonce, requested = (
                            frame[2],
                            int.from_bytes(frame[3:5], "big"),
                            int.from_bytes(frame[5:7], "big"),
                        )
                        if "silent" in name:
                            continue
                        if "malformed" in name:
                            reply(bytes([0xD2, 1, 0]))
                        elif peer_stock:
                            assert op == 0, (name, "direct SET sent to stock")
                            reply(bytes([0xEE, 3]))
                        else:
                            current = max(6, min(747, (q * 39 + 23) // 24))
                            target = (
                                max(
                                    6,
                                    min(
                                        747,
                                        requested + 7
                                        if requested > current + 4
                                        else requested - 6
                                        if requested < current - 4
                                        else requested,
                                    ),
                                )
                                if op
                                else current
                            )
                            if op:
                                sets += 1
                                goal = target * 24 // 39
                            reply(
                                struct.pack(
                                    ">5B4s3Hi2I2H2BH",
                                    0xD2,
                                    1,
                                    3,
                                    op,
                                    0,
                                    b"DT02",
                                    nonce,
                                    requested,
                                    current,
                                    target,
                                    6,
                                    747,
                                    24,
                                    39,
                                    int(bool(op)),
                                    0,
                                    0,
                                )
                            )
                    elif frame == bytes([0x17, 1, 0, 0]):
                        goal = q
                        reply(bytes([0x18, 1, 0, 0]))
                    else:
                        raise AssertionError((name, "unexpected frame", frame.hex()))
                processed = len(found)
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        os.close(master)
        os.close(slave)
        (folder / "events.json").write_text(json.dumps(events, indent=2) + "\n")
    text = re.sub(r"\x1b\[[0-9;]*m", "", (folder / "runtime.log").read_text())
    states = re.findall(r"STATE=([^|\n]+)\|([^|\n]+)\|(\d)\|(\d)\|(\d)\|(\d)", text)
    assert states, (name, "no actions completed", folder)
    assert all(s[5] == "0" for s in states), (
        name,
        "target-number fixture unexpectedly present",
    )
    assert preferences.read_bytes() == before, (name, "journal bytes changed")
    if direct or reconnect:
        assert "MOVE=1" in text and sets >= 1 and "RECOVERY=0|0|1" in text, (
            name,
            text[-5000:],
        )
        initial = next(s for s in states if s[0] == "direct")
        assert initial[1:5] == ("Direct height control", "0", "0", "0"), (name, initial)
    if direct and not reconnect:
        assert "READ3=1" in text and any(
            e["body"].startswith("151f") for e in events
        ), (name, "native preset-3 read did not complete")
        final = next(s for s in states if s[0] == "inspect")
        assert final[1:5] == ("Direct height control", "0", "0", "0"), (name, final)
    if name == "auto_direct_reconnect":
        assert text.count("MOVE=1") == 2 and sets == 2, (name, states, sets)
        assert len([s for s in states if s[0] == "direct"]) == 2
    if (stock and mode != "direct") or name == "auto_stock_reconnect":
        state = next(s for s in states if s[0] == "blocked")
        assert state[1] == "Height control using preset 3" and state[4] == "1", (
            name,
            state,
        )
        assert state[2:5] == ("1", "0", "1"), (
            name,
            "valid retained backup did not protect stock",
            state,
        )
        assert "BLOCKED=1|1|1" in text, (name, text[-5000:])
    if unresolved:
        assert all(s[1] == "Unknown" and s[4] == "0" for s in states), (name, states)
        assert sets == 0 and "BLOCKED=1|1|1" in text, (name, text[-5000:])
    if name in ("auto_silent", "auto_identity_mismatch"):
        expected_model = (
            "Other controller" if "identity_mismatch" in name else "T150-22-03"
        )
        model = f"IDENTITY_MODEL={expected_model}"
        accepted = re.search(r"MAINTENANCE=1\|(\d+)", text)
        assert model in text and accepted, (name, text[-6000:])
        assert int(accepted[1]) <= MAINTENANCE_WAIT_LIMIT_MS, (
            name,
            "maintenance refused for longer than one identity read",
            accepted[0],
        )
        assert text.index(model) < text.index("MAINTENANCE=1"), (
            name,
            "identity did not publish before maintenance",
        )
    if unresolved:
        for action in ("recall", "editing"):
            assert (
                f"RESULT=Checking height control support; preset {action} unavailable"
                in text
            ), (
                name,
                "Unknown preset rejection reason missing",
                text[-6000:],
            )
        assert "Preset height not configured" not in text, (
            name,
            "preset edit assertion was vacuous",
        )
    if name == "direct_stock":
        assert "RECOVERY=0|0|1" in text and sets == 0
        assert all(s[4] == "0" and s[2] == "0" and s[3] == "0" for s in states), (
            name,
            states,
        )
    return {
        "case": name,
        "states": states,
        "direct_sets": sets,
        "journal_unchanged": True,
        "preset_writes": 0,
        "preset_recalls": 0,
        "missing_target_ui": True,
    }


def instrument_main(source):
    """Replace our own prefix when ESPHome preserves it during regeneration."""
    marker = "// Auto generated code by esphome"
    assert marker in source
    return ACCESS + source[source.index(marker) :]


def main():
    """Build once and exercise cold boot, reconnect and positive stock fallback."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", help="Run named scenarios after building")
    args = parse_args(parser=parser)
    work = args.work_dir
    env = test_env(work)
    config = work / "host.yaml"
    config.write_text(configuration(work))
    with (work / "build.log").open("w") as log:
        build_host(
            config,
            work,
            env,
            log,
            uart_expr='getenv("DESK_TEST_SERIAL")',
            edit_main=instrument_main,
        )
    cases = [
        f"{mode}_{kind}"
        for mode in ("auto", "direct")
        for kind in ("retained", "corrupt", "profile", "conflict")
    ]
    cases += [
        "auto_direct_reconnect",
        "auto_stock_reconnect",
        "auto_stock_retained",
        "preset_retained",
        "direct_stock",
        "auto_silent",
        "auto_malformed",
        "auto_no_telemetry",
        "auto_identity_mismatch",
    ]
    assert not args.only or set(args.only) <= set(cases), args.only
    results = []
    for name in args.only or cases:
        results.append(
            run_case(work, env, work / f"build/.pioenvs/{NAME}/program", name)
        )
        print(f"PASS {name}", flush=True)
    summary = {
        "passed": True,
        "hardware_access": False,
        "transport": "real ESPHome host + synthetic PTY",
        "cases": results,
    }
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
