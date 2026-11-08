# -*- coding: utf-8 -*-
# 魂的空身体：保留游戏导出 pskx 的 301 根骨骼，网格换成一个极小三角形，导出 FBX
# 用法：blender.exe -b --python tools/blender/make_hun_blank.py -- <pskx> <骨架参考json> <输出fbx>
import json, os, sys
import bpy

PSK, REF_JSON, OUT = sys.argv[sys.argv.index('--') + 1:][:3]
def log(*a): print('HUNBLANK', *a, flush=True)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
bpy.ops.psk.import_file(filepath=PSK)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
for o in [o for o in bpy.data.objects if o.type == 'MESH']:
    bpy.data.objects.remove(o)

# 与游戏骨架参考姿势核对（psk 导入时 Y 轴取反）
ref = json.load(open(REF_JSON, encoding='utf-8'))
names = [b['Name'] for b in ref['FinalRefBoneInfo']]
pose = ref['FinalRefBonePose']
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)
def qrot(q, v):
    x, y, z, w = q; vx, vy, vz = v
    tx, ty, tz = 2*(y*vz - z*vy), 2*(z*vx - x*vz), 2*(x*vy - y*vx)
    return (vx + w*tx + (y*tz - z*ty), vy + w*ty + (z*tx - x*tz), vz + w*tz + (x*ty - y*tx))
cache = {}
def glob(i):
    if i not in cache:
        p = pose[i]; q = tuple(p['Rotation'][k] for k in 'XYZW'); t = tuple(p['Translation'][k] for k in 'XYZ')
        par = ref['FinalRefBoneInfo'][i]['ParentIndex']
        if par < 0: cache[i] = (q, t)
        else:
            pq, pt = glob(par); r = qrot(pq, t); cache[i] = (qmul(pq, q), tuple(pt[j] + r[j] for j in range(3)))
    return cache[i]
lower = {n.lower(): i for i, n in enumerate(names)}
worst, missing = 0.0, []
for b in arm.data.bones:
    i = lower.get(b.name.lower())
    if i is None: missing.append(b.name); continue
    g = glob(i)[1]; h = b.head_local
    worst = max(worst, abs(h.x - g[0]), abs(h.y + g[1]), abs(h.z - g[2]))
log('骨骼', len(arm.data.bones), '不在游戏骨架里的', missing[:5], '最大位置偏差(cm)', round(worst, 4))

# 小三角形（0.5cm，藏在身体里面），全权重给 Pelvis；太小会被 UE 当成退化三角形删掉
pel = next(b for b in arm.data.bones if b.name.lower() == 'pelvis')
c = pel.head_local
me = bpy.data.meshes.new('SK_HunBlank')
me.from_pydata([(c.x, c.y, c.z), (c.x + 0.5, c.y, c.z), (c.x, c.y + 0.5, c.z)], [], [(0, 1, 2)])
ob = bpy.data.objects.new('SK_HunBlank', me); bpy.context.scene.collection.objects.link(ob)
mat = bpy.data.materials.new('Blank'); ob.data.materials.append(mat)
vg = ob.vertex_groups.new(name=pel.name); vg.add([0, 1, 2], 1.0, 'REPLACE')
md = ob.modifiers.new('Armature', 'ARMATURE'); md.object = arm
ob.parent = arm
arm.name = 'Armature'
bpy.context.scene.unit_settings.system = 'METRIC'
bpy.context.scene.unit_settings.scale_length = 0.01
bpy.ops.object.select_all(action='DESELECT'); arm.select_set(True); ob.select_set(True)
bpy.context.view_layer.objects.active = arm
os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.export_scene.fbx(filepath=OUT, use_selection=True, object_types={'ARMATURE', 'MESH'}, apply_unit_scale=True,
                         apply_scale_options='FBX_SCALE_ALL', axis_forward='-Z', axis_up='Y', add_leaf_bones=False,
                         use_armature_deform_only=False, mesh_smooth_type='FACE', bake_anim=False)
log('导出', OUT)
