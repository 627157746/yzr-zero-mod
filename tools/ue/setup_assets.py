# -*- coding: utf-8 -*-
# 在 UE 编辑器（命令行、无界面）里生成一个 mod 的全部资源。由 tools\build-mod.ps1 调用，参数来自 soul.json + mods\<mod>\mod.json。
# 环境变量：PBZ_ROOT（工作区）、PBZ_MOD_DIR（mod 目录）、PBZ_VARIANT（变体名）、PBZ_PHYS（1=带次级物理）
# 生成：/Game/<mod_name>/ 下的模型、骨架、IK Rig、重定向器、物理资产、后处理动画蓝图；
#       覆盖魂的 SkinParts/Hats（空网格）、布料（空布料资产）、显示部件（替换模型）和材质实例用到的贴图
import json, os, re, sys, traceback
import unreal as u

ROOT = os.environ['PBZ_ROOT']
MOD_DIR = os.environ['PBZ_MOD_DIR']
VARIANT = os.environ['PBZ_VARIANT']
PHYS = os.environ.get('PBZ_PHYS', '1') == '1'
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pbzcfg
CFG = pbzcfg.load(ROOT, MOD_DIR)
S, R, P = CFG['soul'], CFG['retarget'], CFG.get('physics', {})
NAME = CFG['mod_name']
FOLDER = f'/Game/{NAME}'
STUBS = f'/Game/{NAME}Stubs'
MODEL_DIR = os.path.join(MOD_DIR, 'work', VARIANT)
TOOLS = u.AssetToolsHelpers.get_asset_tools()
EAL = u.EditorAssetLibrary
LIB = u.PbzModEditorLibrary
SRC, TGT = u.RetargetSourceOrTarget.SOURCE, u.RetargetSourceOrTarget.TARGET

# 魂这一侧的重定向链（游戏骨架固定，UE5 标准命名）。替换模型在 mod.json 的 retarget.chains 里用同样的链名
SOUL_CHAINS = {'Spine': ('spine_01', 'spine_05'), 'Neck': ('neck_01', 'neck_02'), 'Head': ('Head', 'Head'),
               'LeftClavicle': ('clavicle_l', 'clavicle_l'), 'RightClavicle': ('clavicle_r', 'clavicle_r'),
               'LeftArm': ('upperarm_l', 'hand_l'), 'RightArm': ('upperarm_r', 'hand_r'),
               'LeftLeg': ('thigh_l', 'foot_l'), 'RightLeg': ('thigh_r', 'foot_r'),
               'LeftToe': ('ball_l', 'ball_l'), 'RightToe': ('ball_r', 'ball_r')}
for side, s in (('Left', 'l'), ('Right', 'r')):
    for f, tf in (('thumb', 'Thumb'), ('index', 'Index'), ('middle', 'Middle'), ('ring', 'Ring'), ('pinky', 'Pinky')):
        SOUL_CHAINS[f'{side}{tf}'] = (f'{f}_01_{s}', f'{f}_03_{s}')
    for b_pre, c_pre in (('upperarm', 'UpperArm'), ('lowerarm', 'LowerArm'), ('thigh', 'Thigh'), ('calf', 'Calf')):
        SOUL_CHAINS[f'{side}{c_pre}Twist1'] = (f'{b_pre}_twist_01_{s}', f'{b_pre}_twist_01_{s}')
        SOUL_CHAINS[f'{side}{c_pre}Twist2'] = (f'{b_pre}_twist_02_{s}', f'{b_pre}_twist_02_{s}')
MODEL_CHAINS = {k: tuple(v) for k, v in R['chains'].items()}
ARM_POLE = {'LeftArm': -float(R.get('arm_pole_deg', 10.0)), 'RightArm': float(R.get('arm_pole_deg', 10.0))}

def log(*a): u.log('PBZSETUP ' + ' '.join(str(x) for x in a))
def save(asset): EAL.save_loaded_asset(asset, only_if_is_dirty=False)

