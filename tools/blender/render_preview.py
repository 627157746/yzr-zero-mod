# -*- coding: utf-8 -*-
# 渲染处理后模型的静止姿势预览（正面/侧面/背面），检查附件合并、材质槽、形态键、袖子朝向是否正常
# 用法：blender -b <SK_xxx.blend> --python render_preview.py -- <mod 目录> <输出目录>
import math, os, sys
import bpy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewlib

MOD_DIR, OUT = sys.argv[sys.argv.index('--') + 1:][:2]
os.makedirs(OUT, exist_ok=True)
cfg = viewlib.load_cfg(MOD_DIR)
viewlib.apply_basecolor(cfg, MOD_DIR)
_, mesh = viewlib.model_objects(cfg)
zs = [(mesh.matrix_world @ v.co).z for v in mesh.data.vertices]
top, low = max(zs), min(zs); cz = (top + low) / 2
sc, cam = viewlib.setup_scene(900, 900, (top - low) * 1.15)
for label, loc, rot in (('front', (0, -600, cz), (90, 0, 0)), ('side', (600, 40, cz), (90, 0, 90)), ('back', (0, 600, cz), (90, 0, 180))):
    cam.location = loc; cam.rotation_euler = [math.radians(a) for a in rot]
    sc.render.filepath = os.path.join(OUT, f'preview_{label}.png')
    bpy.ops.render.render(write_still=True)
    print('PREVIEW', sc.render.filepath, flush=True)
