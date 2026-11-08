# 影之刃零主角替换：事实与排错记录

以下数据基于 Steam build 26367565 / UE 5.7.4。游戏更新后，带 ⚠ 的条目要重新核实。

## 版本绑定数据 ⚠

- 引擎：UE 5.7.4（游戏 exe 版本号可确认）。
- usmap：`ProjectPBZ-5.7.4-302256-full.usmap`（pbzusmap v1.1，对应 build 26367565；旧 build 26266538 用 301524）。
- AES：`0x4BC8A51E5381325073F2BC7D4E3089CD557C4E9B7D453901C2C795518867E01D`。
- mod 目录：`<游戏>/ProjectPBZ/Content/Paks/~mods/`，普通用户就有写权限。

## 魂的资源结构

| 资源 | 路径 / 说明 |
|---|---|
| 骨架 | `/HuaShira/Characters/Hero/Hero_Hun/Hun_Skin_Skeleton`，1134 骨（含面部），37 个插槽，10 个虚拟骨 |
| 身体 | `.../Hero_Hun/SkinParts/Hun_Body_Simple`：301 骨，物理资产 `PA_Hun`，后处理 `ABP_Hun_PostProcess`，10 个布料材质槽 |
| 挂新模型的槽 | `.../SkinParts/Hun_Hair_Band`（物理 `Hun_Hair_Band_PhysicsAsset`，材质 `MI_ZeroHun_Cap_01`） |
| 需要清空的部件 | SkinParts 下 `Hun_Head`、`Hun_Head_New`(726 morph + DNA)、`Hun_Head_New_MoQu`、`Hun_Hair`、`Hun_Scarf`、`Hun_Scarf_Climax`、`Hun_BrowLashCard`、`Hun_BrowLashCardMoQu`；`Hats/Hun_Hat_00/01`；布料 `SkinParts/Cloth/CA_Hun_Cape/Hair01/Hair02/Skirt/UpperCloth` |
| 外观表 | `DT_PBZAppearanceTable`：默认服装 `BodyMeshAsset=Hun_Body_Simple`、`MeshBandAsset=Hun_Hair_Band`；其他服装用各自目录下的网格（没覆盖它们就会显示原版） |
| 武器挂点 | `weapon_r_Socket` 挂在 `weapon_r`，`weapon_r ← hand_r ← lowerarm_r ← upperarm_r ← clavicle_r ← spine_05`；武器配置 `ArmedSocket=weapon_r_Socket` |
| 动画 | `Plugins/HuaShira/Content/Animation/Hero/Hun/SH_Sources/<武器类型>/...`（战斗），`Characters/Hero/Hero_Hun/Animation/Locomotion/Unarmed/...`（非战斗）；ACL 压缩 |

骨骼平移重定向模式：1067 根 `AnimationScaled`（含 clavicle/upperarm/lowerarm），65 根 `Animation`（含 hand、weapon、ik_*）。引擎实现在 `AnimationRuntime.cpp` 的 `RetargetBoneTransform`：动画平移 × (当前网格参考平移长度 / 骨架参考平移长度)。

魂手臂参考局部平移：clavicle (4.84, 0.12, ±7.99)、upperarm (±12.14, 0, 0)、lowerarm (±28.46, 0, 0)、hand (±25.40, 0, 0)；臂展 53.9 cm，肩宽 38.7 cm，上臂下垂 43.2°。

缩短手臂参考值对手位置的影响（每缩 10%）：锁骨偏移向内 0.8 cm、向下 0.5 cm；锁骨长度向内 1.1 cm、向上 0.4 cm；上臂长度向内 1.2 cm、向上 2.6 cm。上臂缩太多手会抬高，所以锁骨多缩、上臂少缩。臂长约 40 cm 的模型（魂 53.9 cm）实测 `clavicle 0.45, upperarm 0.55, lowerarm 0.85` 时，待机/走/跑/多数攻击手误差 0 cm，双手长兵器个别帧 8~13 cm。其他臂长按比例起步，用 `tools\preview-retarget.ps1` 的手误差调。

