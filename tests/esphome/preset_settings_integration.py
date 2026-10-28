#!/usr/bin/env python3
"""Build real ESPHome host; all device responses come from a synthetic PTY."""

import json, os, select, subprocess, time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


def main():
    w = parse_args(__doc__).work_dir
    env = test_env(w)
    master, slave = open_pty()
    proc = None
    config = f"""esphome:
  name: preset-settings-test
  build_path: {w}/build
  on_boot:
    priority: -100
    then:
      - lambda: ESP_LOGI("test", "unknown=%d", id(desk)->set_preset_height(1, 80));
      - delay: 7s
      - number.set:
          id: p1
          value: 80
      - lambda: |-
          ESP_LOGI("test", "busy_target=%d", id(desk)->move_to_height(90));
          ESP_LOGI("test", "busy_recall=%d", id(desk)->recall_preset(2));
          ESP_LOGI("test", "busy_timer=%d", id(desk)->set_timer(30));
          ESP_LOGI("test", "busy_limit=%d", id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 4000));
      - delay: 1s
      - number.set:
          id: p2
          value: 81
      - delay: 1s
      - number.set:
          id: p3
          value: 82
      # Avoid the independent load-read poll at exactly 10 seconds.
      - delay: 1200ms
      - number.set:
          id: p1
          value: 83
      - delay: 1s
      - lambda: ESP_LOGI("test", "out_of_range=%d", id(desk)->set_preset_height(1, 120));
      - delay: 1s
      - number.set:
          id: p2
          value: 84
      - delay: 100ms
      - desk_uart.stop: desk
      - delay: 3s
      - lambda: |-
          ESP_LOGI("test", "unlocked=%d", !id(desk)->is_stop_latched());
          id(desk)->move_to_height(85);
      - delay: 100ms
      - lambda: ESP_LOGI("test", "during_target=%d", id(desk)->set_preset_height(3, 90));
      - delay: 3500ms
      - lambda: id(desk)->set_timer(30);
      - lambda: ESP_LOGI("test", "during_timer=%d", id(desk)->set_preset_height(1, 90));
      - delay: 100ms
      - desk_uart.stop: desk
      - delay: 3s
      - lambda: ESP_LOGI("test", "stale=%d", id(desk)->set_preset_height(1, 90));
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
  control_status:
    name: Control status
  command_result:
    name: Result
    on_value:
      - lambda: ESP_LOGI("test", "result=%s", x.c_str());
  upward_load_limit:
    name: Up limit
  preset_1_height:
    name: Preset 1 height
    id: p1
    mode: box
    on_value:
      - lambda: ESP_LOGI("test", "p1=%.1f", x);
  preset_2_height:
    name: Preset 2 height
    id: p2
    mode: box
    on_value:
      - lambda: ESP_LOGI("test", "p2=%.1f", x);
  preset_3_height:
    name: Preset 3 height
    id: p3
    mode: box
    on_value:
      - lambda: ESP_LOGI("test", "p3=%.1f", x);
"""
    cfg = w / "host.yaml"
    cfg.write_text(config)
    try:
        with (w / "build.log").open("w") as log:
            build_host(cfg, w, env, log, uart_port=os.ttyname(slave))
        out = bytearray()
        processed = sequence = 0
        next_height = 0
        q = 6
        events = []
        values = {11: 24, 12: 39, 14: 4800, 29: 90, 30: 0, 31: 456}

        def reply(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + sequence.to_bytes(2, "big")))
            sequence += 1

        with (w / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(w / "build/.pioenvs/preset-settings-test/program")],
                cwd=w,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < 25:
                t = time.monotonic() - start
                assert proc.poll() is None
                if t >= next_height:
                    if t < 19.2:
                        b = bytearray(30)
                        b[0] = 5
                        b[19:21] = q.to_bytes(2, "big")
                        reply(b)
                    next_height = t + 0.1
                if select.select([master], [], [], 0.01)[0]:
                    out.extend(os.read(master, 4096))
                    found = list(frames(out))
                    for f in found[processed:]:
                        events.append({"seconds": round(t, 4), "body_hex": f.hex(" ")})
                        if f[0] == 0x15:
                            v = values[f[1]]
                            reply([0x16, f[1], v >> 8, v & 255])
                        elif f[0] == 0x19:
                            assert f[1] in (29, 30, 31)
                            v = int.from_bytes(f[2:4], "big")
                            # Third edit readback differs from requested; fourth lacks ACK.
                            values[f[1]] = 90 if f[1] == 31 and v == 155 else v
                            if not ((f[1] == 29 and v == 171) or (f[1] == 30 and v == 187)):
                                reply([0x1A, f[1], 0])
                        elif f[:2] == bytes([0x17, 8]):
                            reply([0x18, 8, f[2], 0])
                            if not f[2]:
                                q = values[31] * 24 // 39
                        elif f == bytes([0x17, 1, 0, 0]):
                            reply([0x18, 1, 0, 0])
                        else:
                            assert f[0] == 0x13, f.hex()
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        sent = list(frames(out))
        writes = [(f[1], int.from_bytes(f[2:4], "big")) for f in sent if f[0] == 0x19]
        assert writes == [(29, 122), (30, 139), (31, 155), (29, 171), (30, 187), (31, 204), (31, 90)], writes
        assert len([f for f in sent if f == bytes([0x17, 8, 1])]) == 1
        assert not any(e["seconds"] < 15 and e["body_hex"].startswith("17 08") for e in events)
        assert all(f[0] != 0x1B for f in sent)
        text = (w / "runtime.log").read_text()
        for marker in [
            "unknown=0",
            "busy_target=0",
            "busy_recall=0",
            "busy_timer=0",
            "busy_limit=0",
            "p1=78.0",
            "p1=80.0",
            "p2=81.0",
            "p3=78.0",
            "readback mismatch",
            "Preset setting timeout",
            "out_of_range=0",
            "unlocked=1",
            "during_target=0",
            "during_timer=0",
            "stale=0",
            "p1=nan",
            "result=Braking",
            "Preset setting interrupted by Stop; readback required",
        ]:
            assert marker in text, marker
        # The interruption is logged but does not replace the Stop's own result.
        assert "result=Preset setting interrupted by Stop" not in text
        assert "p3=82.0" not in text, "Requested value published without readback"
        result = {
            "passed": True,
            "transport": "ESPHome host + synthetic UART only",
            "writes": writes,
            "events": events,
            "checks": [
                "three config numbers with startup reads",
                "uninitialized preset replaceable after read",
                "writes verify scale and readback",
                "mismatch publishes actual value",
                "timeout never retries writes",
                "Stop interrupts write without replacing its own result",
                "target/timer/load interlocks",
                "slot3 temporary move restores edited baseline",
                "stale link invalidates settings",
                "no settings edit sends recall or EEPROM save",
            ],
        }
        (w / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"passed": True, "writes": writes}))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
