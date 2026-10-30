"""Decorate only the generated local ESPHome v3 index; never modify ESPHome itself."""
import gzip
import os
import tempfile
from pathlib import Path
import re
import warnings
import zlib

MARKER = '<!-- desk-maintenance-layout-v1 -->'
PAYLOAD = '''<!-- desk-maintenance-layout-v1 -->
<style>
.desk-maintenance-nav{margin:1rem 8px;font:16px ui-monospace,system-ui,Helvetica,Roboto,sans-serif}.desk-maintenance-nav a{color:#03a9f4;text-underline-offset:.2em}
@media(max-width:1024px){.desk-maintenance-nav{margin:1rem 0}}
</style>
<nav class="desk-maintenance-nav" aria-label="Desk pages"><a href="/desk-firmware">Firmware maintenance</a></nav>
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
    if html.count('<esp-app>') != 1 or html.count('</body>') != 1:
        raise ValueError('unrecognized ESPHome v3 HTML anchors')
    html = html.replace('<esp-app>', PAYLOAD + '<esp-app>')
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
