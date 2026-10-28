#!/usr/bin/env python3
"""Build/run real ESPHome on a synthetic PTY; never opens a hardware port.

Requires installed ESPHome. --work-dir must be under $TMPDIR. Generated files
and dependency caches stay there for caller inventory and cleanup.
"""

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
    try:
        config = f"""esphome:
  name: desk-integration
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - delay: 1s
      - lambda: |-
          ESP_LOGI("test", "first=%d", id(desk)->recall_preset(3));
          ESP_LOGI("test", "busy=%d", id(desk)->recall_preset(2));
      - delay: 1s
      - lambda: |-
          ESP_LOGI("test", "second=%d", id(desk)->recall_preset(1));
      - delay: 1s
      - lambda: |-
          ESP_LOGI("test", "fault_blocked=%d", id(desk)->recall_preset(2));
host:
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
logger:
  level: DEBUG
uart:
  id: desk_serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  id: desk
  uart_id: desk_serial
  height:
    name: Desk height
    on_value:
      then:
        - lambda: ESP_LOGI("test", "height=%.1f", x);
  connected:
    name: Desk connected
    on_state:
      then:
        - lambda: ESP_LOGI("test", "connected=%d", x);
  experimental_preset_buttons: true
  preset_3:
    name: Desk preset 3
"""
        cfg = work / "host.yaml"
        cfg.write_text(config)
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        program = work / "build/.pioenvs/desk-integration/program"
        outgoing, events = bytearray(), []
        sequence, processed = 0, 0
        next_height = 0.3
        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen([str(program)], cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
            start = time.monotonic()
            while time.monotonic() - start < 9:
                elapsed = time.monotonic() - start
                assert proc.poll() is None, "ESPHome host exited early"
                if next_height <= elapsed < 4:
                    body = bytearray(32)
                    body[0] = 5
                    # Preset 1 is pressed while fresh height samples still
                    # change after preset 3: forward the button, not a brake.
                    body[20] = 6 if elapsed < 1 else 6 + int((elapsed - 1) * 4)
                    body[30:32] = sequence.to_bytes(2, "big")
                    sequence += 1
                    os.write(master, encode(body))
                    next_height += 0.5
                if select.select([master], [], [], 0.01)[0]:
                    outgoing.extend(os.read(master, 4096))
                    all_frames = list(frames(outgoing))
                    for frame in all_frames[processed:]:
                        events.append({"seconds": elapsed, "body_hex": frame.hex(" ")})
                        # Omit the second release ACK to exercise the fault latch.
                        if frame == bytes([0x17, 8, 0]):
                            os.write(master, encode(bytes([0x18, 8, 0, 0]) + sequence.to_bytes(2, "big")))
                            sequence += 1
                    processed = len(all_frames)
            proc.terminate()
            proc.wait(timeout=5)
        sent = list(frames(outgoing))
        motion = [f for f in sent if f[0] == 0x17]
        assert motion == [bytes([0x17, 8, 1]), bytes([0x17, 8, 0]), bytes([0x17, 6, 1]), bytes([0x17, 6, 0])], motion
        assert all(f[0] in (0x13, 0x17) for f in sent)
        assert sum(f == bytes([0x13, 2, 0, 1, 0xF4, 0, 0]) for f in sent) >= 2
        motion_times = [e["seconds"] for e in events if e["body_hex"].startswith("17 ")]
        pulses = [motion_times[1] - motion_times[0], motion_times[3] - motion_times[2]]
        assert all(0.12 <= value < 0.35 for value in pulses), pulses
        output = (work / "runtime.log").read_text()
        for marker in [
            "first=1",
            "busy=0",
            "second=1",
            "fault_blocked=0",
            "No release ACK",
            "height=73.0",
            "height=73.4",
            "connected=1",
            "connected=0",
        ]:
            assert marker in output, marker
        assert output.rfind("connected=0") > output.find("connected=1")
        assert output.rfind("height=nan") > output.find("height=73.0")
        summary = {
            "passed": True,
            "transport": "synthetic PTY; no hardware",
            "events": events,
            "pulse_durations_s": pulses,
            "checks": [
                "real ESPHome host build",
                "metric height publication",
                "busy pulse rejected",
                "second preset forwarded during changing height, without a synthetic brake",
                "release ACK accepted",
                "missing release ACK latches motion fault",
                "stale telemetry disconnects",
                "subscription retried without replaying motion",
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