def create(path, cls, factory):
    folder, name = path.rsplit('/', 1)
    a = TOOLS.create_asset(name, folder, cls, factory)
    assert a, f'创建失败 {path}'
    return a

def duplicate(path, source):
    folder, name = path.rsplit('/', 1)
    a = TOOLS.duplicate_asset(name, folder, source)
    assert a, f'复制失败 {path}'
    return a

def enum_member(enum, raw):
    key = raw.split('::')[-1].replace('_', '').upper()
    for n in dir(enum):
        if n.replace('_', '').upper() == key:
            return getattr(enum, n)
    raise KeyError(f'{enum.__name__} 没有 {raw}')

def import_skeletal(fbx, path):
    assert os.path.exists(fbx), f'缺 {fbx}'
    folder, name = path.rsplit('/', 1)
    ui = u.FbxImportUI()
    for k, v in (('import_as_skeletal', True), ('import_mesh', True), ('mesh_type_to_import', u.FBXImportType.FBXIT_SKELETAL_MESH),
                 ('automated_import_should_detect_type', False), ('import_materials', False), ('import_textures', False),
                 ('import_animations', False), ('create_physics_asset', False)):
        ui.set_editor_property(k, v)
    sk = ui.get_editor_property('skeletal_mesh_import_data')
    sk.set_editor_property('normal_import_method', u.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)
    sk.set_editor_property('import_morph_targets', False)
    sk.set_editor_property('convert_scene', True)
    t = u.AssetImportTask()
    for k, v in (('filename', fbx), ('destination_path', folder), ('destination_name', name), ('automated', True),
                 ('replace_existing', True), ('save', True), ('options', ui)):
        t.set_editor_property(k, v)
    TOOLS.import_asset_tasks([t])
    mesh = u.load_asset(path)
    assert isinstance(mesh, u.SkeletalMesh), f'导入失败 {fbx}'
    return mesh

def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)
def qrot(q, v):
    x, y, z, w = q; vx, vy, vz = v
    tx, ty, tz = 2*(y*vz - z*vy), 2*(z*vx - x*vz), 2*(x*vy - y*vx)
    return (vx + w*tx + (y*tz - z*ty), vy + w*ty + (z*tx - x*tz), vz + w*tz + (x*ty - y*tx))
def ref_global(ref, name):
    info, pose = ref['FinalRefBoneInfo'], ref['FinalRefBonePose']
    i = next(k for k, b in enumerate(info) if b['Name'].lower() == name.lower())
    chain = []
    while i >= 0:
        chain.append(i); i = info[i]['ParentIndex']
    q, t = (0, 0, 0, 1), (0, 0, 0)
    for i in reversed(chain):
        p = pose[i]; lq = tuple(p['Rotation'][k] for k in 'XYZW'); lt = tuple(p['Translation'][k] for k in 'XYZ')
        r = qrot(q, lt); t = tuple(t[j] + r[j] for j in range(3)); q = qmul(q, lq)
    return t

def used_slots():
    return json.load(open(os.path.join(MODEL_DIR, 'model_info.json'), encoding='utf-8'))['slots']

def stub_materials(slots):
    # 编辑器占位材质（不进包）：网格材质槽指向游戏材质实例的路径，游戏里加载的是原版材质
    base = create(f'{STUBS}/M_Placeholder', u.Material, u.MaterialFactoryNew())
    base.set_editor_property('used_with_skeletal_mesh', True)
    save(base)
    mis = {}
    for slot in slots:
        mi = create(CFG['materials'][slot], u.MaterialInstanceConstant, u.MaterialInstanceConstantFactoryNew())
        u.MaterialEditingLibrary.set_material_instance_parent(mi, base)
        save(mi); mis[slot] = mi
    return mis

