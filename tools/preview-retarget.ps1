# 离线预览重定向效果（不用进游戏）：导出 mod.json 里 preview_anims 列出的魂的动作 →
# 用 UE 里 mod 的重定向器算出替换模型的姿势（与游戏里同一个处理器）→ 打印每段动作的手误差（cm）→
# Blender 渲染对比图并统计手臂穿模（mods\<mod>\work\<变体>\retarget\<动作名>.png）
# 前提：刚用 build-mod.ps1 构建过同一个 mod 和变体（UE 工程只有一份，所有 mod 共用）
# 用法：pwsh tools\preview-retarget.ps1 [-Mod <名字>] [-Count 4] [-NoRender]
param([string]$Mod = '', [int]$Count = 4, [switch]$NoRender)
. "$PSScriptRoot\common.ps1"
$modDir = Resolve-Mod $Mod
$cfg = Get-ModConfig $modDir
Check (Test-Path $PbzActiveMod) 'UE 工程里还没有生成任何 mod：先运行 tools\build-mod.ps1'
$active, $Variant, $mode = ([IO.File]::ReadAllText($PbzActiveMod)).Split('|')
Check ($active -eq $env:PBZ_MOD) "UE 工程里当前是 mod $active，不是 $env:PBZ_MOD：先 pwsh tools\build-mod.ps1 -Mod $env:PBZ_MOD"
Write-Host "mod: $active  变体: $Variant  ($mode)"
$workDir = Join-Path $modDir "work\$Variant"
$animDir = Join-Path $PbzRoot '.tmp\preview\anim'
New-Item -ItemType Directory -Force -Path $animDir | Out-Null
Get-ChildItem $animDir -Filter *.json | Remove-Item

Step '导出魂的动作'
foreach ($a in @($cfg.preview_anims)) {
    $n = ($a -split '/')[-1]
    $o = & $PbzExportExe anim $a --out (Join-Path $animDir "$n.json") --count $Count 2>&1
    if ($LASTEXITCODE -ne 0) { $o | Select-Object -Last 5; throw "导出动作失败: $a" }
}

Step 'UE 计算替换模型的姿势'
$poses = Join-Path $workDir 'poses.json'
$env:PBZ_PREVIEW_ANIM_DIR = $animDir; $env:PBZ_PREVIEW_OUT = $poses
Invoke-UEPython "$PSScriptRoot\ue\preview_retarget.py" 'PBZPREVIEW' (Join-Path $PbzLogs 'ue-preview.log')

if (-not $NoRender) {
    Step 'Blender 渲染对比图'
    $psk = Get-ChildItem (Join-Path $PbzRoot 'work\soul') -Filter 'soul_body.psk*' | Select-Object -First 1
    Check ($null -ne $psk) '缺 work\soul\soul_body.pskx：重跑 tools\extract-game-refs.ps1'
    $blog = Join-Path $PbzLogs "blender-preview-$active.log"; Remove-Item $blog -ErrorAction SilentlyContinue
    Invoke-Blender "$PSScriptRoot\blender\render_retarget.py" @($modDir, $poses, $psk.FullName, (Join-Path $workDir 'retarget')) $blog 'RETARGET' (Join-Path $workDir "SK_$($cfg.mod_name).blend")
}
Step "完成：mods\$active\work\$Variant\"