## 全身比例逆向适配 + 静止姿势烘焙根治方案（`ciel` 实测 9 段动作 72 帧 0.0cm 误差，2026-11-08）

1. **为什么局部缩手臂（`soul.arm_scale`）会「端肩、架肘、大宽肩」**：
   - `Hun_Skin_Skeleton` 中 1067 根骨骼（含 `Pelvis`、`spine_01..05`、`neck_01..02`、`Head`、`clavicle_l/r`、`upperarm_l/r`、`lowerarm_l/r`、`thigh_l/r`、`calf_l/r`、`foot_l/r` 及全部 `*_twist_*` 骨）均为 `EBoneTranslationRetargetingMode::AnimationScaled`，仅 `hand_l/r`（局部长度 `25.40205 cm`）、手指、`weapon_*`、`ik_*` 为 `Animation`（`ball_l/r` 为 `Skeleton`）。
   - 若只改 `clavicle / upperarm / lowerarm`，`Pelvis`（109.2 cm）与 `spine_01..05`（+40.9 cm）仍维持成年男性高度，魂的肩膀停留在 **150.6 cm**，比二次元少女肩高（131~138 cm）高出 12~18 cm，`TwoBoneIK` 强行把少女手臂往上拽。
2. **为什么 `AnimationScaled` 下必须保证 `new_t` 与 `orig_t` 方向平行**：
   - UE `FAnimationRuntime::RetargetBoneTransform` 对 `AnimationScaled` 骨骼的计算是：沿 `Hun_Skin_Skeleton` 的原始局部平移方向 $\frac{\text{orig\_t}}{\|\text{orig\_t}\|}$，乘以空身体网格 `SK_SoulBlank` 的参考局部平移长度 $\|\text{new\_t}\|$，即 $\text{orig\_t} \cdot \frac{\|\text{new\_t}\|}{\|\text{orig\_t}\|}$。
   - 而替换模型在 `IKRetargeter`（`Spine`、`Left/RightClavicle` 的 `TranslationMode = None`）下沿模型自身的静止局部向量 $\text{new\_t}$ 步进。
   - 若模型的内部躯干/锁骨向量 $\text{new\_t}$ 与 $\text{orig\_t}$ 不平行（例如 `UpperChest -> Left_Shoulder` 的仰角/前后偏移与魂 `spine_05 -> clavicle_l` 不同），运行时 `Hun_Body_Simple` 与替换模型的肩关节就会产生 2~3 cm 的三维错位，导致伸直手臂的动作（如持剑待机 `Hun_SoftSword_Stance_Idle_Loop`）出现 1.8 cm 脱手。
   - **解法**：在 Blender 编辑模式（Step 5，不移动任何网格顶点，仅调整内部关节轴心）中，从 `Left/Right_Shoulder` 沿魂 `spine_05 -> clavicle_l` 的精确方向反推 `UpperChest`（`spine_05`），再从 `UpperChest` 到 `Hips` 按魂 `pelvis -> spine_01..05` 的方向向量设置 `Hips`、`Spine`、`Spine_02`、`Spine_03`、`Chest`，使每节骨骼满足 $\text{new\_t} = r_i \cdot \text{orig\_t}$。此时 $\text{orig\_t} \cdot \frac{\|\text{new\_t}\|}{\|\text{orig\_t}\|} \equiv \text{new\_t}$，魂空身体与替换模型在所有动作帧下的关节位置 **100% 数学恒等**。
3. **前臂 `hand_l/r`（`Animation` 模式）等长 + 袖口刚性平移零形变**：
   - 因为 `hand_l/r` 是 `Animation` 模式，动画永远输出固定 `25.40205 cm` 的前臂局部长度。
   - 在 Blender T-pose 下，保持上臂长度与肩宽完全不动，仅把前臂中段袖管沿手臂轴适配到 `25.40205 cm`，而末端 `2.0 cm` 的束口环与百褶袖口做**纯刚性平移**（0% 拉伸变形），手掌绕腕关节放大 `1.12x` 适配刀柄。
