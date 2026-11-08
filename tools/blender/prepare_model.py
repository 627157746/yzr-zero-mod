# -*- coding: utf-8 -*-
# 把替换模型整理成 UE 用的 FBX（全部按 mod.json 的 model 段执行，不含任何特定模型的写死逻辑）：
#   导入主体 + 附件 FBX → 应用变换并换算到厘米 × scale → 附件对位、骨架合并 → 可选钩子脚本 →
#   鞋底落地、加 root 骨 → 形态键烘焙 → 删指定材质的面 → 材质改名为 UE 材质槽 → 合并网格 → 静止姿势旋转 → 导出
# 用法：blender -b --python prepare_model.py -- <mod 目录> <变体名> <输出目录>（由 build-mod.ps1 调用）
import json, os, re, sys
import bpy
from mathutils import Matrix, Vector

MOD_DIR, VARIANT, OUT_DIR = sys.argv[sys.argv.index('--') + 1:][:3]
CFG = json.load(open(os.path.join(MOD_DIR, 'mod.json'), encoding='utf-8'))
ROOT = MOD_DIR   # mod.json 里的本地路径都相对 mod 目录
M = CFG['model']
os.makedirs(OUT_DIR, exist_ok=True)
def log(*a): print('MODEL', *a, flush=True)
def base_name(n): return n.split('.')[0]
def fail(msg): raise SystemExit('MODEL FAIL ' + msg)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)

fbx_dir = os.path.join(ROOT, M['fbx_dir'])
def import_fbx(name):
    path = os.path.join(fbx_dir, name)
    if not os.path.exists(path): fail(f'找不到 {path}')
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, use_custom_normals=True, ignore_leaf_bones=False, automatic_bone_orientation=False)
    return [o for o in bpy.data.objects if o not in before]

variant = CFG.get('variants', {}).get(VARIANT)
if variant is None: fail(f'mod.json 里没有变体 {VARIANT}')
files = list(M.get('extra_fbx', []))
for key in variant.get('include', []):
    files += M.get('optional_fbx', {}).get(key, [])
main_objs = import_fbx(M['main_fbx'])
main_arm = next((o for o in main_objs if o.type == 'ARMATURE' and M['root_bone'] in o.data.bones), None)
if not main_arm: fail(f'{M["main_fbx"]} 里没有包含 {M["root_bone"]} 的骨架')
extras = {n: import_fbx(n) for n in files}
log('变体', VARIANT, '附件', files)
for o in list(bpy.data.objects):
    if o.type == 'MESH' and base_name(o.name) in set(M.get('delete_meshes', [])):
        bpy.data.objects.remove(o)

def apply_all():
    bpy.ops.object.select_all(action='DESELECT')
    for o in bpy.data.objects:
        if o.type in ('ARMATURE', 'MESH'): o.select_set(True)
    bpy.context.view_layer.objects.active = main_arm
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
for o in bpy.data.objects:
    if o.parent and o.type in ('ARMATURE', 'MESH'):
        mw = o.matrix_world.copy(); o.parent = None; o.matrix_world = mw
for o in [o for o in bpy.data.objects if o.type == 'EMPTY']:
    bpy.data.objects.remove(o)
apply_all()
k = float(M.get('blender_to_cm', 100.0)) * float(M.get('scale', 1.0))
for o in bpy.data.objects:
    if o.type in ('ARMATURE', 'MESH'): o.matrix_world = Matrix.Scale(k, 4) @ o.matrix_world
apply_all()
def head(arm, name):
    if name not in arm.data.bones: fail(f'骨架 {arm.name} 没有骨骼 {name}')
    return arm.data.bones[name].head_local.copy()
log('缩放', k, f'{M["root_bone"]} 高度(cm)', round(head(main_arm, M['root_bone']).z, 2))

