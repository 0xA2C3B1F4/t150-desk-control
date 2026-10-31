"""Confirmed-state controls in generated ESPHome 2026.4.2 local v3 assets.

Only builds opting into child_lock use this hook. No installed ESPHome files
are changed. Unknown upstream formats fail the build rather than silently
shipping misleading controls. Explicit on/off commands remain available.
"""
import gzip
from pathlib import Path
import re
import warnings

MARKER = "<!-- desk-confirmed-switch-v1 -->"
# ESPHome 2026.4.2 passes a plain array to Lit's SVG tag for its static logo.
# Construct that fixed SVG as a tagged template, retaining Lit's raw metadata.
OLD_LOGO_INIT = "var Nu=`"
NEW_LOGO_INIT = "var Nu=sa`"
OLD_LOGO_RENDER = "render(){return sa([Nu])}"
NEW_LOGO_RENDER = "render(){return Nu}"
OLD_RENDER = r'render_switch(){if(!!this.entity)return this.entity.assumed_state?S`${this._actionButton(this.entity,"\u274C","turn_off")} ${this._actionButton(this.entity,"\u2714\uFE0F","turn_on")}`:this._switch(this.entity)}'
RENDER = '''render_switch(){
  if(!this.entity)return;
  if(!this.entity.desk_panel_lock)return this.entity.assumed_state?S`${this._actionButton(this.entity,"\\u274C","turn_off")} ${this._actionButton(this.entity,"\\u2714\\uFE0F","turn_on")}`:this._switch(this.entity);
  // The table reuses one renderer for every row; bind this row's entity now.
  const entity=this.entity,state=entity.state;
  const off=state==="OFF",on=state==="ON";
  const actioner=this.actioner,send=action=>actioner?.restAction(entity,action);
  return S`<div role="group" aria-label="${this.entity.name}" style="display:flex;align-items:center;gap:.7em">
    <button type="button" aria-label="Turn off" aria-pressed="${off}" title="Turn off"
      style="color:${off?'#ef5350':'#9e9e9e'};font:inherit;font-size:1.4em;background:none;border:0;cursor:pointer"
      @click="${()=>send('turn_off')}">✕</button>
    <span aria-live="polite" style="min-width:4.5em;text-align:center">${on?'On':off?'Off':'Unknown'}</span>
    <button type="button" aria-label="Turn on" aria-pressed="${on}" title="Turn on"
      style="color:${on?'#4caf50':'#9e9e9e'};font:inherit;font-size:1.4em;background:none;border:0;cursor:pointer"
      @click="${()=>send('turn_on')}">✓</button>
  </div>`;
}'''
# Reuse the existing SSE connection, including its authenticated transport.
# Clear confirmed colors on disconnect; only a new server event restores them.
DISCONNECT = '''<!-- desk-confirmed-switch-v1 -->
<script type="module">
await customElements.whenDefined('esp-app');
const app=document.querySelector('esp-app');
await app.updateComplete;
const table=app.shadowRoot?.querySelector('esp-entity-table');
window.source?.addEventListener('error',()=>{
  if(!table)return;
  for(const entity of table.entities)if(entity.desk_panel_lock)entity.state='UNKNOWN';
  table.requestUpdate();
});
</script>
'''
OLD_JSON = 'set_json_icon_state_value(root, obj, "switch", value ? "ON" : "OFF", value, start_config);'
NEW_JSON = OLD_JSON + '''
  // desk-confirmed-switch-v1: ordinary ESPHome switches do not maintain has_state().
  if (desk_uart::ChildLockSwitch::is_instance(obj)) {
    root[ESPHOME_F("desk_panel_lock")] = true;
    if (!obj->has_state()) {
      root[ESPHOME_F("state")] = "UNKNOWN";
      root[ESPHOME_F("value")] = nullptr;
    }
  }'''
INCLUDE = '#include "esphome/components/desk_uart/desk_uart.h"\n'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Unsupported ESPHome web asset; expected exactly one confirmed-switch anchor")
    return text.replace(old, new, 1)


def decorate_header(source):
    pattern = r'(#ifdef USE_WEBSERVER_GZIP\s+constexpr uint8_t INDEX_GZ\[\] PROGMEM = \{)([^}]+)(\};)'
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise ValueError("Unsupported ESPHome gzip header")
    m = matches[0]
    if re.sub(r'0x[0-9a-fA-F]{2}|[,\s]', '', m[2]):
        raise ValueError("Unsupported ESPHome index bytes")
    html = gzip.decompress(bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m[2]))).decode()
    if MARKER in html:
        if RENDER not in html or DISCONNECT not in html:
            raise ValueError("Inconsistent confirmed-switch decoration")
    else:
        html = replace_once(html, OLD_RENDER, RENDER)
        html = replace_once(html, '</body>', DISCONNECT + '</body>')
    logo_anchors = (html.count(OLD_LOGO_INIT), html.count(OLD_LOGO_RENDER))
    if logo_anchors == (1, 1):
        html = html.replace(OLD_LOGO_INIT, NEW_LOGO_INIT, 1)
        html = html.replace(OLD_LOGO_RENDER, NEW_LOGO_RENDER, 1)
    elif logo_anchors != (0, 0):
        warnings.warn("Unrecognized ESPHome logo format; leaving the optional logo fix out")
    data = gzip.compress(html.encode(), compresslevel=9, mtime=0)
    lines = ['    ' + ', '.join(f'0x{x:02x}' for x in data[i:i+16]) + ',' for i in range(0, len(data), 16)]
    return source[:m.start(2)] + '\n' + '\n'.join(lines) + '\n' + source[m.end(2):]


def decorate_server(source):
    if NEW_JSON in source:
        if INCLUDE not in source:
            raise ValueError("Missing panel lock type declaration")
        return source
    return INCLUDE + replace_once(source, OLD_JSON, NEW_JSON)


def patch_build(build):
    root = build / 'src/esphome/components/web_server'
    header, server = root / 'server_index_v3.h', root / 'web_server.cpp'
    # Validate both assets before modifying either generated file.
    updates = [(header, decorate_header(header.read_text())), (server, decorate_server(server.read_text()))]
    for path, text in updates:
        if path.read_text() != text:
            path.write_text(text)


if 'Import' in globals():
    Import('env')
    patch_build(Path(env.subst('$PROJECT_DIR')))
