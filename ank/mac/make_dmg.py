#!/usr/bin/env python3
"""Make Ank.dmg on Linux: a Rock Ridge disk image that macOS mounts by double-click.
Usage: make_dmg.py <folder> <out.dmg>   (needs: pip install pycdlib)
Keeps file permissions (so the app stays executable) and adds an "Applications" shortcut."""
import io, os, sys
import pycdlib

src, out = sys.argv[1], sys.argv[2]
iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, rock_ridge='1.09', vol_ident='Ank')
counter = [0]
handles = []

def iso_name(is_dir):
    counter[0] += 1
    return f'F{counter[0]:06d}' if is_dir else f'F{counter[0]:06d}.;1'

def add(local_dir, iso_dir, rr_dir):
    for name in sorted(os.listdir(local_dir)):
        lp = os.path.join(local_dir, name)
        rr_path = rr_dir.rstrip('/') + '/' + name
        if os.path.islink(lp):
            ip = iso_dir.rstrip('/') + '/' + iso_name(False).split('.')[0]
            iso.add_symlink(symlink_path=ip, rr_symlink_name=name, rr_path=os.readlink(lp))
        elif os.path.isdir(lp):
            ip = iso_dir.rstrip('/') + '/' + iso_name(True)
            iso.add_directory(ip, rr_name=name, file_mode=0o040755)
            add(lp, ip, rr_path)
        else:
            ip = iso_dir.rstrip('/') + '/' + iso_name(False)
            mode = 0o100755 if os.access(lp, os.X_OK) else 0o100644
            data = open(lp, 'rb').read(); fp = io.BytesIO(data); handles.append(fp)
            iso.add_fp(fp, len(data), ip, rr_name=name, file_mode=mode)

add(src, '/', '/')
if not os.path.exists(os.path.join(src, 'Applications')):
    iso.add_symlink(symlink_path='/' + iso_name(False).split('.')[0], rr_symlink_name='Applications', rr_path='/Applications')
iso.write(out)
iso.close()
print('wrote', out, os.path.getsize(out), 'bytes')
