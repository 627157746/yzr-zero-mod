# -*- coding: utf-8 -*-
# 渲染重定向预览：每个动作一张图，每行一帧：模型正面（红点 = 魂的手，武器在这里） | 魂正面 | 模型侧面
# 同时统计手臂顶点穿进躯干的数量和最大深度（近似，用于对比不同设置）
# 用法：blender -b <SK_xxx.blend> --python render_retarget.py -- <mod 目录> <poses.json> <soul_body.pskx> <输出目录>
# 由 preview-retarget.ps1 调用；身体/手臂骨骼的匹配规则来自 mod.json 的 preview.body_match / preview.arm_match
import json, math, os, re, sys
import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewlib

MOD_DIR, POSES, SOUL_PSK, OUT = sys.argv[sys.argv.index('--') + 1:][:4]
os.makedirs(OUT, exist_ok=True)
cfg = viewlib.load_cfg(MOD_DIR)
PV = cfg.get('preview', {})
D = json.load(open(POSES, encoding='utf-8'))
M = Matrix.Diagonal((1, -1, 1, 1))   # UE 组件空间 <-> Blender：Y 取反
def ue(v): return Matrix.Translation(v[4:7]) @ Quaternion((v[3], v[0], v[1], v[2])).to_matrix().to_4x4()
def log(*a): print('RETARGET', *a, flush=True)

viewlib.apply_basecolor(cfg, MOD_DIR)
t_arm, t_mesh = viewlib.model_objects(cfg)
before = set(bpy.data.objects)
bpy.ops.psk.import_file(filepath=SOUL_PSK)
new = [o for o in bpy.data.objects if o not in before]
h_arm = next(o for o in new if o.type == 'ARMATURE'); h_meshes = [o for o in new if o.type == 'MESH']
grey = bpy.data.materials.new('SoulGrey'); grey.diffuse_color = (0.55, 0.6, 0.7, 1)
for m in h_meshes:
    for s in m.material_slots: s.material = grey

def apply_pose(arm, bones, ref, cur):
    """用 UE 组件空间姿势摆 Blender 骨架：每根骨骼的 Blender 姿势 = M·当前·参考⁻¹·M · 静止矩阵"""
    idx = {b.lower(): i for i, b in enumerate(bones)}
    P = {}
    def walk(b):
        i = idx.get(b.name.lower())
        if i is not None: P[b.name] = M @ ue(cur[i]) @ ue(ref[i]).inverted() @ M @ b.matrix_local
        elif b.parent: P[b.name] = P[b.parent.name] @ b.parent.matrix_local.inverted() @ b.matrix_local   # 姿势里没有的骨骼跟着父骨骼刚性移动
        else: P[b.name] = b.matrix_local.copy()
        for c in b.children: walk(c)
    for b in arm.data.bones:
        if b.parent is None: walk(b)
    for pb in arm.pose.bones:
        b = pb.bone
        if b.parent: pb.matrix_basis = (b.parent.matrix_local.inverted() @ b.matrix_local).inverted() @ (P[b.parent.name].inverted() @ P[b.name])
        else: pb.matrix_basis = b.matrix_local.inverted() @ P[b.name]

def marker(name, color=(1, 0, 0, 1)):
    import bmesh
    me = bpy.data.meshes.new(name); o = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(o)
    bm = bmesh.new(); bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=3.0); bm.to_mesh(me); bm.free()
    mat = bpy.data.materials.new(name); mat.diffuse_color = color; me.materials.append(mat)
    o.show_in_front = True
    return o
marks = [marker('SoulHandL'), marker('SoulHandR')]
# 绿点：双手武器左手「应该握的地方」（原版左手相对武器的位置，preview_retarget.py 的 grip_target；不是握柄帧时隐藏）
grip_mark = marker('GripTarget', (0, 0.9, 0.1, 1))

