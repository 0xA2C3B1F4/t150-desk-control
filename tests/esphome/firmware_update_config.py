#!/usr/bin/env python3
"""Check updater opt-in and mutual exclusion without accessing a device."""
import json
import os
import shutil
import subprocess
import sys
from desk_test_support import ROOT, parse_args
args=parse_args()
w=args.work_dir
(w/'packages').mkdir()
for name in ['firmware-export','firmware-update']:
    shutil.copy2(ROOT/f'examples/esphome/packages/{name}.yaml',w/f'packages/{name}.yaml')
base=(ROOT/'examples/esphome/stamp-c3.yaml').read_text().replace('path: ../../components',f'path: {ROOT}/components').replace('build_directory: .esphome/build/t150-desk-control-stamp-c3',f'build_directory: {w}/build')
(w/'base.yaml').write_text(base)
(w/'secrets.yaml').write_text((ROOT/'examples/esphome/secrets.example.yaml').read_text().replace('REPLACE_WITH_GENERATED_BASE64_KEY','AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='))
env=dict(os.environ,ESPHOME_DATA_DIR=str(w/'data'),PYTHONDONTWRITEBYTECODE='1')
results=[]
for name,packages,valid in [('normal',[],True),('reader',['firmware-export'],True),('updater',['firmware-update'],True),('both',['firmware-export','firmware-update'],False),('no_auth',['firmware-update'],False),('with_ota',['firmware-update'],False)]:
    yaml='packages:\n  base: !include base.yaml\n'+''.join(f'  p{i}: !include packages/{p}.yaml\n' for i,p in enumerate(packages))+f'substitutions:\n  firmware_components_path: {ROOT}/components\n'
    if name=='no_auth': yaml+='web_server:\n  auth: !remove\n'
    if name=='with_ota': yaml+='ota:\n  - platform: esphome\n'
    path=w/f'{name}.yaml';path.write_text(yaml)
    result=subprocess.run([sys.executable,'-m','esphome','compile',str(path),'--only-generate'],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (w/f'{name}.log').write_text(result.stdout)
    assert (result.returncode==0)==valid,(name,result.stdout)
    if valid:
        generated=(w/'build/src/main.cpp').read_text()
        assert ('desk_firmware_update::FirmwareUpdate' in generated)==(name=='updater')
    results.append(dict(configuration=name,expected_valid=valid,passed=True))
(w/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results))
