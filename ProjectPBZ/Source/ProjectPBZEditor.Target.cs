using UnrealBuildTool;

public class ProjectPBZEditorTarget : TargetRules
{
	public ProjectPBZEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("PbzModEditor");
	}
}
