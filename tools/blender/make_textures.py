# -*- coding: utf-8 -*-
# 按 mod.json 的 generated_textures 生成游戏材质需要的辅助贴图：
#   arm           常量 ARM（R=AO=1, G=粗糙度, B=金属度）
#   arm_from_mask 由金属遮罩图生成 ARM：遮罩白=金属（rough_metal），黑=布料（rough）
#   flat_normal   平法线（模型没有法线贴图时用）
# 用法：blender -b --python make_textures.py -- <mod 目录>（路径相对 mod 目录）
import json, os, sys
import bpy
import numpy as np

ROOT = sys.argv[sys.argv.index('--') + 1:][0]
CFG = json.load(open(os.path.join(ROOT, 'mod.json'), encoding='utf-8'))
def log(*a): print('TEX', *a, flush=True)

def load(path):
    img = bpy.data.images.load(path); img.colorspace_settings.name = 'Non-Color'
    w, h = img.size; a = np.empty(w * h * 4, np.float32); img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    return a.reshape(h, w, 4)

def save(path, a):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    h, w = a.shape[:2]
    img = bpy.data.images.new(os.path.basename(path), w, h, alpha=True, float_buffer=False); img.colorspace_settings.name = 'Non-Color'
    img.pixels.foreach_set(np.ascontiguousarray(a, np.float32).ravel())
    img.filepath_raw = path; img.file_format = 'PNG'; img.save()
    bpy.data.images.remove(img); log('写出', path, w, h)

def down(a, size):
    f = a.shape[0] // size
    return a if f <= 1 else a[:f * size, :f * size].reshape(size, f, size, f, a.shape[2]).mean(axis=(1, 3))

for t in CFG.get('generated_textures', []):
    out = os.path.join(ROOT, t['out']); kind = t['type']
    if kind == 'arm':
        s = t.get('size', 256); a = np.ones((s, s, 4), np.float32); a[..., 1] = t['rough']; a[..., 2] = t.get('metal', 0.0)
    elif kind == 'arm_from_mask':
        s = t.get('size', 2048); m = down(load(os.path.join(ROOT, t['mask'])), s)[..., 0:1]
        a = np.ones((m.shape[0], m.shape[1], 4), np.float32)
        a[..., 1:2] = t.get('rough', 0.8) * (1 - m) + t.get('rough_metal', 0.35) * m
        a[..., 2:3] = m
    elif kind == 'flat_normal':
        s = t.get('size', 256); a = np.ones((s, s, 4), np.float32); a[..., 0] = 0.5; a[..., 1] = 0.5
    else:
        raise SystemExit(f'TEX FAIL 未知类型 {kind}')
    save(out, a)
log('DONE')
