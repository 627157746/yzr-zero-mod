# 新建一个替换模型 mod：从模板生成 mods\<名字>\（mod.json + 空的 source/input/hooks/docs 目录）
# 用法：pwsh tools\new-mod.ps1 -Name <文件夹名> [-ModName <UE 资源名，字母数字>]
param([Parameter(Mandatory)][string]$Name, [string]$ModName = '')
. "$PSScriptRoot\common.ps1"
Check ($Name -match '^[A-Za-z0-9_-]+$') '文件夹名只用字母、数字、- 和 _'
if (-not $ModName) { $ModName = (Get-Culture).TextInfo.ToTitleCase($Name) -replace '[^A-Za-z0-9]', '' }
Check ($ModName -match '^[A-Za-z][A-Za-z0-9]*$') 'ModName 只用字母和数字，字母开头'
$dir = Join-Path $PbzRoot "mods\$Name"
Check (-not (Test-Path $dir)) "mods\$Name 已存在"
foreach ($d in 'source', 'input\fbx', 'input\textures', 'hooks', 'docs') { New-Item -ItemType Directory -Force -Path (Join-Path $dir $d) | Out-Null }
$cfg = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'mod-template\mod.json')) | ConvertFrom-Json
$cfg.mod_name = $ModName; $cfg.container = "zzz_${ModName}_P"
[IO.File]::WriteAllText((Join-Path $dir 'mod.json'), ($cfg | ConvertTo-Json -Depth 20))
"已创建 mods\$Name（UE 资源名 $ModName，容器 zzz_${ModName}_P）"
"下一步：模型放 mods\$Name\source → 解出 FBX 到 input\fbx、贴图到 input\textures → 改 mod.json → pwsh tools\build-mod.ps1 -Mod $Name -NoPhys"
