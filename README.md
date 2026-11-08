# 影之刃零主角替换 mod 工作区

把《影之刃零》（Phantom Blade Zero，UE 5.7）的主角**魂**替换成自定义人形模型（VRChat / Unity / VRoid / Mixamo FBX 等）。本目录是一个多 mod 工作区：通用处理管线（解包、Humanoid 全身比例逆向适配、UE 重定向与次级物理生成、烹饪打包、离线预览）全部独立在 `tools/` 与 `ProjectPBZ/` 中，每个替换模型一个文件夹，仅需配置 `mod.json`。

当前模型目录：

| 目录 | 模型 | 状态 |
|---|---|---|
| `mods/ciel` | シエル Ciel Ver1.00（ArmoireLepus，VN3 许可） | **已实现**（`default-phys` / `default-basic`，9 段动作 72 帧手部误差 `0.0 cm`） |

## 核心架构：通用 Humanoid 全身比例逆向适配管线

所有通用代码均独立在 `tools/` 下，模型目录无需编写重复脚本：

1. **通用解包（`tools/extract_package.py`）**：
   在 `mod.json` 配置 `"package"` 段后，`build-mod.ps1` 自动从 `source/*.unitypackage` 提取 `.fbx`、`.png` 与 `.prefab/.controller/.anim` 到 `input/`。
2. **通用 Humanoid 全身比例逆向适配（`tools/blender/adapt_humanoid.py`）**：
   在 `mod.json` 配置 `"model.adapt_humanoid"` 后，`prepare_model.py` 自动完成：
   - **前臂等长 + 袖口零形变平移**：针对魂骨架中唯一采用 `Animation` 模式（固定局部长 `25.40205 cm`）的 `hand_l/r`，在 T-pose 下仅对前臂中段袖管做轴向适配，末端 `cuff_margin_cm`（如 `2.0 cm`）的袖口与手腕刚性平移（百褶袖口 0% 形变），手掌绕腕关节按 `hand_scale`（如 `1.12x`）放大贴合刀柄。
   - **静止姿势烘焙（`Apply Pose as Rest Pose`）**：将模型四肢、手腕、五指对齐魂的静止姿势后直接烘焙为 Rest Pose，随后执行鞋底落地（`Z=0`），配合 `retarget.skip_auto_align: true` 消除自动对齐误差。
   - **脊柱与四肢 16 根 Twist 骨细分 + 躯干向量平行对齐**：在编辑模式下自动细分 `Spine_02/03`、`UpperChest`、`Neck_02` 及 16 根四肢 Twist 骨（`upperarm/lowerarm/thigh/calf_twist_01/02`），按线性投影重分配蒙皮权重；并令 `Hips -> Spine..UpperChest -> Left/Right_Shoulder` 的内部局部平移方向与魂 `orig_t` 完全平行（不移动任何网格顶点）。
   - **全身 `AnimationScaled` 逆向适配（`work/soul_adapted_pose.json`）**：导出全身 42 根骨骼的适配局部平移，由 `tools/ue/setup_assets.py` 写入 `Hun_Body_Simple`（`SK_SoulBlank`），使运行时魂空身体与替换模型的关节位置在所有动作帧下 100% 重合（`ciel` 实测 9 段动作 72 帧手部误差全部为 **`0.0 cm`**）。

## 目录结构

```
yzr-zero-mod/
  AGENTS.md                        AI 工作指引（改本目录前先读）
  README.md                        本文件
  env.ps1                          本机路径配置（tools\common.ps1 读取；从 env.example.ps1 复制）
  soul.json                        魂的资源路径（游戏一侧，所有 mod 共用；游戏更新后核对）
  deps/                            aes.txt + usmap（跟游戏 build 绑定，不入库）
  tools/                           独立公共构建脚本 + PbzExport（C# 解包工具）
    common.ps1                     公共环境、Resolve-Mod、Get-ModConfig、Invoke-UEPython/Invoke-Blender
    pbzcfg.py                      Python 侧读 soul.json + mod.json
    extract_package.py             通用 unitypackage 解包工具
    mod-template/                  new-mod.ps1 用的 mod.json 标准模板
    ue/setup_assets.py             在 UE 里生成全部资源（含全身参考姿势写入与 Twist 链映射）
    ue/preview_retarget.py         离线预览的姿势计算与手误差统计
    blender/
      adapt_humanoid.py            通用 Humanoid 全身比例逆向适配、静止姿势烘焙与 Twist 细分模块
      prepare_model.py             模型导入、附件合并、材质槽映射与 FBX 导出
      make_textures.py             生成 ARM 与平法线贴图
      make_hun_blank.py            生成魂的空身体网格
      render_preview.py / render_closeup.py / render_retarget.py / viewlib.py  离线渲染预览
  ProjectPBZ/                      UE 5.7 工程（共用；PbzModEditor C++ 模块在这里）
  work/soul/                       魂的骨架参考、空身体（共用，extract-game-refs.ps1 生成）
  mods/ciel/
    mod.json                       Ciel 的全部声明式配置（无模型专属脚本）
    source/                        原始文件（unitypackage 等，不入库）
    input/                         解出的 FBX / PNG / Unity 资产（可再生成）
    docs/安装说明.md                给玩家的安装说明（打包时复制进 release）
    work/<变体>/                    中间文件、soul_adapted_pose.json、预览图、retarget 对比图（可再生成）
    release/<变体>-<phys|basic>/    成品：ProjectPBZ\Content\Paks\~mods\ 3 个文件
  .agents/skills/pbz-protagonist-replace/   通用 skill（含 kit 工具模板和完整文档）
  .tmp/                            日志和临时文件（不入库）
```

