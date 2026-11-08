# -*- coding: utf-8 -*-
# 检查打包好的 mod：同时挂上游戏原版和 mod 容器，确认覆盖生效、引用指向正确（不进游戏也能查的部分）
# 用法：python tools/verify_release.py <mod 目录> <release 子目录名> <变体名>   （由 build-mod.ps1 调用，环境变量来自 common.ps1）
import json, os, subprocess, sys

ROOT = os.environ['PBZ_ROOT']
MOD_DIR, REL, VARIANT = sys.argv[1:4]
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pbzcfg
CFG = pbzcfg.load(ROOT, MOD_DIR)
INFO = json.load(open(os.path.join(MOD_DIR, 'work', VARIANT, 'setup_info.json'), encoding='utf-8'))
EXE = os.path.join(ROOT, 'tools', 'PbzExport', 'bin', 'Release', 'net10.0', 'PbzExport.exe')
MODS = os.path.join(MOD_DIR, 'release', REL, 'ProjectPBZ', 'Content', 'Paks', '~mods')
OUT = os.path.join(ROOT, '.tmp', 'verify', os.path.basename(MOD_DIR), REL)
os.makedirs(OUT, exist_ok=True)
S = CFG['soul']
errors = []

def file_path(pkg):
    if pkg.startswith('/Game/'): return 'ProjectPBZ/Content/' + pkg[6:]
    if pkg.startswith('/HuaShira/'): return 'ProjectPBZ/Plugins/HuaShira/Content/' + pkg[10:]
    raise ValueError(pkg)

def run(*a):
    r = subprocess.run([EXE, *a, '--with', MODS], capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.returncode, (r.stdout + r.stderr).strip()

def check(ok, msg):
    print(('OK   ' if ok else 'FAIL ') + msg)
    if not ok: errors.append(msg)

def ref(v): return v.get('ObjectPath', '') if isinstance(v, dict) else str(v)

def props(pkg, typ):
    path = os.path.join(OUT, pkg.rsplit('/', 1)[-1] + '.json')
    code, out = run('json', file_path(pkg), '--out', path)
    if code != 0: return None, out
    return next((o for o in json.load(open(path, encoding='utf-8')) if o.get('Type') == typ), None), out

def source(pkg):
    code, out = run('where', file_path(pkg))
    return code, (out.splitlines()[-1].split('<-')[-1].strip() if code == 0 and out else out[-200:])

# 1) 覆盖：这些文件必须由 mod 容器提供；没用到的材质槽的贴图不应被覆盖
for pkg in [S['body'], S['visible_part']] + S['blank_parts'] + S['blank_cloth'] + INFO['textures']:
    code, src = source(pkg)
    check(code == 0 and CFG['container'] in src, f'覆盖 {pkg.rsplit("/", 1)[-1]} <- {src}')
for t in CFG['textures']:
    if t['target'] not in INFO['textures']:
        code, src = source(t['target'])
        check(code == 0 and CFG['container'] not in src, f'不覆盖 {t["target"].rsplit("/", 1)[-1]} <- {src}')
# 2) 魂的骨架必须仍来自游戏（被覆盖会破坏全部动画）
code, src = source(S['skeleton'])
check(code == 0 and CFG['container'] not in src, f'骨架未被覆盖 <- {src}')

# 3) 显示部件：替换模型的骨架、动画蓝图、材质
band, out = props(S['visible_part'], 'SkeletalMesh')
check(band is not None, f'读取显示部件 {"" if band else out[-300:]}')
if band:
    p = band.get('Properties', {})
    check(INFO['skeleton'] in ref(p.get('Skeleton')), f'显示部件骨架 = {ref(p.get("Skeleton"))}')
    check(INFO['anim_bp'] in ref(p.get('PostProcessAnimBlueprint')), f'显示部件后处理动画蓝图 = {ref(p.get("PostProcessAnimBlueprint"))}')
    mats = sorted(ref(s.get('Material')).split('.')[0] for s in band.get('SkeletalMaterials', []))
    check(mats == sorted(CFG['materials'][s] for s in INFO['slots']), f'显示部件材质 = {mats}')

# 4) 空身体保留游戏骨架和物理资产（受击判定）
body, out = props(S['body'], 'SkeletalMesh')
check(body is not None, f'读取身体 {"" if body else out[-300:]}')
if body:
    p = body.get('Properties', {})
    check(S['skeleton'] in ref(p.get('Skeleton')), f'身体骨架 = {ref(p.get("Skeleton"))}')
    check(S['physics_asset'] in ref(p.get('PhysicsAsset')), f'身体物理资产 = {ref(p.get("PhysicsAsset"))}')

# 5) 动画蓝图、重定向器、物理资产已烹饪进包
items = [(INFO['anim_bp'], 'AnimBlueprintGeneratedClass'), (INFO['retargeter'], 'IKRetargeter')]
if INFO.get('physics'): items.append((INFO['physics'], 'PhysicsAsset'))
for pkg, typ in items:
    code, out = run('json', file_path(pkg), '--out', os.path.join(OUT, pkg.rsplit('/', 1)[-1] + '.json'))
    check(code == 0 and typ in out, f'{pkg.rsplit("/", 1)[-1]} 含 {typ}')

print(f'\n{"全部通过" if not errors else f"{len(errors)} 项失败"}')
sys.exit(1 if errors else 0)
