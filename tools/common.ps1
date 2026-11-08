# 公共环境：其他脚本用 `. "$PSScriptRoot\common.ps1"` 引入。读取工作区根目录的 env.ps1 并检查路径。
# 工作区结构：根目录放共用的 UE 工程、工具、deps、soul.json；每个替换模型一个 mods\<名字>\ 文件夹。
$ErrorActionPreference = 'Stop'
$PbzRoot = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $PbzRoot 'env.ps1'
if (-not (Test-Path $envFile)) { throw "缺少 $envFile（从 env.example.ps1 复制后按本机修改）" }
. $envFile

function Need($path, $what) { if (-not $path -or -not (Test-Path $path)) { throw "$what 不存在: $path（检查 env.ps1）" } }
Need $PbzGameDir '游戏目录'; Need (Join-Path $PbzGameDir 'ProjectPBZ\Content\Paks') '游戏 Paks 目录'
Need $PbzUERoot 'UE 目录'; Need $PbzBlender 'Blender'; Need $PbzCUE4Parse 'CUE4Parse 源码'; Need $PbzNatives 'CUE4Parse 原生库目录'
Need $PbzUsmap 'usmap'; Need $PbzAes 'AES 文件'

$env:PBZ_ROOT      = $PbzRoot
$env:PBZ_PAKS      = Join-Path $PbzGameDir 'ProjectPBZ\Content\Paks'
$env:PBZ_USMAP     = $PbzUsmap
$env:PBZ_AES       = $PbzAes
$env:PBZ_CUE4PARSE = $PbzCUE4Parse
$env:PBZ_NATIVES   = $PbzNatives
$env:CUE4PARSE_SKIP_NATIVE = '1'
$env:DOTNET_NOLOGO = '1'; $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
if ($PbzDotnetHome) {
    $env:DOTNET_CLI_HOME = $PbzDotnetHome
    $env:NUGET_PACKAGES = Join-Path $PbzDotnetHome 'nuget\packages'
    $env:NUGET_HTTP_CACHE_PATH = Join-Path $PbzDotnetHome 'nuget\http-cache'
}
# UE 缓存位置以用户环境变量为准（想放到别的盘，设置用户环境变量 UE-LocalDataCachePath）
$ddc = [Environment]::GetEnvironmentVariable('UE-LocalDataCachePath', 'User')
if ($ddc) { ${env:UE-LocalDataCachePath} = $ddc }

$PbzExportExe = Join-Path $PbzRoot 'tools\PbzExport\bin\Release\net10.0\PbzExport.exe'
$PbzUProject  = Join-Path $PbzRoot 'ProjectPBZ\ProjectPBZ.uproject'
$PbzEditorCmd = Join-Path $PbzUERoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$PbzLogs      = Join-Path $PbzRoot '.tmp\logs'
# UE 工程里当前生成的是哪个 mod（UE 工程只有一份，所有 mod 共用；预览前要核对）
$PbzActiveMod = Join-Path $PbzRoot 'ProjectPBZ\Saved\active_mod.txt'
$env:TEMP = $env:TMP = Join-Path $PbzRoot '.tmp\temp'
New-Item -ItemType Directory -Force -Path $PbzLogs, $env:TEMP | Out-Null
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = 'utf-8'

function Step($t) { Write-Host "== $t  $(Get-Date -Format HH:mm:ss)" }
function Check($ok, $msg) { if (-not $ok) { throw $msg } }

# 选 mod：给了名字就用它；没给且只有一个 mod 就用那个；否则报错并列出可选项
function Resolve-Mod([string]$Name) {
    $all = @(Get-ChildItem (Join-Path $PbzRoot 'mods') -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName 'mod.json') } | ForEach-Object Name)
    if (-not $Name) {
        Check ($all.Count -eq 1) "请用 -Mod 指定 mod。现有: $($all -join ', ')（新建: pwsh tools\new-mod.ps1 -Name <名字>）"
        $Name = $all[0]
    }
    $dir = Join-Path $PbzRoot "mods\$Name"
    Check (Test-Path (Join-Path $dir 'mod.json')) "没有 mods\$Name\mod.json。现有: $($all -join ', ')"
    $env:PBZ_MOD = $Name; $env:PBZ_MOD_DIR = $dir
    return $dir
}
# 读配置：soul.json（游戏一侧，共用）+ mods\<名字>\mod.json；mod.json 的 soul 段覆盖同名字段
function Get-ModConfig($modDir) {
    $cfg = [IO.File]::ReadAllText((Join-Path $modDir 'mod.json')) | ConvertFrom-Json
    $soul = [IO.File]::ReadAllText((Join-Path $PbzRoot 'soul.json')) | ConvertFrom-Json
    if ($cfg.soul) { foreach ($p in $cfg.soul.PSObject.Properties) { $soul | Add-Member -Force -NotePropertyName $p.Name -NotePropertyValue $p.Value } }
    $cfg | Add-Member -Force -NotePropertyName soul -NotePropertyValue $soul
    return $cfg
}

# 在 UE 命令行里跑 Python 脚本；日志里必须出现 "<Tag> DONE"
function Invoke-UEPython($script, $tag, $log) {
    & $PbzEditorCmd $PbzUProject -run=pythonscript "-script=$script" -unattended -nosplash -nullrhi -stdout -FullStdOutLogOutput -NoSound 2>&1 | Out-File -Encoding utf8 $log
    Select-String -Path $log -Pattern "$tag " | ForEach-Object { $_.Line -replace "^.*$tag ", '  ' } | Select-Object -Unique
    if (-not (Select-String -Path $log -Pattern "$tag DONE" -Quiet)) {
        Select-String -Path $log -Pattern 'Error|Traceback|File "' | Select-Object -Last 25 | ForEach-Object { $_.Line }
        throw "UE 脚本失败（见 $log）"
    }
}
# 跑 Blender 脚本（可带 .blend），输出追加到日志，打印带指定前缀的行
function Invoke-Blender($script, [string[]]$scriptArgs, $log, $prefix, $blend = $null) {
    $a = @('-b'); if ($blend) { $a += $blend }
    $a += @('--python-exit-code', '1', '--python', $script, '--') + $scriptArgs
    $skip = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
    & $PbzBlender @a 2>&1 | Out-File -Encoding utf8 -Append $log
    $code = $LASTEXITCODE
    Get-Content $log | Select-Object -Skip $skip | Where-Object { $_ -match "^($prefix) " } | Select-Object -Last 60
    Check ($code -eq 0) "Blender 脚本失败: $(Split-Path -Leaf $script)（见 $log）"
}