4. **静止姿势烘焙（`Apply Pose as Rest Pose`）+ 16 根四肢 Twist 骨细分**：
   - 在 Blender Pose Mode 下将四肢、手腕、五指对齐魂的静止姿势方向，应用 Armature 修改器并 `Apply Pose as Rest Pose`，随后再执行鞋底落地（确保 `prepare_model.py` 后续不再产生二次高度抬升），并在 `mod.json` 中设 `"skip_auto_align": true`。
   - 自动细分 `Spine_02/03`、`UpperChest`、`Neck_02` 及 16 根四肢 Twist 骨（注意魂的小腿/前臂 Twist 顺序是 `02` 在近端 1/3、`01` 在远端 2/3；大腿/上臂是 `01` 在近端 1/3、`02` 在远端 2/3），按投影线性插值重分配蒙皮权重并在 `SOUL_CHAINS` / `retarget.chains` 中一一映射。
   - 完整实现已封装为独立公共模块 `tools/blender/adapt_humanoid.py`，声明式配置参考 `mods/ciel/mod.json` 与 `.agents/skills/pbz-protagonist-replace/examples/ciel/mod.json`。

## 引擎 / 工具坑

- 命令行导入网格时 Interchange 会通知内容浏览器导致崩溃：先执行 `Interchange.FeatureFlags.Import.SyncToBrowser 0`。
- `UIKRetargetBatchOperation` 在命令行下会无条件调用 Slate 通知并崩溃：离线求值直接用 `FIKRetargetProcessor`（`ApplySourceScaleToPose` + `RunRetargeter`）。
- `UPhysicsAsset::EnableCollision/DisableCollision` 不是 UFUNCTION，`UAnimSequenceBase::GetController` 不能被 Python 调用：都要包一层 C++。
- C++ 函数如果用 out 参数，Python 返回值会变成元组，容易拆错：改成返回 `FString` 日志（第一行写 OK/FAIL）。
- `AnimNode_RigidBody`：模拟刚体由关节驱动拉回**绑定姿势**的相对朝向，不跟随动画；`WorldAlpha` 默认 0（没有转身惯性）。绑定姿势里水平的头发链 + `gravity=false` + 小 swing = 游戏里头发永远水平炸开，必须同时修静止姿势和开重力。
- IK 手部重定向（`RunIKRig` + `blend_to_source_translation=1`）只约束手的位置，不约束肘的弯曲幅度：模型臂长与魂动画「肩→手」距离相差 Δ 时，肘偏约由 Δ 决定，而魂动画里这个距离本身就在变化。`arm_pole_deg`（AlignPoleVectorOp 的 `StaticAngularOffset`）和 `AlignAlpha` 只改变弯曲平面的方向，不能消除弯肘；若不使用全身 `adapted_pose_json` 而只把 `soul.arm_scale` 放大到 >1 让肩手距接近臂长，会让攻击动作够不到武器（实测：手误差 3.1→7.7 cm、肘偏 13.5→4 cm、手臂穿模 2616→3822）。根治应使用 `adapt_humanoid` 全身比例逆向适配。
- `FPhysicsAssetUtils::CreateFromSkeletalMesh` 设 `bDisableCollisionsByDefault=true` 时会关闭全部碰撞对；刚体 `CollisionReponse=Disabled` 时会被整体忽略。
- 退化三角形会被剔除：空网格三角形边长至少 0.5 cm。
- Blender 导入 FBX 会去掉形态键名的命名空间前缀（`Body_BS.Breast_Big` → `Breast_Big`），按后缀匹配。
- 旧局部缩放方案下，模型臂长被压缩后攻击帧握不住武器（`IKRigLimbSolver` 无 stretch）：把 `soul.arm_scale` 整体缩小能把 IK 目标距离拉回模型臂长范围（实测 `0.8/0.92/0.98 → 0.7/0.95/0.78` 让最大手误差 10.3→5.0cm、非 0 帧 19/80→5/80）。**锁骨多缩、上臂几乎不缩、前臂缩**：上臂每缩 10% 手抬高 2.6cm；`0.6/0.7/0.8` 那组内收最多但手抬高 4.9cm、穿模翻倍。
- `retarget.arm_motion`（UE `IKRetargeter` 的 `ScaleVertical` / `Extension` / `StaticOffset` / `BlendToSourceWeights` / FK `RotationAlpha`）能真正压低手位和摆幅，但**只移动模型的手、不移动魂的武器挂点**。`goal_scale_vertical=0.95 + goal_extension=0.92` 让跑步最高点 -5.8cm、Z 摆幅 -11%，代价是 80/80 帧全程约 4cm 偏差、穿模翻倍。摆幅与握持在此架构下互斥。
- 想让手/武器一起下移而不脱手，只能改 `soul.arm_offset`（魂参考姿势常量偏移），且必须作用在 **upperarm_l/r**：左右上臂旋转相同、局部 Z 平移为 0，同号偏移保持对称；锁骨不行（`clavicle_l/r` 局部 Z 是 -7.99/+7.99 镜像，同号偏移会把左右手 X 拉歪 6.7cm）。这类常量偏移只改静态位置，不改动画摆幅。
- `StretchChainOp` 的 `MatchSourceLength` 按当前帧**链长**（相邻骨骼距离之和）匹配，不是肩→手直线距离；源链长在动画里恒定，所以救不了短臂够不到远处 IK 目标的问题。
- **旧局部缩放方案下，弓形（肘偏）与脱手严格此消彼长，且都要看 D 而不是只看链长 L**：弓形 = max(0, L-D)，脱手 = max(0, D-L)。缩小 `soul.arm_scale` 会把 D 拉小 → 脱手好了但弓形加重（D 43→40cm，待机肘偏 3~4→9cm）。**调完参数必须同时量化肘偏、手误差、穿模三项**，只盯一项必然翻车。
- **离线「肘偏」不能单独代表游戏内的「弓形」观感**：可能原因：① 肘偏只衡量肘到肩手连线的距离，不含手/武器贴合——手抬高或攻击帧脱手时，绷直但错位更容易被看成姿势怪；② `preview_retarget.py` 每动作只采 4 帧、只跑 `FIKRetargetProcessor`，不执行 PostProcess ABP 与动画混合；③ 第三人称视角下各方向分量的可见度不同。**结论：手臂观感一律以进游戏实测为准；离线三项指标只用于筛选方案和排查回归。**
- `AlignPoleVectorOp.StaticAngularOffset` 是绕「肩→手」链轴旋转链根：3D 肘偏、手误差、3D 姿势幅度**完全不变**，只改变肘在垂直平面里的方位。角度可超出 0~90，负值/大角度都合法。
- `IKChainsOp.BlendToSourceTranslation` 从 1 降到 0 并**不能**让链恒伸直：实测待机肘偏 2.10→3.54cm、手误差 1.1→10.8cm，w=1 本来就是最优（FK 目标位置不是简单的「肩+方向×L」，还叠了 NormalizedLimbLength、PelvisMotion、极向量）。
- UE 5.7 的 IKRig 除 LimbSolver 外还有 `FIKRigStretchLimbSolver`（`bEnableStretching` / `MaximumStretchDistance` / `StretchStartPercent`，FABRIK 迭代）：`bEnableStretching` 能把最大手误差 10.7→3.0cm（够得到远目标），但肘偏 10.90→11.17 **没改善**、且每帧恒定 3cm 偏差——squash 只是压缩链的形变，不消除弯肘。
- **下移 IK 目标（`ScaleVertical` / `StaticOffset.Z`）能让手臂伸直、不能治跑步弯肘**：只动手不动武器挂点，手越往下手臂越压进躯干，且持续 3~5cm 脱手。
- **外观陷阱**：VRChat 和服/宽袖模型正面「手臂张开」常常主要是**大袖网格**（袖子链的静止姿势方向向外下方）而不是手臂骨骼。调参前先确认用户看到的是哪一层，避免调错对象。
- CUE4Parse 读到的动画轨道已经是 UE 局部坐标，ActorX 导出 psa 时才做 Y 轴镜像；在 UE 里重建姿势不用转换坐标。
- 读 ACL 动画需要 `CUE4Parse-Natives.dll`（CMake + VS 编译，ACL v2.1.0）。
- UE 命令行工具运行需要 NetFxSDK；UBT 同时查 HKCU 和 HKLM，所以没有管理员权限也能用。

