# -*- coding: utf-8 -*-
# 离线预览：把魂的真实动作（PbzExport anim 导出的局部变换）用 mod 的重定向器算出替换模型的姿势。
# 模拟游戏里的 AnimationScaled：动画平移 × (空身体网格参考长度 / 骨架参考长度)，所以 soul.arm_scale 的效果也能看到。
# 输出每段动作的“手误差”（模型的手离魂的手/武器握点多远，cm）和两边的组件空间姿势 JSON。只读资源，不保存任何东西。
# 环境变量：PBZ_ROOT、PBZ_MOD_DIR、PBZ_PREVIEW_ANIM_DIR（输入 JSON 目录）、PBZ_PREVIEW_OUT（输出 JSON 文件）
import glob, json, os, sys, traceback
import unreal as u

ROOT = os.environ['PBZ_ROOT']
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pbzcfg
CFG = pbzcfg.load(ROOT, os.environ['PBZ_MOD_DIR'])
NAME = CFG['mod_name']
LIB = u.PbzModEditorLibrary

def log(*a): u.log('PBZPREVIEW ' + ' '.join(str(x) for x in a))
def tr(t):
    q, p = t.rotation, t.translation
    return [round(q.x, 6), round(q.y, 6), round(q.z, 6), round(q.w, 6), round(p.x, 3), round(p.y, 3), round(p.z, 3)]

# ---- 双手握持统计用的小工具（tr() 格式 [qx,qy,qz,qw,px,py,pz]，不依赖 numpy）----
def _qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + y * tz - z * ty, v[1] + w * ty + z * tx - x * tz, v[2] + w * tz + x * ty - y * tx]
def _to_local(t, p):
    """世界（组件空间）点 p 换到变换 t 的局部空间"""
    q = t[:4]; qi = [-q[0], -q[1], -q[2], q[3]]
    return _qrot(qi, [p[i] - t[4 + i] for i in range(3)])
def _to_world(t, p):
    r = _qrot(t[:4], p)
    return [r[i] + t[4 + i] for i in range(3)]
def _dist(a, b): return sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5

def local_transform(v, translation_ratio=1.0):
    # ACL 导出四元数可能略偏离单位长度；先归一化，再转 Rotator（与引擎求值后的旋转一致）。
    # 未归一化时，Euler 换算会把压缩误差放大为接近 1cm 的手位误差。
    n = sum(x * x for x in v[:4]) ** 0.5
    assert n > 1e-8, '动画包含无效四元数'
    rotation = u.Quat(*(x / n for x in v[:4])).rotator()
    return u.Transform(u.Vector(*(x * translation_ratio for x in v[4:7])), rotation, u.Vector(*v[7:10]))

def ref_globals(mesh):
    m = u.SkeletonModifier(); assert m.set_skeletal_mesh(mesh)
    return [tr(m.get_bone_transform(n, True)) for n in LIB.get_mesh_bone_names(mesh)]

def scaled_ratios(mesh):
    ref = json.load(open(os.path.join(ROOT, 'work', 'soul', 'skeleton-reference.json'), encoding='utf-8'))
    skel = {b['Name'].lower(): p['Translation'] for b, p in zip(ref['FinalRefBoneInfo'], ref['FinalRefBonePose'])}
    m = u.SkeletonModifier(); assert m.set_skeletal_mesh(mesh)
    out = {}
    for n in LIB.get_mesh_bone_names(mesh):
        s = skel.get(str(n).lower())
        if not s: continue
        a = sum(s[k] ** 2 for k in 'XYZ') ** 0.5
        t = m.get_bone_transform(n, False).translation
        b = (t.x ** 2 + t.y ** 2 + t.z ** 2) ** 0.5
        if a > 1e-4 and abs(b / a - 1) > 1e-3:
            out[str(n).lower()] = b / a
    return out

