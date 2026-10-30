"""Decorate only the generated local ESPHome v3 index; never modify ESPHome itself."""
import gzip
import os
import tempfile
from pathlib import Path
import re
import warnings
import zlib

MARKER = '<!-- desk-maintenance-layout-v2 -->'
PAYLOAD = '''<!-- desk-maintenance-layout-v2 -->
<nav id="desk-firmware-nav" aria-label="Firmware tools" style="margin:.5rem 1rem;font:inherit">
<a href="/desk-firmware" style="display:inline-block;padding:.5rem 0;color:#03a9f4;text-underline-offset:.2em">Open firmware maintenance</a>
</nav>
<script type="module">
// Move the ordinary link into the optional Firmware group. No device commands.
const navigation=document.getElementById('desk-firmware-nav');
customElements.whenDefined('esp-app').then(async()=>{
  const app=document.querySelector('esp-app');
  await app.updateComplete;
  await customElements.whenDefined('esp-entity-table');
  const appRoot=app.shadowRoot;
  if(!appRoot)return;
  let watched;
  const options={childList:true,subtree:true};
  const place=()=>{
    const root=appRoot.querySelector('esp-entity-table')?.shadowRoot;
    if(root!==watched){
      observer.disconnect();observer.observe(appRoot,options);
      if(root)observer.observe(root,options);
      watched=root;
    }
    const heading=root&&[...root.querySelectorAll('.tab-header')].find(node=>node.textContent.trim()==='Firmware');
    const group=heading?.nextElementSibling;
    if(group?.classList.contains('tab-container')){
      if(group.lastChild!==navigation)group.append(navigation);
    }else if(navigation.parentNode!==document.body){document.body.append(navigation);}
  };
  const observer=new MutationObserver(place);
  observer.observe(appRoot,options);
  place();
}).catch(()=>{if(!navigation.isConnected)document.body.append(navigation);});
</script>
'''


def decorate_header(source):
    """Return a deterministic gzip-only replacement, or reject unknown formats."""
    # The upstream header also contains a Brotli branch. Leave it byte-identical.
    pattern = r'(#ifdef USE_WEBSERVER_GZIP\s+constexpr uint8_t INDEX_GZ\[\] PROGMEM = \{)([^}]+)(\};)'
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise ValueError('unrecognized ESPHome v3 gzip array')
    match = matches[0]
    array = match.group(2)
    if re.sub(r'0x[0-9a-fA-F]{2}|[,\s]', '', array):
        raise ValueError('unrecognized index byte syntax')
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', array))
    html = gzip.decompress(data).decode('utf-8')
    if MARKER in html:
        return source
    if '<!-- desk-maintenance-layout-' in html:
        raise ValueError('older navigation addition; regenerate the upstream index first')
    if html.count('<esp-app>') != 1 or html.count('</body>') != 1:
        raise ValueError('unrecognized ESPHome v3 HTML anchors')
    html = html.replace('</body>', PAYLOAD + '</body>')
    compressed = gzip.compress(html.encode(), compresslevel=9, mtime=0)
    lines = ['    ' + ', '.join(f'0x{x:02x}' for x in compressed[i:i+16]) + ','
             for i in range(0, len(compressed), 16)]
    return source[:match.start(2)] + '\n' + '\n'.join(lines) + '\n' + source[match.end(2):]


def patch_file(path):
    temporary = None
    try:
        source = path.read_text()
        result = decorate_header(source)
        if source != result:
            with tempfile.NamedTemporaryFile(mode='w', dir=path.parent,
                                             prefix=path.name + '.', delete=False) as output:
                temporary = Path(output.name)
                output.write(result)
            os.replace(temporary, path)
        return True
    except (OSError, ValueError, EOFError, UnicodeError, zlib.error) as error:
        warnings.warn(f'Desk page layout skipped: {error}. Open /desk-firmware directly.')
        return False
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if 'Import' in globals():  # PlatformIO pre-build hook; importable by host tests.
    Import('env')
    patch_file(Path(env.subst('$PROJECT_DIR')) / 'src/esphome/components/web_server/server_index_v3.h')
