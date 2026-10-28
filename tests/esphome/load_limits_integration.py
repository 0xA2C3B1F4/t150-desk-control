#!/usr/bin/env python3
"""Real ESPHome host + synthetic PTY load settings; never accesses desk hardware."""

import json
import os
import select
import subprocess
import time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


def main():
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    master, slave = open_pty()
    proc = None
    config = f"""esphome:
  name: desk-load-settings
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - lambda: |-
          ESP_LOGI("test", "unknown_rejected=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::DownwardBase, 2500));
      - delay: 3300ms
      - number.set:
          id: down_limit
          value: 2500
      - lambda: |-
          ESP_LOGI("test", "busy_rejected=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 2700));
      - delay: 800ms
      - button.press: restore_down
      - delay: 800ms
      - number.set:
          id: up_limit
          value: 2700
      - delay: 800ms
      - number.set:
          id: down_limit
          value: 2600
      - delay: 1s
      - number.set:
          id: down_limit
          value: 2450
      - desk_uart.stop: desk
      - delay: 800ms
      - lambda: |-
          ESP_LOGI("test", "stopped_load=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 2901));
      - delay: 3300ms
      - lambda: |-
          ESP_LOGI("test", "moving_load=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 2902));
      - delay: 3900ms
      - lambda: |-
          ESP_LOGI("test", "recall_for_guard=%d", id(desk)->recall_preset(1));
      - delay: 500ms
      - lambda: |-
          ESP_LOGI("test", "recent_recall_load=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 2903));
      - delay: 3200ms
      - lambda: |-
          ESP_LOGI("test", "recall_for_fault=%d", id(desk)->recall_preset(2));
      - delay: 1s
      - lambda: |-
          ESP_LOGI("test", "release_fault_load=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 2904));
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
  experimental_preset_buttons: true
  height:
    name: Height
  upward_load_limit:
    id: up_limit
    name: Up limit
    on_value:
      - lambda: ESP_LOGI("test", "up=%.0f", x);
  downward_load_limit:
    id: down_limit
    name: Down base - affects homing
    on_value:
      - lambda: ESP_LOGI("test", "down=%.0f", x);
  restore_downward_load_limit:
    id: restore_down
    name: Restore down
"""
    try:
        cfg = work / "host.yaml"
        cfg.write_text(config)
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        outgoing, events = bytearray(), []
        values = {14: 4800, 15: 3200}
        processed = sequence = 0
        next_height = 0.3

        def reply(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + sequence.to_bytes(2, "big")))
            sequence += 1

        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-load-settings/program")],
                cwd=work,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < 21:
                elapsed = time.monotonic() - start
                assert proc.poll() is None
                if elapsed >= next_height:
                    body = bytearray(30)
                    body[0] = 5
                    body[20] = 6 if elapsed < 10 else min(26, 6 + int((elapsed - 10) * 10))
                    reply(body)
                    next_height += 0.1
                if select.select([master], [], [], 0.01)[0]:
                    outgoing.extend(os.read(master, 4096))
                    found = list(frames(outgoing))
                    for f in found[processed:]:
                        events.append({"seconds": elapsed, "body_hex": f.hex(" ")})
                        if f[0] == 0x15:
                            assert len(f) == 4 and f[1] in values
                            v = values[f[1]]
                            # Wrong selector must not consume this read transaction.
                            reply([0x16, 0x13, 0x12, 0x34])
                            reply([0x16, f[1], v >> 8, v & 255])
                        elif f[0] == 0x19:
                            assert len(f) == 4 and f[1] in values
                            v = int.from_bytes(f[2:4], "big")
                            values[f[1]] = 2800 if f[1] == 14 else v
                            if v != 2600:
                                reply([0x1A, f[1], 0])
                        elif f[:2] in (bytes([0x17, 6]), bytes([0x17, 7])):
                            if not (f[1] == 7 and f[2] == 0):
                                reply([0x18, f[1], f[2], 0])
                        elif f == bytes([0x17, 1, 0, 0]):
                            reply([0x18, 1, 0, 0])
                        else:
                            assert f[0] == 0x13, f.hex()
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        sent = list(frames(outgoing))
        writes = [f.hex(" ") for f in sent if f[0] == 0x19]
        assert writes == ["19 0f 09 c4", "19 0f 0c 80", "19 0e 0a 8c", "19 0f 0a 28", "19 0f 09 92"], writes
        first_write = next(i for i, f in enumerate(sent) if f[0] == 0x19)
        assert bytes([0x15, 14, 0, 0]) in sent[:first_write] and bytes([0x15, 15, 0, 0]) in sent[:first_write]
        last_write = max(i for i, f in enumerate(sent) if f[0] == 0x19)
        assert sent[last_write + 1] == bytes([0x17, 1, 0, 0]), "Stop did not preempt verification"
        stop_time = next(e["seconds"] for e in events if e["body_hex"] == "17 01 00 00")
        assert not any(
            e["body_hex"].startswith(("15 ", "19 ")) for e in events if stop_time < e["seconds"] < stop_time + 0.65
        ), "Late ACK triggered parameter follow-up"
        output = (work / "runtime.log").read_text()
        for token in [
            "unknown_rejected=0",
            "busy_rejected=0",
            "up=4800",
            "down=3200",
            "down=2500",
            "Load limit1 verified: 2500",
            "Load limit1 verified: 3200",
            "readback mismatch: requested2700 actual2800",
            "up=2800",
            "Load-limit transaction timeout",
            "Parameter transaction interrupted by stop",
            "Brake request acknowledged",
            "stopped_load=0",
            "moving_load=0",
            "recent_recall_load=0",
            "release_fault_load=0",
            "recall_for_guard=1",
            "recall_for_fault=1",
        ]:
            assert token in output, token
        # Later periodic readback may publish 2450; the aborted write itself must not.
        assert "up=2700" not in output and "down=2600" not in output
        summary = {
            "passed": True,
            "transport": "synthetic PTY, no hardware",
            "events": events,
            "checks": [
                "startup reads both, no default writes",
                "unknown baseline and concurrent writes rejected",
                "number entity writes canonical down selector0F",
                "ACK then independent readback",
                "wrong selector ignored",
                "restore button uses initial controller value",
                "mismatch publishes actual value, not requested",
                "timeout never retries write",
                "stop preempts parameter transaction, late ACK ignored",
                "load edits blocked while stopped, moving, after recent recall and after release failure",
            ],
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
