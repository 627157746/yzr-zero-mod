# -*- coding: utf-8 -*-
# 通用 unitypackage 解包工具（由 build-mod.ps1 在配置了 mod.json 的 package 段时自动调用）：
#   从 source/*.unitypackage 解出 input/fbx/*.fbx、input/textures/*.png、input/unity/*.{prefab,controller,anim}
# 用法：python tools/extract_package.py <mod 目录>
import json, os, sys, tarfile
from collections import defaultdict

MOD = sys.argv[1]
CFG = json.load(open(os.path.join(MOD, 'mod.json'), encoding='utf-8')).get('package', {})
PKG = os.path.join(MOD, CFG['unitypackage'])
PREFIX = CFG.get('prefix', '')
TEX_DIRS = [d.lower() for d in CFG.get('tex_dirs', [])]
UNI_DIRS = [d.lower() for d in CFG.get('unity_dirs', [])]
FBX, TEX, UNI = (os.path.join(MOD, 'input', d) for d in ('fbx', 'textures', 'unity'))

if not os.path.exists(PKG):
    if os.path.isdir(FBX) and os.listdir(FBX) and os.path.isdir(TEX) and os.listdir(TEX):
        print('跳过解包（未放置 source 包，直接复用已有 input/）')
        sys.exit(0)
    sys.exit(f'缺 {PKG}')

items = defaultdict(dict)
with tarfile.open(PKG, 'r:gz') as tf:
    for m in tf.getmembers():
        p = m.name.strip('./').split('/')
        if m.isfile() and len(p) == 2 and p[1] in ('pathname', 'asset'):
            items[p[0]][p[1]] = tf.extractfile(m).read()

for d in (FBX, TEX, UNI):
    os.makedirs(d, exist_ok=True)

n = defaultdict(int)
for v in items.values():
    if 'pathname' not in v or 'asset' not in v:
        continue
    path = v['pathname'].decode('utf-8').splitlines()[0]
    if PREFIX and not path.startswith(PREFIX):
        continue
    name = path.rsplit('/', 1)[-1]
    low = path.lower()
    if low.endswith('.fbx'):
        dst = FBX
    elif low.endswith('.png') and (not TEX_DIRS or any(d in low for d in TEX_DIRS)):
        dst = TEX
    elif low.endswith(('.prefab', '.controller', '.anim')) and (not UNI_DIRS or any(d in low for d in UNI_DIRS)):
        dst = UNI
    else:
        continue
    out = os.path.join(dst, name)
    assert not os.path.exists(out) or open(out, 'rb').read() == v['asset'] or dst == UNI, f'重名文件内容不同: {path}'
    open(out, 'wb').write(v['asset'])
    n[os.path.basename(dst)] += 1

assert n['fbx'] >= 1 and n['textures'] >= 1, f'解包结果不全: {dict(n)}'
print('解出', dict(n))
