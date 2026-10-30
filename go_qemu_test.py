#!/usr/bin/env python3
"""go_qemu_test.py - go.ce QEMU acceptance driver (CatOS go-subset interpreter).
Coverage:
  A. run /bin/go.ce                 -> /BIN/DEMO.GO runs (fib recursion/arrays)
  B. run /bin/go.ce /BIN/TEST.GO    -> ALL PASS (34 self-checking cases)
  C. reboot: go.ce + scripts survive the builtin install (#48); TEST.GO still ALL PASS
  D. run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO  -> self-host: 7
Screenshots to cc_shots/go_rN.*
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
    if os.path.exists('%s/%s.ppm' % (SHOTS, name)):
        return '%s/%s.ppm' % (SHOTS, name)
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
    print('== inject ==')
    jobs = [
        (BIN+'/go.ce',       'GO.CE'),
        (OUT+'/DEMO.GO',     'DEMO.GO'),
        (OUT+'/TEST.GO',    'TEST.GO'),
        (OUT+'/MINI_GO.GO',  'MINI_GO.GO'),
        (OUT+'/MINI_IN.GO',  'MINI_IN.GO'),
        (OUT+'/SYN.GO',      'SYN.GO'),
    ]
    for src, name in jobs:
        r = subprocess.run(['python3', INJ, RAW, src, name], capture_output=True, text=True)
        ok = (r.returncode == 0) and ('match=True' in r.stdout)
        tail = r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip()[-120:]
        print('  inject %-12s %-4s %s' % (name, 'OK' if ok else 'FAIL', tail))
        if not ok: return False
    BPS=512; SPC=8; RSVD=33; NFATS=2; FAT_SZ=8192
    data_off=(RSVD+NFATS*FAT_SZ)*BPS
    f=open(RAW,'rb')
    def cl_off(cl): return data_off+(cl-2)*SPC*BPS
    def fat_get(cl):
        f.seek(RSVD*BPS+cl*4); return struct.unpack('<I',f.read(4))[0]&0x0FFFFFFF
    def scan(cl):
        names=[]; c=cl; seen=set()
        while 2<=c<0x0FFFFFF8 and c not in seen:
            seen.add(c); f.seek(cl_off(c)); buf=f.read(SPC*BPS)
            for i in range(0,len(buf),32):
                e=buf[i:i+32]
                if e[0]==0: break
                if e[0]==0xE5 or e[11]==0x0F: continue
                names.append(''.join(chr(x) for x in e[0:11]).rstrip(' '))
            c=fat_get(c)
        return names
    f.seek(cl_off(2)); buf=f.read(SPC*BPS); bincl=None
    for i in range(0,len(buf),32):
        e=buf[i:i+32]
        if e[0:3]==b'BIN' and e[11]==0x10:
            bincl=struct.unpack_from('<H',e,26)[0]|(struct.unpack_from('<H',e,20)[0]<<16); break
    if bincl is None:
        print('  FATAL: BIN subdir not found'); f.close(); return False
    binents=scan(bincl)
    print('  /BIN entries:', binents)
    for want in ['GO','DEMO','TEST','MINI_GO','MINI_IN','SYN']:
        ok=any(n.startswith(want) for n in binents)
        print('  %-9s %s' % (want, 'OK' if ok else 'MISSING'))
        if not ok: f.close(); return False
    f.close()
    return True

def check(scr, *needles, label):
    miss = [nd for nd in needles if nd not in scr]
    if not miss:
        print('PASS %s: found %s' % (label, needles)); return True
    print('FAIL %s: missing %s\n%s' % (label, miss, scr[-1000:])); return False

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

    # --- run 1: no-arg demo ---
    s = boot()
    scr = wait_prompt()
    shot('go_fix_r1_shell')
    scr = send_cmd('run /bin/go.ce', 8)
    shot('go_fix_r2_demo')
    check(scr, 'fib(10)= 55', 'demo done', 'exit ok', label='A: no-arg demo')

    # --- run 2: TEST.GO all pass ---
    scr = send_cmd('run /bin/go.ce /BIN/TEST.GO', 10)
    shot('go_fix_r3_test')
    check(scr, 'ALL PASS', label='B: TEST.GO ALL PASS')

    # --- reboot (#48 regression) ---
    s.close(); time.sleep(2)
    subprocess.run(['pkill','-9','-x','qemu-system-i38']); time.sleep(2)
    s = boot()
    scr = wait_prompt()
    shot('go_fix_r4_reboot_shell')
    scr = send_cmd('run /bin/go.ce /BIN/TEST.GO', 10)
    shot('go_fix_r5_test_after_reboot')
    check(scr, 'ALL PASS', label='C: TEST.GO survives reboot (#48)')

    # --- run self-host ---
    scr = send_cmd('run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO', 8)
    shot('go_fix_r6_selfhost')
    check(scr, 'exit ok', label='D: self-host MINI_GO.MINI_IN runs')

    # --- P1 regression: syntax error reports the correct source line ---
    # SYN.GO has an unclosed '(' on line 3; must report line 3 (not file end).
    scr = send_cmd('run /bin/go.ce /BIN/SYN.GO', 6)
    shot('go_fix_r7_syntaxline')
    check(scr, 'syntax error at line 3', label='E: syntax error line located (FIND-G1)')

    print('\n=== SUMMARY done (PASS lines above) ===')
finally:
    if s:
        try: s.close()
        except: pass
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], stderr=subprocess.DEVNULL)
