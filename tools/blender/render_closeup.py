# -*- coding: utf-8 -*-
# 静止姿势特写（正面/外侧/内侧/俯视），检查局部穿模，例如脚和鞋、脸
# 用法：blender -b <SK_xxx.blend> --python render_closeup.py -- <mod 目录> <输出目录> <骨骼名> <标签> [画面高度cm] [竖直偏移cm] [前后偏移cm]
# 由 build-mod.ps1 按 mod.json 的 preview.closeups 调用
import math, os, sys
import bpy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewlib

a = sys.argv[sys.argv.index('--') + 1:]
MOD_DIR, OUT, BONE, LABEL = a[:4]
SIZE, DZ, DY = (float(x) for x in (a[4:7] + ['45', '0', '0'][len(a[4:7]):]))
os.makedirs(OUT, exist_ok=True)
cfg = viewlib.load_cfg(MOD_DIR)
viewlib.apply_basecolor(cfg, MOD_DIR)
arm, _ = viewlib.model_objects(cfg)
if BONE not in arm.data.bones: raise SystemExit(f'CLOSEUP FAIL 没有骨骼 {BONE}')
sc, cam = viewlib.setup_scene(500, 500, SIZE)
h = arm.data.bones[BONE].head_local
cx, cy, cz = h.x, h.y + DY, h.z + DZ
side = 1 if h.x >= 0 else -1   # 外侧 = 远离身体中线的一侧
views = {'front': ((cx, cy - 600, cz), (90, 0, 0)), 'out': ((cx + 600 * side, cy, cz), (90, 0, 90 * side)),
         'in': ((cx - 600 * side, cy, cz), (90, 0, -90 * side)), 'top': ((cx, cy, cz + 600), (0, 0, 0))}
for v, (loc, rot) in views.items():
    cam.location = loc; cam.rotation_euler = [math.radians(x) for x in rot]
    sc.render.filepath = os.path.join(OUT, f'{LABEL}_{v}.png')
    bpy.ops.render.render(write_still=True)
    print('CLOSEUP', sc.render.filepath, flush=True)
