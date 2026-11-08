# mod.json 字段说明

工作区里每个替换模型一个 `mods/<名字>/mod.json`（`pwsh tools\new-mod.ps1 -Name <名字>` 从模板生成）。游戏一侧的共用数据在工作区根目录的 `soul.json`，`mod.json` 的 `soul` 段只写要覆盖的字段（如 `adapted_pose_json`）。

路径类字段：`/HuaShira/...`、`/Game/...` 是 UE 资源路径；`ProjectPBZ/...` 是 PbzExport 用的游戏文件路径；其余是**相对 mod 目录**的本地路径。完整的真实例子见 `examples/ciel/`（VRChat Humanoid 模型，零专属脚本，纯声明式配置）。

## 顶层

| 字段 | 说明 |
|---|---|
| `mod_name` | 只用字母数字，各 mod 不能重复。生成 `/Game/<mod_name>/SK_<mod_name>`、`IK_<mod_name>`、`RTG_Soul_To_<mod_name>`、`PA_<mod_name>_Secondary`、`ABP_<mod_name>` |
| `container` | 容器文件名，`zzz_` 开头保证后加载，`_P` 结尾表示补丁容器。不同模型用不同名字；但它们都替换魂，游戏里只能装一个 |
| `package` | 可选，配置后 `build-mod.ps1` 自动调用通用 `tools/extract_package.py` 解出 `.unitypackage`：`{"unitypackage": "source/xxx.unitypackage", "prefix": "Assets/...", "tex_dirs": [...], "unity_dirs": [...]}` |
| `prepare_script` | 可选，非 unitypackage 模型可在 Blender 之前运行自定义 Python 脚本（参数：mod 目录） |
| `preview` | 预览图设置：`closeups` = `[{bone, label, size, dz, dy, variants?}]` 静止姿势特写；`body_match` / `arm_match` = 穿模统计用的躯干/手臂骨骼正则 |
| `preview_anims` | 离线预览用的魂的动作（游戏文件路径，用 `PbzExport find Hun_` 查） |
| `variants` | 变体名 → `{"include": [optional_fbx 的键...]}`。每个变体 × phys/basic 输出一个包：`mods/<名字>/release/<变体>-<phys或basic>/` |

## soul（根目录 soul.json；游戏更新后核对）

| 字段 | 说明 |
|---|---|
| `skeleton` / `skeleton_game_path` | 魂的骨架。**绝不能打包**，build-mod 和 verify 都会检查 |
| `body_game_path` | 导出空身体用的魂身体网格 |
| `blank_mesh` | 空网格导入位置；必须和骨架同目录，导入时才会复用游戏骨架 |
| `physics_asset` | 魂身体引用的物理资产（受击判定用），只生成同名空壳让引用成立，不打包 |
| `body` | 魂的身体部件，用空网格覆盖，保留 `physics_asset` 引用 |
| `visible_part` | 替换模型显示在哪个部件上（默认 `Hun_Hair_Band`，默认服装里一直存在） |
| `blank_parts` / `blank_cloth` | 需要清空的网格部件 / 布料资产 |
| `ship_prefixes` | 要打包的游戏目录（被覆盖的部件所在目录） |
| `adapted_pose_json` | 相对 mod 目录的 JSON 路径（默认 `"work/soul_adapted_pose.json"`），由 `tools/blender/adapt_humanoid.py` 自动生成：`{骨骼名小写: [x, y, z]}`（UE 局部坐标 cm）。写入 `SK_SoulBlank`（`Hun_Body_Simple`）的参考姿势局部平移，实现全身 `AnimationScaled` 骨骼（`Pelvis`、`spine_01..05`、`neck_01..02`、`clavicle`、`upperarm`、`lowerarm`、`thigh`、`calf`、`foot`、`ball` 及全部 `twist` 骨）逆向贴合替换模型比例 |
| `arm_scale` | 可选，仅旧方案不启用 `adapted_pose_json` 时缩放 `clavicle`/`upperarm`/`lowerarm` 参考平移；标准方案下保持全 `1.0` |

## model（Blender 处理，`tools/blender/prepare_model.py` + `adapt_humanoid.py`）

