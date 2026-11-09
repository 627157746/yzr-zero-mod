// 编辑器模块：生成后处理动画蓝图、物理资产等（只在编辑器里用，不进 mod 包）
using UnrealBuildTool;

public class PbzModEditor : ModuleRules
{
	public PbzModEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "AssetRegistry", "BlueprintGraph", "PhysicsCore", "PhysicsUtilities", "IKRig" });
	}
}