def grip_stats(a, rec, raw, names, before_src, before_tgt, soul, rt, model, k, nh, nt, diag, i_hr, i_hl, i_wr, m_hl, zero_r, acc):
    """双手握持统计：原版比例下左手在武器（weapon_r）局部空间的位置 = 「应该握的地方」。
    对左手离刀轴 < zero_radius 的帧（=原版左手贴着武器），量模型左手在同一武器空间里离「原版握持线」的径向距离（抓没抓住刀柄）
    和沿刀轴的滑动量（够不着时故意往护手滑）。before = 不加后处理的重定向结果，after = 当前结果。"""
    rawcs = LIB.local_poses_to_component_space(soul, names, raw, k)
    assert len(rawcs) == nh * k, f'{a["name"]}: 原比例姿势转换失败'
    if before_tgt is None:
        before_tgt = LIB.retarget_component_poses(rt, soul, model, before_src, k)
        assert len(before_tgt) == nt * k, f'{a["name"]}: 重定向（修正前）失败'
    rec['soul_before'] = [[tr(before_src[f * nh + b]) for b in range(nh)] for f in range(k)]
    rec['model_before'] = [[tr(before_tgt[f * nt + b]) for b in range(nt)] for f in range(k)]
    rec['grip_alpha'] = [round(d.translation.x, 5) for d in diag] if diag else [0.0] * k
    rows, targets = [], []
    for f in range(k):
        w0 = tr(rawcs[f * nh + i_wr]); l0 = tr(rawcs[f * nh + i_hl])
        p0 = _to_local(w0, l0[4:7])                     # 原版左手在武器空间（刀身沿 +Z）
        rho0 = (p0[0] ** 2 + p0[1] ** 2) ** 0.5
        w = rec['soul'][f][i_wr]                         # 游戏里武器挂在魂的 weapon_r 上（后处理不改它）
        if rho0 >= zero_r:
            targets.append(None)
            continue
        targets.append([round(x, 3) for x in _to_world(w, p0)])
        res = {}
        for tag, pose in (('before', tr(before_tgt[f * nt + m_hl])), ('after', rec['model'][f][m_hl])):
            p = _to_local(w, pose[4:7])
            res[tag] = (((p[0] - p0[0]) ** 2 + (p[1] - p0[1]) ** 2) ** 0.5, p[2] - p0[2], _dist(p, p0))
        alpha = round(diag[f].translation.x, 2) if diag else None
        rows.append((a['frames'][f], round(rho0, 1), alpha, res))
    rec['grip_target'] = targets
    rec['grip_measurements'] = rows
    if not rows:
        log(a['name'], '双手握持：原版左手没有贴在武器上（无握柄帧）')
        return
    mx = lambda tag, j: max(abs(r[3][tag][j]) for r in rows)
    acc['frames'] += len(rows)
    acc['before'] = max(acc['before'], mx('before', 0)); acc['after'] = max(acc['after'], mx('after', 0))
    log(a['name'], f'双手握持：握柄帧 {len(rows)}/{k}',
        f'离握持线(径向cm) 修正前最大 {mx("before", 0):.1f} → 修正后最大 {mx("after", 0):.1f}',
        f'| 离原握点(cm) 前 {mx("before", 2):.1f} → 后 {mx("after", 2):.1f}',
        f'| 沿刀轴滑动(cm) 后 {min(r[3]["after"][1] for r in rows):.1f}~{max(r[3]["after"][1] for r in rows):.1f}',
        f'| alpha {sorted({r[2] for r in rows if r[2] is not None})}' if diag else '')

