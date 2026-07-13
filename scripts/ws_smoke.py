import socket, struct, time, hashlib, base64, os, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 19002

s = socket.socket()
s.settimeout(5)
s.connect(('127.0.0.1', port))

key = base64.b64encode(os.urandom(16)).decode()
req = (
    'GET / HTTP/1.1\r\n'
    'Host: 127.0.0.1:%d\r\n' % port +
    'Upgrade: websocket\r\n'
    'Connection: Upgrade\r\n'
    'Sec-WebSocket-Key: %s\r\n' % key +
    'Sec-WebSocket-Version: 13\r\n'
    '\r\n'
)
s.sendall(req.encode())
resp = s.recv(4096)
print('Handshake resp:', repr(resp[:60]))
assert b'101' in resp, 'handshake failed'
print('Handshake OK')

time.sleep(0.5)

# Send WS binary frame with PING protocol msg
payload = struct.pack('>IH', 6, 0x0001)
mask = os.urandom(4)
masked = bytes(b ^ mask[i%4] for i,b in enumerate(payload))
frame = bytes([0x82, 0x80 | len(payload)]) + mask + masked
print('Sending frame:', frame.hex())
s.sendall(frame)
print('Sent WS PING frame')

time.sleep(1)
try:
    data = s.recv(4096)
    print('Recv', len(data), 'bytes:', data.hex())
except Exception as e:
    print('recv error:', e)
s.close()
