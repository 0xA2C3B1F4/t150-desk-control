#!/usr/bin/env python3
"""Compile actual release assets with a literal ref and verify every clone."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from desk_test_support import ROOT, parse_args, test_env
sys.path.insert(0,str(ROOT/"tools"))
import release


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref")
    parser.add_argument("--expected-commit")
    args=parse_args(parser=parser)
    work=args.work_dir
    commit=release.text(ROOT,"rev-parse","HEAD")
    expected=args.expected_commit or commit
    assert expected==commit and re.fullmatch(release.SHA,expected)
    version=json.loads((ROOT/release.VERSION_FILE).read_text())["version"]
    ref=args.ref or commit
    assert ref in (commit,"v"+version), "Release tests use the candidate SHA or its exact tag"
    package=release.package_bytes(ROOT,version,commit)
    results=[]
    for name in ("stamp-c3","esp32","stamp-c3-firmware-export","stamp-c3-firmware-update"):
        run=work/name;run.mkdir()
        env=test_env(run)
        # Exercise normal user imports, including ignored Python bytecode caches.
        env.pop("PYTHONDONTWRITEBYTECODE", None)
        for filename,data in package.items():
            if not filename.endswith('.yaml'):continue
            text=data.decode()
            # Only pre-tag tests substitute the intended tag in temporary copies.
            text=text.replace(f"github://{release.REPOSITORY}@v{version}",f"github://{release.REPOSITORY}@{ref}")
            text=re.sub(r"  build_directory: .*",f"  build_directory: {run}/build",text)
            (run/filename).write_text(text)
        secrets=(run/'secrets.example.yaml').read_text().replace('REPLACE_WITH_GENERATED_BASE64_KEY','AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=')
        (run/'secrets.yaml').write_text(secrets)
        config=run/(name+'.yaml')
        for action in ('config','compile'):
            with (run/(action+'.log')).open('w') as log:
                subprocess.run(['esphome',action,str(config)],env=env,cwd=run,stdout=log,stderr=subprocess.STDOUT,check=True)
        clones=list((run/'data/external_components').glob('*/.git'))
        assert clones, "No GitHub source was fetched"
        resolved=[]
        for dotgit in clones:
            clone=dotgit.parent
            sha=release.text(clone,'rev-parse','HEAD');assert sha==expected,(name,sha,expected)
            remote=release.text(clone,'remote','get-url','origin')
            assert remote.removesuffix('.git')=='https://github.com/'+release.REPOSITORY
            resolved.append(sha)
        defines=(run/'build/src/esphome/core/defines.h').read_text()
        assert f'#define DESK_SOURCE_REVISION "{expected}"' in defines
        assert '#define DESK_SOURCE_STATE "clean"' in defines
        generated=run/'build/src/esphome/components'
        required=['desk_uart']+(['desk_firmware_export'] if 'firmware' in name else [])+(['desk_firmware_update'] if name.endswith('update') else [])
        for component in required:
            for src in (ROOT/'components'/component).iterdir():
                if src.suffix in ('.h','.cpp'):
                    assert (generated/component/src.name).read_bytes()==src.read_bytes(),str(src)
        results.append({'name':name,'passed':True,'ref':ref,'resolved_commits':resolved})
    (work/'summary.json').write_text(json.dumps({'passed':True,'candidate':commit,'ref':ref,'configurations':results,'uploaded':False},indent=2)+'\n')
    print(json.dumps(results))


if __name__=='__main__':main()
