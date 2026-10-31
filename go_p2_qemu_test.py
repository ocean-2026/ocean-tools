#!/usr/bin/env python3
"""go_p2_qemu_test.py - go.c v1.2 P2 fixes QEMU acceptance.
Injects ALL CE apps (go.ce v1.2 + cc.ce + node.ce + others) + test scripts.
Runs A1..A6. Screenshots to cc_shots/go_p2_*.{ppm,png}.
"""
import socket, time, subprocess, re, struct, os, sys

OUT   = '/home/user/Doubao/chats/38448438262014978/catos-qemu'
ISO   = OUT + '/Catos_Better_better/Catos_Better_better/catos.iso'
RAW   = '/tmp/disk-raw.img'
QEMU  = '/home/user/Doubao/chats/38444715132904962/_qemu/pkg3/usr/bin/qemu-system-i386'
QEMU_LD = 'LD_LIBRARY_PATH=/home/user/Doubao/chats/38444715132904962/_qemu/libs/all:/home/user/Doubao/chats/38444715132904962/_qemu/libs/x/usr/lib'
QEMU_L  = '/home/user/Doubao/chats/38444715132904962/_qemu/libs/x/usr/share/qemu'
INJ   = OUT + '/ocean_tools/inject_any.py'
BIN   = OUT + '/Catos_Better_better/Catos_Better_better/extapp/bin'
SHOTS = OUT + '/cc_shots'
os.makedirs(SHOTS, exist_ok=True)

def reset_env():
    subprocess.run(['pkill', '-9', '-x', 'qemu-system-i38'], stderr=subprocess.DEVNULL)
    time.sleep(2)
    r = subprocess.run(['bash','-c','ss -tln | grep -c :4444'], capture_output=True, text=True)
    if r.stdout.strip() != '0':
        print('FATAL: port 4444 busy'); sys.exit(1)
    print('env clean: no qemu, port 4444 free')

def make_disk():
    if os.path.exists(RAW): os.remove(RAW)
    subprocess.run(['truncate','-s','4G',RAW], check=True)
    print('disk: 4G sparse at', RAW)

def boot():
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], stderr=subprocess.DEVNULL)
    time.sleep(2)
    cmd = ('%s %s -L %s -m 128 -no-reboot -no-shutdown -net none '
           '-drive file=%s,format=raw -cdrom %s -boot order=d -display none '
           '-monitor tcp:127.0.0.1:4444,server,nowait' % (QEMU_LD, QEMU, QEMU_L, RAW, ISO))
    subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return connect(retries=10)

def connect(retries=10):
    for i in range(retries):
        try:
            return socket.create_connection(('127.0.0.1',4444),timeout=5)
        except Exception:
            time.sleep(4)
    raise RuntimeError('no monitor')

s = None
def send(c, w=0.6):
    s.sendall(c.encode()+b'\n'); time.sleep(w)
    d=b''
    while True:
        try:
            s.settimeout(0.3); x=s.recv(65536)
            if not x: break
            d+=x
        except: break
    return d.decode('latin1','replace')

