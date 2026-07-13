import socket, struct, threading, time, subprocess, sys, os, signal

PORT_GW = 19011
PORT_BE = 17781

# Start mock backend (so gateway doesn't spam connection refused)
backend_sock = socket.socket()
backend_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
backend_sock.bind(('127.0.0.1', PORT_BE))
backend_sock.listen(5)
backend_sock.settimeout(10)
be_conn_holder = []
def accept_backend():
    try:
        conn, addr = backend_sock.accept()
        conn.settimeout(5)
        be_conn_holder.append(conn)
    except:
        pass
t = threading.Thread(target=accept_backend, daemon=True)
t.start()
time.sleep(0.3)

# Start gateway
proc = subprocess.Popen(
    ['./build/bin/chaos_gateway', '--port', str(PORT_GW), '--backend', f'127.0.0.1:{PORT_BE}', '--max-connections', '3', '--heartbeat-timeout', '2000', '--heartbeat-interval', '1000'],
    cwd='/home/zhongfangdao/chaos-engine',
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)
time.sleep(1.5)

# Test 1: UDP port reachable
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.settimeout(2)
udp.sendto(b'\x01\x00\x00\x00' + b'\x00'*20, ('127.0.0.1', PORT_GW))
print('UDP sent to gateway')
try:
    data, addr = udp.recvfrom(4096)
    print('UDP got response:', len(data), 'bytes from', addr)
except socket.timeout:
    print('UDP no response (expected - KCP conv may be invalid)')

# Test 2: Max connections = 3, 4th should fail
conns = []
for i in range(4):
    try:
        s = socket.socket()
        s.settimeout(2)
        s.connect(('127.0.0.1', PORT_GW))
        conns.append(s)
        print(f'Conn {i+1}: connected')
    except Exception as e:
        print(f'Conn {i+1}: FAILED ({e})')

# Test 3: Heartbeat timeout - conn 1 should be dropped after 2s
time.sleep(0.5)
# Send PING on conn 0 to keep it alive
msg = struct.pack('>IH', 6, 0x0001)
conns[0].sendall(msg)
print('Sent PING on conn 0')
try:
    pong = conns[0].recv(1024)
    print('Conn 0 PONG:', pong.hex())
except:
    print('Conn 0 no PONG')

# Wait for heartbeat timeout (2s + margin)
time.sleep(4)
# Check conn 1 (idle) is closed
try:
    conns[1].sendall(msg)
    pong = conns[1].recv(1024)
    print('Conn 1 still alive (got:', pong.hex(), ') - UNEXPECTED')
except Exception as e:
    print(f'Conn 1 closed by heartbeat timeout: {e} - EXPECTED')

# Cleanup
for s in conns:
    try: s.close()
    except: pass
proc.send_signal(signal.SIGTERM)
proc.wait(timeout=3)
backend_sock.close()
for c in be_conn_holder:
    c.close()
udp.close()
