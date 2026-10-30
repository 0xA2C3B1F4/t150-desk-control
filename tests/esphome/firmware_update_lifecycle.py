#!/usr/bin/env python3
"""Exercise the real updater with host UART/HTTP/heap/preferences stand-ins.

Optional private image arguments exercise the complete hash-bound workflow.
The repository contains no controller firmware. No network or device access.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import esphome
from desk_test_support import ROOT, parse_args

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--original', type=Path)
parser.add_argument('--patched', type=Path)
args = parse_args(parser=parser)
if bool(args.original) != bool(args.patched):
    parser.error('Provide both private images or neither')
work = args.work_dir
host = ROOT / 'tests/esphome/firmware_export_host'
update = ROOT / 'tests/esphome/firmware_update_host'
package = Path(esphome.__file__).resolve().parent
for name in ['desk_firmware_export', 'desk_firmware_update']:
    link = work / 'include/esphome/components' / name
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(ROOT / 'components' / name, target_is_directory=True)
flags = ['-std=c++17','-DUSE_HOST','-pthread','-Wall','-Wextra','-Wpedantic','-Werror',
         '-fsanitize=address,undefined','-fno-omit-frame-pointer','-O1','-g']
for path in [update/'include',host/'include',work/'include',ROOT/'components/desk_firmware_update',
             ROOT/'lib/desk_protocol/include',package.parent]:
    flags += ['-I'+str(path)]
openssl = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
sources = [ROOT/'components/desk_firmware_export/desk_firmware_export.cpp',
           ROOT/'components/desk_firmware_update/desk_firmware_update.cpp',
           package/'components/sha256/sha256.cpp',update/'update_test.cpp']
command = [os.environ.get('CXX','c++'),*flags,*map(str,sources),*openssl,'-o',str(work/'update-test')]
with (work/'build.log').open('w') as log:
    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
command = [str(work/'update-test')]
if args.original:
    command += [str(args.original.resolve()),str(args.patched.resolve())]
with (work/'run.log').open('w') as log:
    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
summary = dict(passed=True,private_images=bool(args.original),hardware_access=False,
               boundary='Real adapter and SHA; host stand-ins for IDF, HTTP transport, UART, heap and preferences')
(work/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print((work/'run.log').read_text())
print(json.dumps(summary))