def read_screen():
    txt=''
    for off in range(0,4000,1600):
        raw=send('xp /%d 0xb%04x'%(1600//4,0x8000+off),0.5)
        for line in raw.splitlines():
            m=re.search(r':\s+(.*)', line)
            if m:
                for wd in m.group(1).split():
                    if re.fullmatch(r'0x[0-9a-f]{8}', wd):
                        v=int(wd,16)
                        txt+=chr(v&0xFF)+chr((v>>16)&0xFF)
    out=[]
    for r in range(25):
        row=txt[r*80:(r+1)*80].rstrip(' ')
        if row: out.append(row)
    return '\n'.join(out)

def shot(name):
    time.sleep(1.2)
    try:
        send('screendump %s/%s.ppm' % (SHOTS, name), 1.2)
        time.sleep(1.2)
    except OSError:
        print('  (shot %s: monitor closed)' % name); return None
    ppm = '%s/%s.ppm' % (SHOTS, name)
    if os.path.exists(ppm):
        # convert to png
        try:
            subprocess.run(['python3','-c',
                'from PIL import Image; im=Image.open("%s"); im.save("%s/%s.png")' % (ppm, SHOTS, name)],
                capture_output=True, timeout=10)
        except Exception:
            pass
        return ppm
    return None

def type_text(t, spd=0.10):
    for ch in t:
        m={' ':'spc','/':'slash','.':'dot','_':'shift-minus','-':'minus',
           ':':'shift-semicolon',',':'comma'}
        if ch.isupper():
            send('sendkey shift-%s'%ch.lower(), spd)
        elif ch in m:
            send('sendkey %s'%m[ch], spd)
        else:
            send('sendkey %s'%ch, spd)

def wait_prompt(timeout=25):
    scr=''
    for i in range(int(timeout/2)):
        scr = read_screen()
        if 'catos:' in scr: return scr
        send('sendkey ret', 1.0)
        time.sleep(1)
    return scr

def send_cmd(cmd, settle=8):
    type_text(cmd); send('sendkey ret', 0.5); time.sleep(settle)
    return read_screen()

def step_fmt():
    print('== format ==')
    type_text('format'); send('sendkey ret', 0.5); time.sleep(2)
    scr=''
    for i in range(12):
        scr=read_screen()
        if 'Proceed' in scr: break
        time.sleep(1)
    if 'Proceed' in scr:
        type_text('y'); send('sendkey ret', 0.5); time.sleep(12)
    return read_screen()

def step_inject():
    print('== inject (all CE apps + scripts) ==')
    jobs = [
        (BIN+'/go.ce',       'GO.CE'),
        (BIN+'/cc.ce',       'CC.CE'),
        (BIN+'/node.ce',     'NODE.CE'),
        (OUT+'/DEMO.GO',     'DEMO.GO'),
        (OUT+'/TEST.GO',    'TEST.GO'),
        (OUT+'/MINI_GO.GO',  'MINI_GO.GO'),
        (OUT+'/MINI_IN.GO',  'MINI_IN.GO'),
        (OUT+'/SYN.GO',      'SYN.GO'),
        (OUT+'/BIG.GO',      'BIG.GO'),
    ]
    for src, name in jobs:
        if not os.path.exists(src):
            print('  inject %-12s MISSING (no file: %s)' % (name, src)); return False
        r = subprocess.run(['python3', INJ, RAW, src, name], capture_output=True, text=True)
        ok = (r.returncode == 0) and ('match=True' in r.stdout)
        tail = r.stdout.strip().splitlines()[-1] if r.stdout else (r.stderr.strip()[-120:] if r.stderr else 'no output')
        print('  inject %-12s %-4s %s' % (name, 'OK' if ok else 'FAIL', tail))
        if not ok: return False
    return True

def check(scr, *needles, label):
    miss = [nd for nd in needles if nd not in scr]
    if not miss:
        print('PASS %s: found %s' % (label, needles)); return True
    print('FAIL %s: missing %s\n---screen---\n%s\n---end---' % (label, miss, scr[-1500:])); return False

results = []
s = None
try:
    reset_env()
    make_disk()
    s = boot()
    scr = wait_prompt()
    step_fmt()
    s.close(); time.sleep(2)
    subprocess.run(['pkill','-9','-x','qemu-system-i38']); time.sleep(2)

    if not step_inject():
        print('FATAL: injection failed'); sys.exit(1)

    # --- A1: no-arg demo ---
    s = boot()
    scr = wait_prompt()
    shot('go_p2_r1_shell')
    scr = send_cmd('run /bin/go.ce', 8)
    shot('go_p2_r2_demo')
    results.append(('A1 no-arg demo', check(scr, 'fib(10)= 55', 'demo done', 'exit ok', label='A1')))

    # --- A2: TEST.GO all pass ---
    scr = send_cmd('run /bin/go.ce /BIN/TEST.GO', 12)
    shot('go_p2_r3_test')
    results.append(('A2 TEST.GO ALL PASS', check(scr, 'ALL PASS', label='A2')))

    # --- A3: reboot regression ---
    s.close(); time.sleep(2)
    subprocess.run(['pkill','-9','-x','qemu-system-i38']); time.sleep(2)
    s = boot()
    scr = wait_prompt()
    shot('go_p2_r4_reboot_shell')
    scr = send_cmd('run /bin/go.ce /BIN/TEST.GO', 12)
    shot('go_p2_r5_test_after_reboot')
    results.append(('A3 TEST.GO after reboot', check(scr, 'ALL PASS', label='A3')))

    # --- A4: self-host ---
    scr = send_cmd('run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO', 8)
    shot('go_p2_r6_selfhost')
    results.append(('A4 self-host output 7', check(scr, '7', 'exit ok', label='A4')))

    # --- A5: SYN.GO line 3 ---
    scr = send_cmd('run /bin/go.ce /BIN/SYN.GO', 6)
    shot('go_p2_r7_syntaxline')
    results.append(('A5 SYN.GO line 3', check(scr, 'syntax error at line 3', label='A5')))

    # --- A6: G5 big file readfile chunking ---
    scr = send_cmd('run /bin/go.ce /BIN/BIG.GO /BIN/BIG.GO', 8)
    shot('go_p2_r8_bigread')
    results.append(('A6 G5 chunked readfile (>16K)', check(scr, '18204', 'big-ok', label='A6')))

    # --- iso integrity: cc.ce present (just run it, expect it starts without "no such file") ---
    scr = send_cmd('run /bin/cc.ce', 5)
    shot('go_p2_r9_cc_present')
    cc_ok = ('no such' not in scr.lower()) and ('cannot' not in scr.lower())
    print('%s cc.ce present and starts' % ('PASS' if cc_ok else 'FAIL'))
    print('  screen tail: %s' % scr.splitlines()[-3:] if scr else '(empty)')
    results.append(('iso integrity: cc.ce present', cc_ok))

    print('\n=== SUMMARY ===')
    for name, ok in results:
        print('  [%s] %s' % ('PASS' if ok else 'FAIL', name))
finally:
    if s:
        try: s.close()
        except: pass
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], stderr=subprocess.DEVNULL)