## VRChat 模型常见处理

- `generated_textures` 的 `arm_from_mask` 把遮罩灰度**直接写进 B 通道当金属度**（白=1.0），不是只调粗糙度。别把皮肤渐变/身体遮罩当金属遮罩用：若误用大面积白色的身体遮罩，整套服装金属度接近 1.0，白裙会渲染成银甲、深色袖套被高光洗成银灰。没有真正的金属装饰遮罩时直接用 `type: arm` + `metal: 0`。
- 头发绑骨从水平改成下垂贴身后，与 head/spine 挡板的碰撞对会产生持续接触抖动。贴身长发要么别开碰撞，要么收硬到接近静态（swing ~10°、stiff 300+、damp 45+、关重力）；绑定姿势本身已是下垂时，关重力不会让头发重新水平。
- 卡通模型（lilToon/UTS2）的 albedo 是平涂的，粉发的饱和度、黑袖套/袜子的黑度靠 shader 的阴影分级叠出来，换到写实 PBR 材质后会整体发白。补偿办法：把饱和度与暗部对比**预烘焙进 BC 贴图**（只压低饱和的深色区域，保住彩色装饰），并把 `N` 槽换成平法线 —— 魂材质实例常有 `NormalMap_NormalStrength=5.0` 这类参数，4096² 卡通法线被放大 5 倍后高光会把颜色洗成银白；换平法线还能把包体减半。
- 移动/转身首帧布料「瞬移一下」：`WorldAlpha>0` 的世界惯性会让刚体在组件位移突变时 snap。设 `world_alpha: 0`、或让贴身的头发/飘带链改成跟随动画（`skip_match` 里排除），都能减少变量；如果连上衣（无物理骨）也跳，就要怀疑动画切换/组件姿势而不是物理，需进游戏用无物理包对照。
- Humanoid 只映射了部分骨骼，其余是 PhysBone 链（头发、尾巴、袖子、飘带）：按名字分组设物理参数，参数可参考 prefab 里的 VRCPhysBone（pull/spring/stiffness/gravity/limit）。
- 形态键默认值在 Unity 动画控制器（FX Layer）和参数资源里，不在 FBX 里；漏掉会导致穿模（脚、胸、衣服）。
- 配件骨架（面具、尾巴、袖子）在 prefab 里挂在身体骨骼下，Blender 合并时按 prefab 的局部变换对位。
- lilToon 的卡通阴影/描边没法用魂的写实材质还原，交付时说明。

## 交付检查清单

- [ ] 成品放在工作区 `release/`，没有写入游戏目录
- [ ] 每个变体 3 个文件：`.pak`、`.utoc`、`.ucas`
- [ ] `verify_release.py` 全部 OK（覆盖来源、骨架未被覆盖、动画蓝图、物理资产、材质）
- [ ] `preview-retarget.ps1`：待机/走/跑手误差接近 0
- [ ] 给玩家的安装说明：变体区别、安装/卸载、3 个文件、已知限制
- [ ] 说明哪些只在离线预览里验证、没进游戏测过
