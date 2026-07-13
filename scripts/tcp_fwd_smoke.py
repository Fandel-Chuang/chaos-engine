import socket, struct, threading, time, subprocess, sys, os, signal

PORT_GW = 19010
PORT_BE = 17780

# Start mock backend
backend_data = []
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
        print('Backend accepted from', addr)
        while True:
            data = conn.recv(4096)
            if not data:
                break
            backend_data.append(data)
            print('Backend got:', data.hex())
    except Exception as e:
        print('backend thread done:', e)

t = threading.Thread(target=accept_backend, daemon=True)
t.start()
time.sleep(0.3)

# Start gateway AFTER backend is ready
proc = subprocess.Popen(
    ['./build/bin/chaos_gateway', '--port', str(PORT_GW), '--backend', f'127.0.0.1:{PORT_BE}', '--max-connections', '10'],
    cwd='/home/zhongfangdao/chaos-engine',
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)
time.sleep(1.5)

# Check gateway is alive
if proc.poll() is not None:
    print('Gateway exited early!')
    print(proc.stdout.read().decode())
    sys.exit(1)

# Connect TCP client
s = socket.socket()
s.settimeout(3)
s.connect(('127.0.0.1', PORT_GW))

# Send GAME_DATA
payload = b'TEST'
msg = struct.pack('>IH', 6 + len(payload), 0x0100) + payload
s.sendall(msg)
print('Sent GAME_DATA:', msg.hex())

time.sleep(1)
print('Backend received:', [d.hex() for d in backend_data])

s.close()
proc.send_signal(signal.SIGTERM)
proc.wait(timeout=3)
backend_sock.close()
for c in be_conn_holder:
    c.close()