def main():
    soul = u.load_asset(CFG['soul']['blank_mesh'])
    model = u.load_asset(f'/Game/{NAME}/SK_{NAME}')
    rt = u.load_asset(f'/Game/{NAME}/RTG_Soul_To_{NAME}')
    assert soul and model and rt, '缺 UE 资源：先运行 tools\\build-mod.ps1'
    soul_bones = [str(n) for n in LIB.get_mesh_bone_names(soul)]
    model_bones = [str(n) for n in LIB.get_mesh_bone_names(model)]
    chains = CFG['retarget']['chains']
    hands = [(s, chains[c][1]) for s, c in (('hand_l', 'LeftArm'), ('hand_r', 'RightArm')) if c in chains]
    ratios = scaled_ratios(soul)
    log('AnimationScaled 比例', {k: round(v, 3) for k, v in ratios.items()})
    # 双手握持修正（soul.two_hand_grip）：启用时魂的组件姿势用真的后处理蓝图 ABP_SoulGrip 跑出来（和游戏里同一个资产）
    G = CFG['soul'].get('two_hand_grip') or {}
    grip_cls = None
    if G.get('enabled'):
        gp = CFG['soul']['body'].rsplit('/', 1)[0] + '/ABP_SoulGrip'
        grip_bp = u.load_asset(gp)
        assert grip_bp, f'缺 {gp}：先运行 tools\\build-mod.ps1'
        grip_cls = u.load_object(None, gp + '.ABP_SoulGrip_C')
        assert grip_cls and isinstance(grip_cls, u.Class), f'{gp} 没有生成类: {grip_cls}'
        log('双手握持修正 启用：魂的姿势 = 动画 →', gp)
    zero_r = float(G.get('zero_radius_cm', 20.0))
    i_hr, i_hl, i_wr = soul_bones.index('hand_r'), soul_bones.index('hand_l'), soul_bones.index('weapon_r')
    m_hl = model_bones.index(chains['LeftArm'][1]) if 'LeftArm' in chains else None
    grip_worst = {'before': 0.0, 'after': 0.0, 'frames': 0}
    out = {'soul_bones': soul_bones, 'model_bones': model_bones, 'soul_ref': ref_globals(soul), 'model_ref': ref_globals(model), 'anims': []}
    wanted = {b.lower() for b in soul_bones}
    worst = 0.0
    for path in sorted(glob.glob(os.path.join(os.environ['PBZ_PREVIEW_ANIM_DIR'], '*.json'))):
        a = json.load(open(path, encoding='utf-8'))
        idx = [i for i, b in enumerate(a['bones']) if b.lower() in wanted]
        names = [a['bones'][i] for i in idx]
        k = len(a['poses'])
        local = []
        raw = []
        for pose in a['poses']:
            for i in idx:
                v = pose[i]; r = ratios.get(a['bones'][i].lower(), 1.0)
                local.append(local_transform(v, r))
                raw.append(local_transform(v))
        src = LIB.local_poses_to_component_space(soul, names, local, k)
        assert len(src) == len(soul_bones) * k, f'{a["name"]}: 源姿势转换失败'
        nh, nt = len(soul_bones), len(model_bones)
        before = src
        diag = []
        if grip_cls:
            pp = LIB.evaluate_post_process_poses(soul, grip_cls, names, local, k, 4, 'GripAlpha', 'GripOffset')
            assert len(pp) == (nh + 1) * k, f'{a["name"]}: 后处理蓝图求值失败（返回 {len(pp)} 个变换）'
            src = [pp[f * (nh + 1) + b] for f in range(k) for b in range(nh)]
            diag = [pp[f * (nh + 1) + nh] for f in range(k)]
        tgt = LIB.retarget_component_poses(rt, soul, model, src, k)
        assert len(tgt) == len(model_bones) * k, f'{a["name"]}: 重定向失败'
        rec = {'name': a['name'], 'frames': a['frames'],
               'soul': [[tr(src[f * nh + b]) for b in range(nh)] for f in range(k)],
               'model': [[tr(tgt[f * nt + b]) for b in range(nt)] for f in range(k)]}
        errs = []
        for f in range(k):
            for sb, mb in hands:
                h = rec['soul'][f][soul_bones.index(sb)]; t = rec['model'][f][model_bones.index(mb)]
                errs.append(round(sum((h[4 + j] - t[4 + j]) ** 2 for j in range(3)) ** 0.5, 1))
        worst = max([worst] + errs)
        log(a['name'], '帧', a['frames'], '手误差(cm) 左/右交替', errs)
        if m_hl is not None:
            grip_stats(a, rec, raw, names, before, tgt if not grip_cls else None, soul, rt, model, k, nh, nt, diag,
                       i_hr, i_hl, i_wr, m_hl, zero_r, grip_worst)
        out['anims'].append(rec)
    dst = os.environ['PBZ_PREVIEW_OUT']
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    json.dump(out, open(dst, 'w', encoding='utf-8'))
    log('写出', dst, '动作数', len(out['anims']), '最大手误差(cm)', worst)
    if grip_worst['frames']:
        log('双手握持 汇总：握柄帧', grip_worst['frames'], '模型左手离原版握持线(径向,cm) 修正前最大', round(grip_worst['before'], 1),
            '修正后最大', round(grip_worst['after'], 1))
    log('DONE')

try:
    main()
except Exception:
    u.log_error('PBZPREVIEW FAILED\n' + traceback.format_exc())
