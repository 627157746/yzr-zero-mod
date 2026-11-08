# -*- coding: utf-8 -*-
# 通用 Humanoid 模型 → 影之刃零「魂」全身比例逆向适配模块（由 tools/blender/prepare_model.py 调用）：
#   1. 规范化骨骼/顶点组命名（全角空格 -> 普通空格），清理无权重的 *_end 末端叶骨。
#   2. 烘焙形态键（保留作者默认值，覆盖 model.shape_keys 指定值）。
#   3. 在 T-pose 下将前臂中段袖管适配到魂 hand_l/r 的精确 Animation 长度（约 25.402cm），
#      末端袖口（cuff_margin_cm）与手腕做纯刚性平移（0% 拉伸形变），手掌绕腕关节放大（hand_scale）。
#   4. 将四肢、手腕、五指对齐魂的静止姿势并烘焙为新静止姿势（Apply Pose as Rest Pose），随后执行鞋底落地（Z=0）。
#   5. 在编辑模式下令 Hips -> Spine..UpperChest -> Left/Right Shoulder 的局部平移方向与魂 orig_t 完全平行，
#      细分 Spine_02/03、UpperChest、Neck_02 及 16 根四肢 Twist 骨，按投影线性插值重分配蒙皮权重。
#   6. 导出全身 42 根骨骼的适配局部平移到 soul.adapted_pose_json，并将骨骼/顶点组名中的空格/连字符/点号统一转为 '_'。
import json, os, re
from collections import defaultdict
from mathutils import Matrix, Quaternion, Vector


def _base_name(n):
    return n.split('.')[0]


def _canonical_humanoid_name(name):
    s = name.replace('\u3000', ' ')
    m = re.match(r'^(Shoulder|Upper[_ ]?[Aa]rm|Lower[_ ]?[Aa]rm|Hand|Upper[_ ]?[Ll]eg|Lower[_ ]?[Ll]eg|Foot|Toes?|(?:Thumb|Index|Middle|Ring|Little)[_ ](?:Proximal|Intermediate|Distal))[\._ ]([LR])$', s)
    if not m:
        return s
    body, lr = m.groups()
    side = 'Left' if lr == 'L' else 'Right'
    low = body.lower().replace('_', ' ')
    mapping = {
        'shoulder': 'Shoulder',
        'upper arm': 'Upper Arm', 'upperarm': 'Upper Arm',
        'lower arm': 'Lower Arm', 'lowerarm': 'Lower Arm',
        'hand': 'Hand',
        'upper leg': 'Upper Leg', 'upperleg': 'Upper Leg',
        'lower leg': 'Lower Leg', 'lowerleg': 'Lower Leg',
        'foot': 'Foot',
        'toe': 'Toes', 'toes': 'Toes',
    }
    if low in mapping:
        return f'{side} {mapping[low]}'
    return f'{side} {body.replace("_", " ")}'


