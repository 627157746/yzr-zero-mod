# 从游戏里导出魂的参考数据（所有 mod 共用，游戏更新后重跑）：
#   work\soul\skeleton-reference.json  骨架参考姿势（骨骼名、父子关系、局部 TRS）
#   work\soul\SK_SoulBlank.fbx         只有魂骨架 + 一个小三角形的空网格（覆盖魂的身体/头/头发等部件）
#   work\soul\soul_body.pskx           魂的身体网格（预览对比图用）
# 用法：pwsh tools\extract-game-refs.ps1
. "$PSScriptRoot\common.ps1"
$S = [IO.File]::ReadAllText((Join-Path $PbzRoot 'soul.json')) | ConvertFrom-Json
$out = Join-Path $PbzRoot 'work\soul'; $tmp = Join-Path $PbzRoot '.tmp\export'
New-Item -ItemType Directory -Force -Path $out, $tmp | Out-Null

Step '导出魂的骨架'
$skelJson = Join-Path $tmp 'soul-skeleton.json'
& $PbzExportExe json $S.skeleton_game_path --out $skelJson | Select-Object -Last 1
Check ($LASTEXITCODE -eq 0) "导出骨架失败: $($S.skeleton_game_path)"
& $PbzPython -c @"
import json, sys
d = json.load(open(sys.argv[1], encoding='utf-8'))
ref = next(o for o in d if o.get('Type') == 'Skeleton')['ReferenceSkeleton']
json.dump({'FinalRefBoneInfo': ref['FinalRefBoneInfo'], 'FinalRefBonePose': ref['FinalRefBonePose']}, open(sys.argv[2], 'w', encoding='utf-8'), indent=1)
print('骨骼数', len(ref['FinalRefBoneInfo']))
"@ $skelJson (Join-Path $out 'skeleton-reference.json')
Check ($LASTEXITCODE -eq 0) '整理骨架参考姿势失败'

Step '导出魂的身体网格（不含插槽骨骼）'
$pskDir = Join-Path $tmp 'psk-nosocket'
if (Test-Path $pskDir) { Remove-Item -Recurse -Force $pskDir }
& $PbzExportExe export $S.body_game_path --out $pskDir --mesh psk --sockets none | Select-Object -Last 1
Check ($LASTEXITCODE -eq 0) "导出身体失败: $($S.body_game_path)"
$psk = Get-ChildItem $pskDir -Recurse -Include *.pskx, *.psk | Select-Object -First 1
Check ($null -ne $psk) "没有导出 psk: $pskDir"
Copy-Item $psk.FullName (Join-Path $out "soul_body$($psk.Extension)") -Force

Step 'Blender 生成空身体'
$blog = Join-Path $PbzLogs 'blender-blank.log'; Remove-Item $blog -ErrorAction SilentlyContinue
Invoke-Blender "$PSScriptRoot\blender\make_hun_blank.py" @($psk.FullName, (Join-Path $out 'skeleton-reference.json'), (Join-Path $out 'SK_SoulBlank.fbx')) $blog 'HUNBLANK'
Step '完成'
