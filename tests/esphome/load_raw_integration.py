#!/usr/bin/env python3
"""Raw channel-2 load publication through real ESPHome and a synthetic PTY.

No hardware port is opened. All injected frames and test values are synthetic.
"""

import json
import os
import re
import select
import subprocess
import time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


def telemetry(raw, sequence, *, size=32, kind=5):
    body = bytearray(size)
    body[0] = kind
    body[2] = 0xA5
    body[3:5] = raw.to_bytes(2, "big")
    body[5] = 0x5A
    body[19:21] = (6).to_bytes(2, "big")
    body[30:min(size, 32)] = sequence.to_bytes(2, "big")[:max(0, min(size, 32) - 30)]
    return encode(body)


def main():
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    master, slave = open_pty()
    proc = None
    try:
        config = work / "host.yaml"
        config.write_text(f"""esphome:
  name: desk-load-raw-test
  build_path: {work}/build
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
  height:
    name: Height
    on_value:
      - lambda: ESP_LOGI("test", "RAW_SAMPLE|height|%.1f|%u", x, unsigned(millis()));
  load_raw:
    name: Raw load
    on_value:
      - lambda: ESP_LOGI("test", "RAW_SAMPLE|load|%.0f|%u", x, unsigned(millis()));
  connected:
    name: Connected
    on_state:
      - lambda: ESP_LOGI("test", "RAW_SAMPLE|connected|%d|%u", x, unsigned(millis()));
""")
        with (work / "build.log").open("w") as log:
            build_host(config, work, env, log, uart_port=os.ttyname(slave))

        corrupt = bytearray(telemetry(32104, 1))
        # Flip an unescaped body byte, retaining the original checksum.
        corrupt[4] ^= 1
        schedule = [
            (0.4, "minimum", telemetry(0, 0xFFFE)),
            (0.8, "maximum", telemetry(65535, 0xFFFF)),
            (1.2, "sequence_wrap", telemetry(4660, 0)),
            (1.6, "duplicate", telemetry(32100, 0)),
            (2.0, "out_of_order", telemetry(32101, 0xFFFF)),
            (2.4, "short", telemetry(32102, 1, size=31)),
            (2.8, "long", telemetry(32103, 1, size=33)),
            (3.2, "bad_checksum", bytes(corrupt)),
            (3.8, "duplicate_near_deadline", telemetry(32105, 0)),
            (4.6, "wrong_kind_while_stale", telemetry(32106, 1, kind=6)),
            (5.5, "recovery_with_escaping", telemetry(0xFEFE, 1)),
        ]
        outgoing = bytearray()
        injected = []
        next_event = 0
        subscription = bytes([0x13, 2, 0, 1, 0xF4, 0, 0])
        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-load-raw-test/program")],
                cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            startup_begin = time.monotonic()
            start = None
            # Align injections with the real subscription clock, not Popen.
            while start is None:
                assert proc.poll() is None, "ESPHome host exited before its initial subscription"
                assert time.monotonic() - startup_begin < 10, "Timed out waiting for the initial subscription"
                if select.select([master], [], [], 0.01)[0]:
                    outgoing.extend(os.read(master, 4096))
                    initial = list(frames(outgoing))
                    if initial:
                        assert initial[0] == subscription, initial[0].hex(" ")
                        start = time.monotonic()
            startup_seconds = start - startup_begin
            while time.monotonic() - start < 6.2:
                elapsed = time.monotonic() - start
                assert proc.poll() is None, "ESPHome host exited early"
                while next_event < len(schedule) and elapsed >= schedule[next_event][0]:
                    _, name, wire = schedule[next_event]
                    os.write(master, wire)
                    injected.append({"case": name, "seconds": round(elapsed, 3), "wire_hex": wire.hex()})
                    next_event += 1
                if select.select([master], [], [], 0.01)[0]:
                    outgoing.extend(os.read(master, 4096))
            proc.terminate()
            proc.wait(timeout=5)
        assert next_event == len(schedule)
        output = (work / "runtime.log").read_text()
        samples = [
            {"kind": kind, "value": value, "milliseconds": int(now)}
            for kind, value, now in re.findall(r"RAW_SAMPLE\|(height|load|connected)\|([^|\s]+)\|(\d+)", output)
        ]
        by_kind = {kind: [event for event in samples if event["kind"] == kind]
                   for kind in ("height", "load", "connected")}
        assert [event["value"] for event in by_kind["load"]] == [
            "nan", "0", "65535", "4660", "nan", "65278",
        ], by_kind["load"]
        assert [event["value"] for event in by_kind["height"]] == [
            "nan", "73.0", "73.0", "73.0", "nan", "73.0",
        ], by_kind["height"]
        assert [event["value"] for event in by_kind["connected"]] == ["1", "0", "1"], by_kind["connected"]
        accepted = by_kind["load"][3]
        stale = by_kind["load"][4]
        stale_age = stale["milliseconds"] - accepted["milliseconds"]
        assert 3000 <= stale_age < 3200, stale_age
        disconnect = by_kind["connected"][1]
        assert abs(stale["milliseconds"] - disconnect["milliseconds"]) <= 25
        assert abs(stale["milliseconds"] - by_kind["height"][4]["milliseconds"]) <= 25
        sent = list(frames(outgoing))
        assert sent == [subscription, subscription], [body.hex(" ") for body in sent]
        summary = {
            "passed": True,
            "transport": "synthetic PTY; no hardware",
            "checks": [
                "injection schedule starts after the initial subscription within a bounded startup wait",
                "raw BE16 minimum, maximum and escaped bytes published without scaling",
                "load changes published when height is unchanged",
                "sequence wrap accepted; fresh duplicates and out-of-order frames ignored",
                "wrong length, wrong kind and bad checksum ignored",
                "same 3000 ms freshness as connected; one stale NaN and fresh recovery",
                "UART TX contains only the two existing channel-2 subscriptions",
            ],
            "startup_seconds": round(startup_seconds, 3),
            "stale_age_ms": stale_age,
            "samples": samples,
            "injected": injected,
            "uart_tx_bodies": [body.hex(" ") for body in sent],
        }
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
