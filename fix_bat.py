import os, glob

targets = (
    glob.glob(r'd:\workspace\chaos-engine\scripts\*.bat') +
    glob.glob(r'd:\workspace\chaos-engine\ci\*.bat') +
    [r'd:\workspace\chaos-engine\run_cluster.bat']
)

for f in targets:
    if not os.path.exists(f):
        continue
    with open(f, 'rb') as fp:
        raw = fp.read()
    txt = raw.decode('utf-8', errors='replace')
    if 'chcp 65001' not in txt:
        txt = txt.replace('@echo off', '@echo off\r\nchcp 65001 > nul', 1)
    # normalize to CRLF
    txt = txt.replace('\r\n', '\n').replace('\n', '\r\n')
    with open(f, 'wb') as fp:
        fp.write(txt.encode('utf-8'))
    print('fixed:', os.path.basename(f))
