#!/usr/bin/env python3
"""Delayed-reply regressions for public DT02 review F1/F2/F3. Synthetic PTY only."""
import json
import os
import select
import struct
import subprocess
import time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


def main():
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    master, slave = open_pty()
    proc = None
    config = f"""esphome:
  name: direct-review-test
  build_path: {work}/build
  includes:
    - {ROOT}/tests/esphome/direct_target_review_access.h
  on_boot:
    priority: -100
    then:
      - wait_until:
          condition:
            lambda: return DirectReviewAccess::probing(id(desk));
          timeout: 5s
      - lambda: ESP_LOGI("review", "probe_request_rejected=%d", !id(desk)->move_to_height(78));
      - wait_until:
          condition:
            lambda: return DirectReviewAccess::known(id(desk));
          timeout: 2s
      - lambda: ESP_LOGI("review", "probe_preserved=%d", DirectReviewAccess::known(id(desk)));
      - delay: 400ms
      - lambda: |-
          ESP_LOGI("review", "first=%d", id(desk)->move_to_height(78));
          DirectReviewAccess::poll_due(id(desk));
      - delay: 200ms
      - lambda: ESP_LOGI("review", "uncertain_preset_rejected=%d", !id(desk)->recall_preset(1));
      - delay: 2800ms
      - lambda: |-
          ESP_LOGI("review", "second=%d", id(desk)->move_to_height(79));
          DirectReviewAccess::poll_due(id(desk));
      - delay: 300ms
      - lambda: ESP_LOGI("review", "healthy_preset=%d", id(desk)->recall_preset(1));
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
  target_height_mode: direct
  keep_awake: auto
  height:
    name: Height
  upward_load_limit:
    name: Upward limit
  command_result:
    name: Command result
"""
    cfg = work / "host.yaml"
    cfg.write_text(config)
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        raw, events, deferred = bytearray(), [], []
        processed = sequence = get_count = set_count = 0
        next_height = 0

        def send(body, suffix=True):
            nonlocal sequence
            value = bytes(body) + (sequence.to_bytes(2, "big") if suffix else b"")
            os.write(master, encode(value))
            sequence = (sequence + 1) & 65535

        with (work / "runtime.log").open("w") as log:
            proc = subprocess.Popen([str(work / "build/.pioenvs/direct-review-test/program")],
                                    cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
            began = time.monotonic()
            while time.monotonic() - began < 10:
                elapsed = time.monotonic() - began
                assert proc.poll() is None
                if elapsed >= next_height:
                    sample = bytearray(30)
                    sample[0], sample[20] = 5, 6
                    send(sample)
                    next_height = elapsed + 0.1
                for due, body, suffix in deferred[:]:
                    if elapsed >= due:
                        send(body, suffix)
                        deferred.remove((due, body, suffix))
                if select.select([master], [], [], 0.01)[0]:
                    raw.extend(os.read(master, 65536))
                    found = list(frames(raw))
                    for body in found[processed:]:
                        events.append(dict(seconds=round(elapsed, 3), body_hex=body.hex()))
                        if body[0] == 0x13:
                            continue
                        if body[0] == 0x15:
                            # Deliberately slower than GET: the old poll race cancels SET.
                            deferred.append((elapsed + 0.4, bytes([0x16, body[1], 0, 100]), True))
                        elif body[0] == 0xD1:
                            operation, nonce, requested = body[2], int.from_bytes(body[3:5], "big"), int.from_bytes(body[5:7], "big")
                            if operation:
                                set_count += 1
                            else:
                                get_count += 1
                            target = requested + 7 if requested > 104 else requested - 6 if requested < 96 else requested
                            target = max(6, min(747, target)) if operation else 100
                            reply = struct.pack(">5B4s3Hi2I2H2B2H", 0xD2, 1, 3, operation, 0, b"DT02",
                                                nonce, requested, 100, target, 6, 747, 24, 39, operation, 0, 0, sequence)
                            delay = 0.4 if not operation and get_count == 1 else 0.1 if not operation else 0.5 if set_count == 1 else 0
                            deferred.append((elapsed + delay, reply, False))
                        elif body == bytes.fromhex("17 01 00 00"):
                            send(bytes.fromhex("18 01 00 00"))
                        elif body[:2] == bytes.fromhex("17 06"):
                            send(bytes([0x18, 6, body[2], 0]))
                        else:
                            raise AssertionError(f"Unexpected command {body.hex()}")
                    processed = len(found)
            proc.terminate()
            proc.wait(timeout=5)
        log_text = (work / "runtime.log").read_text()
        for marker in ("probe_request_rejected=1", "probe_preserved=1", "first=1", "second=1",
                       "uncertain_preset_rejected=1", "healthy_preset=1",
                       "Checking DT02 capability; select target again when Ready",
                       "SET admission uncertain; braking, press preset again after settling"):
            assert marker in log_text, (marker, events)
        assert get_count == 3 and set_count == 2, (get_count, set_count, events)
        stops = [event for event in events if event["body_hex"] == "17010000"]
        presses = [event for event in events if event["body_hex"] == "170601"]
        gets = [event for event in events if event["body_hex"].startswith("d10100")]
        sets = [event for event in events if event["body_hex"].startswith("d10101")]
        assert len(stops) == 1 and sets[0]["seconds"] < stops[0]["seconds"] < sets[0]["seconds"] + 0.5, stops
        assert len(presses) == 1 and presses[0]["seconds"] > sets[1]["seconds"], presses
        assert sets[0]["seconds"] > gets[1]["seconds"], events
        assert not any(event["body_hex"].startswith("d10101") and
                       stops[0]["seconds"] < event["seconds"] < gets[2]["seconds"] for event in events)
        for low, high in ((gets[1]["seconds"], stops[0]["seconds"]),
                          (gets[2]["seconds"], presses[0]["seconds"])):
            assert not any(event["body_hex"].startswith("15") and low <= event["seconds"] <= high for event in events), events
        assert not any(event["body_hex"].startswith(("19", "1b", "ef")) for event in events)
        summary = dict(passed=True, transport="synthetic PTY only", get_count=get_count,
                       set_count=set_count, controlled_stops=len(stops), native_preset_presses=len(presses),
                       events=events, checks=[
                           "F1: due parameter poll blocked during direct GET/SET/Active with delayed 0x16 peer",
                           "F2: user request does not cancel automatic capability probe or queue a target",
                           "F3: uncertain SET plus preset request sends one Stop, no preset and no resume",
                           "healthy accepted direct motion still permits native preset without a second brake",
                           "no preset RAM, EEPROM or reset writes"])
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: value for key, value in summary.items() if key != "events"}, indent=2))
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
