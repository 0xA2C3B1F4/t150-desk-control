#!/usr/bin/env python3
"""Inch entities with real ESPHome and a synthetic UART peer; no hardware."""

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
  name: desk-units-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - lambda: |-
          for (auto *n : std::vector<number::Number *>{{id(target), id(p1), id(p2), id(p3)}}) {{
            ESP_LOGI("test", "metadata=%s,%.1f,%.1f,%.1f", n->get_unit_of_measurement().c_str(),
                     n->traits.get_min_value(), n->traits.get_max_value(), n->traits.get_step());
          }}
          ESP_LOGI("test", "height_unit=%s", id(height)->get_unit_of_measurement().c_str());
      - delay: 7s
      - number.set:
          id: p1
          value: 31.5
      - delay: 1500ms
      - number.set:
          id: target
          value: 41.5
      - delay: 5s
      - button.press: step_down
      - delay: 5s
      - number.set:
          id: target
          value: 50
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
  display_unit: in
  experimental_target_height: true
  height:
    id: height
    name: Height
    on_value:
      - lambda: ESP_LOGI("test", "height=%.4f", x);
  target_height:
    id: target
    name: Target
    on_value:
      - lambda: ESP_LOGI("test", "target=%.4f", x);
  preset_1_height:
    id: p1
    name: Preset 1
    on_value:
      - lambda: ESP_LOGI("test", "p1=%.4f", x);
  preset_2_height:
    id: p2
    name: Preset 2
    on_value:
      - lambda: ESP_LOGI("test", "p2=%.4f", x);
  preset_3_height:
    id: p3
    name: Preset 3
    on_value:
      - lambda: ESP_LOGI("test", "p3=%.4f", x);
  down:
    id: step_down
    name: Down
""")
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        output = bytearray()
        processed = sequence = 0
        next_height = 0
        q = 328  # 105.3 cm: convert before the metric panel truncates to 105 cm.
        values = {11: 24, 12: 39, 29: 90, 30: 616, 31: 8}
        writes, recalls = [], 0

        def reply(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + sequence.to_bytes(2, "big")))
            sequence += 1

        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-units-test/program")],
                cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            start = time.monotonic()
            while time.monotonic() - start < 21:
                elapsed = time.monotonic() - start
                assert proc.poll() is None
                if elapsed >= next_height:
                    body = bytearray(30)
                    body[0] = 5
                    body[19:21] = q.to_bytes(2, "big")
                    reply(body)
                    next_height = elapsed + 0.1
                if select.select([master], [], [], 0.01)[0]:
                    output.extend(os.read(master, 4096))
                    found = list(frames(output))
                    for f in found[processed:]:
                        if f[0] == 0x15:
                            v = values[f[1]]
                            reply([0x16, f[1], v >> 8, v & 255])
                        elif f[0] == 0x19:
                            assert f[1] in (29, 31)
                            values[f[1]] = int.from_bytes(f[2:4], "big")
                            writes.append((f[1], values[f[1]]))
                            reply([0x1A, f[1], 0])
                        elif f[:2] == bytes([0x17, 8]):
                            reply([0x18, 8, f[2], 0])
                            if not f[2]:
                                recalls += 1
                                q = values[31] * 24 // 39
                        else:
                            assert f[0] == 0x13, f.hex()
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        # 31.5 in -> 80.0 cm; 41.5 in -> 105.4 cm; down retains a 1 cm step.
        assert writes == [(29, 122), (31, 535), (31, 8), (31, 519), (31, 8)], writes
        assert recalls == 2
        log = (work / "runtime.log").read_text()
        assert log.count("metadata=in,28.8,46.4,0.1") == 4
        for marker in ["height_unit=in", "height=41.4567", "target=41.4567",
                       "p1=30.7087", "p1=31.4961", "p2=43.4646", "p3=28.7402",
                       "target=41.5000", "target=41.1024"]:
            assert marker in log, marker
        assert "target=50.0000" not in log
        assert log.count("Original preset 3 restored after motion began; monitoring target move") == 2
        summary = {"passed": True, "transport": "ESPHome host + synthetic UART, no hardware",
                   "writes": writes, "recalls": recalls,
                   "checks": ["inch API metadata for all four numbers and height",
                              "unrounded height and startup target conversion above 100 cm",
                              "all three preset publications converted",
                              "preset input converts once and verifies readback",
                              "target input converts once; original raw preset restored",
                              "relative step remains 1 cm and publishes inches",
                              "out-of-range inch target sends no movement"]}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