# 附件对位：按“附件里的某根骨骼应该与主体某根骨骼重合”整体平移
for al in M.get('align_accessories', []):
    if al['ref_fbx'] not in extras: continue
    ref_arm = next(o for o in extras[al['ref_fbx']] if o.type == 'ARMATURE')
    shift = head(main_arm, al['to']) - head(ref_arm, al['bone'])
    for f in al.get('apply_to', [al['ref_fbx']]):
        for o in extras.get(f, []):
            if o.name in bpy.data.objects and o.type in ('ARMATURE', 'MESH'):
                o.matrix_world = Matrix.Translation(shift) @ o.matrix_world
    apply_all()
    log('附件平移(cm)', al['ref_fbx'], tuple(round(v, 2) for v in shift))

# 合并附件骨架到主体：同名骨骼复用；附件根骨骼按 attach 挂到主体骨骼下
attach, skip = M.get('attach', {}), set(M.get('skip_bones', []))
for fname, objs in extras.items():
    arm = next((o for o in objs if o.type == 'ARMATURE'), None)
    meshes = [o for o in objs if o.type == 'MESH' and o.name in bpy.data.objects]
    if arm is None:
        for m in meshes: m.parent = main_arm
        continue
    bones = [(b.name, b.head_local.copy(), b.tail_local.copy(), b.parent.name if b.parent else None) for b in arm.data.bones]
    bpy.context.view_layer.objects.active = arm; bpy.ops.object.mode_set(mode='EDIT')
    rolls = {b.name: b.roll for b in arm.data.edit_bones}
    bpy.ops.object.mode_set(mode='OBJECT')
    existing = {b.name for b in main_arm.data.bones}
    bpy.context.view_layer.objects.active = main_arm; bpy.ops.object.mode_set(mode='EDIT')
    eb = main_arm.data.edit_bones; added = 0
    for name, h, t, _ in bones:
        if name in eb or name in skip: continue
        nb = eb.new(name); nb.head, nb.tail, nb.roll = h, t, rolls[name]; added += 1
    for name, _, _, parent in bones:
        if name in existing or name in skip or name not in eb: continue
        p = attach.get(name) or (parent if parent and parent not in skip else M['root_bone'])
        if p not in eb: fail(f'{fname}: {name} 的父骨骼 {p} 不存在（检查 model.attach）')
        eb[name].parent = eb[p]
    bpy.ops.object.mode_set(mode='OBJECT')
    for m in meshes:
        for md in m.modifiers:
            if md.type == 'ARMATURE': md.object = main_arm
    bpy.data.objects.remove(arm)
    log(f'{fname}: 并入 {added} 根骨骼，网格 {[m.name for m in meshes]}')

# 通用 Humanoid 全身比例逆向适配（前臂轴段适配 + 静止姿势烘焙 + 脊柱/四肢 Twist 细分 + 导出 soul.adapted_pose_json）
if M.get('adapt_humanoid') is not None:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import adapt_humanoid
    ws_root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    adapt_humanoid.run(bpy, main_arm, CFG, ROOT, ws_root, log, apply_all)

# 模型专属处理的扩展点（例如把附件摆成作者预设的姿势）：exec 一个 Python 文件。
# 可用：bpy、arm（主骨架）、extras（附件 FBX 名 → 导入的对象）、K（Blender 单位→厘米×放大）、CFG、ROOT（mod 目录）、VARIANT、log、apply_all
hook = M.get('blender_hook')
if hook:
    path = os.path.join(ROOT, hook)
    exec(compile(open(path, encoding='utf-8').read(), path, 'exec'),
         {'bpy': bpy, 'arm': main_arm, 'extras': extras, 'K': k, 'CFG': CFG, 'ROOT': ROOT, 'log': log, 'VARIANT': VARIANT, 'apply_all': apply_all})
    log('钩子脚本', hook)

