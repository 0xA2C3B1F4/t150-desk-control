#!/usr/bin/env python3
"""Identity diagnostics with a real ESPHome host and synthetic UART, no device."""
import json,os,select,subprocess,time
from desk_test_support import ROOT,build_host,encode,frames,open_pty,parse_args,test_env

def main():
    work=parse_args(__doc__).work_dir;env=test_env(work);master,slave=open_pty();proc=None
    fields={'controller_model':'T150-22-03','controller_manufacturer':'LUMI LEGEND GROUP','controller_identifiers':'05: 1.5.0; 06: 1.6; 07: 1.9.0118','controller_date':'21/01/18'}
    values={3:b'T150-22-03'+bytes(6),4:b'LUMI LEGEND GROUP',5:b'1.5.0',6:b'1.6'+bytes(2),7:b'1.9.0118',9:b'21/01/18'}
    cfg=work/'host.yaml'
    cfg.write_text(f'''esphome:
  name: desk-identity-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - delay: 6s
      - button.press: stop_button
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
  height:
    name: Height
  stop:
    id: stop_button
    name: Stop
'''+''.join(f'''  {key}:
    name: {key}
    on_value:
      - lambda: ESP_LOGI("identity_test", "{key}=%s", x.c_str());
''' for key in fields))
    results=[]
    try:
        with (work/'build.log').open('w') as log:build_host(cfg,work,env,log,uart_port=os.ttyname(slave))
        for scenario,seconds in [('all_fields',16),('missing_malformed_and_moving',27)]:
            while select.select([master],[],[],0)[0]:os.read(master,4096)
            output=bytearray();processed=seq=0;next_height=0;counts={};queries=[];stops=[]
            def reply(body):
                nonlocal seq
                os.write(master,encode(bytes(body)+seq.to_bytes(2,'big')));seq+=1
            def identity(selector,data):reply(bytes([0x12,selector])+data)
            with (work/(scenario+'.log')).open('w') as log:
                proc=subprocess.Popen([str(work/'build/.pioenvs/desk-identity-test/program')],cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT)
                start=time.monotonic()
                while time.monotonic()-start<seconds:
                    t=time.monotonic()-start;assert proc.poll() is None
                    if t>=2 and t>=next_height:
                        q=10 if scenario=='all_fields' or t<11 else int(min(t,13)*10)
                        body=bytearray(30);body[0]=5;body[19:21]=q.to_bytes(2,'big');reply(body);next_height=t+.1
                    if select.select([master],[],[],.01)[0]:
                        output.extend(os.read(master,4096));found=list(frames(output))
                        for frame in found[processed:]:
                            if frame[0]==0x11:
                                selector=frame[1];assert frame==bytes([0x11,selector,0,0]);assert selector in values
                                queries.append([round(t,3),selector]);counts[selector]=counts.get(selector,0)+1
                                assert t>=3.4,('query without stable fresh height',t)
                                if scenario!='all_fields':
                                    assert not 11.2<t<14.4,('query during local movement',t)
                                    if selector==9:continue
                                    if selector==3 and counts[selector]==1:
                                        identity(4,values[4]);identity(3,values[3][:-1]);continue
                                    if selector==6 and counts[selector]==1:
                                        identity(6,b'1\x006\0\0');continue
                                identity(selector,values[selector])
                            elif frame==bytes([0x17,1,0,0]):
                                stops.append(t);reply([0x18,1,0,0])
                            else:assert frame[0]==0x13,('unexpected write or movement',frame.hex())
                        processed=len(found)
                proc.terminate();proc.wait(timeout=5)
            text=(work/(scenario+'.log')).read_text()
            for key,value in fields.items():
                if scenario!='all_fields' and key=='controller_date':assert f'{key}=' not in text
                else:assert f'{key}={value}' in text,(key,text[-2000:])
            assert len(stops)==1 and 5.7<stops[0]<6.7,stops
            if scenario=='all_fields':assert counts=={k:1 for k in values},counts
            else:assert counts=={3:2,4:1,5:1,6:2,7:1,9:3},counts
            assert text.count('controller_identifiers=')==1
            results.append(dict(scenario=scenario,passed=True,queries=queries,stops=stops))
        result=dict(passed=True,no_hardware=True,scenarios=results)
        (work/'summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
    finally:
        if proc is not None and proc.poll() is None:proc.kill();proc.wait()
        os.close(master);os.close(slave)
if __name__=='__main__':main()