def textures(slots):
    # 覆盖游戏材质实例引用的贴图：路径、虚拟纹理、sRGB、压缩方式与原贴图一致。只处理当前变体用到的材质槽
    done = []
    for spec in CFG['textures']:
        if spec['slot'] not in slots:
            continue
        folder, name = spec['target'].rsplit('/', 1)
        src = os.path.join(MOD_DIR, spec['source'])
        assert os.path.exists(src), f'缺贴图源文件 {src}'
        t = u.AssetImportTask()
        for k, v in (('filename', src), ('destination_path', folder), ('destination_name', name), ('automated', True), ('replace_existing', True), ('save', False)):
            t.set_editor_property(k, v)
        TOOLS.import_asset_tasks([t])
        tex = u.load_asset(spec['target'])
        assert isinstance(tex, u.Texture2D), f'贴图导入失败 {spec["target"]}'
        kind = spec['kind']
        tex.set_editor_property('srgb', kind == 'BC')
        tex.set_editor_property('compression_settings', u.TextureCompressionSettings.TC_NORMALMAP if kind == 'N' else u.TextureCompressionSettings.TC_DEFAULT)
        tex.set_editor_property('flip_green_channel', kind == 'N' and spec.get('opengl_normal', True))   # Unity/Blender 法线是 OpenGL 方向
        tex.set_editor_property('compression_no_alpha', kind == 'ARM')
        tex.set_editor_property('lod_group', enum_member(u.TextureGroup, spec['lod_group']))
        tex.set_editor_property('virtual_texture_streaming', True)
        save(tex); done.append(spec['target'])
    log('贴图覆盖', len(done))
    return done

def arm_scale_of(bone):
    sc = S.get('arm_scale', {})
    for k in ('clavicle', 'upperarm', 'lowerarm'):
        if bone in (f'{k}_l', f'{k}_r'):
            return float(sc.get(k, 1.0))
    return 1.0

def build_soul():
    blank = import_skeletal(os.path.join(ROOT, 'work', 'soul', 'SK_SoulBlank.fbx'), S['blank_mesh'])
    skel = blank.get_editor_property('skeleton')
    assert skel.get_path_name().split('.')[0] == S['skeleton'], f'魂骨架路径不对: {skel.get_path_name()}（blank_mesh 必须和游戏骨架在同一目录、同名网格）'
    # 参考姿势改成游戏里的精确值（FBX 往返有误差）。arm_scale 不是 1 时缩短手臂参考平移（见 references 的“握武器”）
    ref = json.load(open(os.path.join(ROOT, 'work', 'soul', 'skeleton-reference.json'), encoding='utf-8'))
    poses = {b['Name'].lower(): p for b, p in zip(ref['FinalRefBoneInfo'], ref['FinalRefBonePose'])}
    mod = u.SkeletonModifier(); assert mod.set_skeletal_mesh(blank)
    names = list(mod.get_all_bone_names()); trs = []
    adapted = json.load(open(os.path.join(MOD_DIR, S['adapted_pose_json']), encoding='utf-8')) if S.get('adapted_pose_json') else {}
    for n in names:
        key = str(n).lower()
        p = poses[key]; t = u.Transform(); s = arm_scale_of(key)
        if key in adapted:
            t.translation = u.Vector(*[float(v) for v in adapted[key]])
        else:
            t.translation = u.Vector(*(p['Translation'][k] * s for k in 'XYZ'))
        t.rotation = u.Quat(*(p['Rotation'][k] for k in 'XYZW'))
        t.scale3d = u.Vector(*(p['Scale3D'][k] for k in 'XYZ'))
        trs.append(t)
    assert mod.set_bones_transforms(names, trs, True) and mod.commit_skeleton_to_skeletal_mesh(), '设置魂参考姿势失败'
    chk = u.SkeletonModifier(); chk.set_skeletal_mesh(blank)
    got = chk.get_bone_transform('hand_r', True).translation; want = ref_global(ref, 'hand_r')
    err = max(abs(got.x - want[0]), abs(got.y - want[1]), abs(got.z - want[2]))
    scaled = bool(adapted) or any(abs(float(v) - 1.0) > 1e-6 for v in S.get('arm_scale', {}).values())
    log('魂空身体 骨骼', len(names), '全身适配', len(adapted), '手臂缩放', S.get('arm_scale', {}), 'hand_r 相对原版(cm)', round(err, 2))
    assert (1 < err < 60) if scaled else (err < 0.05), '魂参考姿势与预期不符'
    save(blank)
    pa = LIB.create_empty_physics_asset(S['physics_asset']); assert pa; save(pa)
    body = duplicate(S['body'], blank)
    body.set_editor_property('physics_asset', pa)   # 保留原版物理资产引用：受击判定靠它
    save(body)
    for p in S['blank_parts']:
        save(duplicate(p, blank))
    for p in S['blank_cloth']:
        save(create(p, u.ChaosClothAsset, u.ChaosClothAssetFactory()))
    log('魂部件：身体 1，空部件', len(S['blank_parts']), '，空布料', len(S['blank_cloth']))
    return blank

