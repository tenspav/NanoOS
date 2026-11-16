#!/usr/bin/env python3
"""Host-only checkpoint packet/actual loader export tests; no device access."""
from pathlib import Path
import subprocess
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def main():
 build=HERE/'build';build.mkdir(exist_ok=True)
 flags=['cc','-std=c99','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-int-to-pointer-cast','-Wno-int-to-void-pointer-cast','-fsanitize=address,undefined','-I'+str(ROOT/'NanoEmu/probe/chainloader'),'-I'+str(ROOT/'NanoOS/src')]
 for name in ('checkpoint_report','checkpoint_export'):
  exe=build/('test_'+name);subprocess.run(flags+[str(HERE/('test_'+name+'.c')),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
