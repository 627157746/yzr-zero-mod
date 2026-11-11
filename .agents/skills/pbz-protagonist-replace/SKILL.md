---
name: pbz-protagonist-replace
description: 影之刃零（Phantom Blade Zero，UE 5.7）把主角魂（Soul/Hun）替换成任意人形模型（VRChat/Unity/VRoid/Mixamo FBX 等）的 mod 制作。自带完整工具包：新建工作区、解包游戏参考、通用 unitypackage 解包、Humanoid 全身比例逆向适配与静止姿势烘焙、UE 无界面生成重定向/物理/动画蓝图、烹饪、IoStore 打包、成品检查、离线预览手部误差。用户要做影之刃零换人物/换主角模型，或调握武器、穿模、头发尾巴裙子物理时使用。
compatibility: Windows 10/11；游戏同版本的 UE 5.7.x + MSVC 14.44.35211 以上；Blender 5.x + io_scene_psk_psa；.NET 10 SDK；Python 3.10+；CUE4Parse 源码
---

# 影之刃零主角替换

本 skill 随工作区仓库内置于 `.agents/skills/pbz-protagonist-replace/`，公共工具与 UE 工程位于工作区根目录 `tools/` 与 `ProjectPBZ/`。

## 原理（先讲给用户，再动手）

不改写游戏原版骨架 `Hun_Skin_Skeleton`，采用**魂空身体全身比例逆向适配 + 静止姿势烘焙 + 运行时重定向**：

1. 魂的身体、头、头发、帽子、布料全部用**空网格**覆盖（只有魂的骨架和一个藏在骨盆里的小三角形），身体保留原版物理资产引用（受击判定靠它）。
2. **通用 Humanoid 全身比例逆向适配（`tools/blender/adapt_humanoid.py`，消除“大宽肩、猩猩臂、端肩架肘”）**：
   - 魂骨架中 `Pelvis`、`spine_01..05`、`clavicle_l/r`、`upperarm_l/r`、`lowerarm_l/r`、`thigh/calf/foot` 及全部 `twist` 骨均为 `AnimationScaled` 模式（仅 `hand_l/r` 为 `Animation` 模式，固定长 `25.40205 cm`）。
   - 若只缩放手臂三节骨骼，魂的骨盆与脊柱仍停留在成年男性高度（肩膀 150.6 cm），比二次元少女高出 10~17 cm，`TwoBoneIK` 会强行把少女手臂往上拽。
   - 通用模块 `tools/blender/adapt_humanoid.py`（通过 `mod.json` 的 `model.adapt_humanoid` 启用）自动在 T-pose 下将模型前臂中段袖管适配为 `25.40205 cm`（末端袖口刚性平移零形变），将四肢/手指旋转对齐魂的静止姿势并 `Apply Pose as Rest Pose`，细分脊柱与 16 根四肢 Twist 骨，令 `Hips -> Spine..UpperChest -> Left/Right_Shoulder` 的局部平移方向与魂 `orig_t` 完全平行，并导出全身 42 根骨骼的适配局部平移到 `work/soul_adapted_pose.json`（由 `soul.adapted_pose_json` 写入 `Hun_Body_Simple`）。实测全动作手部误差为 **`0.0 cm`**。
3. 替换模型挂到魂的 `Hun_Hair_Band` 部件上（默认服装里一直存在），用模型自己的骨架。
4. 模型的后处理动画蓝图：`RetargetPoseFromMesh`（从父组件取魂的姿势）→ 可选 `RigidBody`（头发/尾巴/裙摆/袖子物理）→ 输出。
5. 材质借用魂已有的材质实例，只覆盖它们引用的贴图（不新建材质，避免着色器问题）。
6. 只打包 mod 自己的资源、被覆盖的部件和贴图；**绝不打包魂的骨架**（会破坏全部动画）。

## 开工前对齐（一次问完）

- 模型授权是否允许改造和分发（VRChat 模型常见 VN3 许可）。
- 范围：默认只替换成年魂默认服装；其他服装/少年魂/过场不保证。
- 放大倍数、要保留/去掉的部件、需要哪些变体（带不带物理、可选配件）。
- 工作区放哪；工具装哪；UE 缓存放哪（用户环境变量 `UE-LocalDataCachePath`）。
- **成品放工作区 `release/`，不要写进游戏目录**，用户自己拷。

