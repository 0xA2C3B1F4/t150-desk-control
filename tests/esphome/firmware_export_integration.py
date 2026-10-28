#!/usr/bin/env python3
"""Real ESPHome host ownership/interlock test. No hardware or network access."""
import json
import os
import subprocess
from desk_test_support import ROOT, build_host, open_pty, parse_args, test_env
w = parse_args(__doc__).work_dir
env = test_env(w)
master, slave = open_pty()
try:
    cfg = w / "host.yaml"
    cfg.write_text(f"""esphome:
  name: firmware-export-handoff
  build_path: {w}/build
  includes:
    - {ROOT}/tests/esphome/firmware_export_regression.h
  on_boot:
    priority: -100
    then:
      - lambda: |-
          firmware_export_test::Probe::run(id(height));
          exit(0);
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
  uart_id: serial
  height:
    id: height
    name: Height
""")
    with (w/'build.log').open('w') as log:
        build_host(cfg, w, env, log, uart_port=os.ttyname(slave))
    with (w/'runtime.log').open('w') as log:
        result = subprocess.run([str(w/'build/.pioenvs/firmware-export-handoff/program')], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=15)
    assert result.returncode == 0, (w/'runtime.log').read_text()
    assert 'FIRMWARE_HANDOFF=passed' in (w/'runtime.log').read_text()
    summary = {'passed': True, 'transport': 'synthetic host only', 'checks': ['stale plus nine busy cases rejected', 'idle ownership accepted once', 'all public controls blocked without TX or stop mutation', 'loop and shutdown emit no normal UART during ownership', 'release resumes subscription but requires fresh telemetry for motion']}
    (w/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2))
finally:
    os.close(master)
    os.close(slave)