| 字段 | 说明 |
|---|---|
| `fbx_dir` / `main_fbx` | FBX 所在目录、主体 FBX |
| `extra_fbx` | 每个变体都要的附件 FBX（尾巴、袖子等单独导出的部件） |
| `optional_fbx` | 键 → FBX 列表，被变体 `include` 时才导入 |
| `blender_to_cm` | Blender 里 1 单位等于多少厘米。Unity/VRChat FBX 导入后是米，填 100 |
| `scale` | 额外等比放大（使角色头顶高度约 172~180 cm） |
| `root_bone` | 模型的骨盆/根骨（`Hips`）。脚本会在它上面加一根 `root` |
| `attach` | 附件骨骼名 → 挂到主体哪根骨骼下 |
| `skip_bones` | 合并时丢弃的附件骨骼（例如附件自带的空根骨） |
| `align_accessories` | `[{ref_fbx, bone, to, apply_to}]`：平移 `apply_to` 里的附件，使 `ref_fbx` 的 `bone` 与主体的 `to` 重合 |
| `adapt_humanoid` | 启用通用 Humanoid 全身比例逆向适配（`tools/blender/adapt_humanoid.py`）：`{"hand_scale": 1.12, "cuff_margin_cm": 2.0, "merge_bones": {"Elbow_support.L": "Left Upper Arm", ...}, "reparent_bones": {"Left Sleeve": "Left Lower Arm", ...}}`。自动完成 Blender `.L`/`.R` 后缀骨骼名规范化、辅助骨合并（`merge_bones`）、前臂等长适配（末端袖口刚性平移零形变）、静止姿势烘焙、脊柱与 16 根四肢 Twist 骨细分、肩窝/髋部异常轴心自适应修正、子骨骼重挂（`reparent_bones`）、导出 `soul.adapted_pose_json` 及骨骼名规范化 |
| `blender_hook` | 可选，mod 目录内的 Python 文件，在 `adapt_humanoid` 之后执行，仅用于特殊附件处理 |
| `delete_meshes` | 删除的网格对象名（备选服装等） |
| `shape_keys` | 网格名 → {形态键名: 值}。没列出的保持 FBX 里作者设的初始值（**不要清零**，否则 Shrink 类形态键失效会穿模） |
| `material_slots` | 模型材质名 → UE 材质槽名。槽名必须在顶层 `materials` 里；没映射的材质会直接报错 |
| `drop_materials` | 删除用这些材质的面（半透明叠加阴影等游戏材质画不出的层） |
| `sole_bones` | 鞋底落地用的骨骼（权重 > 0.5 的最低顶点抬到 z=0） |
| `left_foot` / `left_toe` | 朝向检查用。Blender 里要面朝 -Y、左脚在 +X |
| `rest_rotations` | `[{bone, dir_to, target}]`：把 `bone`（连同子骨骼和网格）绕自身转到使 `bone→dir_to` 指向 `target`（Blender 坐标），再设为静止姿势 |

## retarget

| 字段 | 说明 |
|---|---|
| `chains` | 链名 → [起始骨, 末端骨]（模型一侧）。链名必须是魂一侧已有的：基础链 `Spine Neck Head LeftClavicle RightClavicle LeftArm RightArm LeftLeg RightLeg LeftToe RightToe`、五指链 `Left/Right` + `Thumb Index Middle Ring Pinky`、四肢 Twist 链 `Left/Right` + `UpperArmTwist1 UpperArmTwist2 LowerArmTwist1 LowerArmTwist2 ThighTwist1 ThighTwist2 CalfTwist1 CalfTwist2`。只写模型有的链 |
| `hand_ik` | 手部 IK：把模型的手拉到魂的手（武器）位置 |
| `arm_pole_deg` | 肘部绕“肩→手”连线外转的角度；全身比例适配方案（`adapt_humanoid`）下设为 `0.0` 即可 |
| `skip_auto_align` | 布尔值（启用 `adapt_humanoid` 时设为 `true`）。跳过 UE `IKRetargeterController.auto_align_all_bones`，直接使用 Blender 已精确烘焙的静止姿势 |
| `finger_alpha` | 可选浮点数（默认不写 = `1.0`）。缩放 10 条手指 FK 链的 `rotation_alpha`（例如 `0.85`），防止细长手指完全照搬魂的半握爪形手势时过度弯曲 |

## physics（只在 phys 包里生效）

| 字段 | 说明 |
|---|---|
| `world_alpha` | 转身惯性，0 = 头发尾巴瞬移跟随，0.5~0.7 有滞后 |
| `skip_match` | 匹配到的骨骼永远跟随动画（躯干、四肢、手指、Twist 骨与各链根骨） |
| `groups` | 按顺序匹配（第一个命中的组生效）：`match` 正则；`gravity` 重力；`lin`/`ang` 阻尼；`swing`/`twist` 关节角度上限（度）；`stiff`/`damp` 回到静止姿势的驱动强度；`collide` 是否参与碰撞 |
| `kinematic_colliders` | 组名 → 骨骼列表：跟随动画的挡板刚体（如躯干 `spine`、双腿 `leg`） |
| `collide_pairs` | 打开哪些组之间的碰撞，例如 `[["twintail", "spine"], ["skirt", "leg"]]`。其余组之间不碰撞 |

## materials / textures / generated_textures

| 字段 | 说明 |
|---|---|
| `materials` | UE 材质槽名 → 借用的魂材质实例路径。可选的有 `MI_ZeroHun_Cap_01_b`、`MI_ZeroHun_Cloth_01_b` … `05_b`，位于 `/Game/PBZContent/Characters/Hero/Hun_New_V2b/Material/` |
| `textures` | 覆盖这些材质实例引用的贴图：`target` 游戏贴图路径（用 PbzExport `json` 导出材质实例查），`slot` 所属材质槽（槽在当前变体不存在时自动跳过、不覆盖），`source` 本地 PNG，`kind` = `BC`（底色，sRGB）/ `N`（法线，默认按 OpenGL 方向翻转绿通道，DirectX 法线加 `"opengl_normal": false`）/ `ARM`（R=AO, G=粗糙度, B=金属度） |
| `generated_textures` | 生成辅助贴图：`arm`（常量 `rough`、`metal`）、`arm_from_mask`（`mask` 白=金属，`rough`、`rough_metal`）、`flat_normal` |

覆盖的贴图在游戏别处也被用到时会一起变，交付时写进已知限制。