## 一次性环境

需要用户亲自做的步骤（装软件、登录、管理员权限）全部放在最前面。

| 组件 | 说明 |
|---|---|
| 游戏 build | `steamapps/appmanifest_4115450.acf` 的 `buildid`；usmap 和 AES 必须匹配 |
| usmap | 社区仓库 `KintaroModding/phantombladezero_pbzusmap`，按 build 选；放 `deps/` |
| AES | 写进 `deps/aes.txt`（值和来源见 references/facts.md） |
| UE 5.7.x | 与游戏引擎版本一致；Epic 启动器只用于安装 |
| MSVC | ≥ 14.44.35211；UBT 按 cl.exe 的 ProductVersion 判断 |
| NetFxSDK 4.8.1 | 没有管理员权限：解压 NDP481-DevPack，在 HKCU `Software\Microsoft\Microsoft SDKs\NETFXSDK\4.8.1` 写 `KitsInstallationFolder` |
| CUE4Parse | `git clone --recursive FabianFG/CUE4Parse`；另需 `oodle-data-shared.dll`、`zlib-ng2.dll` 放一个目录；离线预览读 ACL 动画还需要自己编译 `CUE4Parse-Natives`（含 ACL） |
| Blender 5.x | 安装 `io_scene_psk_psa` 扩展（导入游戏 pskx） |

## 流程

工作区结构（通用代码全部独立在 `tools/`，每个替换模型一个目录，通常只需 `mod.json`）：

```
<工作区>/
  env.ps1  soul.json  deps/  tools/  ProjectPBZ/   共用：本机路径、魂的资源路径、usmap/AES、独立脚本、UE 工程
  work/soul/                                     共用：魂的骨架参考、空身体
  mods/<名字>/
    mod.json  source/  input/  docs/               这个模型的声明式配置、原始文件、解出的 FBX/PNG、安装说明
    work/<变体>/   release/<变体>-<phys|basic>/   中间文件与预览图、成品包
```

```powershell
Copy-Item env.example.ps1 env.ps1   # 按本机修改路径（deps\aes.txt 与 deps\*.usmap 已随仓库自带）
pwsh tools\build-tools.ps1          # 编译 PbzExport + UE 编辑器模块，并试挂载游戏
pwsh tools\extract-game-refs.ps1    # 魂的骨架参考 + 空身体（所有 mod 共用；游戏更新后重跑）
pwsh tools\new-mod.ps1 -Name <名字>   # 生成 mods\<名字>\mod.json（标准全身适配模板）和空目录
# 原始包放 mods\<名字>\source\，按 examples\ciel\mod.json 配置 package 与 model.adapt_humanoid
pwsh tools\build-mod.ps1 -Mod <名字> -Variant <变体> [-NoPhys] [-SkipPrepare]
pwsh tools\preview-retarget.ps1 -Mod <名字>   # 离线预览：手误差（cm）+ 对比图 + 手臂穿模统计
```

`build-mod.ps1` 依次执行：`tools/extract_package.py`（若配置了 `package`）→ Blender（`make_textures.py`、`prepare_model.py` + `adapt_humanoid.py` + `keep_morph.py`、静止姿势预览图）→ UE（`setup_assets.py`）→ 烹饪 → UnrealPak IoStore 打包 → `verify_release.py` → 复制 `docs/安装说明.md`。每一步都会断言，失败时直接报错并给出日志路径。

UE 工程只有一份：**同一时间只能构建一个 mod**，构建会清掉上一次生成的覆盖资源。`ProjectPBZ/Saved/active_mod.txt` 记录当前生成的是哪个 mod/变体，preview 会核对，不对就报错。各 mod 的 `mod_name` 不能重复。

### 配置 mod.json 的顺序

