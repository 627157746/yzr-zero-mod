# 影之刃零主角替换 mod 工作区 — AI 工作指引

## 这是什么项目

把《影之刃零》（UE 5.7，Steam build 26367565）的主角魂替换成自定义人形模型的多 mod 工作区。原理：魂的部件用空网格覆盖并写入全身逆向适配参考姿势（`soul.adapted_pose_json`），替换模型挂到 `Hun_Hair_Band` 槽位，后处理动画蓝图做运行时重定向（`RetargetPoseFromMesh` → 可选 `RigidBody` 次级物理）。

目录结构、常用命令、外部依赖的完整说明在 `README.md`。工具的详细文档（mod.json 每个字段、引擎坑、游戏资源结构）在 `.agents/skills/pbz-protagonist-replace/references/`（`mod-json.md`、`facts.md`）。

## 硬性约束

- **绝不写入游戏目录**（`PBZ\ProjectPBZ\Content\Paks`）。成品只放 `mods/<mod>/release/`，用户自己拷。
- **绝不打包魂的骨架** `Hun_Skin_Skeleton`，会破坏全部动画。`build-mod.ps1` 和 `verify_release.py` 都有检查，别绕过。
- `ProjectPBZ/Content/` 和 `ProjectPBZ/Plugins/HuaShira/Content/` 是**每次构建都会清空的生成目录**，别往里放任何手工文件。同理 `mods/<mod>/work/`、`input/` 都是可再生成的。
- UE 工程和 `tools/` 是所有 mod 共用的；通用流程（unitypackage 解包 `tools/extract_package.py`、Humanoid 全身比例适配 `tools/blender/adapt_humanoid.py`）全部独立放在 `tools/` 下，各 mod 只需配置 `mods/<mod>/mod.json`（仅在确有特殊附件处理时才写 `mods/<mod>/hooks/`）。
- 同一时间只能构建一个 mod（UE 工程只有一份）；跨 mod 操作前看 `ProjectPBZ\Saved\active_mod.txt`。

## 环境（Windows 本机）

- 外部工具路径全在根目录 `env.ps1`（从 `env.example.ps1` 复制配置，包含 `$PbzGameDir`、`$PbzUERoot`、`$PbzBlender`、`$PbzDotnet`、`$PbzCUE4Parse` 等）。**不要**把任何本机绝对路径写死进脚本或文档，一律从 `env.ps1` / `common.ps1` 取。
- UE 缓存建议配置用户环境变量 `UE-LocalDataCachePath` 指向大容量磁盘目录，避免撑满系统盘。
- Node 项目用 pnpm；文本搜索用 `rg`，禁止 grep；临时文件写 `.tmp/`，不写系统临时目录。
- 依赖版本敏感点：UE 5.7 要求 MSVC ≥ 14.44.35211（UBT 按 cl.exe ProductVersion 判断）；usmap/AES 必须与游戏 build 一致（现在 26367565）。

## 典型任务怎么做

**改某个 mod 的效果**（穿模、物理、握武器）：改 `mods/<mod>/mod.json` → `pwsh tools\build-mod.ps1 -Mod <名字> -Variant <变体> [-SkipPrepare]` → `pwsh tools\preview-retarget.ps1 -Mod <名字>`。参数含义和调法见 references/mod-json.md。

**加新模型**：`pwsh tools\new-mod.ps1 -Name <名字>` → 原始文件放 `source/` → 在 `mod.json` 配置 `package`（自动调用 `tools/extract_package.py` 解出 FBX/PNG）与 `model.adapt_humanoid`（自动调用 `tools/blender/adapt_humanoid.py` 完成全身比例适配）→ 先 `-NoPhys` 跑通再开物理。参考 `mods/ciel/mod.json`。

**加表情/眨眼**：在 `mod.json` 的 `model.keep_shape_keys`（源名 → UE 名）与 `face.blink` 里配置 → `build-mod.ps1` 会自动保留形态键并在后处理 ABP 里生成定时眨眼节点链。形态键默认会被烘焙成静止形状，不配 `keep_shape_keys` 就到不了游戏。参数与引擎坑见 references/mod-json.md 的 `face` 节和 facts.md 的「表情 / 眨眼」。

**改了 C#（tools/PbzExport）或 C++（ProjectPBZ/Source）**：`pwsh tools\build-tools.ps1` 重编译后再构建。

**游戏更新了**：核对 build → 换 deps/ 里的 usmap 和 AES → `pwsh tools\extract-game-refs.ps1` 重导魂的参考 → 逐 mod 重打。soul.json 里的路径也要重新核对。

## 验证要求（每个改动都必须走完）

