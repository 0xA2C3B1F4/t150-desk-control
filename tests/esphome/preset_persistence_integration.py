#!/usr/bin/env python3
"""Real ESPHome host + PTY persistence regression; no devices or network I/O.

The synthetic controller has independent RAM and EEPROM preset banks. Local
saves copy all slots, and controller reboot reloads RAM from EEPROM. Adapter
behavior is exercised through production UART/control methods, not reimplemented.
"""
import argparse
import concurrent.futures
import json
import os
import re
import select
import shutil
import struct
import subprocess
import time
import zlib

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


class Controller:
    def __init__(self):
        self.ram = [111, 222, 456]
        self.eeprom = self.ram.copy()
        self.events = []

    def save_local(self, reason):
        self.eeprom = self.ram.copy()
        self.events.append({'event': reason, 'ram': self.ram.copy(), 'eeprom': self.eeprom.copy()})

    def reboot(self):
        self.ram = self.eeprom.copy()
        self.events.append({'event': 'controller reboot', 'ram': self.ram.copy()})


def set_legacy_magic(preferences, magic):
    """Replace the fixture's v4 backup with an original eight-byte record.

    Host preferences encode native uint32 key, uint8 length, then data. Use the
    original component-ID key so the next process exercises actual migration.
    """
    legacy_key = zlib.crc32(b"desk-preset-journal-v1:desk")
    current_key = zlib.crc32(b"desk-preset-journal-v4:desk")
    changed = 0
    for path in preferences.rglob('*.prefs'):
        data = path.read_bytes()
        offset = 0
        records = []
        while offset < len(data):
            assert offset + 5 <= len(data)
            key, length = struct.unpack_from('=IB', data, offset)
            start = offset + 5
            assert start + length <= len(data)
            payload = data[start:start + length]
            if key == current_key:
                assert length == 64 and struct.unpack_from('<I', payload)[0] == 0x44534B34
                assert payload[6] == 1, 'Fixture must have a retained v4 backup'
                assert zlib.crc32(payload[:60]) == struct.unpack_from('<I', payload, 60)[0]
                original, temporary = struct.unpack_from('<HH', payload, 56)
                records.append((legacy_key, struct.pack('<IHH', magic, original, temporary)))
                changed += 1
            elif key != legacy_key:
                records.append((key, payload))
            offset = start + length
        path.write_bytes(b''.join(struct.pack('=IB', key, len(value)) + value for key, value in records))
    assert changed == 1, ('Expected exactly one recovery record', changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenarios", nargs="+", help="Run only named persistence scenarios")
    args = parse_args(parser=parser)
    work = args.work_dir
    env = test_env(work)
    cfg = work / 'host.yaml'
    cfg.write_text(f'''esphome:
  name: preset-persistence-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - delay: 2500ms
      - lambda: |-
          const std::string phase(getenv("PHASE"));
          if (phase == "borrow" || phase == "live_reboot" || phase == "local_edit" || phase == "repeated_borrow")
            ESP_LOGI("fixture", "MOVE=%d", id(desk)->move_to_height(80));
          if (phase == "inspect")
            ESP_LOGI("fixture", "REBORROW=%d", id(desk)->move_to_height(82));
          if (phase == "restore")
            ESP_LOGI("fixture", "RESTORE=%d", id(desk)->restore_temporary_preset());
          if (phase == "accept")
            ESP_LOGI("fixture", "ACCEPT=%d", id(desk)->accept_current_preset());
      - delay: 10s
      - lambda: |-
          const std::string phase(getenv("PHASE"));
          if (phase == "live_reboot" || phase == "local_edit" || phase == "repeated_borrow")
            ESP_LOGI("fixture", "REBORROW=%d", id(desk)->move_to_height(82));
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
  keep_awake: true
  height:
    name: Height
  preset_3_height:
    name: Preset 3
  preset_restore_pending:
    name: Restore pending
    trigger_on_initial_state: true
    on_state:
      - lambda: ESP_LOGI("fixture", "PENDING=%d", x);
  command_result:
    name: Result
    on_value:
      - lambda: ESP_LOGI("fixture", "RESULT=%s", x.c_str());
''')
    with (work / 'build.log').open('w') as log:
        build_host(cfg, work, env, log, uart_expr='getenv("DESK_TEST_SERIAL")')
    binary = work / 'build/.pioenvs/preset-persistence-test/program'

    def stage(folder, controller, phase, *, save_kind=None, gap=False, seconds=8):
        folder.mkdir(parents=True, exist_ok=True)
        master, slave = open_pty()
        prefs = folder.parent / 'prefs'
        run_env = dict(env, DESK_TEST_PREFS_HOME=str(prefs), DESK_TEST_SERIAL=os.ttyname(slave), PHASE=phase)
        received = bytearray()
        sequence = processed = 0
        writes, events, recalls = [], [], 0
        saved = rebooted = edited = False
        silence_until = 0
        next_height = 0
        proc = None
        log_path = folder / 'runtime.log'

        def send(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + (sequence % 65536).to_bytes(2, 'big')))
            sequence += 1

        try:
            with log_path.open('w') as log:
                proc = subprocess.Popen([str(binary)], cwd=folder, env=run_env, stdout=log, stderr=subprocess.STDOUT)
                start = time.monotonic()
                while time.monotonic() - start < seconds:
                    t = time.monotonic() - start
                    assert proc.poll() is None, (phase, 'host exited', log_path)
                    if phase == 'live_reboot' and t >= 6.5 and not rebooted:
                        assert controller.ram[2] == 456, ('RAM not restored before reboot', writes)
                        controller.reboot()
                        rebooted = True
                        silence_until = t + (3.5 if gap else 0)
                    if phase == 'local_edit' and t >= 6.5 and not edited:
                        controller.ram[2] = 555
                        controller.save_local('later legitimate M+3 save')
                        edited = True
                    if t >= next_height:
                        if t >= silence_until:
                            body = bytearray(30)
                            body[0] = 5
                            body[19:21] = (75).to_bytes(2, 'big')
                            send(body)
                        next_height = t + .1
                    if not select.select([master], [], [], .01)[0]:
                        continue
                    received.extend(os.read(master, 4096))
                    found = list(frames(received))
                    for frame in found[processed:]:
                        events.append({'seconds': round(t, 3), 'frame': frame.hex()})
                        assert frame[0] != 0x1B, 'Adapter must not issue EEPROM save'
                        if t < silence_until:
                            continue
                        if frame[0] == 0x15:
                            selector = frame[1]
                            assert selector in (0x0B, 0x0C, 0x17, 0x1F), frame.hex()
                            value = {0x0B: 24, 0x0C: 39, 0x17: 123, 0x1F: controller.ram[2]}[selector]
                            send([0x16, selector, value >> 8, value & 255])
                        elif frame[0] == 0x19:
                            assert frame[1] == 0x1F, frame.hex()
                            value = int.from_bytes(frame[2:4], 'big')
                            controller.ram[2] = value
                            writes.append(value)
                            if save_kind and save_kind != 'M-armed keep-awake' and not saved and value != 456:
                                # A local M+1/M+2 save serializes the borrowed third
                                # slot too. Sleep/reset reuse that same serializer.
                                if save_kind == 'M+1':
                                    controller.ram[0] = 123
                                if save_kind == 'M+2':
                                    controller.ram[1] = 123
                                controller.save_local(save_kind)
                                saved = True
                            send([0x1A, 0x1F, 0])
                        elif frame[:2] == bytes([0x17, 8]):
                            if frame[2]:
                                recalls += 1
                                if save_kind == 'M-armed keep-awake':
                                    controller.ram[2] = 123
                                    controller.save_local(save_kind)
                            send([0x18, 8, frame[2], 0])
                        elif frame == bytes([0x17, 1, 0, 0]):
                            send([0x18, 1, 0, 0])
                        else:
                            assert frame[0] == 0x13, frame.hex()
                    processed = len(found)
                proc.terminate()
                proc.wait(timeout=5)
            text = re.sub(r'\x1b\[[0-9;]*m', '', log_path.read_text())
            result = {'phase': phase, 'writes': writes, 'recalls': recalls,
                      'ram': controller.ram.copy(), 'eeprom': controller.eeprom.copy(),
                      'events': events, 'controller_events': controller.events.copy()}
            (folder / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            return result, text
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)

    def scenario(name):
        directory = work / name
        # Only this test's isolated synthetic preferences are removed.
        if (directory / 'prefs').exists():
            shutil.rmtree(directory / 'prefs')
        c = Controller()
        live = name in ('controller_reboot_gap', 'controller_reboot_hidden', 'local_edit')
        phase = 'local_edit' if name == 'local_edit' else 'live_reboot' if live else 'keep_awake' if name == 'armed_keep_awake' else 'repeated_borrow' if name == 'repeated_borrow' else 'borrow'
        kind = {'save_slot1': 'M+1', 'save_slot2': 'M+2', 'sleep_save': 'sleep save',
                'controller_reboot_gap': 'M+1', 'controller_reboot_hidden': 'M+1',
                'armed_keep_awake': 'M-armed keep-awake', 'repeated_borrow': 'M+1',
                'legacy_user': 'M+1', 'legacy_keep_awake': 'M+1'}.get(name)
        first, text = stage(directory / 'first', c, phase, save_kind=kind,
                            gap=name == 'controller_reboot_gap', seconds=37 if phase == 'keep_awake' else 18 if phase == 'repeated_borrow' else 16 if live else 8)
        borrowed = 123 if phase == 'keep_awake' else 122
        expected_writes = [122, 456, 155, 456] if name == 'repeated_borrow' else [borrowed, 456]
        assert first['writes'] == expected_writes, (name, first['writes'])
        assert first['recalls'] == (2 if name == 'repeated_borrow' else 1), (name, first['recalls'])
        if name == 'clean_restart_then_edit':
            assert c.ram[2] == c.eeprom[2] == 456
            clean, text = stage(directory / 'clean_restart', c, 'plain', seconds=5)
            assert not clean['writes'] and 'PENDING=0' in text
            c.ram[2] = 555
            c.save_local('later legitimate M+3 save after clean restart')
            c.reboot()
        if name.startswith('legacy_'):
            set_legacy_magic(directory / 'prefs', 0x44534B31 if name == 'legacy_user' else 0x44534B32)
        expected = 555 if name in ('local_edit', 'clean_restart_then_edit') else borrowed
        if not live and name != 'clean_restart_then_edit':
            assert c.ram[2] == 456 and c.eeprom[2] == expected, (name, c.ram, c.eeprom)
            c.reboot()
        assert c.ram[2] == expected
        inspect, text = stage(directory / 'inspect', c, 'inspect')
        assert not inspect['writes'] and inspect['recalls'] == 0, (name, 'restart reused or overwrote an ambiguous preset', inspect)
        assert 'PENDING=1' in text, (name, 'retained backup not visible after restart')
        assert c.ram[2] == expected
        if name in ('local_edit', 'clean_restart_then_edit'):
            # Explicit acceptance must preserve the user's RAM/EEPROM value.
            accepted, text = stage(directory / 'accept', c, 'accept')
            assert 'ACCEPT=1' in text and not accepted['writes']
            clean, text = stage(directory / 'after_accept', c, 'plain', seconds=5)
            assert 'PENDING=1' not in text and not clean['writes']
            assert c.ram[2] == c.eeprom[2] == 555
        else:
            restored, text = stage(directory / 'restore', c, 'restore')
            assert 'RESTORE=1' in text and restored['writes'] == [456], (name, restored['writes'], text[-1500:])
            assert c.ram[2] == 456 and c.eeprom[2] == borrowed
            # RAM restore is not EEPROM repair: another controller boot must
            # still leave a usable original backup instead of losing it.
            c.reboot()
            again, text = stage(directory / 'reboot_again', c, 'inspect')
            assert not again['writes'] and again['recalls'] == 0 and 'PENDING=1' in text, (name, again)
        return {'scenario': name, 'passed': True, 'ram': c.ram, 'eeprom': c.eeprom}

    names = ['save_slot1', 'save_slot2', 'sleep_save', 'controller_reboot_gap',
             'controller_reboot_hidden', 'local_edit', 'armed_keep_awake',
             'repeated_borrow', 'clean_restart_then_edit', 'legacy_user', 'legacy_keep_awake']
    if args.scenarios:
        assert set(args.scenarios) <= set(names), 'Unknown persistence scenario'
        names = args.scenarios
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(scenario, names))
    (work / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps({'passed': True, 'scenarios': names}))


if __name__ == '__main__':
    main()
