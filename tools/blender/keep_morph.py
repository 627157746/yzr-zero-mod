# -*- coding: utf-8 -*-
# 保留需要的形态键（例如眨眼）：模型的形态键默认会被烘焙成静止形状，游戏里就再也驱动不了。
# 这里在烘焙前记下 delta（换算到主导骨骼的空间），等骨架/网格全部定型后再按新的骨骼静止矩阵重建形态键，
# 之后由后处理动画蓝图用同名曲线（morph target 曲线）驱动。由 tools/blender/prepare_model.py 调用。
import re
from mathutils import Vector


def plan(cfg):
    """读 mod.json：model.keep_shape_keys（源名 → UE 里的形态键/曲线名）；没配就退回 face.blink.morph。"""
    keep = cfg.get('model', {}).get('keep_shape_keys')
    if keep is None:
        blink = (cfg.get('face') or {}).get('blink') or {}
        keep = {blink['morph']: (blink.get('ue_name') or blink['morph'])} if blink.get('morph') else {}
    return {str(k): str(v) for k, v in keep.items()}


def clean_bone_name(name):
    return re.sub(r'[\s\u3000\-\.]+', '_', name)


def _candidates(name):
    """骨骼在适配前（原名）和适配后（规范名/去空格）的可能叫法，按可能性排序。"""
    out = [clean_bone_name(name)]
    try:
        import adapt_humanoid
        out.append(clean_bone_name(adapt_humanoid._canonical_humanoid_name(name)))
    except Exception:
        pass
    return list(dict.fromkeys(out))


def _find_key(mesh, name):
    keys = mesh.data.shape_keys
    if not keys:
        return None
    if name in keys.key_blocks:
        return keys.key_blocks[name]
    return keys.key_blocks.get(name.rsplit('.', 1)[-1])


def snapshot(arm, meshes, keep, log):
    """记下要保留的形态键：逐顶点 delta（主导骨骼空间）+ 当时的骨骼静止矩阵。keep 为空返回 None。"""
    if not keep:
        return None
    bones = {b.name for b in arm.data.bones}
    snap = {'bones': {b.name: (arm.matrix_world @ b.matrix_local).copy() for b in arm.data.bones}, 'meshes': {}}
    found = set()
    for m in meshes:
        keys = {}
        for src, dst in keep.items():
            kb = _find_key(m, src)
            if kb is None:
                continue
            found.add(src)
            basis = m.data.shape_keys.reference_key
            to_arm = m.matrix_world.to_3x3()
            verts = []
            for i, v in enumerate(m.data.vertices):
                d = kb.data[i].co - basis.data[i].co
                if d.length <= 1e-6:
                    continue
                lead = max(v.groups, key=lambda g: g.weight, default=None)
                gname = m.vertex_groups[lead.group].name if lead else ''
                if gname not in bones:
                    raise SystemExit(f'BLENDER FAIL 形态键 {src} 的顶点 {i} 没有骨骼权重，无法跟随骨骼重建')
                d_arm = to_arm @ d
                verts.append((i, snap['bones'][gname].to_3x3().inverted() @ d_arm, gname))
            keys[dst] = verts
            log('保留形态键', m.name, src, '->', dst, '位移顶点', len(verts))
            # 默认值必须归零：否则烘焙时会叠加进静止形状，眨眼被永久闭着
            if abs(kb.value) > 1e-6:
                log('形态键默认值归零', m.name, src, round(kb.value, 3))
                kb.value = 0.0
        if keys:
            snap['meshes'][m.name] = keys
    missing = sorted(set(keep) - found)
    if missing:
        raise SystemExit('BLENDER FAIL 找不到形态键: ' + ', '.join(missing))
    return snap


def drop_all(bpy, meshes, keep, log):
    """把所有形态键（含要保留的）删掉：带形态键的网格无法 Apply armature modifier，必须先删。
    保留键的 delta 已经在 snapshot 里记下了，骨骼/网格定型后再 restore。"""
    for m in meshes:
        if not m.data.shape_keys:
            continue
        for kb in m.data.shape_keys.key_blocks:
            kb.value = 0.0
        bpy.context.view_layer.objects.active = m
        while m.data.shape_keys and len(m.data.shape_keys.key_blocks) > 0:
            m.active_shape_key_index = len(m.data.shape_keys.key_blocks) - 1
            bpy.ops.object.shape_key_remove(all=False)
    if keep:
        log('形态键已清空（保留项待重建）', sorted(keep.values()))


def restore(bpy, arm, snap, log):
    """骨架定型后按新的骨骼静止矩阵把 delta 重建为形态键（value=0，交给动画曲线驱动）。"""
    if not snap:
        return
    new_arm = {b.name: (arm.matrix_world @ b.matrix_local).copy() for b in arm.data.bones}
    for mname, keys in snap['meshes'].items():
        m = bpy.data.objects.get(mname)
        if m is None:
            raise SystemExit(f'BLENDER FAIL 重建形态键时找不到网格 {mname}')
        if not m.data.shape_keys:
            m.shape_key_add(name='Basis', from_mix=False)
        from_mesh = m.matrix_world.to_3x3().inverted()
        for dst, verts in keys.items():
            kb = m.data.shape_keys.key_blocks.get(dst) or m.shape_key_add(name=dst, from_mix=False)
            for idx, d_bone, gname in verts:
                bone = next((new_arm[c] for c in _candidates(gname) if c in new_arm), None)
                if bone is None:
                    raise SystemExit(f'BLENDER FAIL 形态键 {dst} 的主导骨骼 {gname} 定型后不存在（候选 {_candidates(gname)}）')
                kb.data[idx].co = m.data.vertices[idx].co + (from_mesh @ (bone.to_3x3() @ d_bone))
            kb.value = 0.0
            log('重建形态键', mname, dst, '位移顶点', len(verts))