# 鞋底落地：sole_bones 权重 > 0.5 的最低顶点抬到 z=0
sole = set(M.get('sole_bones', []))
low = 0.0
for m in [o for o in bpy.data.objects if o.type == 'MESH']:
    gi = {g.index for g in m.vertex_groups if g.name in sole}
    if not gi: continue
    for v in m.data.vertices:
        if any(g.group in gi and g.weight > 0.5 for g in v.groups): low = min(low, v.co.z)
if low < 0:
    for o in bpy.data.objects:
        if o.type in ('ARMATURE', 'MESH'): o.matrix_world = Matrix.Translation((0, 0, -low)) @ o.matrix_world
    apply_all()
log('鞋底抬高(cm)', round(-low, 2))

bpy.context.view_layer.objects.active = main_arm; bpy.ops.object.mode_set(mode='EDIT')
eb = main_arm.data.edit_bones
if 'root' not in eb:
    r = eb.new('root'); r.head = Vector((0, 0, 0)); r.tail = Vector((0, 0, 10)); r.roll = 0
    eb[M['root_bone']].parent = r
bpy.ops.object.mode_set(mode='OBJECT')

# 形态键：保留 FBX 作者设的初始值，只覆盖 shape_keys 里列出的（按后缀匹配，Blender 会去掉 "XXX_BS." 前缀），然后烘焙
meshes = [o for o in bpy.data.objects if o.type == 'MESH']
for m in meshes:
    keys = m.data.shape_keys
    if not keys: continue
    for key, val in M.get('shape_keys', {}).get(base_name(m.name), {}).items():
        short = key.rsplit('.', 1)[-1]
        if short in keys.key_blocks: keys.key_blocks[short].value = val
        else: log('缺少形态键', m.name, key)
    log('形态键', base_name(m.name), {kb.name: round(kb.value, 2) for kb in keys.key_blocks[1:] if abs(kb.value) > 1e-6})
    bpy.context.view_layer.objects.active = m
    bpy.ops.object.shape_key_remove(all=True, apply_mix=True)

# 删掉指定材质的面（半透明叠加层等游戏材质画不出来的部分）
drop = set(M.get('drop_materials', []))
for m in meshes:
    idx = [i for i, s in enumerate(m.material_slots) if s.material and base_name(s.material.name) in drop]
    if not idx: continue
    bpy.context.view_layer.objects.active = m; bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.object.mode_set(mode='OBJECT')
    n = 0
    for p in m.data.polygons:
        p.select = p.material_index in idx; n += p.select
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.delete(type='FACE'); bpy.ops.object.mode_set(mode='OBJECT')
    log('删除面', m.name, n)

# 材质 → UE 材质槽（槽名必须在 mod.json 的 materials 里）
slot_of = M['material_slots']
for s in set(slot_of.values()) - set(CFG['materials']):
    fail(f'材质槽 {s} 没有在 materials 里指定游戏材质实例')
targets = {n: bpy.data.materials.get(n) or bpy.data.materials.new(n) for n in set(slot_of.values())}
unmapped = set()
for m in meshes:
    for s in m.material_slots:
        src = base_name(s.material.name) if s.material else ''
        if src in slot_of: s.material = targets[slot_of[src]]
        elif src not in targets and src not in drop: unmapped.add(f'{base_name(m.name)}:{src}')
if unmapped: fail(f'这些材质没写进 model.material_slots: {sorted(unmapped)}')

bpy.ops.object.select_all(action='DESELECT')
for m in meshes: m.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.join()
body = bpy.context.view_layer.objects.active
name = 'SK_' + CFG['mod_name']; body.name = body.data.name = name
# join 会把同一个材质数据块占成多个槽（多个源材质映射到同一个 UE 材质槽时，例如 PMX 的脸/眼/口都进 Face）。
# 合并成唯一槽，否则 UE 里会多出十几条一模一样的材质槽。没有重复材质的 mod 不受影响。
slot_of_mat = {}
remap = {}
for i, s in enumerate(body.material_slots):
    key = s.material.name if s.material else ''
    remap[i] = slot_of_mat.setdefault(key, i)
