"""Exercise the DLNA renderer with request shapes seen from Android controllers.

Covers discovery (startup NOTIFY burst, case-insensitive M-SEARCH), the DLNA
device description, HEAD and absolute-URI requests, lenient SOAP (missing
namespace, missing InstanceID, Speed "1.0", ABS_TIME and plain-second seeks,
Channel omitted, boolean words), chunked bodies and Expect: 100-continue.
"""
import functools
import http.client
import http.server
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

exe, fixture = sys.argv[1:]


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(
    ('0.0.0.0', 0), functools.partial(Handler, directory=str(Path(fixture).parent)))
threading.Thread(target=server.serve_forever, daemon=True).start()

# Listen for the startup NOTIFY burst before the renderer starts. Sharing port
# 1900 may be refused by the host; the check is skipped only in that case.
notify_sock = None
try:
    notify_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    notify_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        notify_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    notify_sock.bind(('', 1900))
    notify_sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                           struct.pack('4s4s', socket.inet_aton('239.255.255.250'),
                                       socket.inet_aton('0.0.0.0')))
    notify_sock.settimeout(0.5)
except OSError:
    if notify_sock:
        notify_sock.close()
    notify_sock = None

env = dict(os.environ, SDL_AUDIODRIVER='dummy')
process = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True, env=env)
checked = []
try:
    location = process.stdout.readline().strip()
    assert location.startswith('http://'), 'renderer starts on LAN'
    host, port = location.split('//')[1].split('/')[0].split(':')
    port = int(port)

    if notify_sock:
        deadline = time.monotonic() + 4
        alive = 0
        while time.monotonic() < deadline and alive < 2:
            try:
                data = notify_sock.recv(8192).decode(errors='replace')
            except socket.timeout:
                continue
            if (location in data and 'ssdp:alive' in data and
                    'NT: urn:schemas-upnp-org:device:MediaRenderer:1' in data):
                assert 'max-age=1800' in data, data
                alive += 1
        notify_sock.close()
        assert alive >= 2, 'startup NOTIFY is repeated'
        checked.append('NOTIFY burst')

    def request(method, path, body=None, headers=None, chunked=False):
        c = http.client.HTTPConnection(host, port, timeout=4)
        c.request(method, path, body=body, headers=headers or {}, encode_chunked=chunked)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, r.headers, data

    code, headers, data = request('GET', '/device.xml')
    assert code == 200 and headers['Server'] and headers['Date']
    root = ET.fromstring(data)
    ns = {'d': 'urn:schemas-upnp-org:device-1-0', 'dlna': 'urn:schemas-dlna-org:device-1-0'}
    assert root.find('.//dlna:X_DLNADOC', ns).text == 'DMR-1.50'
    assert root.find('d:URLBase', ns).text == location.rsplit('/', 1)[0] + '/'
    for field in ('modelNumber', 'serialNumber', 'modelDescription'):
        assert root.find('.//d:' + field, ns).text
    checked.append('DLNA description')

    code, headers, data = request('HEAD', '/device.xml')
    assert code == 200 and data == b'' and int(headers['Content-Length']) > 0
    code, _, data = request('GET', f'http://{host}:{port}/device.xml?x=1')
    assert code == 200 and b'MediaRenderer' in data
    checked.append('HEAD and absolute URI')

    code, _, data = request('GET', '/AVTransport/scpd.xml')
    scpd = ET.fromstring(data)
    sns = {'s': 'urn:schemas-upnp-org:service-1-0'}
    modes = [v.text for v in scpd.findall(
        ".//s:stateVariable[s:name='A_ARG_TYPE_SeekMode']//s:allowedValue", sns)]
    assert 'ABS_TIME' in modes and 'REL_TIME' in modes, modes
    checked.append('SCPD value lists')

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((host, 0))
        sock.settimeout(3)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(host))
        sock.sendto(b'M-Search * HTTP/1.1\r\nhost: 239.255.255.250:1900\r\n'
                    b'man: "ssdp:discover"\r\nmx: 1\r\nst: SSDP:ALL\r\n\r\n',
                    ('239.255.255.250', 1900))
        deadline = time.monotonic() + 3
        seen = set()
        wanted = {'urn:schemas-upnp-org:device:MediaRenderer:1',
                  'urn:schemas-upnp-org:service:AVTransport:1'}
        while time.monotonic() < deadline and not wanted <= seen:
            reply = sock.recv(8192).decode(errors='replace')
            if location in reply:
                assert 'DATE:' in reply and 'EXT:' in reply, reply
                for line in reply.split('\r\n'):
                    if line.startswith('ST: '):
                        seen.add(line[4:])
        assert 'urn:schemas-upnp-org:device:MediaRenderer:1' in seen, seen
        assert 'urn:schemas-upnp-org:service:AVTransport:1' in seen, seen
    checked.append('case-insensitive M-SEARCH')

    def envelope(action, inner, urn=None):
        action_open = f'<u:{action} xmlns:u="{urn}">' if urn else f'<{action}>'
        action_close = f'</u:{action}>' if urn else f'</{action}>'
        return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                '<s:Body>' + action_open + inner + action_close + '</s:Body></s:Envelope>').encode()

    def soap(action, inner='', service='AVTransport', namespaced=True, expected=200, chunked=False):
        urn = 'urn:schemas-upnp-org:service:' + service + ':1'
        body = envelope(action, inner, urn if namespaced else None)
        code, headers, data = request(
            'POST', '/' + service + '/control', body=iter([body]) if chunked else body,
            headers={'Content-Type': 'text/xml; charset="utf-8"',
                     'SOAPACTION': f'"{urn}#{action}"'}, chunked=chunked)
        assert code == expected, (action, code, data)
        if code == 200:
            assert headers['EXT'] is not None, headers
        return ET.fromstring(data)

    soap('GetTransportInfo', namespaced=False)
    soap('GetTransportInfo')  # no InstanceID at all
    soap('GetTransportInfo', '<InstanceID>0</InstanceID>', chunked=True)
    soap('GetVolume', '<InstanceID>0</InstanceID>', service='RenderingControl')
    soap('GetTransportInfo', '<InstanceID>3</InstanceID>', expected=500)
    checked.append('lenient SOAP and chunked body')

    urn = 'urn:schemas-upnp-org:service:AVTransport:1'
    body = envelope('GetPositionInfo', '<InstanceID>0</InstanceID>', urn)
    with socket.create_connection((host, port), timeout=4) as raw:
        raw.sendall(f'POST /AVTransport/control HTTP/1.1\r\nHost: {host}:{port}\r\n'
                    f'Content-Type: text/xml\r\nSOAPACTION: "{urn}#GetPositionInfo"\r\n'
                    f'Expect: 100-continue\r\nContent-Length: {len(body)}\r\n\r\n'.encode())
        interim = raw.recv(64)
        assert interim.startswith(b'HTTP/1.1 100'), interim
        raw.sendall(body)
        final = b''
        while True:
            chunk = raw.recv(8192)
            if not chunk:
                break
            final += chunk
        assert final.startswith(b'HTTP/1.1 200'), final[:80]
    checked.append('Expect: 100-continue')

    uri = f'http://{host}:{server.server_port}/{Path(fixture).name}'
    soap('SetAVTransportURI', f'<InstanceID>0</InstanceID><CurrentURI>{uri}</CurrentURI>'
                              '<CurrentURIMetaData></CurrentURIMetaData>')
    soap('Play', '<InstanceID>0</InstanceID><Speed>1.0</Speed>')
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        info = soap('GetPositionInfo', '<InstanceID>0</InstanceID>')
        if info.find('.//TrackDuration').text not in (None, '00:00:00'):
            break
        time.sleep(0.1)
    else:
        raise AssertionError('duration known')
    soap('Seek', '<InstanceID>0</InstanceID><Unit>ABS_TIME</Unit><Target>0:00:01.500</Target>')
    soap('Seek', '<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>2</Target>')
    soap('Seek', '<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>00:61</Target>', expected=500)
    soap('SetMute', '<InstanceID>0</InstanceID><DesiredMute>true</DesiredMute>', service='RenderingControl')
    mute = soap('GetMute', '<InstanceID>0</InstanceID>', service='RenderingControl')
    assert mute.find('.//CurrentMute').text == '1'
    soap('Stop', '<InstanceID>0</InstanceID>')
    checked.append('playback controls')

    process.stdin.write('quit\n')
    process.stdin.flush()
    assert process.wait(timeout=5) == 0
    print('DLNA Android compatibility: ' + ', '.join(checked) + ': passed')
finally:
    if process.poll() is None:
        process.kill()
    server.shutdown()