1. 先填 `package`（指定 `source/*.unitypackage`），或用 `tools/extract_package.py` 解出 FBX/PNG 看清骨骼名、网格名、材质名、形态键（作者默认值在 Unity 动画控制器/预制体里）。
2. 填 `model`：`material_slots` 必须覆盖所有材质；启用 `adapt_humanoid`（配置 `hand_scale`、`cuff_margin_cm`、`reparent_bones`）。
3. 填 `retarget.chains`（标准 Humanoid + 16 条 Twist 链已内置在 `new-mod.ps1` 模板中，参考 `examples/ciel/mod.json`）。
4. 填 `physics.groups`：先跑 `-NoPhys` 确认模型、姿势、材质正确，再配置头发、尾巴、裙摆、飘带物理。
5. 填 `materials`/`textures`：每个材质槽借一个魂的材质实例，覆盖它的 BC/N/ARM 贴图。
6. （可选）要眨眼/表情：`model.keep_shape_keys`（源形态键名 → UE 名）+ `face.blink`（`interval_sec` / `jitter_sec` / `duration_sec` / `phase_sec`）；构建日志应出现 `模型形态键 ['Blink']` 与 `眨眼事件图 编译 True`。
7. （可选）双手武器左手脱柄：在 `soul.two_hand_grip` 配置握持修正（全身适配后左右手相对关系会变化，手腕跟随但可能离刀柄；后处理 ABP 用 `ik_hand_gun → ik_hand_l` 驱动左臂 IK 并沿刀轴限制可达范围，详见 `references/two-hand-grip.md`）。

## 问题与修法

| 现象 | 原因 | 修法 |
|---|---|---|
| 大宽肩、端肩架肘、手握不住武器 | 未启用全身比例逆向适配，魂肩高比少女高 10~17 cm；或 `hand_l/r`（25.402cm）长度不匹配 | 启用 `model.adapt_humanoid` + `soul.adapted_pose_json` + `retarget.skip_auto_align: true`（见 `examples/ciel/mod.json`，全动作手误差 `0.0 cm`） |
| 双手武器左手握不住刀柄（攻击后回待机脱手） | 全身比例适配改变了左右手在武器空间的相对关系；手腕对齐魂不等于握住武器 | 在 `soul.two_hand_grip` 启用双手握持修正（详见 `references/two-hand-grip.md`，离线预览中用实际后处理蓝图与武器空间握持统计量化） |
| 手腕/肩膀挥刀时拧麻花 | 缺少四肢 Twist 扭转骨映射 | `adapt_humanoid` 自动生成 16 根 Twist 骨，确认 `retarget.chains` 中包含 16 条 `*Twist1/2` 链 |
| 袖口荷叶边/百褶被拉变形 | 前臂拉长时把袖口一起拉伸了 | 调大 `model.adapt_humanoid.cuff_margin_cm`（如 `2.0`~`3.0`），让末端袖口做纯刚性平移 |
| 袖子/裙摆乱飞 | 物理拉回绑定姿势，T 姿势下它们是水平的 | `model.rest_rotations` 转到下垂方向；根部用重力 + 弱驱动 + 大阻尼 |
| 转身时头发/尾巴瞬移 | `WorldAlpha` 默认 0 | `physics.world_alpha` 0.5~0.7 |
| 尾巴/裙子穿腿 | 默认关闭全部碰撞 | `kinematic_colliders` 加腿 + `collide_pairs` |
| 脚、身体从衣服里穿出 | 形态键未设作者默认值 | 在 `shape_keys` 里填入 prefab/anim 里的默认 Shrink 形态键值 |
| 要不眨眼（死的表情） | 形态键被烘焙成静止形状，游戏里无法再驱动 | `model.keep_shape_keys` + `face.blink`（见 references/mod-json.md），后处理 ABP 用 `SetMorphTarget` 定时驱动；默认 `interval_sec` 4.2 → 平均 4.17s 一次，不会过频 |
| 刘海下一块色片 | 半透明叠加阴影层 | `drop_materials` |
| 角色没换 / 动画全坏 | 文件不全、版本不符 / 打包了魂骨架 | 看 verify 输出；`PbzExport where <路径>` 查资源来自哪个容器 |

细节、版本号、引擎坑：references/facts.md。

## 工作原则

- 不确定的游戏资源结构，先用 `tools\PbzExport\bin\Release\net10.0\PbzExport.exe`（`list/find/where/json/export/anim`）导出来看，不凭记忆。引擎行为查 UE 的 `Engine/Source` 源码确认。
- 每次改动后至少跑一遍 build-mod（含 verify）和 preview。交付时说明哪些只在离线检查里验证过、哪些没进游戏测。
- 用户反馈游戏内问题时，要截图和具体动作，先在 preview 里复现并量化，再改参数。
