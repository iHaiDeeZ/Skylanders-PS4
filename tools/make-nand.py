"""Creates the NAND files the Skylanders runtime's IOS serves, for a USA console:
  shared2/sys/SYSCONF                        system settings (Dolphin's SysConf.cpp defaults)
  title/00000001/00000002/data/setting.txt   region/video (Dolphin's SettingsHandler, encrypted)
  title/00010000/53535045/data/              the game's save folder (empty)
  tmp/
Usage: python3 make-nand.py <nand folder>"""
import os, struct, sys

root = sys.argv[1]

# ---- SYSCONF ----
BIG, SMALL, BYTE, SHORT, LONG, LONGLONG, BOOL = 1, 2, 3, 4, 5, 6, 7
nick = bytearray(22)
for i, c in enumerate('dolphin'):
    nick[2 * i + 1] = ord(c)
nick[21] = len('dolphin')
sadr = bytearray(0x1007 + 1)
sadr[0] = 0x6C  # USA
pc = bytearray(0x49 + 1)
pc[1], pc[2] = 0x04, 0x14
# BT.DINF: the paired Wii Remotes (Dolphin's ConfPads: count, 10 registered, 5 active, 1 more; each
# a 6-byte address in reverse order and a 0x40-byte name). One remote: the runtime's emulated one
# (bt.cpp: 11:02:19:79:00:00).
dinf = bytearray(0x461)
dinf[0] = 1
remote = bytes(reversed(bytes([0x11, 0x02, 0x19, 0x79, 0x00, 0x00])))
name = b'Nintendo RVL-CNT-01'
for record in (1, 1 + 10 * 0x46):  # registered[0], active[0]
    dinf[record:record + 6] = remote
    dinf[record + 6:record + 6 + len(name)] = name
entries = [
    (BIG, 'BT.DINF', bytes(dinf)),
    (BIG, 'BT.CDIF', bytes(0x204 + 1)),
    (LONG, 'BT.SENS', bytes([0, 0, 0, 3])),
    (BYTE, 'BT.BAR', bytes([1])),
    (BYTE, 'BT.SPKV', bytes([0x58])),
    (BYTE, 'BT.MOT', bytes([1])),
    (SMALL, 'IPL.NIK', bytes(nick)),
    (BYTE, 'IPL.LNG', bytes([1])),        # English
    (BIG, 'IPL.SADR', bytes(sadr)),
    (SMALL, 'IPL.PC', bytes(pc)),
    (LONG, 'IPL.CB', bytes(4)),
    (BYTE, 'IPL.AR', bytes([1])),         # 16:9
    (BYTE, 'IPL.SSV', bytes([1])),
    (BOOL, 'IPL.CD', bytes([0])),
    (BOOL, 'IPL.CD2', bytes([0])),
    (BOOL, 'IPL.EULA', bytes([1])),
    (BYTE, 'IPL.UPT', bytes([2])),
    (BYTE, 'IPL.PGS', bytes([0])),        # progressive scan off
    (BYTE, 'IPL.E60', bytes([1])),
    (BYTE, 'IPL.DH', bytes([0])),
    (LONG, 'IPL.INC', bytes([0, 0, 0, 8])),
    (LONG, 'IPL.FRC', bytes([0, 0, 0, 0x28])),
    (SMALL, 'IPL.IDL', bytes([0, 1])),
    (LONG, 'NET.WCFG', bytes([0, 0, 0, 1])),
    (LONG, 'NET.CTPC', bytes(4)),
    (BYTE, 'WWW.RST', bytes([0])),
    (BOOL, 'MPLS.MOVIE', bytes([1])),
]
body = bytearray()
offsets = []
begin = 4 + 2 + 2 * (len(entries) + 1)
for kind, name, data in entries:
    offsets.append(begin + len(body))
    body.append((kind << 5) | (len(name) - 1))
    body += name.encode()
    if kind == BIG:
        body += struct.pack('>H', len(data) - 1)
    elif kind == SMALL:
        body.append(len(data) - 1)
    body += data
sysconf = bytearray(b'SCv0' + struct.pack('>H', len(entries)))
for o in offsets + [begin + len(body)]:
    sysconf += struct.pack('>H', o)
sysconf += body
assert len(sysconf) <= 0x4000 - 4
sysconf += bytes(0x4000 - len(sysconf))
sysconf[-4:] = b'SCed'

# ---- setting.txt ----
text = ('AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\nSERNO=000000000\r\n'
        'VIDEO=NTSC\r\nGAME=US\r\n').encode()
setting = bytearray(0x100)
key = 0x73B5DBFA
for i, b in enumerate(text):
    setting[i] = b ^ (key & 0xFF)
    key = ((key << 1) | (key >> 31)) & 0xFFFFFFFF


def write(path, data):
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, 'wb') as f:
        f.write(data)
    print(f'{path}: {len(data)} bytes')


write('shared2/sys/SYSCONF', bytes(sysconf))
write('title/00000001/00000002/data/setting.txt', bytes(setting))
os.makedirs(os.path.join(root, 'title/00010000/53535045/data'), exist_ok=True)
os.makedirs(os.path.join(root, 'tmp'), exist_ok=True)
