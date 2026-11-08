# 本机环境配置：首次使用时复制为工作区根目录的 env.ps1，按自己的电脑路径修改。
# 只影响当前 PowerShell 进程，不改系统 PATH 和系统环境变量。所有脚本通过 tools\common.ps1 读取它。

# ---- 必填：游戏与引擎 ----
$PbzGameDir = 'C:\Program Files (x86)\Steam\steamapps\common\PBZ'       # 游戏根目录（包含 ProjectPBZ\Content\Paks）
$PbzUERoot  = 'C:\Program Files\Epic Games\UE_5.7'                      # UE 5.7.x，版本必须和游戏一致

# ---- 必填：工具 ----
$PbzBlender   = 'C:\tools\blender-5.2\blender.exe'                       # Blender 5.x，已装 io_scene_psk_psa 扩展
$PbzDotnet    = 'dotnet'                                                 # .NET 10 SDK；便携版写完整路径
$PbzCUE4Parse = 'C:\tools\CUE4Parse'                                     # FabianFG/CUE4Parse 源码（含子模块）
$PbzNatives   = 'C:\tools\CUE4Parse\runtime'                             # oodle-data-shared.dll、zlib-ng2.dll、CUE4Parse-Natives.dll 所在目录
$PbzPython    = 'python'                                                 # Python 3.10+

# ---- 可选：便携 .NET 的数据目录（不想写到用户目录时设置，留空则不改）----
$PbzDotnetHome = ''

# ---- 跟游戏版本绑定（已随仓库自带在 deps\ 下，游戏更新后替换）----
$PbzUsmap = Join-Path $PSScriptRoot 'deps\ProjectPBZ-5.7.4-302256-full.usmap'  # 与游戏 build 26367565 对应的 usmap
$PbzAes   = Join-Path $PSScriptRoot 'deps\aes.txt'                              # 一行 0x 开头的 AES 密钥
