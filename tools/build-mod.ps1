# 一键生成一个 mod（全部在工作区内完成，不碰游戏目录）：
#   [mod 的预处理脚本] → Blender 处理模型/贴图 + 预览图 → UE 生成资源 → 烹饪 → IoStore 打包 → 检查成品
# 输出：mods\<mod>\release\<变体>-<phys|basic>\ProjectPBZ\Content\Paks\~mods\（3 个文件）
# 前提：已运行 tools\build-tools.ps1 和 tools\extract-game-refs.ps1
# 用法：pwsh tools\build-mod.ps1 [-Mod <mods 下的文件夹名>] [-Variant <mod.json 的变体名>] [-NoPhys] [-SkipPrepare]
#   UE 工程所有 mod 共用：同一时间只能构建一个 mod，构建会清掉上一次生成的覆盖资源
param([string]$Mod = '', [string]$Variant = '', [switch]$NoPhys, [switch]$SkipPrepare)
. "$PSScriptRoot\common.ps1"
$modDir = Resolve-Mod $Mod
$cfg = Get-ModConfig $modDir
if (-not $Variant) { $Variant = @($cfg.variants.PSObject.Properties.Name)[0] }
Check ($null -ne $cfg.variants.$Variant) "mod.json 里没有变体 $Variant（有: $(@($cfg.variants.PSObject.Properties.Name) -join ', ')）"
$mode = if ($NoPhys) { 'basic' } else { 'phys' }
$rel = "$Variant-$mode"
$name = $cfg.mod_name
$workDir = Join-Path $modDir "work\$Variant"
$proj = Join-Path $PbzRoot 'ProjectPBZ'
Check (Test-Path (Join-Path $PbzRoot 'work\soul\SK_SoulBlank.fbx')) '缺 work\soul\SK_SoulBlank.fbx：先运行 tools\extract-game-refs.ps1'
Check (Test-Path $PbzExportExe) '缺 PbzExport：先运行 tools\build-tools.ps1'
Write-Host "mod: $env:PBZ_MOD（$name）  变体: $Variant  物理: $(-not $NoPhys)"

if (-not $SkipPrepare) {
    if ($cfg.package) {
        Step "解包：$($cfg.package.unitypackage)"
        & $PbzPython "$PSScriptRoot\extract_package.py" $modDir
        Check ($LASTEXITCODE -eq 0) "解包失败: $($cfg.package.unitypackage)"
    }
    if ($cfg.prepare_script) {
        Step "预处理：$($cfg.prepare_script)"
        & $PbzPython (Join-Path $modDir $cfg.prepare_script) $modDir
        Check ($LASTEXITCODE -eq 0) "预处理脚本失败: $($cfg.prepare_script)"
    }
    Step "Blender：贴图 + 模型（变体 $Variant）"
    $blog = Join-Path $PbzLogs "blender-$env:PBZ_MOD.log"; Remove-Item $blog -ErrorAction SilentlyContinue
    Invoke-Blender "$PSScriptRoot\blender\make_textures.py" @($modDir) $blog 'TEX'
    Invoke-Blender "$PSScriptRoot\blender\prepare_model.py" @($modDir, $Variant, $workDir) $blog 'MODEL'
    Step '预览图（静止姿势）'
    $blend = Join-Path $workDir "SK_$name.blend"; $pv = Join-Path $workDir 'preview'
    Invoke-Blender "$PSScriptRoot\blender\render_preview.py" @($modDir, $pv) $blog 'PREVIEW' $blend
    foreach ($c in @($cfg.preview.closeups)) {
        if (-not $c -or ($c.variants -and $c.variants -notcontains $Variant)) { continue }
        Invoke-Blender "$PSScriptRoot\blender\render_closeup.py" @($modDir, $pv, $c.bone, $c.label, "$($c.size)", "$($c.dz)", "$($c.dy)") $blog 'CLOSEUP' $blend
    }
}
Check (Test-Path "$workDir\SK_$name.fbx") "缺 $workDir\SK_$name.fbx（去掉 -SkipPrepare 重跑）"

Step '清理上次生成的 UE 资源'
foreach ($d in "Content\$name", "Content\${name}Stubs", 'Content\PBZContent', 'Plugins\HuaShira\Content', 'Saved\Cooked') {
    $p = Join-Path $proj $d
    if (Test-Path $p) { Get-ChildItem $p -Force | Where-Object Name -ne '.keep' | Remove-Item -Recurse -Force }
}
Remove-Item $PbzActiveMod -ErrorAction SilentlyContinue

Step "UE 生成资源（$env:PBZ_MOD / $rel）"
$env:PBZ_VARIANT = $Variant; $env:PBZ_PHYS = if ($NoPhys) { '0' } else { '1' }
Invoke-UEPython "$PSScriptRoot\ue\setup_assets.py" 'PBZSETUP' (Join-Path $PbzLogs 'ue-setup.log')
New-Item -ItemType Directory -Force -Path (Split-Path $PbzActiveMod) | Out-Null
[IO.File]::WriteAllText($PbzActiveMod, "$env:PBZ_MOD|$Variant|$mode")
$info = [IO.File]::ReadAllText("$workDir\setup_info.json") | ConvertFrom-Json

