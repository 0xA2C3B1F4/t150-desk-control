#pragma once
// Generated from ui.html by tools/embed_firmware_ui.py.
namespace esphome::desk_firmware_update {
static const char FIRMWARE_UI[] = R"DESKUI(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk firmware maintenance</title>
<style>
:root{color-scheme:light dark;font:16px system-ui}body{max-width:760px;margin:2rem auto;padding:0 1rem}h1{font-size:1.6rem}section{border:1px solid #8886;border-radius:10px;padding:1rem;margin:1rem 0}button,input{font:inherit}button{padding:.65rem .9rem;margin:.25rem .2rem .25rem 0;cursor:pointer}button:disabled{opacity:.5;cursor:default}label{display:block;margin:.8rem 0}code{overflow-wrap:anywhere;font-size:.85rem}#message{white-space:pre-wrap}progress{width:100%}.note{font-size:.9rem}a{color:inherit}
</style>
<h1>Desk firmware maintenance</h1>
<p><a href="/">Back to desk controls</a></p>
<p>Read the controller application, save a backup, then install the supported DT02 patch or restore an original file. Normal desk controls stay paused throughout maintenance.</p>
<section><h2>1. Prepare and read</h2>
<p>Disconnect the motor cable with the controller powered off. Keep the ESP32 and UART cable connected. Power the controller on when the reader asks for it.</p>
<label><input type="checkbox" id="isolated"> The motor is disconnected.</label>
<label><input type="checkbox" id="takeover"> Take over an idle maintenance session from another tab.</label>
<button id="claim">Enter maintenance</button><button id="read">Read controller firmware</button>
<p id="boot" role="status"></p>
</section>
<section><h2>2. Save and prepare</h2>
<button id="backupDownload">Download original backup</button>
<label><input type="checkbox" id="saved"> I have saved the backup file on my computer and can select it again.</label>
<button id="patch">Prepare DT02 patch</button>
<p>Only the exact supported original is patchable. An unknown image can still be saved for research. Sending a file for research is a separate, optional step below.</p>
<label>Or select a previously saved original or DT02 application file:
<input type="file" id="file" accept=".bin,application/octet-stream"></label>
<button id="stage">Prepare selected file</button><button id="preparedDownload">Download prepared firmware</button>
<p class="note">Files in this tab are temporary. The computer download is your persistent backup. Only 55,296-byte application files are accepted for installation.</p>
</section>
<section id="researchSection" hidden><h2>Send an unknown version for research</h2>
<p>The operator stores this file privately until manually removed. Sending it does not approve a patch or install anything.</p>
<label><input type="checkbox" id="researchConsent"> I agree to send my downloaded controller image to <span id="researchHost"></span> for analysis.</label>
<button id="research">Send firmware for research</button><p id="receipt"></p>
</section>
<section><h2>3. Install and check</h2>
<p>Installation erases application flash. Keep power connected during the transfer. A failed transfer may require restoring the original file or ST-Link recovery.</p>
<label><input type="checkbox" id="installConsent"> I want to install the prepared image and will keep power connected during writing.</label>
<button id="install">Arm installation</button><button id="verify">Verify by full readback</button>
<button id="cancel">Cancel waiting or transfer</button><button id="exit">Finish maintenance</button>
<p>After installation, select Verify and power-cycle the controller once when prompted. A transfer ACK is not verification.</p>
</section>
<section aria-live="polite"><strong id="state">Connecting…</strong><progress id="progress" max="55296" value="0"></progress>
<p id="message"></p><p>Application SHA-256: <code id="hash">Unavailable</code></p><p id="heap"></p><p id="freshness"></p></section>
<script>
'use strict';
const $=id=>document.getElementById(id);
let status=null,locked=false,original=null,originalHash=null,selected=null,cancelUpload=false;
let owner=sessionStorage.getItem('desk-firmware-owner');
if(!owner){const bytes=new Uint8Array(16);crypto.getRandomValues(bytes);owner=Array.from(bytes,x=>x.toString(16).padStart(2,'0')).join('');sessionStorage.setItem('desk-firmware-owner',owner);}
async function refresh(){const r=await fetch('/desk-firmware/status',{cache:'no-store'});if(!r.ok)throw Error(await r.text());status=await r.json();render();return status;}
function render(){
 $('researchSection').hidden=!status.research_url;
 if(status.research_url)$('researchHost').textContent=new URL(status.research_url).hostname;
 $('research').disabled=locked||!original;
 $('state').textContent=status.state.replaceAll('_',' ');
 $('hash').textContent=status.sha256||'Unavailable';
 $('progress').max=status.progress_max||55296;$('progress').value=status.progress;
 $('heap').textContent=`Free RAM: ${status.free_heap} bytes; largest block: ${status.largest_block} bytes.`;
 $('freshness').textContent=status.preflight_seconds ? `Prepare and arm within ${status.preflight_seconds} seconds, or read again.` : '';
 if(status.error)$('message').textContent=status.error;
 $('boot').textContent=status.waiting_for_power_cycle ? 'Armed. Power-cycle only the controller once now. Keep the ESP32 powered and the motor disconnected.' : status.active ? 'Transfer running. Leave controller power connected.' : status.state==='readback_required' ? 'Transfer ended. Select Verify by full readback before the next power cycle.' : status.state==='verified' ? 'All application bytes verified. You can finish maintenance.' : '';
 for(const id of ['claim','read','patch','stage','install','verify','exit','backupDownload','preparedDownload'])$(id).disabled=locked||status.pending||status.active;
 $('cancel').disabled=status.pending||(!status.active&&status.state!=='uploading');
 $('backupDownload').disabled=locked||status.state!=='read_complete';
 $('preparedDownload').disabled=locked||!['staged','verified'].includes(status.state);
 $('patch').disabled=locked||status.state!=='read_complete'||status.sha256!=='bf52d596c81d6d499480a9dd3173e6991de5ee0c72364e0b53bc91ea89053faf';
 $('install').disabled=locked||status.state!=='staged';
 $('verify').disabled=locked||!['readback_required','staged'].includes(status.state)||!status.recovery_required;
 $('exit').disabled=locked||status.active||status.recovery_required;
}
async function action(action,extra={}){
 await refresh();
 const r=await fetch('/desk-firmware/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Desk-CSRF':status.csrf},body:new URLSearchParams({owner,revision:String(status.revision),action,...extra})});
 if(!r.ok)throw Error(await r.text());
 for(let i=0;i<100;i++){await new Promise(r=>setTimeout(r,30));await refresh();if(!status.pending)break;}
 if(status.pending)throw Error('Action is still pending. Refresh status before continuing.');
 if(status.error)throw Error(status.error);
}
async function task(fn){if(locked)return;locked=true;render();$('message').textContent='';try{await fn();}catch(e){$('message').textContent=e.message;}finally{locked=false;await refresh().catch(()=>{});}}
function save(bytes,name){const url=URL.createObjectURL(new Blob([bytes],{type:'application/octet-stream'}));const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),60000);}
async function download(){const r=await fetch('/desk-firmware/download',{cache:'no-store'});if(!r.ok)throw Error(await r.text());return new Uint8Array(await r.arrayBuffer());}
async function confirmBackup(){if(!$('saved').checked||!original||original.length!==status.size||originalHash!==status.source_sha256)throw Error('Download this controller read and confirm the saved backup first.');if(status.state==='read_complete')await action('backup',{confirmed:'yes'});}
$('claim').onclick=()=>task(async()=>{if(!$('isolated').checked)throw Error('Disconnect the motor with controller power off first.');await action('claim',{motor_isolated:'yes',takeover:$('takeover').checked?'yes':'no'});});
$('read').onclick=()=>task(async()=>{original=null;$('saved').checked=false;await action('read');});
$('backupDownload').onclick=()=>task(async()=>{original=await download();originalHash=status.sha256;save(original,`t150-backup-${status.sha256.slice(0,12)}.bin`);});
$('patch').onclick=()=>task(async()=>{await confirmBackup();await action('patch');});
$('file').onchange=async()=>{const f=$('file').files[0];selected=null;if(f){if(f.size!==55296){$('message').textContent='Select a 55,296-byte application file.';return;}selected=new Uint8Array(await f.arrayBuffer());}};
$('stage').onclick=()=>task(async()=>{if(!selected)throw Error('Select an application file first.');await confirmBackup();await action('stage');cancelUpload=false;for(let offset=0;offset<selected.length;offset+=128){const data=Array.from(selected.slice(offset,offset+128),x=>x.toString(16).padStart(2,'0')).join('');if(cancelUpload){await action('cancel');throw Error('File preparation cancelled.');}
 const r=await fetch('/desk-firmware/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Desk-CSRF':status.csrf},body:new URLSearchParams({owner,revision:String(status.revision),action:'chunk',offset:String(offset),data})});
 if(!r.ok)throw Error(await r.text());status.revision=(await r.json()).revision;status.progress=offset+data.length/2;render();}await action('seal');});