if len(set(remap.values())) < len(body.material_slots):
    for p in body.data.polygons:
        p.material_index = remap.get(p.material_index, p.material_index)
    keep = set(remap.values())
    for i in reversed(range(len(body.material_slots))):
        if i not in keep:
            body.active_material_index = i
            bpy.ops.object.material_slot_remove()
for i, s in reversed(list(enumerate(body.material_slots))):
    if not any(p.material_index == i for p in body.data.polygons):
        body.active_material_index = i; bpy.ops.object.material_slot_remove()
mods = [md for md in body.modifiers if md.type == 'ARMATURE']
for md in mods[1:]: body.modifiers.remove(md)
mods[0].object = main_arm; body.parent = main_arm
main_arm.name = main_arm.data.name = 'Armature'

# 静止姿势旋转：把某根骨骼（连同子骨骼和网格）绕自身头部转到目标方向，再设为静止姿势。
# 用于 T 姿势下朝外的袖子/裙摆：物理会把它们拉回静止姿势，静止姿势必须是游戏动作里“自然下垂”的方向
rot = M.get('rest_rotations', [])
if rot:
    bpy.context.view_layer.objects.active = main_arm; bpy.ops.object.mode_set(mode='POSE')
    for r in rot:
        b0 = main_arm.data.bones[r['bone']]
        d0 = (head(main_arm, r['dir_to']) - b0.head_local).normalized()
        q = d0.rotation_difference(Vector(r['target']).normalized())
        main_arm.pose.bones[b0.name].matrix = Matrix.Translation(b0.head_local) @ q.to_matrix().to_4x4() @ Matrix.Translation(-b0.head_local) @ b0.matrix_local
        bpy.context.view_layer.update()
        log('静止姿势旋转', r['bone'], round(q.angle * 57.2958, 1), '度')
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.modifier_apply(modifier=next(md.name for md in body.modifiers if md.type == 'ARMATURE'))
    bpy.context.view_layer.objects.active = main_arm; bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    md = body.modifiers.new('Armature', 'ARMATURE'); md.object = main_arm

z = [v.co.z for v in body.data.vertices]
lf, lt = head(main_arm, M['left_foot']), head(main_arm, M['left_toe'])
log('顶点', len(body.data.vertices), '三角面', sum(len(p.vertices) - 2 for p in body.data.polygons), '材质槽', [s.material.name for s in body.material_slots])
log('高度(cm)', round(min(z), 2), '~', round(max(z), 2), '骨骼', len(main_arm.data.bones), '左脚', tuple(round(v, 1) for v in lf), '左脚尖', tuple(round(v, 1) for v in lt))
# Blender 里模型应朝 -Y、左侧在 +X（导出后在 UE 里朝 +Y，和魂一致）
if not (lf.x > 0 and lt.y < lf.y): fail('朝向不对：Blender 里模型应面朝 -Y、左脚在 +X。先在 DCC 里转好或改 FBX 导出轴')

bpy.context.scene.unit_settings.system = 'METRIC'
bpy.context.scene.unit_settings.scale_length = 0.01
bpy.ops.object.select_all(action='DESELECT'); main_arm.select_set(True); body.select_set(True)
bpy.context.view_layer.objects.active = main_arm
out = os.path.join(OUT_DIR, name + '.fbx')
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={'ARMATURE', 'MESH'}, apply_unit_scale=True,
                         apply_scale_options='FBX_SCALE_ALL', axis_forward='-Z', axis_up='Y', add_leaf_bones=False,
                         use_armature_deform_only=False, mesh_smooth_type='FACE', use_mesh_modifiers=False, bake_anim=False)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT_DIR, name + '.blend'))
json.dump({'slots': [s.material.name for s in body.material_slots]}, open(os.path.join(OUT_DIR, 'model_info.json'), 'w', encoding='utf-8'))
log('导出', out)
log('DONE')