Step '烹饪'
$cookLog = Join-Path $PbzLogs 'ue-cook.log'
& $PbzEditorCmd $PbzUProject -run=Cook -TargetPlatform=Windows "-CookDir=$proj\Content\$name" "-CookDir=$proj\Content\PBZContent" "-CookDir=$proj\Plugins\HuaShira\Content" -NoDefaultMaps -NoGameAlwaysCook -unattended -nosplash -nullrhi -stdout -FullStdOutLogOutput 2>&1 | Out-File -Encoding utf8 $cookLog
if ($LASTEXITCODE -ne 0) { Select-String -Path $cookLog -Pattern 'Error' | Select-Object -Last 20 | ForEach-Object { $_.Line }; throw "烹饪失败（见 $cookLog）" }

Step '打包'
$cooked = Join-Path $proj 'Saved\Cooked\Windows'
$meta = Join-Path $cooked 'ProjectPBZ\Metadata'
Check (Test-Path "$meta\packagestore.manifest") "烹饪结果缺少 $meta\packagestore.manifest"
$relDir = Join-Path $modDir "release\$rel"
$out = Join-Path $relDir 'ProjectPBZ\Content\Paks\~mods'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
function CookedPath($p) {
    if ($p.StartsWith('/Game/')) { return Join-Path $cooked ('ProjectPBZ\Content\' + $p.Substring(6).Replace('/', '\')) }
    if ($p.StartsWith('/HuaShira/')) { return Join-Path $cooked ('ProjectPBZ\Plugins\HuaShira\Content\' + $p.Substring(10).Replace('/', '\')) }
    throw "未知挂载点: $p"
}
# 只打包：mod 自己的目录、魂被覆盖的部件目录、被覆盖的贴图。占位材质和魂的骨架绝不能进包
$files = [System.Collections.Generic.List[string]]::new()
foreach ($p in @("/Game/$name/") + @($cfg.soul.ship_prefixes)) {
    $d = CookedPath $p.TrimEnd('/')
    Check (Test-Path $d) "没有烹饪结果: $p"
    Get-ChildItem $d -Recurse -File | ForEach-Object { $files.Add($_.FullName) }
}
foreach ($t in @($info.textures)) {
    $hit = @(Get-ChildItem ((CookedPath $t) + '.*') -File -ErrorAction SilentlyContinue)
    Check ($hit.Count -gt 0) "没有烹饪结果: $t"
    $hit | ForEach-Object { $files.Add($_.FullName) }
}
$bad = @($files | Where-Object { [IO.Path]::GetFileNameWithoutExtension($_) -eq (Split-Path -Leaf $cfg.soul.skeleton) })
Check ($bad.Count -eq 0) "打包列表里有魂的骨架（会破坏全部动画）: $($bad[0])"
$container = $cfg.container
$pakTmp = Join-Path $PbzRoot '.tmp\pak'; New-Item -ItemType Directory -Force -Path $pakTmp | Out-Null
$resp = Join-Path $pakTmp "response-$env:PBZ_MOD-$rel.txt"
[IO.File]::WriteAllLines($resp, ($files | ForEach-Object { '"{0}" "../../../{1}"' -f $_.Replace('\', '/'), $_.Substring($cooked.Length + 1).Replace('\', '/') }))
$cmds = Join-Path $pakTmp "commands-$env:PBZ_MOD-$rel.txt"
[IO.File]::WriteAllText($cmds, ('-Output="{0}" -ContainerName={1} -ResponseFile="{2}"' -f (Join-Path $out "$container.utoc").Replace('\', '/'), $container, $resp.Replace('\', '/')))
$unrealPak = Join-Path $PbzUERoot 'Engine\Binaries\Win64\UnrealPak.exe'
$pakLog = Join-Path $PbzLogs "unrealpak-$env:PBZ_MOD-$rel.log"
& $unrealPak $PbzUProject "-CreateGlobalContainer=$pakTmp\global.utoc" "-CookedDirectory=$cooked" "-Commands=$cmds" "-PackageStoreManifest=$meta\packagestore.manifest" "-ScriptObjects=$meta\scriptobjects.bin" -TargetPlatform=Windows -unattended 2>&1 | Out-File -Encoding utf8 $pakLog
Check ($LASTEXITCODE -eq 0) "UnrealPak（IoStore）失败（见 $pakLog）"
# 与 .utoc/.ucas 同名的小 .pak：游戏按它来挂载这组容器
$marker = Join-Path $pakTmp 'marker.txt'; [IO.File]::WriteAllText($marker, "$name replaces Soul ($rel)")
$markerResp = Join-Path $pakTmp "marker-$env:PBZ_MOD-$rel.txt"; [IO.File]::WriteAllText($markerResp, ('"{0}" "../../../ProjectPBZ/Content/{1}/marker.txt"' -f $marker.Replace('\', '/'), $name))
& $unrealPak (Join-Path $out "$container.pak") "-Create=$markerResp" 2>&1 | Out-File -Encoding utf8 -Append $pakLog
Check ($LASTEXITCODE -eq 0) "UnrealPak（pak）失败（见 $pakLog）"
Get-ChildItem $out | ForEach-Object { '  {0}  {1:n1} MB' -f $_.Name, ($_.Length / 1MB) }
"  打包文件数: $($files.Count)"

Step '检查成品'
& $PbzPython "$PSScriptRoot\verify_release.py" $modDir $rel $Variant
Check ($LASTEXITCODE -eq 0) '成品检查未通过'
$doc = Join-Path $modDir 'docs\安装说明.md'
if (Test-Path $doc) { Copy-Item $doc (Join-Path $relDir '安装说明.md') -Force }
Step "完成：mods\$env:PBZ_MOD\release\$rel"
