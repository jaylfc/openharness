"""Source-level guarantees for the LAN transport that are properties of the code, not of one run.

These read the production sources: a psk or a bind token must never reach a log macro, only the cable
may set or forget a network, logs are never framed onto the LAN, and a factory reset forgets the network
and the bind. They are deliberately blunt; the behaviour itself is exercised by test_lan_session.py.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'
files = ['wifi_sta.c', 'wifi_cable.c', 'lan_bind.c', 'lan_wifi_msg.c', 'config_store.c', 'cable_client.c', 'cable_link.c']
src = {f: (main / f).read_text() for f in files}

def statements(text, macro):
    """Every macro(...) statement, balanced across lines, with string literals blanked out."""
    out = []
    for m in re.finditer(r'\b' + macro + r'\w*\s*\(', text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {'(': 1, ')': -1}.get(text[i], 0)
            i += 1
        stmt = re.sub(r'"(?:\\.|[^"\\])*"', '""', text[m.start():i])
        out.append(stmt)
    return out

# 1. No log macro is ever handed the psk or the token, by name.
secret = re.compile(r'\b(psk|pass|password|wcfg|s_bind|bind|token)\b')
checked = 0
for name, text in src.items():
    for stmt in statements(text, 'ESP_LOG'):
        checked += 1
        assert not secret.search(stmt), (name, stmt)
    for stmt in statements(text, 'printf') + statements(text, 'puts'):
        assert not secret.search(stmt), (name, stmt)
assert checked > 30

# 2. Only the cable may set or forget a network.
client = src['cable_client.c']
for handler in ('handle_wifi_set', 'handle_wifi_forget'):
    body = re.search(r'static void ' + handler + r'\([^)]*\)\n\{(.*?)^\}', client, re.M | re.S).group(1)
    first = body.strip().splitlines()[0]
    assert 'via != CABLE_XPORT_USB' in first and 'return' in first, handler
# wifi.* is decided before the session gate, so a LAN peer reaches the (refusing) handler, not the sockets.
assert client.index('"wifi.set"') < client.index('from_session_wire(via)) {')

# 3. Logs go to the cable only.
link = src['cable_link.c']
log_sink = re.search(r'static int log_vprintf.*?^\}', link, re.M | re.S).group(0)
assert 'send_locked(CABLE_XPORT_USB, CABLE_TYPE_LOG' in log_sink and 'TCP' not in log_sink
assert 'if (via == CABLE_XPORT_USB) cable_link_set_log_framing(true);' in client

# 4. The token compare never short-circuits on content.
eq = re.search(r'bool lan_bind_equal.*?^\}', src['lan_bind.c'], re.M | re.S).group(0)
assert not re.search(r'\b(strcmp|strncmp|memcmp)\b', eq)

# 5. Factory reset forgets the network and the bind (both live in the "lan" namespace).
store = src['config_store.c']
clear_all = re.search(r'bool config_clear_all\(void\)\n\{.*?^\}', store, re.M | re.S).group(0)
assert 'NS_LAN' in clear_all and 'nvs_erase_all' in clear_all
assert store.count('NS_LAN') >= 6 and '"bind"' in store and '"ssid"' in store and '"psk"' in store
# ...and the driver keeps no copy of its own that the reset would miss.
assert 'WIFI_STORAGE_RAM' in src['wifi_sta.c']

# 6. One client, never during a USB session, and unaccepted connections do not linger.
lan = src['wifi_cable.c']
assert 'listen(fd, 1)' in lan and 'CABLE_XPORT_USB' in lan and 'WIFI_CABLE_AUTH_MS' in lan
assert '_harness-dial' in lan and 'WIFI_CABLE_PORT' in lan and '"mac"' in lan

# 7. No hot loop against an AP that refuses: every disconnect goes through the backoff timer.
sta = src['wifi_sta.c']
disc = re.search(r'static void on_disconnected.*?^\}', sta, re.M | re.S).group(0)
assert 'schedule_retry(' in disc and 'esp_wifi_connect' not in disc
print('LAN transport: no psk or token in %d log statements, cable-only wifi.set/forget, USB-only logs, constant-time compare, factory reset, backoff PASS' % checked)
