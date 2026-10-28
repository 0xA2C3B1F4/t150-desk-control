#!/usr/bin/env python3
"""Real ESPHome host, synthetic UART; no hardware or network device commands."""

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
  name: desk-target-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - delay: 2500ms
      - lambda: ESP_LOGI("test", "down_at_min=%d", id(desk)->step_height(false));
      - number.set:
          id: target_height
          value: 80
      - lambda: |-
          ESP_LOGI("test", "busy_preset=%d", id(desk)->recall_preset(3));
          ESP_LOGI("test", "busy_target=%d", id(desk)->move_to_height(90));
      - delay: 5s
      - button.press: step_up
      - delay: 5s
      - button.press: step_down
      - delay: 5s
      - number.set:
          id: target_height
          value: 105.3
      - delay: 5s
      - number.set:
          id: target_height
          value: 90
      - delay: 800ms
      - desk_uart.stop: desk
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
  height:
    name: Height
  target_height:
    name: Target height
    id: target_height
    mode: slider
    on_value:
      - lambda: ESP_LOGI("test", "target=%.1f", x);
  command_result:
    name: Command result
    on_value:
      - lambda: ESP_LOGI("test", "result=%s", x.c_str());
  up:
    id: step_up
    name: Up
  down:
    id: step_down
    name: Down
  restore_preset_3:
    name: Restore preset
  preset_restore_pending:
    name: Restore pending
    on_state:
      - lambda: ESP_LOGI("test", "restore_pending=%d", x);
""")
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        outgoing, events = bytearray(), []
        values = {11: 24, 12: 39, 31: 456}
        processed = sequence = recalls = 0
        next_height = 0.1
        q = 6

        def reply(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + sequence.to_bytes(2, "big")))
            sequence += 1

        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-target-test/program")],
                cwd=work,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < 27:
                elapsed = time.monotonic() - start
                assert proc.poll() is None
                if elapsed >= next_height:
                    body = bytearray(30)
                    body[0] = 5
                    body[19:21] = q.to_bytes(2, "big")
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
                            reply([0x16, f[1], v >> 8, v & 255])
                        elif f[0] == 0x19:
                            assert len(f) == 4 and f[1] == 31
                            values[31] = int.from_bytes(f[2:4], "big")
                            reply([0x1A, 31, 0])
                        elif f[:2] == bytes([0x17, 8]):
                            reply([0x18, 8, f[2], 0])
                            if not f[2]:
                                recalls += 1
                                # The final move has not started before Stop;
                                # retain coverage of an outstanding preset backup.
                                if recalls < 5:
                                    q = values[31] * 24 // 39
                        elif f == bytes([0x17, 1, 0, 0]):
                            # A local user changes preset3 during the final move.
                            # Restoration must not overwrite this value.
                            values[31] = 999
                            reply([0x18, 1, 0, 0])
                        else:
                            assert f[0] == 0x13, f.hex()
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        sent = list(frames(outgoing))
        writes = [int.from_bytes(f[2:4], "big") for f in sent if f[0] == 0x19]
        assert writes == [122, 456, 139, 456, 122, 456, 533, 456, 285], writes
        assert recalls == 5, recalls
        output = (work / "runtime.log").read_text()
        for marker in [
            "down_at_min=0",
            "busy_preset=0",
            "busy_target=0",
            "restore_pending=1",
            "restore_pending=0",
            "Preset changed locally; refusing overwrite",
            "Brake request acknowledged",
            "result=Step outside configured height range",
        ]:
            assert marker in output, marker
        assert output.count("Original preset 3 restored after motion began; monitoring target move") == 4
        releases = [e for e in events if e["body_hex"] == "17 08 00"]
        restores = [e for e in events if e["body_hex"] == "19 1f 01 c8"]
        assert len(restores) == 4
        for release, restore in zip(releases, restores):
            assert 0 <= restore["seconds"] - release["seconds"] < 0.3
        # The settled slider keeps UART precision above 100 cm, unlike the panel.
        assert "target=105.3" in output and "target=105.0" not in output
        # The Stop's own result is not replaced by the interruption it caused,
        # but the later restoration conflict is still published.
        braking = output.rfind("result=Braking")
        assert braking >= 0
        assert "result=Target move interrupted" not in output
        assert output.find("result=Preset changed locally; refusing overwrite", braking) > braking
        assert values[31] == 999
        # Restart same host preferences with outstanding durable recovery record.
        outgoing.clear()
        processed = 0
        with (work / "restart.log").open("w") as log:
            proc = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-target-test/program")],
                cwd=work,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            next_height = 0
            while time.monotonic() - start < 4:
                elapsed = time.monotonic() - start
                if elapsed >= next_height:
                    body = bytearray(30)
                    body[0] = 5
                    body[20] = 75
                    reply(body)
                    next_height += 0.1
                if select.select([master], [], [], 0.01)[0]:
                    outgoing.extend(os.read(master, 4096))
            proc.terminate()
            proc.wait(timeout=5)
        assert all(f[0] == 0x13 for f in frames(outgoing)), "Restart replayed motion or overwrote preset"
        assert "Interrupted target move" in (work / "restart.log").read_text()
        summary = {
            "passed": True,
            "transport": "ESPHome host + synthetic PTY, no hardware",
            "events": events,
            "checks": [
                "target number invokes read-scale/read-original/write/verify/recall/restore",
                "up and down buttons use 1 cm relative targets",
                "rejected step publishes its reason",
                "settled slider keeps decimals above 100 cm",
                "stop result is not replaced by its own interruption",
                "four original values restored with readback on first movement, within 300 ms of release",
                "concurrent movements rejected, no queued replay",
                "stop cancels move and preserves concurrent local preset edit",
                "persistent journal survives restart and blocks automatic movement",
            ],
        }
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({"passed": True, "writes": writes, "recalls": recalls}))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
