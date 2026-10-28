#!/usr/bin/env python3
"""Real ESPHome host with synthetic UART; no desk or network device access."""

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
    cfg = work / "host.yaml"
    cfg.write_text(f"""esphome:
  name: desk-timer-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - lambda: ESP_LOGI("test", "startup_rejected=%d", id(desk)->set_timer(30));
      - delay: 2500ms
      - button.press: timer_30
      - lambda: |-
          ESP_LOGI("test", "busy_timer=%d", id(desk)->cancel_timer());
          ESP_LOGI("test", "busy_preset=%d", id(desk)->recall_preset(3));
          ESP_LOGI("test", "busy_target=%d", id(desk)->move_to_height(80));
          ESP_LOGI("test", "busy_limit=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 2700));
      - delay: 12s
      - button.press: cancel_timer
      - delay: 16s
      - button.press: timer_60
      - delay: 5050ms
      - desk_uart.stop: desk
      - delay: 2500ms
      - lambda: ESP_LOGI("test", "auto_unlocked=%d", !id(desk)->is_stop_latched());
      - button.press: timer_90
      - delay: 6500ms
      - lambda: |-
          ESP_LOGI("test", "fault_timer=%d", id(desk)->set_timer(120));
          ESP_LOGI("test", "fault_preset=%d", id(desk)->recall_preset(3));
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
  experimental_target_height: true
  experimental_timer_buttons: true
  height:
    name: Height
  command_result:
    name: Result
    on_value:
      - lambda: ESP_LOGI("test", "result=%s", x.c_str());
  target_height:
    name: Target height
  upward_load_limit:
    name: Up limit
  timer_30min:
    name: Timer 30 min
    id: timer_30
  timer_60min:
    name: Timer 60 min
    id: timer_60
  timer_90min:
    name: Timer 90 min
    id: timer_90
  timer_120min:
    name: Timer 120 min
  cancel_timer:
    name: Cancel timer
    id: cancel_timer
  timer_command_pending:
    name: Timer command pending
    on_state:
      - lambda: ESP_LOGI("test", "timer_pending=%d", x);
""")
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        outgoing, events = bytearray(), []
        processed = sequence = presses = 0
        next_height = 0.1

        def reply(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + sequence.to_bytes(2, "big")))
            sequence += 1

        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-timer-test/program")],
                cwd=work,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < 45:
                elapsed = time.monotonic() - start
                assert proc.poll() is None
                if elapsed >= next_height:
                    body = bytearray(30)
                    body[0] = 5
                    body[20] = 6
                    reply(body)
                    next_height += 0.1
                if select.select([master], [], [], 0.01)[0]:
                    outgoing.extend(os.read(master, 4096))
                    found = list(frames(outgoing))
                    for f in found[processed:]:
                        events.append({"seconds": round(elapsed, 4), "body_hex": f.hex(" ")})
                        if f[:2] == bytes([0x17, 9]):
                            if f[2]:
                                presses += 1
                            # Lose the release ACK of the final, fourth request.
                            if not (presses == 8 and f[2] == 0):
                                reply([0x18, 9, f[2], 0])
                        elif f == bytes([0x17, 1, 0, 0]):
                            reply([0x18, 1, 0, 0])
                        elif f[:2] == bytes([0x15, 14]):
                            reply([0x16, 14, 4800 >> 8, 4800 & 255])
                        else:
                            assert f[0] == 0x13, f.hex()
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        sent = list(frames(outgoing))
        timer_events = [e for e in events if e["body_hex"].startswith("17 09")]
        assert presses == 8 and len(timer_events) == 16, timer_events
        assert timer_events[0]["seconds"] >= 7.4, "Timer sent during initial quiet interval"
        for i in range(0, len(timer_events), 2):
            press, release = timer_events[i : i + 2]
            assert press["body_hex"] == "17 09 01" and release["body_hex"] == "17 09 00"
            hold = release["seconds"] - press["seconds"]
            assert 0 <= hold < 0.4, hold
        brake = sent.index(bytes([0x17, 1, 0, 0]))
        assert sent[brake - 1] == bytes([0x17, 9, 0]), "Stop did not release T before braking"
        output = (work / "runtime.log").read_text()
        for marker in [
            "startup_rejected=0",
            "busy_timer=0",
            "busy_preset=0",
            "busy_target=0",
            "busy_limit=0",
            "auto_unlocked=1",
            "fault_timer=0",
            "fault_preset=0",
            "No T release ACK",
            "result=No T release ACK",
            "result=Braking",
            "Timer sequence interrupted; timer state unknown",
            "timer_pending=1",
            "timer_pending=0",
        ]:
            assert marker in output, marker
        # A Stop's own interruption is logged, not published over its result.
        assert "result=Timer sequence interrupted" not in output
        assert output.count("T-key sequence finished; controller timer state has no readback") == 2
        summary = {
            "passed": True,
            "transport": "ESPHome host + synthetic PTY, no hardware",
            "checks": [
                "no startup timer commands",
                "30-minute button uses one bounded T pulse",
                "cancel button uses five separately acknowledged T pulses",
                "concurrent timer, preset, target and parameter writes rejected",
                "stop releases T before brake and cancels remaining presses",
                "stop result is not replaced by the timer interruption it caused",
                "missing release ACK blocks timer and preset requests without retry",
                "pending entity reports transaction only, no claimed timer readback",
            ],
            "events": events,
        }
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({"passed": True, "timer_presses": presses, "completed_sequences": 2}))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