$('preparedDownload').onclick=()=>task(async()=>save(await download(),`t150-prepared-${status.sha256.slice(0,12)}.bin`));
$('install').onclick=()=>task(async()=>{if(!$('isolated').checked||!$('installConsent').checked)throw Error('Confirm motor isolation and installation first.');await action('install',{confirmed:'yes'});});
$('verify').onclick=()=>task(async()=>{await action('verify');});
$('research').onclick=()=>task(async()=>{
 if(!original||!$('researchConsent').checked||!status.research_url)throw Error('Download the controller image and agree to send it first.');
 const r=await fetch(status.research_url,{method:'POST',credentials:'omit',headers:{'Content-Type':'application/octet-stream','X-Upload-Consent':'yes'},body:original});
 const receipt=await r.text();if(!r.ok)throw Error('Research upload failed: '+receipt.slice(0,500));
 $('receipt').textContent='Stored for research. Receipt: '+receipt.slice(0,1000);
});
$('cancel').onclick=()=>{if(locked&&status.state==='uploading'){cancelUpload=true;return;}task(()=>action('cancel'));};
$('exit').onclick=()=>task(()=>action('exit'));
refresh().catch(e=>$('message').textContent=e.message);
setInterval(()=>{if(!locked)refresh().catch(e=>$('message').textContent='Status unavailable: '+e.message);},1000);
</script></html>
)DESKUI";
}
