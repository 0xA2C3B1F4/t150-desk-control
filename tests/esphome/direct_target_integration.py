#!/usr/bin/env python3
"""Real ESPHome host + synthetic PTY DT02 peer. No hardware; no firmware images."""
import json
import os
import select
import struct
import subprocess
import time
import zlib

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


def main():
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    master, slave = open_pty()
    proc = None
    config = f"""esphome:
  name: desk-direct-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - delay: 4s
      - lambda: ESP_LOGI("direct_test", "journal_blocked=%d", !id(desk)->move_to_height(78));
      - delay: 1s
      - lambda: ESP_LOGI("direct_test", "accept_current=%d", id(desk)->accept_current_preset());
      - delay: 3s
      - lambda: |-
          ESP_LOGI("direct_test", "first=%d", id(desk)->move_to_height(78));
          ESP_LOGI("direct_test", "export_blocked=%d", !id(desk)->begin_firmware_export());
          ESP_LOGI("direct_test", "timer_blocked=%d", !id(desk)->set_timer(30));
          ESP_LOGI("direct_test", "limit_blocked=%d", !id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward,200));
          ESP_LOGI("direct_test", "restore_blocked=%d", !id(desk)->restore_temporary_preset());
      - delay: 400ms
      - lambda: ESP_LOGI("direct_test", "retarget_requires_new_request=%d", !id(desk)->move_to_height(80));
      - delay: 3600ms
      - lambda: id(desk)->move_to_height(79);
      - delay: 400ms
      - lambda: ESP_LOGI("direct_test", "native_preset=%d", id(desk)->recall_preset(1));
      - delay: 4600ms
      - lambda: id(desk)->move_to_height(78);
      - delay: 4s
      - lambda: id(desk)->move_to_height(77);
      - delay: 9s
      - lambda: id(desk)->move_to_height(76);
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
  experimental_timer_buttons: true
  target_height_mode: direct
  keep_awake: auto
  uart_observation: true
  height:
    name: Height
  target_height:
    name: Target
  upward_load_limit:
    name: Upward limit
  control_status:
    name: Control status
  command_result:
    name: Command result
"""
    cfg = work / "host.yaml"
    cfg.write_text(config)
    preferences = work / "preferences/.esphome/prefs/desk-direct-test.prefs"
    preferences.parent.mkdir(parents=True, exist_ok=True)
    old = struct.pack("<IHH", 0x44534B33, 456, 122)
    preferences.write_bytes(struct.pack("=IB", zlib.crc32(b"desk-preset-journal-v1:desk"), len(old)) + old)
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        wire = bytearray()
        processed = sequence = sets = brakes = 0
        q = goal = 14
        banks = [111, 222, 500]
        original = banks.copy()
        events = []
        next_height = 0

        def reply(body, sequence_suffix=True):
            nonlocal sequence
            value = bytes(body) + (sequence.to_bytes(2, "big") if sequence_suffix else b"")
            os.write(master, encode(value))
            sequence = (sequence + 1) & 65535

        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen([str(work / "build/.pioenvs/desk-direct-test/program")],
                                    cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
            start = time.monotonic()
            while time.monotonic() - start < 33:
                elapsed = time.monotonic() - start
                assert proc.poll() is None
                if elapsed >= next_height:
                    q += max(-2, min(2, goal - q))
                    if not 22 <= elapsed < 26:
                        body = bytearray(30)
                        body[0] = 5
                        body[19:21] = q.to_bytes(2, "big")
                        reply(body)
                    next_height = elapsed + 0.1
                if select.select([master], [], [], 0.01)[0]:
                    wire.extend(os.read(master, 65536))
                    found = list(frames(wire))
                    for frame in found[processed:]:
                        events.append(dict(seconds=round(elapsed, 3), body_hex=frame.hex()))
                        if frame[0] == 0x13:
                            continue
                        if frame[0] == 0x15:
                            value = banks[2] if frame[1] == 31 else 100
                            reply(bytes([0x16, frame[1]]) + value.to_bytes(2, "big"))
                        elif frame[0] == 0xD1:
                            op, nonce, requested = frame[2], int.from_bytes(frame[3:5], "big"), int.from_bytes(frame[5:7], "big")
                            current = max(6, min(747, (q * 39 + 23) // 24))
                            target = requested + 7 if requested > current + 4 else requested - 6 if requested < current - 4 else requested
                            target = max(6, min(747, target)) if op else current
                            status = guards = mode = 0
                            if op:
                                sets += 1
                                if sets == 5:
                                    status, guards, mode = 2, 1, 1
                                else:
                                    mode = 1
                                    goal = target * 24 // 39
                                if sets == 3:  # Commit but lose ACK: exactly one brake, never resend SET.
                                    continue
                            body = struct.pack(">5B4s3Hi2I2H2B2H", 0xD2, 1, 3, op, status, b"DT02",
                                               nonce, requested, current, target, 6, 747, 24, 39, mode, 0, guards, sequence)
                            reply(body, False)
                        elif frame == bytes.fromhex("17 01 00 00"):
                            brakes += 1
                            goal = q
                            reply(bytes.fromhex("18 01 00 00"))
                        elif frame[:2] == bytes.fromhex("17 06"):
                            reply(bytes([0x18, 6, frame[2], 0]))
                            if not frame[2]:
                                goal = banks[0] * 24 // 39
                        else:
                            raise AssertionError(f"Unexpected command {frame.hex()}")
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        out = (work / "runtime.log").read_text()
        for marker in ("journal_blocked=1", "accept_current=1", "first=1", "export_blocked=1",
                       "timer_blocked=1", "limit_blocked=1", "restore_blocked=1",
                       "retarget_requires_new_request=1", "native_preset=1",
                       "Stopped; select target again after the desk settles"):
            assert marker in out, marker
        assert sets == 5, (sets, events)
        assert brakes == 3, (brakes, events)
        assert banks == original
        assert not any(e["body_hex"].startswith(("19", "1b", "ef")) for e in events)
        assert not any(8.5 < e["seconds"] < 11.9 and e["body_hex"].startswith("d10101") for e in events)
        assert not any(22 < e["seconds"] < 29.9 and e["body_hex"].startswith("d10101") for e in events)
        snapshots = [json.JSONDecoder().raw_decode(line.split("UART observation: ", 1)[1])[0] for line in out.splitlines() if "UART observation: " in line]
        assert len(snapshots) >= 5
        assert len({s["boot"] for s in snapshots}) == 1 and snapshots[0]["boot"] != 0
        assert snapshots[-1]["gaps"] >= 1 and snapshots[-1]["handoffs"] == 0
        assert snapshots[-1]["errors"] == 0
        for key in ("ms", "samples", "gaps", "errors", "tx", "changes"):
            assert all(a[key] <= b[key] for a, b in zip(snapshots, snapshots[1:])), key
        summary = dict(passed=True, transport="synthetic PTY only", sets=sets, brakes=brakes,
                       preset_ram=original, preset_eeprom=original, events=events,
                       checks=["legacy journal blocks direct until explicit double-read acceptance",
                               "no preset, EEPROM or reset writes", "Stop then new explicit request, no automatic retarget",
                               "native preset preempts direct without a second brake",
                               "lost SET ACK brakes once", "stale telemetry brakes and never replays after reconnect",
                               "explicit SET rejection does not brake", "export/timer/limit/recovery interlocks",
                               "coherent cumulative diagnostics including a latched RX gap"])
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({k: v for k, v in summary.items() if k != "events"}, indent=2))
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
