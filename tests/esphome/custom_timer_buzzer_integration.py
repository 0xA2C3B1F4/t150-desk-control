#!/usr/bin/env python3
"""Real ESPHome host with synthetic PTY. Never connects to desk hardware."""

import json
import os
import select
import subprocess
import time

from custom_timer_buzzer_peer import discovery_reply
from desk_test_support import (
    ROOT,
    build_host,
    encode,
    frames,
    open_pty,
    parse_args,
    test_env,
)


def main():
    """Verify serialized requests, polling, interlocks and unknown outcomes."""
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    master, slave = open_pty()
    proc = None
    try:
        config = f"""esphome:
  name: custom-adapter-host-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - lambda: ESP_LOGI("test", "before_cap=%d", id(desk)->custom_set_timer(15));
      - delay: 5s
      - lambda: |-
          ESP_LOGI("test", "timer_sent=%d", id(desk)->custom_set_timer(15));
          ESP_LOGI("test", "overlap_rejected=%d", !id(desk)->custom_beep(200, 300, 3));
          ESP_LOGI("test", "maintenance_rejected=%d", !id(desk)->begin_firmware_export(false));
          ESP_LOGI("test", "button_timer_rejected=%d", !id(desk)->set_timer(30));
      - delay: 2s
      - lambda: ESP_LOGI("test", "beep_sent=%d", id(desk)->custom_beep(200, 300, 3));
      - delay: 2s
      - lambda: ESP_LOGI("test", "cancel_sent=%d", id(desk)->custom_cancel_timer());
      - delay: 4500ms
      - lambda: ESP_LOGI("test", "unknown_blocks=%d", !id(desk)->custom_set_timer(15));
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
  experimental_custom_timer_buzzer: true
  experimental_timer_buttons: true
  height:
    name: Height
  upward_load_limit:
    name: Upward limit
  controller_model:
    name: Model
  custom_available:
    name: Available
  custom_command_result:
    name: Result
"""
        cfg = work / "host.yaml"
        cfg.write_text(config)
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        program = work / "build/.pioenvs/custom-adapter-host-test/program"
        received, events, pending = bytearray(), [], []
        processed = sequence = 0
        next_height = 0.1
        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(program)], cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT
            )
            start = time.monotonic()
            while time.monotonic() - start < 15.5:
                elapsed = time.monotonic() - start
                assert proc.poll() is None, "Host program exited early"
                if elapsed >= next_height:
                    height = bytearray(32)
                    height[0] = 5
                    height[20] = 6
                    height[30:32] = sequence.to_bytes(2, "big")
                    sequence += 1
                    os.write(master, encode(height))
                    next_height += 0.5
                ready, pending = (
                    [p for p in pending if p[0] <= elapsed],
                    [p for p in pending if p[0] > elapsed],
                )
                for _, reply in ready:
                    os.write(master, encode(reply))
                if select.select([master], [], [], 0.01)[0]:
                    received.extend(os.read(master, 4096))
                    all_frames = list(frames(received))
                    for f in all_frames[processed:]:
                        events.append((elapsed, f))
                        if f[0] == 0xD7:
                            assert len(f) == 10 and f[1] == 1
                            # Delay real replies to force overlap with poll loops.
                            if f[2] != 2:
                                status = 3 if f[2] == 3 else 0
                                pending.append(
                                    (
                                        elapsed + 0.4,
                                        bytes([0xD8, 1, f[2], f[3], f[4], status]),
                                    )
                                )
                        else:
                            reply = discovery_reply(f, sequence)
                            if reply is not None:
                                os.write(master, encode(reply))
                                sequence = (sequence + 1) & 65535
                    processed = len(all_frames)
            proc.kill()
            proc.wait(timeout=5)
        custom = [(t, f) for t, f in events if f[0] == 0xD7]
        assert [f[2] for _, f in custom] == [0, 1, 3, 2], custom
        assert custom[1][1][5:10] == bytes([0, 15, 0, 0, 0])
        assert custom[2][1][5:10] == bytes([0, 200, 1, 44, 3])
        # While each delayed response was outstanding, background UART reads
        # must wait. Incoming telemetry continues throughout the experiment.
        for sent_at, f in custom[:3]:
            assert not [
                (t, x)
                for t, x in events
                if sent_at < t < sent_at + 0.35 and x[0] != 0xD7
            ], events
        assert any(f[0] == 0x15 for _, f in events) and any(
            f[0] == 0x11 for _, f in events
        )
        output = (work / "runtime.log").read_text()
        for marker in [
            "before_cap=0",
            "timer_sent=1",
            "overlap_rejected=1",
            "maintenance_rejected=1",
            "button_timer_rejected=1",
            "beep_sent=1",
            "cancel_sent=1",
            "unknown_blocks=1",
            "Controller accepted request",
            "Controller backend error",
            "No acknowledgement; result unknown, no retry",
        ]:
            assert marker in output, marker
        summary = {
            "passed": True,
            "transport": "ESPHome host and synthetic PTY, no hardware",
            "operations": [frame[2] for _, frame in custom],
            "checks": [
                "capability discovery before writes",
                "delayed acknowledgements hold background polling",
                "overlapping writes, maintenance and keypad timer rejected",
                "accepted and backend-error acknowledgements published",
                "missing cancel acknowledgement has unknown outcome and no retry",
            ],
            "events": [
                {"seconds": round(t, 4), "body_hex": frame.hex(" ")}
                for t, frame in events
            ],
        }
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({"passed": True, "operations": summary["operations"]}))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