def build_model(mis):
    mesh = import_skeletal(os.path.join(MODEL_DIR, f'SK_{NAME}.fbx'), f'{FOLDER}/SK_{NAME}')
    mats = mesh.get_editor_property('materials')
    for i in range(len(mats)):
        s = mats[i]; slot = str(s.get_editor_property('material_slot_name'))
        assert slot in mis, f'未知材质槽 {slot}'
        s.set_editor_property('material_interface', mis[slot]); mats[i] = s
    mesh.set_editor_property('materials', mats)
    # 动作幅度大时姿势离参考姿势很远，扩大包围盒避免被提前剔除
    mesh.set_editor_property('positive_bounds_extension', u.Vector(150, 150, 100))
    mesh.set_editor_property('negative_bounds_extension', u.Vector(150, 150, 50))
    save(mesh)
    m = u.SkeletonModifier(); m.set_skeletal_mesh(mesh)
    M = CFG['model']
    foot = m.get_bone_transform(M['left_foot'], True).translation; toe = m.get_bone_transform(M['left_toe'], True).translation
    log('模型 骨骼', len(list(m.get_all_bone_names())), '左脚', (round(foot.x, 1), round(foot.y, 1)), '左脚尖', (round(toe.x, 1), round(toe.y, 1)))
    assert foot.x > 0 and toe.y > foot.y, '模型朝向和魂不一致（UE 里魂朝 +Y、左侧在 +X）'
    return mesh

def ik_rig(path, mesh, root, chains, limbs=None):
    rig = create(path, u.IKRigDefinition, u.IKRigDefinitionFactory())
    c = u.IKRigController.get_controller(rig)
    assert c.set_skeletal_mesh(mesh), f'{path} 设置网格失败'
    assert c.set_retarget_root(root), f'{path} 根骨骼 {root} 失败'
    for name, (a, b) in chains.items():
        r = str(c.add_retarget_chain(name, a, b, ''))
        assert r == name, f'{path} 链 {name} ({a} -> {b}) 失败: {r}'
    # 手部 IK：每只手一个目标 + 肢体解算器，重定向时把手拉到魂的手的位置（武器挂在魂的手上）
    for chain, (goal, start, end) in (limbs or {}).items():
        g = str(c.add_new_goal(goal, end))
        assert g == goal, f'{path} 目标 {goal} 失败: {g}'
        s = c.add_solver('/Script/IKRig.IKRigLimbSolver')
        assert s >= 0, f'{path} 添加肢体解算器失败'
        assert c.connect_goal_to_solver(goal, s), f'{path} 目标 {goal} 连不上解算器'
        assert c.set_start_bone(start, s) and str(c.get_start_bone(s)) == start, f'{path} 解算器起始骨骼 {start} 失败'
        assert c.set_retarget_chain_goal(chain, goal), f'{path} 链 {chain} 绑定目标 {goal} 失败'
    save(rig)
    return rig