def run(bpy, arm, cfg, mod_dir, ws_root, log, apply_all):
    m_cfg = cfg['model']
    a_cfg = m_cfg.get('adapt_humanoid') or {}
    ref_path = os.path.join(ws_root, 'work', 'soul', 'skeleton-reference.json')
    ref = json.load(open(ref_path, encoding='utf-8'))
    info, pose = ref['FinalRefBoneInfo'], ref['FinalRefBonePose']
    idx_of = {b['Name'].lower(): i for i, b in enumerate(info)}

    m_ue_bl = Matrix.Diagonal((1, -1, 1, 1))
    orig_glob_ue = {}

    def get_orig_glob_ue(i):
        if i not in orig_glob_ue:
            p = pose[i]
            q = Quaternion((p['Rotation']['W'], p['Rotation']['X'], p['Rotation']['Y'], p['Rotation']['Z']))
            t = Vector((p['Translation']['X'], p['Translation']['Y'], p['Translation']['Z']))
            m = Matrix.Translation(t) @ q.to_matrix().to_4x4()
            par = info[i]['ParentIndex']
            orig_glob_ue[i] = m if par < 0 else get_orig_glob_ue(par) @ m
        return orig_glob_ue[i]

    def soul_pos_bl(name):
        return (m_ue_bl @ get_orig_glob_ue(idx_of[name.lower()]) @ m_ue_bl).translation

    def soul_dir_bl(a, b):
        return (soul_pos_bl(b) - soul_pos_bl(a)).normalized()

    meshes = [o for o in bpy.data.objects if o.type == 'MESH']

    # ---------- 1. 规范化骨骼命名（全角空格、Blender .L/.R 后缀）、合并辅助骨，清理 *_end 末端叶骨 ----------
    merge_bones = a_cfg.get('merge_bones') or {}
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones
    for b in eb:
        b.use_connect = False
    ren = {b.name: _canonical_humanoid_name(b.name) for b in eb if _canonical_humanoid_name(b.name) != b.name}
    for old, new in ren.items():
        eb[old].name = new
    for b in list(eb):
        if b.name.endswith('_end'):
            if b.parent:
                b.parent.tail = b.head.copy()
            eb.remove(b)
    for src_b, dst_b in merge_bones.items():
        if src_b in eb and dst_b in eb:
            for c in list(eb):
                if c.parent == eb[src_b]:
                    c.parent = eb[dst_b]
            eb.remove(eb[src_b])
    bpy.ops.object.mode_set(mode='OBJECT')

    for m in meshes:
        for g in m.vertex_groups:
            cn = _canonical_humanoid_name(g.name)
            if cn != g.name:
                g.name = cn
        for src_b, dst_b in merge_bones.items():
            src_g = m.vertex_groups.get(src_b)
            if not src_g:
                continue
            dst_g = m.vertex_groups.get(dst_b) or m.vertex_groups.new(name=dst_b)
            si = src_g.index
            for v in m.data.vertices:
                w = sum(g.weight for g in v.groups if g.group == si)
                if w > 1e-6:
                    dst_g.add([v.index], w, 'ADD')
            m.vertex_groups.remove(src_g)

    # ---------- 2. 提前烘焙形态键 ----------
    for m in meshes:
        keys = m.data.shape_keys
        if not keys:
            continue
        for key, val in m_cfg.get('shape_keys', {}).get(_base_name(m.name), {}).items():
            short = key.rsplit('.', 1)[-1]
            if short in keys.key_blocks:
                keys.key_blocks[short].value = val
            else:
                log('缺少形态键', m.name, key)
        log('形态键', _base_name(m.name), {kb.name: round(kb.value, 2) for kb in keys.key_blocks[1:] if abs(kb.value) > 1e-6})
        bpy.context.view_layer.objects.active = m
        bpy.ops.object.shape_key_remove(all=True, apply_mix=True)

    # ---------- 3. T-pose 下前臂轴向适配 hand_l/r 长度（袖口刚性平移零形变）+ 手掌放大 ----------
    hl_t = pose[idx_of['hand_l']]['Translation']
    default_la = (hl_t['X'] ** 2 + hl_t['Y'] ** 2 + hl_t['Z'] ** 2) ** 0.5
    target_la = float(a_cfg.get('target_forearm_cm', default_la))
    hand_scale = float(a_cfg.get('hand_scale', 1.12))
    cuff_margin = float(a_cfg.get('cuff_margin_cm', 2.0))
    extra_pat = a_cfg.get('arm_extra_pattern', 'Frill|Sleeve|Cuff')
    arm_tree_re = re.compile(rf'^(Left|Right) (Upper Arm|Lower Arm|{extra_pat}|Hand|Thumb|Index|Middle|Ring|Little)')
    hand_tree_re = re.compile(r'^(Left|Right) (Hand|Thumb|Index|Middle|Ring|Little)')
    new_heads, new_tails = {}, {}

    for side in ('Left', 'Right'):
        S = arm.data.bones[f'{side} Upper Arm'].head_local.copy()
        E = arm.data.bones[f'{side} Lower Arm'].head_local.copy()
        W = arm.data.bones[f'{side} Hand'].head_local.copy()
        ua_len = (E - S).length
        la_len = (W - E).length
        chain = ua_len + la_len
        axis = (W - S).normalized()
        W_new = S + axis * (ua_len + target_la)

        def map_d_shaft(d, ua_len=ua_len, la_len=la_len, chain=chain):
            if d <= ua_len:
                return d
            if d <= chain - cuff_margin:
                return ua_len + (d - ua_len) * ((target_la - cuff_margin) / (la_len - cuff_margin))
            return d + (target_la - la_len)

        for m in meshes:
            gi_arm = {g.index for g in m.vertex_groups if g.name.startswith(side) and arm_tree_re.match(g.name)}
            gi_hand = {g.index for g in m.vertex_groups if g.name.startswith(side) and hand_tree_re.match(g.name)}
            if not gi_arm and not gi_hand:
                continue
            for v in m.data.vertices:
                a = min(1.0, sum(g.weight for g in v.groups if g.group in gi_arm))
                if a > 0.0:
                    d = (v.co - S).dot(axis)
                    v.co += axis * ((map_d_shaft(d) - d) * a)
                ah = min(1.0, sum(g.weight for g in v.groups if g.group in gi_hand))
                if ah > 0.0:
                    v.co = W_new + (v.co - W_new) * (1.0 + (hand_scale - 1.0) * ah)

        for b in arm.data.bones:
            if b.name.startswith(side) and arm_tree_re.match(b.name):
                for attr, store in (('head_local', new_heads), ('tail_local', new_tails)):
                    p = getattr(b, attr).copy()
                    d = (p - S).dot(axis)
                    p_mapped = p + axis * (map_d_shaft(d) - d)
                    if hand_tree_re.match(b.name):
                        p_mapped = W_new + (p_mapped - W_new) * hand_scale
                    store[b.name, attr] = p_mapped

    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    for child_b, parent_b in a_cfg.get('reparent_bones', {}).items():
        if child_b in arm.data.edit_bones and parent_b in arm.data.edit_bones:
            arm.data.edit_bones[child_b].parent = arm.data.edit_bones[parent_b]
    for (bname, _), val in new_heads.items():
        arm.data.edit_bones[bname].head = val
    for (bname, _), val in new_tails.items():
        arm.data.edit_bones[bname].tail = val
    bpy.ops.object.mode_set(mode='OBJECT')

    # ---------- 4. 对齐魂静止姿势并烘焙（Apply Pose as Rest Pose）+ 鞋底落地 ----------
    align_pairs = [
        ('Left Upper Leg',  'Left Lower Leg',        soul_dir_bl('thigh_l', 'calf_l')),
        ('Left Lower Leg',  'Left Foot',             soul_dir_bl('calf_l', 'foot_l')),
        ('Right Upper Leg', 'Right Lower Leg',       soul_dir_bl('thigh_r', 'calf_r')),
        ('Right Lower Leg', 'Right Foot',            soul_dir_bl('calf_r', 'foot_r')),
        ('Left Shoulder',   'Left Upper Arm',        soul_dir_bl('clavicle_l', 'upperarm_l')),
        ('Left Upper Arm',  'Left Lower Arm',        soul_dir_bl('upperarm_l', 'lowerarm_l')),
        ('Left Lower Arm',  'Left Hand',             soul_dir_bl('lowerarm_l', 'hand_l')),
        ('Left Hand',       'Left Middle Proximal',  soul_dir_bl('hand_l', 'middle_01_l')),
        ('Right Shoulder',  'Right Upper Arm',       soul_dir_bl('clavicle_r', 'upperarm_r')),
        ('Right Upper Arm', 'Right Lower Arm',       soul_dir_bl('upperarm_r', 'lowerarm_r')),
        ('Right Lower Arm', 'Right Hand',            soul_dir_bl('lowerarm_r', 'hand_r')),
        ('Right Hand',      'Right Middle Proximal', soul_dir_bl('hand_r', 'middle_01_r')),
    ]
    for side, s in (('Left', 'l'), ('Right', 'r')):
        for fn, sn in (('Thumb', 'thumb'), ('Index', 'index'), ('Middle', 'middle'), ('Ring', 'ring'), ('Little', 'pinky')):
            align_pairs += [
                (f'{side} {fn} Proximal',     f'{side} {fn} Intermediate', soul_dir_bl(f'{sn}_01_{s}', f'{sn}_02_{s}')),
                (f'{side} {fn} Intermediate', f'{side} {fn} Distal',       soul_dir_bl(f'{sn}_02_{s}', f'{sn}_03_{s}')),
                (f'{side} {fn} Distal',       None,                        soul_dir_bl(f'{sn}_02_{s}', f'{sn}_03_{s}')),
            ]

    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='POSE')
    for b_name, child_name, target_dir in align_pairs:
        if b_name not in arm.pose.bones or (child_name and child_name not in arm.pose.bones):
            continue
        pb = arm.pose.bones[b_name]
        cur_dir = ((arm.pose.bones[child_name].head - pb.head) if child_name else (pb.tail - pb.head)).normalized()
        q = cur_dir.rotation_difference(target_dir)
        pb.matrix = Matrix.Translation(pb.head) @ q.to_matrix().to_4x4() @ Matrix.Translation(-pb.head) @ pb.matrix
        bpy.context.view_layer.update()
    bpy.ops.object.mode_set(mode='OBJECT')

    for m in meshes:
        for md in [md for md in m.modifiers if md.type == 'ARMATURE']:
            bpy.context.view_layer.objects.active = m
            bpy.ops.object.modifier_apply(modifier=md.name)

    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode='OBJECT')

    for m in meshes:
        md = m.modifiers.new('Armature', 'ARMATURE')
        md.object = arm

    sole = {s.replace('_', ' ') for s in m_cfg.get('sole_bones', [])} | set(m_cfg.get('sole_bones', []))
    low = 0.0
    for m in meshes:
        gi = {g.index for g in m.vertex_groups if g.name in sole}
        for v in m.data.vertices:
            if any(g.group in gi and g.weight > 0.5 for g in v.groups):
                low = min(low, v.co.z)
    if abs(low) > 1e-4:
        for o in bpy.data.objects:
            if o.type in ('ARMATURE', 'MESH'):
                o.matrix_world = Matrix.Translation((0, 0, -low)) @ o.matrix_world
        apply_all()
    log('静止姿势烘焙后鞋底落地(cm)', round(-low, 2))

    # ---------- 5. 细分脊柱/颈部与 16 根四肢 Twist 骨 + 躯干向量平行对齐 + 线性重分配权重 ----------
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones

    p_nk = eb['Neck'].head.copy()
    p_hd = eb['Head'].head.copy()
    r_sp, r_ch, r_nk = eb['Spine'].roll, eb['Chest'].roll, eb['Neck'].roll

    p_sh_l = eb['Left Shoulder'].head.copy()
    p_ua_l = eb['Left Upper Arm'].head.copy()
    v_clav_l = soul_pos_bl('clavicle_l') - soul_pos_bl('spine_05')
    v_ua_l = soul_pos_bl('upperarm_l') - soul_pos_bl('clavicle_l')
    if p_sh_l.x > 0.45 * p_ua_l.x:
        s_ua = (0.68 * p_ua_l.x) / v_ua_l.x
        p_sh_l = p_ua_l - s_ua * v_ua_l
        eb['Left Shoulder'].head = p_sh_l
        eb['Right Shoulder'].head = Vector((-p_sh_l.x, p_sh_l.y, p_sh_l.z))
    s_clav = p_sh_l.x / v_clav_l.x
    p_uch = Vector((0.0, p_sh_l.y - s_clav * v_clav_l.y, p_sh_l.z - s_clav * v_clav_l.z))

    s_pelvis = soul_pos_bl('pelvis')
    v_sp_tot = soul_pos_bl('spine_05') - s_pelvis
    hips_z = eb['Hips'].head.z
    ul_z = eb['Left Upper Leg'].head.z
    if hips_z - ul_z > 5.0:
        v_hip_l = soul_pos_bl('thigh_l') - s_pelvis
        hips_z = ul_z - (eb['Left Upper Leg'].head.x / v_hip_l.x) * v_hip_l.z
    s_spine = (p_uch.z - hips_z) / v_sp_tot.z
    p_hips = p_uch - s_spine * v_sp_tot

    def spine_pt(soul_name):
        return p_hips + s_spine * (soul_pos_bl(soul_name) - s_pelvis)

    eb['Hips'].head = p_hips
    p_sp = spine_pt('spine_01')
    p_sp2 = spine_pt('spine_02')
    p_sp3 = spine_pt('spine_03')
    p_ch = spine_pt('spine_04')

    eb['Spine'].head = p_sp
    eb['Chest'].head = p_ch
    sp2 = eb.new('Spine_02');   sp2.head = p_sp2; sp2.tail = p_sp3.copy();           sp2.roll = r_sp
    sp3 = eb.new('Spine_03');   sp3.head = p_sp3; sp3.tail = p_ch.copy();            sp3.roll = r_sp
    uch = eb.new('UpperChest'); uch.head = p_uch; uch.tail = p_nk.copy();            uch.roll = r_ch
    nk2 = eb.new('Neck_02');    nk2.head = p_nk.lerp(p_hd, 0.50); nk2.tail = p_hd.copy(); nk2.roll = r_nk

    eb['Hips'].tail = p_sp.copy()
    eb['Spine'].tail = sp2.head.copy()
    sp2.parent = eb['Spine']
    sp3.parent = sp2
    eb['Chest'].parent = sp3; eb['Chest'].tail = uch.head.copy()
    uch.parent = eb['Chest']
    eb['Neck'].parent = uch; eb['Neck'].tail = nk2.head.copy()
    nk2.parent = eb['Neck']
    eb['Head'].parent = nk2
    for side in ('Left', 'Right'):
        eb[f'{side} Shoulder'].parent = uch
    for child_b, parent_b in a_cfg.get('reparent_bones', {}).items():
        if child_b in eb and parent_b in eb:
            eb[child_b].parent = eb[parent_b]

    for side, s in (('Left', 'l'), ('Right', 'r')):
        ua_b = eb[f'{side} Upper Arm']; la_b = eb[f'{side} Lower Arm']; hd_b = eb[f'{side} Hand']
        ul_b = eb[f'{side} Upper Leg']; ll_b = eb[f'{side} Lower Leg']; ft_b = eb[f'{side} Foot']; to_b = eb[f'{side} Toes']
        ua_b.tail = la_b.head.copy(); la_b.tail = hd_b.head.copy()
        ul_b.tail = ll_b.head.copy(); ll_b.tail = ft_b.head.copy(); ft_b.tail = to_b.head.copy()

        def make_twist(name, parent_eb, p0, p1, f0, f1):
            tb = eb.new(name)
            tb.head = p0.lerp(p1, f0)
            tb.tail = p0.lerp(p1, f1)
            tb.roll = parent_eb.roll
            tb.parent = parent_eb

        make_twist(f'upperarm_twist_01_{s}', ua_b, ua_b.head, la_b.head, 1.0 / 3.0, 2.0 / 3.0)
        make_twist(f'upperarm_twist_02_{s}', ua_b, ua_b.head, la_b.head, 2.0 / 3.0, 1.0)
        make_twist(f'lowerarm_twist_02_{s}', la_b, la_b.head, hd_b.head, 1.0 / 3.0, 2.0 / 3.0)
        make_twist(f'lowerarm_twist_01_{s}', la_b, la_b.head, hd_b.head, 2.0 / 3.0, 1.0)
        make_twist(f'thigh_twist_01_{s}',    ul_b, ul_b.head, ll_b.head, 1.0 / 3.0, 2.0 / 3.0)
        make_twist(f'thigh_twist_02_{s}',    ul_b, ul_b.head, ll_b.head, 2.0 / 3.0, 1.0)
        make_twist(f'calf_twist_02_{s}',     ll_b, ll_b.head, ft_b.head, 1.0 / 3.0, 2.0 / 3.0)
        make_twist(f'calf_twist_01_{s}',     ll_b, ll_b.head, ft_b.head, 2.0 / 3.0, 1.0)

    bpy.ops.object.mode_set(mode='OBJECT')

    def seg_weights(co, pA, pB, nodes):
        ab = pB - pA; L2 = ab.length_squared
        t = max(0.0, min(1.0, (co - pA).dot(ab) / L2)) if L2 > 1e-6 else 0.0
        n = len(nodes) - 1; x = t * n; k = min(n - 1, int(x)); f = x - k
        res = [(nodes[k], 1.0 - f)]
        if f > 1e-4:
            res.append((nodes[k + 1], f))
        return res

    seg_map = {
        'Spine': (p_sp, p_ch, ['Spine', 'Spine_02', 'Spine_03']),
        'Chest': (p_ch, p_nk, ['Chest', 'UpperChest']),
        'Neck':  (p_nk, p_hd, ['Neck', 'Neck_02']),
    }
    for side, s in (('Left', 'l'), ('Right', 'r')):
        ua = arm.data.bones[f'{side} Upper Arm'].head_local.copy()
        la = arm.data.bones[f'{side} Lower Arm'].head_local.copy()
        hd = arm.data.bones[f'{side} Hand'].head_local.copy()
        ul = arm.data.bones[f'{side} Upper Leg'].head_local.copy()
        ll = arm.data.bones[f'{side} Lower Leg'].head_local.copy()
        ft = arm.data.bones[f'{side} Foot'].head_local.copy()
        seg_map[f'{side} Upper Arm'] = (ua, la, [f'{side} Upper Arm', f'upperarm_twist_01_{s}', f'upperarm_twist_02_{s}'])
        seg_map[f'{side} Lower Arm'] = (la, hd, [f'{side} Lower Arm', f'lowerarm_twist_02_{s}', f'lowerarm_twist_01_{s}'])
        seg_map[f'{side} Upper Leg'] = (ul, ll, [f'{side} Upper Leg', f'thigh_twist_01_{s}',    f'thigh_twist_02_{s}'])
        seg_map[f'{side} Lower Leg'] = (ll, ft, [f'{side} Lower Leg', f'calf_twist_02_{s}',     f'calf_twist_01_{s}'])

    for m in meshes:
        gname_of = {g.index: g.name for g in m.vertex_groups}
        if not any(gn in seg_map for gn in gname_of.values()):
            continue
        for _, _, nodes in seg_map.values():
            for nd in nodes:
                if nd not in m.vertex_groups:
                    m.vertex_groups.new(name=nd)
        vg_by_name = {g.name: g for g in m.vertex_groups}
        for v in m.data.vertices:
            updates = defaultdict(float)
            to_clear = []
            for g in v.groups:
                gn = gname_of.get(g.group)
                if gn in seg_map and g.weight > 1e-5:
                    to_clear.append(gn)
                    pA, pB, nodes = seg_map[gn]
                    for target_n, sw in seg_weights(v.co, pA, pB, nodes):
                        updates[target_n] += g.weight * sw
            if to_clear:
                for gn in to_clear:
                    vg_by_name[gn].remove([v.index])
                for target_n, w in updates.items():
                    if w > 1e-5:
                        vg_by_name[target_n].add([v.index], w, 'REPLACE')

    # ---------- 6. 生成全身比例适配的魂参考姿势 soul.adapted_pose_json ----------
    def c_ue(bname):
        p = arm.data.bones[bname].head_local
        return Vector((p.x, -p.y, p.z))

    target_pos_ue = {
        'root':     Vector((0.0, 0.0, 0.0)),
        'pelvis':   c_ue('Hips'),
        'spine_01': c_ue('Spine'),
        'spine_02': c_ue('Spine_02'),
        'spine_03': c_ue('Spine_03'),
        'spine_04': c_ue('Chest'),
        'spine_05': c_ue('UpperChest'),
        'neck_01':  c_ue('Neck'),
        'neck_02':  c_ue('Neck_02'),
        'head':     c_ue('Head'),
    }
    for side, s in (('Left', 'l'), ('Right', 'r')):
        target_pos_ue.update({
            f'clavicle_{s}':          c_ue(f'{side} Shoulder'),
            f'upperarm_{s}':          c_ue(f'{side} Upper Arm'),
            f'upperarm_twist_01_{s}': c_ue(f'upperarm_twist_01_{s}'),
            f'upperarm_twist_02_{s}': c_ue(f'upperarm_twist_02_{s}'),
            f'lowerarm_{s}':          c_ue(f'{side} Lower Arm'),
            f'lowerarm_twist_02_{s}': c_ue(f'lowerarm_twist_02_{s}'),
            f'lowerarm_twist_01_{s}': c_ue(f'lowerarm_twist_01_{s}'),
            f'hand_{s}':              c_ue(f'{side} Hand'),
            f'thigh_{s}':             c_ue(f'{side} Upper Leg'),
            f'thigh_twist_01_{s}':    c_ue(f'thigh_twist_01_{s}'),
            f'thigh_twist_02_{s}':    c_ue(f'thigh_twist_02_{s}'),
            f'calf_{s}':              c_ue(f'{side} Lower Leg'),
            f'calf_twist_02_{s}':     c_ue(f'calf_twist_02_{s}'),
            f'calf_twist_01_{s}':     c_ue(f'calf_twist_01_{s}'),
            f'foot_{s}':              c_ue(f'{side} Foot'),
            f'ball_{s}':              c_ue(f'{side} Toes'),
        })

    new_glob_ue = {}
    adapted_local = {}
    for i, b in enumerate(info):
        n = b['Name'].lower()
        par = b['ParentIndex']
        q_glob = get_orig_glob_ue(i).to_quaternion()
        p = pose[i]
        orig_t = Vector((p['Translation']['X'], p['Translation']['Y'], p['Translation']['Z']))
        if n in target_pos_ue:
            t_glob = target_pos_ue[n]
            new_t = t_glob if par < 0 else (new_glob_ue[par].inverted() @ t_glob)
            adapted_local[n] = [round(new_t.x, 6), round(new_t.y, 6), round(new_t.z, 6)]
        else:
            new_t = orig_t
            t_glob = new_t if par < 0 else (new_glob_ue[par] @ new_t)
        new_glob_ue[i] = Matrix.Translation(t_glob) @ q_glob.to_matrix().to_4x4()

    rel_out = cfg.get('soul', {}).get('adapted_pose_json', 'work/soul_adapted_pose.json')
    out_adapted = os.path.join(mod_dir, rel_out)
    os.makedirs(os.path.dirname(out_adapted), exist_ok=True)
    json.dump(adapted_local, open(out_adapted, 'w', encoding='utf-8'), indent=2)

    # ---------- 7. 将骨骼名与顶点组名中的空格/连字符/点号统一替换为 '_' ----------
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    ren_final = {}
    for b in arm.data.edit_bones:
        clean = re.sub(r'[\s\u3000\-\.]+', '_', b.name)
        if clean != b.name:
            ren_final[b.name] = clean
    for old, new in ren_final.items():
        arm.data.edit_bones[old].name = new
    bpy.ops.object.mode_set(mode='OBJECT')

    for m in meshes:
        for g in m.vertex_groups:
            clean = re.sub(r'[\s\u3000\-\.]+', '_', g.name)
            if clean != g.name:
                g.name = clean

    log('Humanoid 全身骨骼适配完成，总骨骼数', len(arm.data.bones), '改名', len(ren_final), '已写出', out_adapted, '条目', len(adapted_local))
