// 编辑器工具函数：给 Python 脚本调用，生成物理资产和后处理动画蓝图（只在编辑器里用，不进 mod 包）
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PbzModEditorLibrary.generated.h"

class UAnimBlueprint;
class UPhysicsAsset;
class USkeletalMesh;

UCLASS()
class UPbzModEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// 新建空物理资产（给游戏路径占位用）
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static UPhysicsAsset* CreateEmptyPhysicsAsset(const FString& PackagePath);

	// 按网格权重为每根骨骼生成胶囊体和关节；不自动设到网格上
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static UPhysicsAsset* CreatePhysicsAssetFromMesh(const FString& PackagePath, USkeletalMesh* Mesh, float MinBoneSize);

	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static TArray<FName> GetPhysicsBodyBones(UPhysicsAsset* PhysicsAsset);

	// 刚体：模拟/运动学、重力、阻尼、碰撞开关。bEnableCollision 为 true 时该刚体可以和其他开启碰撞的刚体相撞（尾巴撞腿）
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static bool ConfigureBody(UPhysicsAsset* PhysicsAsset, FName BoneName, bool bSimulated, bool bEnableGravity, float LinearDamping, float AngularDamping, bool bEnableCollision);

	// 碰撞对：物理资产默认关闭所有碰撞对，这里按骨骼名单独打开/关闭一对（引擎的 EnableCollision 不是 UFUNCTION，Python 调不到）
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static bool SetBodyPairCollision(UPhysicsAsset* PhysicsAsset, FName BoneA, FName BoneB, bool bEnable);

	// 关节：锁定位移，限制摆动/扭转角度；Stiffness > 0 时加 SLERP 角度驱动，让骨骼回到原姿势
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static bool ConfigureJoint(UPhysicsAsset* PhysicsAsset, FName ChildBone, float SwingLimitDeg, float TwistLimitDeg, float DriveStiffness, float DriveDamping);

	// 后处理动画蓝图：RetargetPoseFromMesh（从父组件取魂的姿势）→ [刚体模拟] → 输出姿势，并编译
	// WorldAlpha 把角色的世界空间加速度传进局部模拟：0 时头发和尾巴转身没有惯性（直接跟过去），大于 0 时会慢慢摆过去。返回日志，第一行是 OK 或 FAIL
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static FString BuildRetargetPostProcessGraph(UAnimBlueprint* AnimBlueprint, UObject* IKRetargeter, UPhysicsAsset* SecondaryPhysics, float WorldAlpha);

	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static bool SetPostProcessAnimBlueprint(USkeletalMesh* Mesh, UAnimBlueprint* AnimBlueprint);

	// 后处理动画蓝图的事件图表里生成「定时眨眼」：Update Animation → 相位/三角窗算权重 → SetMorphTarget(名字, 权重)
	// IntervalSec 两次眨眼中心点的间隔（秒）；DurationSec 单次眨眼的闭+张总时长（秒）；PhaseSec 起始相位偏移（秒）
	// JitterSec 间隔抖动幅度（秒）：每帧把正弦抖动加在时间上，避免机械的定频眨眼。返回日志，第一行是 OK 或 FAIL
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static FString BuildBlinkEventGraph(UAnimBlueprint* AnimBlueprint, FName MorphTargetName, float IntervalSec, float DurationSec, float PhaseSec, float JitterSec);

	// ---- 离线预览（不创建资源） ----
	// 网格上的形态键（morph target）名，用于确认眨眼形态键真的导入进 UE 了
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static TArray<FName> GetMorphTargetNames(USkeletalMesh* Mesh);

	// 回读眨眼事件图：把 SetMorphTarget 节点的 morph 名、关键常量、连线状态导出成文本（离线验证用）
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static FString DescribeBlinkEventGraph(UAnimBlueprint* AnimBlueprint);

	// 形态键的逐 LOD 统计（有位移的顶点数、最大位移 cm），用来确认几何真的在网格里而不只是名字
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static FString DescribeMorphTargets(USkeletalMesh* Mesh);

	// 回读物理资产里每根骨骼的真实设置（是否模拟/重力/阻尼/碰撞、关节 swing/twist 上限与驱动刚度阻尼）。
	// 用来确认 mod.json 的 physics 参数真的写进了资产，而不是只看构建日志里的分组名单。
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static FString DescribePhysicsBodies(UPhysicsAsset* PhysicsAsset);
	// 网格参考骨架的骨骼名（下面两个函数的输入输出都按这个顺序）
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static TArray<FName> GetMeshBoneNames(USkeletalMesh* Mesh);

	// 多帧局部姿势（帧优先，每帧按 BoneNames 顺序）→ 组件空间姿势（帧优先，网格骨骼顺序）。缺的骨骼用网格参考姿势；输入不对返回空数组
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static TArray<FTransform> LocalPosesToComponentSpace(USkeletalMesh* Mesh, const TArray<FName>& BoneNames, const TArray<FTransform>& LocalTransforms, int32 NumFrames);

	// 用重定向资源把源网格的组件空间姿势转成目标网格的（和游戏里 RetargetPoseFromMesh 节点同一个处理器）；失败返回空数组
	UFUNCTION(BlueprintCallable, Category = "PbzMod")
	static TArray<FTransform> RetargetComponentPoses(UObject* IKRetargeter, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, const TArray<FTransform>& SourceComponentPoses, int32 NumFrames);
};
