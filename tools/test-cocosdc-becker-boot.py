#!/usr/bin/env python3
"""Check CoCoSDC boot does not contaminate Becker, then check status selection."""
import argparse
import socket
import subprocess
import tempfile
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xroar',required=True)
parser.add_argument('--rompath',required=True)
parser.add_argument('--sdc-rom',required=True)
args=parser.parse_args()
workspace=tempfile.TemporaryDirectory(prefix='cocosdc-becker-')
root=Path(workspace.name)
(root/'SD').mkdir()
for name,text,expected in [('boot','',b''),('select','A=PEEK(&HFF41):POKE&HFF42,226:POKE&HFF42,0\r',bytes([226,0]))]:
 with socket.socket() as s:
  s.bind(('127.0.0.1',0));s.listen();s.settimeout(10)
  cmd=[args.xroar,'-ui','null','-ao','null','-machine','coco3','-rompath',args.rompath,'-cart','cocosdc','-cart-rom',args.sdc_rom,'-sdc-root',str(root/'SD'),'-cart-becker','-becker-ip','127.0.0.1','-becker-port',str(s.getsockname()[1]),'-timeout','5']
  if text:cmd+=['-type',text]
  with open(root/(name+'.log'),'w') as log:
   p=subprocess.Popen(cmd,stdout=log,stderr=log)
   c,_=s.accept();c.settimeout(10); data=b''
   with c:
    while True:
     d=c.recv(4096)
     if not d:break
     data+=d
   p.wait(timeout=10)
  print(name, data.hex(' '),flush=True); assert data==expected,(name,data,expected)
print('BECKER_BOOT=PASS')

workspace.cleanup()
