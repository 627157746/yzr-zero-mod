# 换模型时做双手武器握持适配

本流程适用于使用 `soul.adapted_pose_json` 全身比例适配的替换模型。它针对的是“手腕准确跟随魂的动画，但改比例后左手离武器握柄”的情况。它**不**把模型重新绑定到游戏骨架，也不改原动画。

实现放在公共 `tools/` 和 `ProjectPBZ/Source/PbzModEditor/` 中。每个模型只需在自己的 `mods/<mod>/mod.json` 中启用 `soul.two_hand_grip`；不要拷贝或改写 Ciel 的模型专属数据。此修正运行于魂身体 `Hun_Body_Simple` 的后处理蓝图，模型替换网格继续由 `Hun_Hair_Band` 上的重定向蓝图驱动。

## 1. 先确认问题类型

先运行正常构建和预览，记录模型左/右手腕对魂手腕的位置误差。若手腕已贴合（常接近 `0.0 cm`），但画面中左手没有抓住双手武器，才进入本流程。手腕误差和武器握持误差是两种指标，不能互相代替。

确定目标武器及测试状态：至少找出其常态待机、战斗待机、移动/跑步、攻击、攻击结束回待机的动画；再选单手、双刀或徒手动作作为不应触发修正的对照。动画资产路径可用：

```powershell
. .\tools\common.ps1
& $PbzExportExe find LongBlade --limit 100
& $PbzExportExe find <武器类型或动作名> --limit 100
```

不要只凭动画名称判断双手握持。用 `PbzExport.exe anim <资源路径> --out .tmp/<动作>.json --count 8` 导出采样，检查骨骼 `hand_r`、`weapon_r`、`ik_hand_gun`、`ik_hand_l`。把选中的攻击和待机路径加入该 mod 的 `preview_anims`，并保留非握持对照动作。

## 2. 在该 mod 启用

编辑 `mods/<mod>/mod.json` 中的 `soul` 段，加入：

```json
"two_hand_grip": {
  "enabled": true,
  "full_radius_cm": 12.0,
  "zero_radius_cm": 20.0,
  "max_reach": 0.97,
  "min_hand_gap_cm": 9.0
}
```

这些是 Ciel 的**起始值，不是所有模型的标准答案**。参数是魂/武器坐标空间中的握持判断和可达限制，不是替换模型的尺寸比例：

- `full_radius_cm` / `zero_radius_cm`：动画的左手目标离 `weapon_r` 局部 Z 轴的径向距离。小于等于前者时修正权重为 1；大于等于后者为 0；区间中线性混合。按选中的武器动作实测分布调整，两个值都必须大于 0，且 `zero_radius_cm > full_radius_cm`。
- `max_reach`：左臂的目标距离上限，按空身体适配后的左臂参考长度比例计算，范围 `0.5 < max_reach <= 1.0`。不够到握点时，算法沿刀轴正方向把握点滑近右手，不拉长骨骼。若仍够不到，继续下调只会更早滑柄或造成明显姿势变化，需检查目标动作和真实模型效果，不要盲目调大。
- `min_hand_gap_cm`：轴向滑动时左手与右手间的最小间距，单位 cm。过小会让双手重叠，过大可能让左手无法滑到可达范围。

若没有明确双手武器用例，保持 `enabled: false` 或不写这一段。它是逐 mod 开关，不会自动影响其他模型。

## 3. 构建并检查成品

先确认 `ProjectPBZ/Saved/active_mod.txt` 当前不是另一 mod 的未完成操作；UE 工程共用，一次只能构建一个 mod。之后运行：

```powershell
pwsh tools\build-tools.ps1 -SkipExport
pwsh tools\build-mod.ps1 -Mod <mod> -Variant <变体> -SkipPrepare
```

构建应生成并编译 `ABP_SoulGrip`。检查日志中的 `双手握持 ABP_SoulGrip 编译 True` 和节点回读。尤其确认：