TW, TH = 300, 360
sc, cam = viewlib.setup_scene(TW, TH, 250)
VIEWS = {'front': ((0, -600, 100), (90, 0, 0)), 'side': ((600, 0, 100), (90, 0, 90))}
TILE = os.path.join(OUT, '_tile.png')
def render_tile(view, show_model, grip=False):
    for o in (t_mesh, *marks): o.hide_render = not show_model
    grip_mark.hide_render = not (show_model and grip)
    for o in h_meshes: o.hide_render = show_model
    loc, rot = VIEWS[view]
    cam.location = loc; cam.rotation_euler = [math.radians(a) for a in rot]
    sc.render.filepath = TILE
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(TILE, check_existing=False)
    px = np.empty(TW * TH * 4, np.float32); img.pixels.foreach_get(px); bpy.data.images.remove(img)
    return px.reshape(TH, TW, 4)

# 穿模检查：每个手臂顶点找 3cm 内最近的躯干三角面，在其法线反侧超过 0.3cm 算穿进去
body_re = re.compile(PV.get('body_match', r'^(Hips|Spine|Chest|UpperChest)$|UpLeg$'))
arm_re = re.compile(PV.get('arm_match', r'^(Left|Right)(Arm|ForeArm|Hand)'))
gname = {g.index: g.name for g in t_mesh.vertex_groups}
dom = [gname[max(v.groups, key=lambda g: g.weight).group] if v.groups else '' for v in t_mesh.data.vertices]
body_set = {i for i, n in enumerate(dom) if body_re.search(n)}
body_polys = [tuple(p.vertices) for p in t_mesh.data.polygons if all(v in body_set for v in p.vertices)]
def part(n): return 'hand' if 'Hand' in n else 'fore' if 'ForeArm' in n else 'upper'
def side(n): return 'L' if re.search(r'Left|_l$|\.L$|_L$', n) else 'R'
arm_idx = {}
for i, n in enumerate(dom):
    if n and arm_re.search(n): arm_idx.setdefault(side(n) + part(n), []).append(i)
log('穿模检查 躯干三角面', len(body_polys), '手臂顶点', {k: len(v) for k, v in arm_idx.items()})
def clipping():
    from mathutils.bvhtree import BVHTree
    dg = bpy.context.evaluated_depsgraph_get(); ev = t_mesh.evaluated_get(dg); me = ev.to_mesh()
    n = len(me.vertices); co = np.empty(n * 3, np.float32); me.vertices.foreach_get('co', co); co = co.reshape(n, 3)
    ev.to_mesh_clear()
    if not body_polys: return {}
    bvh = BVHTree.FromPolygons([tuple(c) for c in co], body_polys)
    res = {}
    for key, idx in arm_idx.items():
        cnt, deep = 0, 0.0
        for i in idx:
            p = Vector(co[i]); loc, nor, _, _ = bvh.find_nearest(p, 3.0)
            if loc is not None:
                d = (p - loc).dot(nor)
                if d < -0.3: cnt += 1; deep = max(deep, -d)
        res[key] = (cnt, round(deep, 1))
    return res

hb = {b.lower(): i for i, b in enumerate(D['soul_bones'])}
for a in D['anims']:
    k = len(a['frames'])
    sheet = np.ones((TH * k, TW * 3, 4), np.float32)
    for f in range(k):
        apply_pose(t_arm, D['model_bones'], D['model_ref'], a['model'][f])
        apply_pose(h_arm, D['soul_bones'], D['soul_ref'], a['soul'][f])
        for mk, bn in zip(marks, ('hand_l', 'hand_r')):
            v = a['soul'][f][hb[bn]]; mk.location = (v[4], -v[5], v[6])
        gt = (a.get('grip_target') or [None] * k)[f]
        if gt: grip_mark.location = (gt[0], -gt[1], gt[2])
        bpy.context.view_layer.update()
        log('穿模', a['name'], '帧', a['frames'][f], '穿进躯干的顶点数/最大深度cm', clipping())
        y0 = TH * (k - 1 - f)   # Blender 图像从下往上存，第 0 帧放最上面
        for col, (view, show_m) in enumerate((('front', True), ('front', False), ('side', True))):
            sheet[y0:y0 + TH, col * TW:(col + 1) * TW] = render_tile(view, show_m, bool(gt))
    out = bpy.data.images.new('sheet', TW * 3, TH * k, alpha=True)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = os.path.join(OUT, f'{a["name"]}.png'); out.file_format = 'PNG'; out.save()
    bpy.data.images.remove(out)
    log(a['name'], '帧', a['frames'], '->', os.path.join(OUT, f'{a["name"]}.png'))
if os.path.exists(TILE): os.remove(TILE)