def retargeter(path, src_rig, tgt_rig, src_mesh, tgt_mesh):
    rt = create(path, u.IKRetargeter, u.IKRetargetFactory())
    c = u.IKRetargeterController.get_controller(rt)
    c.set_ik_rig(SRC, src_rig); c.set_ik_rig(TGT, tgt_rig)
    c.set_preview_mesh(SRC, src_mesh); c.set_preview_mesh(TGT, tgt_mesh)
    c.remove_all_ops()
    c.add_default_ops()
    pole = c.add_retarget_op('/Script/IKRig.IKRetargetAlignPoleVectorOp')
    assert pole >= 0, '添加极向量操作失败'
    c.move_retarget_op_in_stack(pole, 2)   # FK 链之后、IK 之前：先把肘部往外转，IK 再把手拉回武器上
    c.assign_ik_rig_to_all_ops(SRC, src_rig); c.assign_ik_rig_to_all_ops(TGT, tgt_rig)
    ops = {}
    for i in range(c.get_num_retarget_ops()):
        ctl = c.get_op_controller(i)
        kind = ctl.get_class().get_name() if ctl else 'none'
        ops[i] = (str(c.get_op_name(i)), kind)
        c.run_op_initial_setup(i)
        if 'RootMotion' in kind or 'CurveRemap' in kind:
            c.set_retarget_op_enabled(i, False)   # 根运动和曲线由魂的组件承担
    c.auto_map_chains(u.AutoMapChainType.EXACT, True)
    fk = next(n for n, k in ops.values() if 'FKChains' in k)
    run_ik = next(n for n, k in ops.values() if 'RunIKRig' in k)
    arms = [a for a in ('LeftArm', 'RightArm') if R.get('hand_ik', True) and a in MODEL_CHAINS]
    unmapped = [n for n in MODEL_CHAINS if str(c.get_source_chain(n, fk)) != n] + \
               [f'IK:{n}' for n in arms if str(c.get_source_chain(n, run_ik)) != n]
    log('未对上的链', unmapped)
    assert not unmapped, '重定向链映射不完整'
    if arms:
        ik_ctl = next(c.get_op_controller(i) for i, (n, k) in ops.items() if 'IKChains' in k)
        st = ik_ctl.get_settings(); chains = list(st.get_editor_property('chains_to_retarget')); done = []
        for i, ch in enumerate(chains):
            name = str(ch.get_editor_property('target_chain_name'))
            if name in arms:
                ch.set_editor_property('enable_ik', True)
                ch.set_editor_property('blend_to_source', 1.0)
                ch.set_editor_property('blend_to_source_translation', 1.0)   # 手的位置完全取魂的手（武器在那里）
                ch.set_editor_property('blend_to_source_rotation', 0.0)
                chains[i] = ch; done.append(name)
        st.set_editor_property('chains_to_retarget', chains); ik_ctl.set_settings(st)
        assert sorted(done) == sorted(arms), f'手臂 IK 链没有生成: {done}'
    # 可选：手指旋转幅度 retarget.finger_alpha（默认不写 = 1.0）。魂的待机手是半握的「爪」形，
    # 手指细长的模型照搬会显得像异形；0.5~0.85 能把手指放松成半弯。
    if R.get('finger_alpha') is not None:
        fk_ctl = next(c.get_op_controller(i) for i, (n, k) in ops.items() if 'FKChains' in k)
        fs = fk_ctl.get_settings(); fc = list(fs.get_editor_property('chains_to_retarget')); fdone = []
        fingers = {f'{s}{t}' for s in ('Left', 'Right') for t in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky')}
        for i, ch in enumerate(fc):
            name = str(ch.get_editor_property('target_chain_name'))
            if name in fingers:
                ch.set_editor_property('rotation_alpha', float(R['finger_alpha']))
                fc[i] = ch; fdone.append(name)
        fs.set_editor_property('chains_to_retarget', fc); fk_ctl.set_settings(fs)
        got = sorted({str(ch.get_editor_property('target_chain_name')) for ch in fk_ctl.get_settings().get_editor_property('chains_to_retarget')
                      if str(ch.get_editor_property('target_chain_name')) in fingers and abs(ch.get_editor_property('rotation_alpha') - float(R['finger_alpha'])) < 1e-4})
        log('手指旋转幅度', R['finger_alpha'], '链', len(fdone), '读回生效', len(got))
        assert len(got) == len(fingers), '手指旋转幅度没生效'
    pole_ctl = next(c.get_op_controller(i) for i, (n, k) in ops.items() if 'AlignPoleVector' in k)
    ps = pole_ctl.get_settings(); pchains = list(ps.get_editor_property('chains_to_align'))
    want = {k: v for k, v in ARM_POLE.items() if k in MODEL_CHAINS}
    for i, ch in enumerate(pchains):
        name = str(ch.get_editor_property('target_chain_name'))
        if name in want:
            ch.set_editor_property('enabled', True)
            ch.set_editor_property('align_alpha', 0.0)
            ch.set_editor_property('static_angular_offset', want[name])
            pchains[i] = ch
    ps.set_editor_property('chains_to_align', pchains); pole_ctl.set_settings(ps)
    got = {str(ch.get_editor_property('target_chain_name')): ch.get_editor_property('static_angular_offset')
           for ch in pole_ctl.get_settings().get_editor_property('chains_to_align') if ch.get_editor_property('enabled')}
    log('重定向操作', [(n, c.get_retarget_op_enabled(i)) for i, (n, k) in ops.items()], '手臂张开(度)', got)
    assert got == want, '手臂张开设置没生效'
    # 模型多半是 T 姿势、魂是 A 姿势：新建重定向姿势，把模型骨骼方向对齐到魂（已在 Blender 固化成魂姿势的 mod 可设 skip_auto_align=true）
    if not R.get('skip_auto_align', False):
        pose = c.create_retarget_pose('AlignToSoul', TGT)
        c.set_current_retarget_pose(pose, TGT)
        c.auto_align_all_bones(TGT, u.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)
    save(rt)
    return rt

def secondary_physics(path, mesh):
    pa = LIB.create_physics_asset_from_mesh(path, mesh, float(P.get('min_bone_size', 1.0)))
    assert pa, '生成物理资产失败'
    skip = re.compile(P.get('skip_match', r'^$'))
    kin = {b: g for g, bones in P.get('kinematic_colliders', {}).items() for b in bones}
    pair_groups = {g for pair in P.get('collide_pairs', []) for g in pair}
    groups, demoted = {}, []
    bodies = [str(x) for x in LIB.get_physics_body_bones(pa)]
    for b in bodies:
        if b in kin:
            LIB.configure_body(pa, b, False, False, 0.0, 0.0, kin[b] in pair_groups)   # 跟随动画，只当挡板
            groups.setdefault(kin[b], []).append(b); continue
        g = None if skip.search(b) else next((x for x in P.get('groups', []) if re.search(x['match'], b)), None)
        if g and LIB.configure_joint(pa, b, g['swing'], g['twist'], g['stiff'], g['damp']):
            LIB.configure_body(pa, b, True, g['gravity'], g['lin'], g['ang'], g['name'] in pair_groups or g.get('collide', False))
            groups.setdefault(g['name'], []).append(b)
        else:
            if g: demoted.append(b)   # 没有关节连着的刚体不能模拟，否则会掉下去
            LIB.configure_body(pa, b, False, False, 0.0, 0.0, False)
            groups.setdefault('follow', []).append(b)
    of = {b: g for g, v in groups.items() for b in v}
    opened = {}
    for i, a in enumerate(bodies):
        for b in bodies[i + 1:]:
            for x, y in P.get('collide_pairs', []):
                if {of[a], of[b]} == {x, y}:
                    assert LIB.set_body_pair_collision(pa, a, b, True), f'打开碰撞对失败 {a} × {b}'
                    opened[f'{x}×{y}'] = opened.get(f'{x}×{y}', 0) + 1
    for k, v in groups.items():
        log('物理分组', k, len(v), v[:4])
    log('无关节改为跟随', len(demoted), demoted[:6], '碰撞对', opened)
    for x, y in P.get('collide_pairs', []):
        assert opened.get(f'{x}×{y}', 0) > 0, f'{x}×{y} 没有生成碰撞对（分组里没有刚体？）'
    save(pa)
    return pa

def anim_bp(path, skel, mesh, rt, pa):
    f = u.AnimBlueprintFactory()
    f.set_editor_property('target_skeleton', skel)
    f.set_editor_property('preview_skeletal_mesh', mesh)
    f.set_editor_property('parent_class', u.AnimInstance.static_class())
    abp = create(path, u.AnimBlueprint, f)
    text = LIB.build_retarget_post_process_graph(abp, rt, pa, float(P.get('world_alpha', 0.6)) if pa else 0.0)
    ok = text.startswith('OK')
    log(path.rsplit('/', 1)[-1], '编译', ok, '|', text.replace('\n', ' | ')[:600])
    assert ok, f'{path} 编译失败'
    save(abp)
    return abp

def main():
    u.SystemLibrary.execute_console_command(None, 'Interchange.FeatureFlags.Import.FBX 0')   # 用传统 FBX 导入，选项可控
    u.SystemLibrary.execute_console_command(None, 'Interchange.FeatureFlags.Import.SyncToBrowser 0')   # 命令行没有界面，同步内容浏览器会崩
    bad = [k for k in MODEL_CHAINS if k not in SOUL_CHAINS]
    assert not bad, f'retarget.chains 里有魂没有的链名: {bad}，可用 {sorted(SOUL_CHAINS)}'
    slots = used_slots()
    mis = stub_materials(slots)
    tex = textures(slots)
    soul = build_soul()
    model = build_model(mis)
    skel = model.get_editor_property('skeleton')
    ik_src = ik_rig(f'{FOLDER}/IK_Soul', soul, 'Pelvis', {k: SOUL_CHAINS[k] for k in MODEL_CHAINS})
    limbs = {}
    if R.get('hand_ik', True):
        for a in ('LeftArm', 'RightArm'):
            if a in MODEL_CHAINS:
                limbs[a] = (a.replace('Arm', 'HandGoal'), MODEL_CHAINS[a][0], MODEL_CHAINS[a][1])
    ik_tgt = ik_rig(f'{FOLDER}/IK_{NAME}', model, CFG['model']['root_bone'], MODEL_CHAINS, limbs)
    rt = retargeter(f'{FOLDER}/RTG_Soul_To_{NAME}', ik_src, ik_tgt, soul, model)
    pa = secondary_physics(f'{FOLDER}/PA_{NAME}_Secondary', model) if PHYS and P.get('groups') else None
    abp = anim_bp(f'{FOLDER}/ABP_{NAME}', skel, model, rt, pa)
    band = duplicate(S['visible_part'], model)   # 替换模型显示在魂的这个部件上
    assert LIB.set_post_process_anim_blueprint(band, abp)
    save(band)
    for d in (STUBS, '/Game/PBZContent', FOLDER, '/HuaShira'):
        if EAL.does_directory_exist(d):
            EAL.save_directory(d, only_if_is_dirty=False, recursive=True)
    info = {'variant': VARIANT, 'phys': bool(pa), 'slots': slots, 'textures': tex,
            'skeleton': f'{FOLDER}/SK_{NAME}_Skeleton', 'anim_bp': f'{FOLDER}/ABP_{NAME}',
            'retargeter': f'{FOLDER}/RTG_Soul_To_{NAME}', 'physics': f'{FOLDER}/PA_{NAME}_Secondary' if pa else None}
    json.dump(info, open(os.path.join(MODEL_DIR, 'setup_info.json'), 'w', encoding='utf-8'), indent=1)
    log('变体', VARIANT, '物理', bool(pa))
    log('DONE')

try:
    main()
except Exception:
    u.log_error('PBZSETUP FAILED\n' + traceback.format_exc())
