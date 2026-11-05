"""Confirmed-state controls in generated ESPHome 2026.10.1 local v3 assets.

Only builds opting into confirmed desk controls use this hook.
No installed ESPHome files are changed. Unknown upstream formats fail the build rather than silently
shipping misleading controls. Explicit on/off commands remain available.
"""

import gzip
import re

MARKER = "<!-- desk-confirmed-switch-v1 -->"
SPEED_MARKER = "<!-- desk-confirmed-speed-v1 -->"
TRIM_MARKER = "<!-- desk-confirmed-trim-v1 -->"
OLD_NUMBER = "render_number(){if(this.entity)return D`${this._range(this.entity,`set`,`value`,this.entity.value,this.entity.min_value,this.entity.max_value,this.entity.step)} ${this.entity.uom}`}"
NUMBER = """render_number(){
  if(!this.entity)return;
  if(this.entity.desk_display_timeout){
    const entity=this.entity,actioner=this.actioner,confirmed=Number(entity.value);
    const valid=value=>Number.isInteger(value)&&value>=1&&value<=65535;
    if(entity.value===null||entity.value===undefined||!valid(confirmed))return D`<span aria-live="polite">Unknown</span>`;
    const send=event=>{
      const raw=event.target.value,value=Number(raw);
      if(raw.trim()!==''&&valid(value))actioner?.restAction(entity,'set?value='+encodeURIComponent(value));
      event.target.value=String(confirmed);
    };
    return D`<input type="number" aria-label="${entity.name}" min="1" max="65535" step="1" .value="${String(confirmed)}" @change="${send}"> ${entity.uom}`;
  }
  if(!this.entity.desk_motion_trim)return D`${this._range(this.entity,'set','value',this.entity.value,this.entity.min_value,this.entity.max_value,this.entity.step)} ${this.entity.uom}`;
  const entity=this.entity,actioner=this.actioner,confirmed=Number(entity.value);
  const known=entity.value!==null&&entity.value!==undefined&&Number.isInteger(confirmed)&&confirmed>=-3&&confirmed<=3;
  if(!known)return D`<span aria-live="polite">Unknown</span>`;
  const send=event=>{
    const raw=event.target.value,value=Number(raw);
    if(raw.trim()!==''&&Number.isInteger(value)&&value>=-3&&value<=3)actioner?.restAction(entity,'set?value='+encodeURIComponent(value));
    event.target.value=String(confirmed);
  };
  return D`<input type="number" aria-label="${entity.name}" min="-3" max="3" step="1" .value="${String(confirmed)}" @change="${send}">`;
}"""
OLD_SELECT = "render_select(){if(this.entity)return this._select(this.entity,`set`,`option`,this.entity.option||[],this.entity.value)}"
SELECT = """render_select(){
  if(!this.entity)return;
  if(this.entity.desk_display_mode){
    const entity=this.entity,actioner=this.actioner;
    const valid=value=>value==='Auto blank'||value==='Normal';
    if(!valid(entity.value))return D`<span aria-live="polite">Unknown</span>`;
    const send=event=>{
      if(valid(event.target.value))actioner?.restAction(entity,'set?option='+encodeURIComponent(event.target.value));
      event.target.value=entity.value;
    };
    return D`<select aria-label="${entity.name}" .value="${entity.value}" @change="${send}">
      <option value="Auto blank" ?selected="${entity.value==='Auto blank'}">Auto blank</option>
      <option value="Normal" ?selected="${entity.value==='Normal'}">Normal</option>
    </select>`;
  }
  if(!this.entity.desk_motion_speed)return this._select(this.entity,"set","option",this.entity.option||[],this.entity.value);
  const entity=this.entity,actioner=this.actioner;
  const known=entity.value==='Normal'||entity.value==='Slow';
  if(!known)return D`<span aria-live="polite">Unknown</span>`;
  const send=event=>{
    const option=event.target.value;
    if(option==='Normal'||option==='Slow')actioner?.restAction(entity,'set?option='+encodeURIComponent(option));
    // A request is not confirmation. Retain only the last server value.
    event.target.value=entity.value;
  };
  return D`<select aria-label="${entity.name}" .value="${entity.value}" @change="${send}">
    <option value="Normal" ?selected="${entity.value==='Normal'}">Normal</option>
    <option value="Slow" ?selected="${entity.value==='Slow'}">Slow</option>
  </select>`;
}"""
OLD_RENDER = "render_switch(){if(this.entity)return this.entity.assumed_state?D`${this._actionButton(this.entity,`❌`,`turn_off`)} ${this._actionButton(this.entity,`✔️`,`turn_on`)}`:this._switch(this.entity)}"
RENDER = """render_switch(){
  if(!this.entity)return;
  if(!this.entity.desk_panel_lock)return this.entity.assumed_state?D`${this._actionButton(this.entity,"\\u274C","turn_off")} ${this._actionButton(this.entity,"\\u2714\\uFE0F","turn_on")}`:this._switch(this.entity);
  // The table reuses one renderer for every row; bind this row's entity now.
  const entity=this.entity,state=entity.state;
  const off=state==="OFF",on=state==="ON";
  const actioner=this.actioner,send=action=>actioner?.restAction(entity,action);
  return D`<div role="group" aria-label="${this.entity.name}" style="display:flex;align-items:center;gap:.7em">
    <button type="button" aria-label="Turn off" aria-pressed="${off}" title="Turn off"
      style="color:${off?'#ef5350':'#9e9e9e'};font:inherit;font-size:1.4em;background:none;border:0;cursor:pointer"
      @click="${()=>send('turn_off')}">✕</button>
    <span aria-live="polite" style="min-width:4.5em;text-align:center">${on?'On':off?'Off':'Unknown'}</span>
    <button type="button" aria-label="Turn on" aria-pressed="${on}" title="Turn on"
      style="color:${on?'#4caf50':'#9e9e9e'};font:inherit;font-size:1.4em;background:none;border:0;cursor:pointer"
      @click="${()=>send('turn_on')}">✓</button>
  </div>`;
}"""
# Reuse the existing SSE connection, including its authenticated transport.
# Clear confirmed colors on disconnect; only a new server event restores them.
DISCONNECT = """<!-- desk-confirmed-switch-v1 -->
<script type="module">
await customElements.whenDefined('esp-app');
const app=document.querySelector('esp-app');
await app.updateComplete;
const table=app.shadowRoot?.querySelector('esp-entity-table');
window.source?.addEventListener('error',()=>{
  if(!table)return;
  for(const entity of table.entities){
    if(entity.desk_panel_lock)entity.state='UNKNOWN';
    if(entity.desk_motion_speed||entity.desk_display_mode){entity.state='UNKNOWN';entity.value=null;}
    if(entity.desk_motion_trim||entity.desk_display_timeout){entity.state='UNKNOWN';entity.value=null;}
  }
  table.requestUpdate();
});
</script>
"""
OLD_JSON = 'set_json_icon_state_value(root, obj, "switch", value ? "ON" : "OFF", value, start_config);'
NEW_JSON = (
    OLD_JSON
    + """
  // desk-confirmed-switch-v1: ordinary ESPHome switches do not maintain has_state().
  if (desk_uart::PanelLockSwitch::is_instance(obj)) {
    root[ESPHOME_F("desk_panel_lock")] = true;
    if (!obj->has_state()) {
      root[ESPHOME_F("state")] = "UNKNOWN";
      root[ESPHOME_F("value")] = nullptr;
    }
  }"""
)
INCLUDE = '#include "esphome/components/desk_uart/desk_uart.h"\n'
OLD_SELECT_JSON = 'set_json_icon_state_value(root, obj, "select", value.c_str(), value.c_str(), start_config);'
NEW_SELECT_JSON = (
    OLD_SELECT_JSON
    + """
  if (desk_uart::MotionSpeedSelect::is_instance(obj) || desk_uart::DisplayModeSelect::is_instance(obj)) {
    root[ESPHOME_F("desk_motion_speed")] = desk_uart::MotionSpeedSelect::is_instance(obj);
    root[ESPHOME_F("desk_display_mode")] = desk_uart::DisplayModeSelect::is_instance(obj);
    if (!obj->has_state()) {
      root[ESPHOME_F("state")] = "UNKNOWN";
      root[ESPHOME_F("value")] = nullptr;
    }
  }"""
)
OLD_NUMBER_JSON = (
    'set_json_icon_state_value(root, obj, "number", state_str, val_str, start_config);'
)
NEW_NUMBER_JSON = (
    OLD_NUMBER_JSON
    + """
  if (desk_uart::MotionTrimNumber::is_instance(obj) || desk_uart::DisplayTimeoutNumber::is_instance(obj)) {
    root[ESPHOME_F("desk_motion_trim")] = desk_uart::MotionTrimNumber::is_instance(obj);
    root[ESPHOME_F("desk_display_timeout")] = desk_uart::DisplayTimeoutNumber::is_instance(obj);
    if (!obj->has_state()) {
      root[ESPHOME_F("state")] = "UNKNOWN";
      root[ESPHOME_F("value")] = nullptr;
    }
  }"""
)


