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
    out = {'soul_bones': soul_bones, 'model_bones': model_bones, 'soul_ref': ref_globals(soul), 'model_ref': ref_globals(model), 'anims': []}
    wanted = {b.lower() for b in soul_bones}
    worst = 0.0
    for path in sorted(glob.glob(os.path.join(os.environ['PBZ_PREVIEW_ANIM_DIR'], '*.json'))):
        a = json.load(open(path, encoding='utf-8'))
        idx = [i for i, b in enumerate(a['bones']) if b.lower() in wanted]
        names = [a['bones'][i] for i in idx]
        k = len(a['poses'])
        local = []
        for pose in a['poses']:
            for i in idx:
                v = pose[i]; r = ratios.get(a['bones'][i].lower(), 1.0)
                local.append(u.Transform(u.Vector(v[4] * r, v[5] * r, v[6] * r), u.Quat(v[0], v[1], v[2], v[3]).rotator(), u.Vector(v[7], v[8], v[9])))
        src = LIB.local_poses_to_component_space(soul, names, local, k)
        assert len(src) == len(soul_bones) * k, f'{a["name"]}: 源姿势转换失败'
        tgt = LIB.retarget_component_poses(rt, soul, model, src, k)
        assert len(tgt) == len(model_bones) * k, f'{a["name"]}: 重定向失败'
        nh, nt = len(soul_bones), len(model_bones)
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
        out['anims'].append(rec)
    dst = os.environ['PBZ_PREVIEW_OUT']
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    json.dump(out, open(dst, 'w', encoding='utf-8'))
    log('写出', dst, '动作数', len(out['anims']), '最大手误差(cm)', worst)
    log('DONE')

try:
    main()
except Exception:
    u.log_error('PBZPREVIEW FAILED\n' + traceback.format_exc())