1. `build-mod.ps1` 全程通过（每步都有断言，失败会给出 `.tmp\logs\` 下的日志路径，**不静默降级、不假装成功**）。
2. `verify_release.py` 全部 OK：每个覆盖路径来自 mod 容器、魂的骨架未被覆盖、`Hun_Body_Simple` 仍引用游戏骨架和 `PA_Hun`、`Hun_Hair_Band` 的骨架/动画蓝图/材质正确。
3. `preview-retarget.ps1`：对比改动前后的手误差（cm）和手臂穿模统计，量化说明效果，不要只说"应该好了"。
4. 配了 `face.blink` 的 mod：确认构建日志有 `模型形态键 ['Blink']` 与 `眨眼事件图 编译 True`；形态键几何用 `PbzModEditorLibrary.DescribeMorphTargets` 回读（位移顶点数/最大位移），节点常量与连线用 `DescribeBlinkEventGraph` 回读，再用 Blender 渲染权重 0/0.5/1 的脸部特写肉眼确认。
5. 交付时明确区分：离线检查通过 ≠ 进游戏验证过。没进游戏测的要标注"未验证"。

## 排错

- 任何资源结构拿不准，用 `tools\PbzExport\bin\Release\net10.0\PbzExport.exe` 现场查（`list/find/where/json/export/anim`，`--with` 可挂载 mod 容器对比覆盖来源），**不凭记忆猜**。
- 引擎行为拿不准，查 `$PbzUERoot\Engine\Source` 源码确认。
- UE 命令行脚本崩在 Slate/内容浏览器：先确认 `Interchange.FeatureFlags.Import.SyncToBrowser 0` 已执行（setup_assets.py 开头）。
- Blender 导入 FBX 会去掉形态键名的 `XXX_BS.` 前缀，按后缀匹配；别把形态键清零（作者默认值防穿模）。
- 带形态键的网格无法 `Apply armature modifier`（`Modifier cannot be applied to a mesh with shape keys`）：要保留的形态键必须先记 delta 再全部删掉，骨架定型后用 `tools/blender/keep_morph.py` 重建。
- 用户报游戏内问题：要截图和具体动作名，先在离线预览复现并量化，再改参数。

## 语义约定

- "魂/Soul/Hun" 指游戏原主角；"模型" 指 `mods/<mod>/` 里的替换模型；"变体" 指 mod.json 的 variants；"包" 指变体 × phys/basic 的成品。
- `adapt_humanoid`、`adapted_pose_json`、`hand_ik`、`arm_pole_deg`、`keep_shape_keys`、`face.blink` 等参数的原理见 references/mod-json.md 和 facts.md，改动前先读。

## 已决策记录（不要翻案）

- 用运行时重定向（`RetargetPoseFromMesh` + 可选 `RigidBody`），不把模型直接绑到魂的 1134 骨原版骨架上，也不需要安装 UE4SS。
- **眨眼用后处理 ABP 的事件图表 + `UAnimInstance::SetMorphTarget`**，不用 `ModifyCurve` 节点、也不往魂骨架上加曲线：`SetMorphTarget` 写组件的 `MorphTargetCurves`，不依赖骨骼曲线的 MorphTarget 标记和每帧骨骼求值。节点链由 `PbzModEditorLibrary.BuildBlinkEventGraph` 在 C++ 里生成，不需要手进 Persona。
- **形态键要先全删再重建**（`tools/blender/keep_morph.py`）：带形态键的网格无法 `Apply armature modifier`，所以不可能「保留着形态键跑适配」；delta 必须换到主导骨骼空间存档、定型后再乘新静止矩阵贴回，否则眨眼会沿旧方向合眼。眨眼的 `0.5-0.5*cos` 用 **π 不用 2π**（2π 会在一个窗口里合两次眼，实测最短间隔 0.07 s 像抽搐）。
- **公共代码全部独立在 `tools/`，标准全流程方案（`tools/extract_package.py` + `tools/blender/adapt_humanoid.py` + `tools/ue/setup_assets.py`）**：
  - **为什么局部缩手臂会出现「大宽肩、猩猩臂、端肩架肘」**：魂的 `Hun_Skin_Skeleton` 中 1067 根骨骼（含 `Pelvis`、`spine_01..05`、`clavicle_l/r`、`upperarm_l/r`、`lowerarm_l/r`、`thigh/calf/foot` 及全部 `twist` 骨）都是 `AnimationScaled` 模式，只有 `hand_l/r`（局部长 `25.40205 cm`）、手指、`weapon_*`、`ik_*` 是 `Animation` 模式。若只缩手臂三节，成年男性的骨盆（109.2 cm）和脊柱（+40.9 cm）会让魂的肩高（150.6 cm）比二次元少女高出 10~17 cm，`TwoBoneIK` 强行把少女手臂往上拽。
  - **通用独立模块 `tools/blender/adapt_humanoid.py` 的四步根治做法**：
    1. T-pose 下将模型前臂中段袖管适配为魂 `hand_l/r` 的精确长度 `25.40205 cm`，末端袖口（`cuff_margin_cm`）做纯刚性平移（保持袖口百褶 0% 形变），手掌绕腕关节放大（`hand_scale`）适配刀柄。
    2. 将模型四肢、手腕、五指对齐魂的静止姿势，执行 `Apply Pose as Rest Pose` 烘焙静止姿势（配合 `mod.json` 的 `retarget.skip_auto_align: true`），之后执行鞋底落地（`Z=0`）。
    3. 在编辑模式下细分 `Spine_02/03`、`UpperChest`、`Neck_02` 及 16 根四肢 Twist 骨（`upperarm/lowerarm/thigh/calf_twist_01/02`），按线性投影重分配蒙皮权重；并令 `Hips -> Spine..UpperChest -> Left/Right_Shoulder` 的局部平移方向与魂 `orig_t` 完全平行（仅调整内部关节轴心，不移动网格顶点），使运行时 `AnimationScaled` 平移向量与模型 100% 重合。
    4. 导出全身 42 根骨骼的适配局部平移到 `work/soul_adapted_pose.json`，由 `setup_assets.py`（`soul.adapted_pose_json`）写入 `SK_SoulBlank`（`Hun_Body_Simple`）。`ciel` 实测 9 段动作 72 个采样点手部误差全部为 **`0.0 cm`**。
- **双手握持要单独量武器空间误差**：模型到魂手腕误差为 0 不代表握住武器。`soul.two_hand_grip` 在魂空身体上生成 `ABP_SoulGrip`，恢复动画自带 IK 手的相对握持关系；参数、验证结果和局限统一见 `references/mod-json.md` / `references/facts.md` 的双手握持节。AnimGraph 字段有同名引脚时必须同步引脚默认值，成品回读验证，不能只读编辑器结构体。
- 当前工作区内置 `mods/ciel` 作为标准示例 mod。
- 成品放项目 release，不部署进游戏目录（用户要求手动测试）。