def replace_once(text, old, new):
    """Replace one expected web-source anchor, rejecting missing or repeated matches."""
    if text.count(old) != 1:
        raise ValueError(
            "Unsupported ESPHome web asset; expected exactly one confirmed-switch anchor"
        )
    return text.replace(old, new, 1)


def decorate_header(source):
    """Add confirmed desk controls to a gzip header, rejecting inconsistent patches."""
    pattern = r"(#ifdef USE_WEBSERVER_GZIP\s+constexpr uint8_t INDEX_GZ\[\] PROGMEM = \{)([^}]+)(\};)"
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise ValueError("Unsupported ESPHome gzip header")
    m = matches[0]
    if re.sub(r"0x[0-9a-fA-F]{2}|[,\s]", "", m[2]):
        raise ValueError("Unsupported ESPHome index bytes")
    html = gzip.decompress(
        bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m[2]))
    ).decode()
    if MARKER in html:
        if RENDER not in html or DISCONNECT not in html:
            raise ValueError("Inconsistent confirmed-switch decoration")
    else:
        html = replace_once(html, OLD_RENDER, RENDER)
        html = replace_once(html, "</body>", DISCONNECT + "</body>")
    if SPEED_MARKER in html:
        if SELECT not in html:
            raise ValueError("Inconsistent confirmed-speed decoration")
    else:
        html = replace_once(html, OLD_SELECT, SELECT)
        html = replace_once(html, "</body>", SPEED_MARKER + "</body>")
    if TRIM_MARKER in html:
        if NUMBER not in html:
            raise ValueError("Inconsistent confirmed-trim decoration")
    else:
        html = replace_once(html, OLD_NUMBER, NUMBER)
        html = replace_once(html, "</body>", TRIM_MARKER + "</body>")
    data = gzip.compress(html.encode(), compresslevel=9, mtime=0)
    lines = [
        "    " + ", ".join(f"0x{x:02x}" for x in data[i : i + 16]) + ","
        for i in range(0, len(data), 16)
    ]
    return source[: m.start(2)] + "\n" + "\n".join(lines) + "\n" + source[m.end(2) :]


def decorate_server(source):
    """Add desk-specific JSON fields without duplicating existing decorations."""
    if NEW_JSON in source:
        if INCLUDE not in source:
            raise ValueError("Missing panel lock type declaration")
    else:
        source = INCLUDE + replace_once(source, OLD_JSON, NEW_JSON)
    if NEW_SELECT_JSON not in source:
        source = replace_once(source, OLD_SELECT_JSON, NEW_SELECT_JSON)
    if NEW_NUMBER_JSON not in source:
        source = replace_once(source, OLD_NUMBER_JSON, NEW_NUMBER_JSON)
    return source