## 从 GitHub 克隆后的首次初始化流程

仓库已内置全部公共脚本（`tools/`）、UE 5.7 工程源码（`ProjectPBZ/`）、AI Skill 文档（`.agents/`）、当前游戏版本的 `deps/aes.txt` 与 `deps/*.usmap`、主角「魂」的参考数据（`work/soul/`），以及 Ciel 的完整声明式配置（`mods/ciel/mod.json`）。克隆后只需 4 步即可出包：

```powershell
# 1. 复制本机环境配置并按自己的电脑修改游戏、UE 5.7、Blender、.NET 10、CUE4Parse 路径
Copy-Item env.example.ps1 env.ps1

# 2. 编译 C# 解包工具 PbzExport 与 UE 编辑器 C++ 模块 PbzModEditor
pwsh tools\build-tools.ps1

# 3. 将 Ciel 原始包（ciel_1.0.unitypackage）放入 mods\ciel\source\

# 4. 一键构建 Ciel（自动解包 -> Blender 全身适配 -> UE 生成与烹饪 -> IoStore 打包 -> 校验）
pwsh tools\build-mod.ps1 -Mod ciel
```

## 常用命令

```powershell
pwsh tools\build-tools.ps1                       # 编译 PbzExport + UE 编辑器模块（改了 C# / C++ 后重跑）
pwsh tools\extract-game-refs.ps1                 # 导出魂的参考数据（游戏更新后重跑；所有 mod 共用）

pwsh tools\new-mod.ps1 -Name <名字>              # 加新模型：从标准模板生成 mods\<名字>\mod.json
pwsh tools\build-mod.ps1 -Mod ciel               # 构建 Ciel 带物理包（default-phys）
pwsh tools\build-mod.ps1 -Mod ciel -NoPhys       # 构建 Ciel 无物理包（default-basic）
pwsh tools\build-mod.ps1 -Mod ciel -SkipPrepare  # 复用上次 Blender 结果（只重跑 UE + Cook + Pak）
pwsh tools\preview-retarget.ps1 -Mod ciel        # 离线预览：手误差（cm）+ 对比图 + 穿模统计
```

输出在 `mods/<mod>/release/<变体>-<phys|basic>/ProjectPBZ/Content/Paks/~mods/`，把 `~mods` 里的 3 个 `zzz_*_P.*` 文件拷进 `PBZ\ProjectPBZ\Content\Paks\~mods\` 即可生效。

## 外部依赖（在 `env.ps1` 中配置本机路径）

| 组件 | `env.ps1` 变量 | 说明 |
|---|---|---|
| 游戏本体 | `$PbzGameDir` | Steam build 26367565（包含 `ProjectPBZ\Content\Paks`）；`deps/` 下的 usmap/AES 与之绑定 |
| UE 5.7.4 | `$PbzUERoot` | 必须与游戏引擎版本一致；建议设置用户环境变量 `UE-LocalDataCachePath` 将 DDC 放到大容量磁盘 |
| Blender 5.x | `$PbzBlender` | 需安装 `io_scene_psk_psa` 扩展（导入游戏 `.pskx` 用） |
| .NET 10 SDK | `$PbzDotnet` | 编译 `tools/PbzExport` |
| CUE4Parse 源码 | `$PbzCUE4Parse` / `$PbzNatives` | `FabianFG/CUE4Parse`（含子模块）；`$PbzNatives` 目录需包含 `oodle-data-shared.dll`、`zlib-ng2.dll`、`CUE4Parse-Natives.dll` |

## 修改 mod 行为

所有可调参数都在 `mods/<mod>/mod.json`，字段说明见 `.agents/skills/pbz-protagonist-replace/references/mod-json.md`；游戏版本绑定数据和引擎底层原理见同目录 `facts.md`。
