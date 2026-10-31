#!/usr/bin/env python3
"""Exercise the shipped renderer and generated-asset compatibility offline."""
import gzip
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


web = module('panel_web', ROOT / 'components/desk_uart/web_panel_lock.py')
layout = module('maintenance_layout', ROOT / 'components/desk_firmware_update/web_layout.py')


def decode(header):
    data = re.search(r'#ifdef USE_WEBSERVER_GZIP\s+constexpr uint8_t INDEX_GZ\[\] PROGMEM = \{([^}]+)', header)[1]
    return gzip.decompress(bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', data))).decode()


class WebTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from esphome.components import web_server
        root = Path(web_server.__file__).parent
        cls.header = (root / 'server_index_v3.h').read_text()
        cls.server = (root / 'web_server.cpp').read_text()

    def test_installed_assets_and_idempotence(self):
        patched = web.decorate_header(self.header)
        self.assertEqual(web.decorate_header(patched), patched)
        self.assertEqual(decode(patched).replace(web.RENDER, web.OLD_RENDER).replace(web.DISCONNECT, ''), decode(self.header))
        self.assertEqual(patched.split('#else')[1:], self.header.split('#else')[1:])
        server = web.decorate_server(self.server)
        self.assertEqual(web.decorate_server(server), server)
        self.assertEqual(server.replace(web.NEW_JSON, web.OLD_JSON).removeprefix(web.INCLUDE), self.server)

    def test_both_build_hook_orders(self):
        a = layout.decorate_header(web.decorate_header(self.header))
        b = web.decorate_header(layout.decorate_header(self.header))
        for result in (a, b):
            self.assertIn(layout.PAYLOAD, decode(result))
            self.assertIn(web.DISCONNECT, decode(result))
            self.assertEqual(web.decorate_header(result), result)

    def test_unknown_upstream_fails(self):
        with self.assertRaises(ValueError):
            web.decorate_header(self.header.replace('INDEX_GZ', 'OTHER'))
        with self.assertRaises(ValueError):
            web.decorate_server(self.server.replace(web.OLD_JSON, 'changed'))
        with self.assertRaises(ValueError):
            web.replace_once('twice twice', 'twice', 'new')

    def test_actual_render_and_disconnect(self):
        # Execute the exact renderer inserted into the bundled Lit application.
        # Preserve template values so click handlers are exercised, not regexed.
        script = r'''
const assert=require('node:assert/strict');
const data=JSON.parse(require('node:fs').readFileSync(0,'utf8'));
const S=(strings,...values)=>({strings,values});
const Renderer=new Function('S','return class {'+data.render+'}')(S);
const r=new Renderer(),sent=[];
r._switch=()=> 'ordinary-switch';
r.actioner={restAction:(entity,action)=>sent.push({name:entity.name,action})};
function render(state){r.entity={name:'Panel lock',domain:'switch',assumed_state:true,desk_panel_lock:true,state};return r.render_switch();}
function view(t){return t.strings.reduce((s,p,i)=>s+p+(typeof t.values[i]==='function'?'handler':t.values[i]??''),'');}
let t=render('OFF');
assert.match(view(t),/color:#ef5350/);assert.doesNotMatch(view(t),/#4caf50/);
assert.match(view(t),/>Off<\/span>/);
const originalEntity=r.entity;
r.entity={name:'Timer',state:'OFF'}; // Shared renderer has advanced to another row.
t.values.filter(x=>typeof x==='function')[1]();
assert.deepEqual(sent,[{name:'Panel lock',action:'turn_on'}]);assert.equal(originalEntity.state,'OFF');
t=render('ON');assert.match(view(t),/color:#4caf50/);assert.doesNotMatch(view(t),/#ef5350/);
assert.match(view(t),/>On<\/span>/);
t.values.filter(x=>typeof x==='function')[0]();
assert.deepEqual(sent,[{name:'Panel lock',action:'turn_on'},{name:'Panel lock',action:'turn_off'}]);
for(const state of ['UNKNOWN',undefined,null]){
 t=render(state);assert.match(view(t),/>Unknown<\/span>/);
 assert.doesNotMatch(view(t),/#ef5350|#4caf50/);
 assert.equal((view(t).match(/aria-pressed="false"/g)||[]).length,2);
}
r.entity={assumed_state:false,state:'ON'};assert.equal(r.render_switch(),'ordinary-switch');
r._actionButton=(entity,label,action)=>action;
r.entity={assumed_state:true,state:'ON'};
assert.deepEqual(r.render_switch().values,['turn_off','turn_on']);
const entities=[{domain:'switch',assumed_state:true,desk_panel_lock:true,state:'ON'},
 {domain:'switch',assumed_state:true,desk_panel_lock:true,state:'OFF'},{domain:'sensor',state:'73.0'},
 {domain:'switch',assumed_state:true,state:'OFF'},
 {domain:'switch',assumed_state:false,state:'ON'}];
let updates=0,listener;
const table={entities,requestUpdate:()=>updates++};
const app={updateComplete:Promise.resolve(),shadowRoot:{querySelector:()=>table}};
const run=new Function('customElements','document','window', 'return (async()=>{'+data.disconnect+'})()');
run({whenDefined:()=>Promise.resolve()},{querySelector:()=>app},
 {source:{addEventListener:(type,fn)=>{assert.equal(type,'error');listener=fn;}}}).then(()=>{
 listener();assert.equal(updates,1);
 assert.deepEqual(entities.map(e=>e.state),['UNKNOWN','UNKNOWN','73.0','OFF','ON']);
 assert.deepEqual(sent,[{name:'Panel lock',action:'turn_on'},{name:'Panel lock',action:'turn_off'}]);
 console.log('PASS: confirmed colors, explicit actions, unknown state, disconnect, ordinary switch');
}).catch(e=>{console.error(e);process.exitCode=1;});
'''
        disconnect = web.DISCONNECT.split('<script type="module">')[1].split('</script>')[0]
        subprocess.run(['node', '-e', script], input=json.dumps({'render': web.RENDER, 'disconnect': disconnect}), text=True, check=True)


if __name__ == '__main__':
    unittest.main()
