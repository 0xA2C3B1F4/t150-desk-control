#!/usr/bin/env python3
"""Real ESPHome host against a synthetic UART peer. Never connects to hardware."""

import json, os, select, subprocess, time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


def main():
    w = parse_args(__doc__).work_dir
    env = test_env(w)
    master, slave = open_pty()
    proc = None
    config = f"""esphome:
  name: desk-retarget-test
  build_path: {w}/build
  on_boot:
    priority: -100
    then:
      - delay: 2500ms
      - number.set:
          id: target
          value: 80
      - delay: 800ms
      - lambda: |-
          ESP_LOGI("test", "retarget=%d", id(desk)->move_to_height(84));
          ESP_LOGI("test", "latest=%d", id(desk)->move_to_height(82));
          ESP_LOGI("test", "timer_blocked=%d", id(desk)->set_timer(30));
      - delay: 9s
      - lambda: |-
          ESP_LOGI("test", "unlocked=%d", !id(desk)->is_stop_latched());
          id(desk)->move_to_height(90);
      - delay: 800ms
      - lambda: |-
          id(desk)->move_to_height(91);
          id(desk)->stop();
      - delay: 5s
      - lambda: id(desk)->move_to_height(90);
      - delay: 800ms
      - lambda: id(desk)->move_to_height(91);
      - delay: 10s
      - lambda: ESP_LOGI("test", "end_latched=%d", id(desk)->is_stop_latched());
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
  height:
    name: Height
  target_height:
    name: Target
    id: target
  control_status:
    name: Control status
  command_result:
    name: Command result
  stop_locked:
    name: Stop pending
  preset_restore_pending:
    name: Restore pending
"""
    cfg = w / "host.yaml"
    cfg.write_text(config)
    try:
        with (w / "build.log").open("w") as log:
            build_host(cfg, w, env, log, uart_port=os.ttyname(slave))
        wire = bytearray()
        processed = seq = 0
        events = []
        values = {11: 24, 12: 39, 31: 456}
        q = 6
        goal = 6
        next_height = 0
        coast_until = 0
        brakes = 0

        def reply(body):
            nonlocal seq
            os.write(master, encode(bytes(body) + seq.to_bytes(2, "big")))
            seq += 1

        with (w / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(w / "build/.pioenvs/desk-retarget-test/program")],
                cwd=w,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < 31:
                elapsed = time.monotonic() - start
                assert proc.poll() is None
                if elapsed >= next_height:
                    if coast_until and elapsed >= coast_until:
                        goal = q
                        coast_until = 0
                    q += max(-2, min(2, goal - q))
                    # Third replacement loses telemetry long enough to cancel. Reconnection
                    # must only restore the slot, never replay the requested movement.
                    if not 19 <= elapsed < 24:
                        body = bytearray(30)
                        body[0] = 5
                        body[19:21] = q.to_bytes(2, "big")
                        reply(body)
                    next_height = elapsed + 0.1
                if select.select([master], [], [], 0.01)[0]:
                    wire.extend(os.read(master, 4096))
                    found = list(frames(wire))
                    for f in found[processed:]:
                        events.append({"seconds": round(elapsed, 4), "body_hex": f.hex(" ")})
                        if f[0] == 0x15:
                            v = values[f[1]]
                            reply([0x16, f[1], v >> 8, v & 255])
                        elif f[0] == 0x19:
                            assert f[1] == 31
                            values[31] = int.from_bytes(f[2:4], "big")
                            reply([0x1A, 31, 0])
                        elif f[:2] == bytes([0x17, 8]):
                            reply([0x18, 8, f[2], 0])
                            if not f[2]:
                                goal = values[31] * 24 // 39
                        elif f == bytes([0x17, 1, 0, 0]):
                            brakes += 1
                            reply([0x18, 1, 0, 0])
                            coast_until = elapsed + 0.5
                        else:
                            assert f[0] == 0x13, f.hex()
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        sent = list(frames(wire))
        writes = [int.from_bytes(f[2:4], "big") for f in sent if f[0] == 0x19]
        assert writes == [122, 456, 155, 456, 285, 456, 285, 456], writes
        assert brakes == 3, brakes
        recalls = [e for e in events if e["body_hex"] == "17 08 01"]
        assert len(recalls) == 4, recalls
        bs = [e for e in events if e["body_hex"] == "17 01 00 00"]
        assert recalls[1]["seconds"] - bs[0]["seconds"] >= 2
        assert not any(e["seconds"] >= 24 and e["body_hex"].startswith("17 08") for e in events)
        assert values[31] == 456
        # Intentional retarget braking must not overwrite acceptance with rejection.
        first_result = (w / "runtime.log").read_text().split("Pending target cancelled by Stop")[0]
        assert "Command rejected: Target move interrupted" not in first_result
        out = (w / "runtime.log").read_text()
        for marker in [
            "retarget=1",
            "latest=1",
            "timer_blocked=0",
            "unlocked=1",
            "end_latched=0",
            "Pending target cancelled by Stop",
            "Pending target cancelled by connection loss; request again",
            "Stop completed; remote controls ready",
        ]:
            assert marker in out, marker
        result = {
            "passed": True,
            "transport": "synthetic PTY only",
            "writes": writes,
            "brakes": brakes,
            "recalls": recalls,
            "events": events,
            "checks": [
                "brake before retarget",
                "latest target wins",
                "restore and readback before next recall",
                "Stop discards pending move",
                "link loss discards pending move across reconnect",
                "automatic unlock",
                "timer blocked during retarget",
                "diagnostic entities generated",
            ],
        }
        (w / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"passed": True, "writes": writes, "brakes": brakes}))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
