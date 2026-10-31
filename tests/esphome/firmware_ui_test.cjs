// Exercise the real page script with simulated status/HTTP, without device access.
const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const vm=require('node:vm');
const assert=require('node:assert/strict');
const html=readFileSync(join(__dirname,'../../components/desk_firmware_update/ui.html'),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const base={state:'idle',claimed:false,active:false,pending:false,recovery_required:false,waiting_for_power_cycle:false,preflight_seconds:120,size:55296,progress:0,revision:0,error:'',research_url:'',sha256:'',source_sha256:''};
async function page(){
 const elements={};
 for(const tag of html.matchAll(/<[^>]+\bid="([^"]+)"[^>]*>/g))elements[tag[1]]={textContent:'',hidden:/\bhidden\b/.test(tag[0]),disabled:/\bdisabled\b/.test(tag[0]),checked:false,files:[],tag:tag[0],scrollIntoView(){},focus(){}};
 const io={status:{...base},offline:false,reject:false};
 const context=vm.createContext({document:{getElementById:id=>elements[id],querySelectorAll:()=>Object.values(elements).filter(x=>x.tag.startsWith('<button'))},crypto:require('node:crypto').webcrypto,URL,URLSearchParams,Uint8Array,Blob,setInterval(){},setTimeout:fn=>{fn();},fetch:async(url,options)=>{
  if(io.offline)throw Error('offline');
  if(options?.method==='POST'){
   if(io.reject)return {ok:false,text:async()=> 'This tab does not own maintenance'};
   const action=options.body.get('action');
   if(action==='exit')io.status={...base};
   if(action==='claim')io.status.claimed=true;
   return {ok:true};
  }
  return {ok:true,json:async()=>({...io.status})};
 }});
 vm.runInContext(script,context);await vm.runInContext('refresh()',context);
 return {elements,io,run:s=>vm.runInContext(s,context),state:async(s,owned=true)=>{io.status={...base,claimed:owned,...s};vm.runInContext(`sessionOwned=${owned}`,context);await vm.runInContext('refresh()',context);}};
}
(async()=>{
 const p=await page(),e=p.elements;
 await p.state({state:'installing',active:true,waiting_for_power_cycle:true});
 assert.match(e.guidanceTitle.textContent,/OFF, then ON/);assert.equal(e.cancel.hidden,false);
 p.io.offline=true;await assert.rejects(p.run('refresh()'));
 assert.equal(e.message.hidden,false);assert.match(e.guidanceTitle.textContent,/Connection unavailable/);assert.equal(e.cancel.disabled,true);
 p.io.offline=false;await p.state({state:'installing',active:true});
 assert.match(e.guidanceTitle.textContent,/keep controller power ON/);assert.equal(e.message.textContent,'');
 await p.state({state:'staged',recovery_required:true});assert.equal(e.verify.hidden,false);assert.equal(e.verify.disabled,false);
 await p.state({state:'read_complete',sha256:'e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff'});
 assert.equal(e.guidanceTitle.textContent,'DT02 is already installed');assert.equal(e.patch.disabled,true);
 await p.state({state:'staged',sha256:'e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff'});assert.doesNotMatch(e.guidanceTitle.textContent,/already installed/);
 await p.state({state:'staged',preflight_seconds:0});assert.match(e.guidanceTitle.textContent,/expired/);assert.equal(e.read.hidden,false);assert.equal(e.install.disabled,true);
 await p.state({state:'error',error:'Read timed out'});assert.match(e.guidanceTitle.textContent,/stopped/);assert.equal(e.message.hidden,false);
 p.run('finishedAt=0;render()');await e.exit.onclick();assert.match(e.guidanceTitle.textContent,/Restart the controller/);
 await p.state({claimed:true},false);assert.equal(e.sessionControls.hidden,false);assert.match(e.guidanceTitle.textContent,/Take over/);
 await p.state({claimed:true});p.io.reject=true;await e.read.onclick();assert.equal(e.sessionControls.hidden,false);
 await p.state({state:'uploading'});assert.match(e.guidanceTitle.textContent,/not running/);assert.equal(e.cancel.hidden,false);
 const errors=await page();await errors.state({state:'error',error:'Old failure'});await errors.state({state:'idle'});assert.equal(errors.elements.message.textContent,'');
 await p.state({state:'read_complete',recovery_required:true,sha256:'partial'});assert.match(e.guidanceDetail.textContent,/saved original application file/);assert.doesNotMatch(e.guidanceDetail.textContent,/finish maintenance/);assert.equal(e.exit.hidden,true);
 await p.state({state:'read_complete',sha256:'unknown',research_url:''});assert.doesNotMatch(e.guidanceDetail.textContent,/research section/);
 e.file.files=[{size:1}];await e.file.onchange();assert.match(e.message.textContent,/55,296/);e.file.files=[{size:55296,arrayBuffer:async()=>new ArrayBuffer(55296)}];await e.file.onchange();assert.equal(e.message.textContent,'');
 const fresh=await page();fresh.io.offline=true;fresh.run('status=null');await assert.rejects(fresh.run('refresh()'));assert.equal(fresh.elements.message.hidden,false);
 console.log('Firmware UI: power instructions, connection loss/recovery, staged verification, DT02 labels, expiry, failed exit, takeover and initial failure passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
