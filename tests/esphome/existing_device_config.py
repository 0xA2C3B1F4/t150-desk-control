#!/usr/bin/env python3
"""Compile the minimal snippet in an existing ESP32 configuration without web."""
import json
import subprocess
from desk_test_support import ROOT, parse_args, test_env

args=parse_args(description=__doc__)
work=args.work_dir
env=test_env(work)
snippet=(ROOT/'examples/esphome/existing-device.yaml').read_text()
snippet=snippet.replace('source: github://0xA2C3B1F4/t150-desk-control@main',
                       f'source:\n      type: local\n      path: {ROOT}/components')
config=f'''esphome:
  name: existing-desk-device
  build_path: {work}/build
  project:
    name: owner.existing_device
    version: "9.0"
esp32:
  variant: esp32c3
  framework:
    type: esp-idf
logger:
  baud_rate: 0
wifi:
  ssid: "test-only-network"
  password: "test-only-password"
api:
  encryption:
    key: AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=
ota:
  - platform: esphome
    password: test-only-ota-password
'''+snippet
cfg=work/'existing.yaml';cfg.write_text(config)
for action in ('config','compile'):
    with (work/(action+'.log')).open('w') as log:
        subprocess.run(['esphome',action,str(cfg)],env=env,cwd=work,stdout=log,stderr=subprocess.STDOUT,check=True)
defines=(work/'build/src/esphome/core/defines.h').read_text()
assert '"owner.existing_device"' in defines and '"9.0"' in defines
assert '#define USE_WEBSERVER' not in defines
main=(work/'build/src/main.cpp').read_text()
for marker in ('test-only-network','test-only-password','test-only-ota-password'):
    assert marker in main
(work/'summary.json').write_text(json.dumps({'passed':True,'existing_project_preserved':True,
    'network_api_ota_preserved':True,'web_server':False,'uploaded':False},indent=2)+'\n')