- `CopyBone` 为 `hand_r → ik_hand_gun`，`bCopyTranslation=True`、`bCopyRotation=True`、`ControlSpace=BCS_ComponentSpace`。
- `TwoBoneIK` 的 IK 骨为 `hand_l`，effector 为 `ik_hand_l`，空间为 `BCS_BoneSpace`，从目标取旋转且禁止拉伸。
- 两个 AnimGraph 输出引脚连接到 `GripAlpha` 和 `GripOffset`；事件图执行 `Set GripAlpha` 和 `Set GripOffset`。

UE 编译器会用未连线引脚的默认值覆盖 AnimGraph 结构体字段。编辑器里读到 `True` 不够，成品检查会直接解析 cooked `ABP_SoulGrip` 常量；若 `verify_release.py` 报错，不能交付该包。

每次调完参数都重构建，并运行：

```powershell
pwsh tools\preview-retarget.ps1 -Mod <mod> -Count 8 -NoRender
```

核对逐动作的 `双手握持` 行，分别观察握柄帧数、修正前/后的径向误差、沿武器轴滑动量、`alpha`，以及原有左右手腕误差。对于筛选为握持的帧，修正后径向误差应明显下降；滑动只能解释轴向偏移，不能把径向脱手藏起来。检查单手/双刀/徒手对照的 `alpha` 应为 0 或接近 0。

需要逐帧检查时，可提高导出采样数；普通 `preview-retarget.ps1` 对每个动作均匀采样，并非完整游戏状态机。长循环待机适合用于密集连续帧离线评估，但“攻击→回待机”插值探针只用于筛选回归，不代表实际游戏混合。最终仍须进游戏实际测试动作切换和武器。

若需要渲染对比，运行 `preview-retarget.ps1`（不带 `-NoRender`），对照图的绿点是原版武器空间握点。渲染器不画武器本体，绿点只用于判断手相对原握点的位置，不代替游戏截图。

## 4. 结果如何判断

- 常态/战斗待机：观察手是否落到握柄上；记录径向误差与轴向滑动。手臂长度不足时，沿柄滑动是有意的可达补偿，不等于精准回到原握点。
- 攻击：逐个动作观察是否有短暂脱柄、手臂过伸或肘部翻转。`alpha` 由上一帧姿势计算，快速动作变化会有一帧滞后。
- 非握持对照：确认几何门控不会让单手、双刀、徒手动作误触发。若这些姿势的左手靠近主武器轴，单一径向门控可能误判；该实现没有读取游戏的武器类型或握持状态变量。
- 穿模：同时记录穿模顶点数/深度和手部误差。修正会移动左臂，穿模可能改善也可能局部变差；不要只以总数或某一个动作作结论。
- 实机：用目标武器做“常态待机→攻击→松开/攻击结束→常态待机”，再测移动、重新攻击、拔收刀、处决、换武器和不同物理包。拍图/录屏，并确认掌心、手指和刀柄的真实接触。

完全通过构建与离线预览，只能描述为“成品检查及离线姿势验证通过”。没有实际进游戏就必须标明“实机未验证”。

## 5. 已知边界

当前启用判据是几何距离，不是游戏状态机里的武器类型/双手持握曲线；没有保证覆盖所有武器、动作、LOD 或动画混合。它修改魂身体的源姿势，替换模型随后重定向，因此不同模型臂长仍会影响可达性。若要精确支持新的武器系列，先添加其动作和非握持反例，收集手/武器/IK 目标数据，再校准门控；几何条件分不开时，不应靠放宽阈值假装识别成功。

Ciel 的实测数据和具体误差见 [Ciel 双手握持修正验证记录](../../../../mods/ciel/docs/双手握持验证.md)。字段定义见 [mod.json 字段说明](mod-json.md)；AnimGraph 引脚默认值的引擎行为及成品检查原因见 [事实与排错记录](facts.md)。
