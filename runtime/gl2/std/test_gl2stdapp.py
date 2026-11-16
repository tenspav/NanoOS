from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as temp:
 d=Path(temp);objs=[]
 for src in ['gl2api.c','gl2rt.c','gl2vsc.c','gl2tex.c']:
  o=d/(src+'.o');subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(root),'-include',str(root/'std/gl2prefix.h'),'-c',str(root/src),'-o',str(o)],check=True);objs.append(str(o))
 for src in ['gl2std.c','gl2std_host.c','app_multipass.c','test_gl2stdapp.c']:
  o=d/(src+'.o');subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(root),'-I'+str(root/'std'),'-c',str(root/'std'/src),'-o',str(o)],check=True);objs.append(str(o))
 exe=d/'app';subprocess.run(['cc',*objs,'-lm','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
