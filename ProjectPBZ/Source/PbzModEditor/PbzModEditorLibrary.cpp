#include "PbzModEditorLibrary.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AnimGraphNode_Base.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_Self.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetProcessor.h"
#include "Retargeter/IKRetargetProfile.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	// 按类路径创建动画图节点（用反射，不直接链接各节点所在模块）
	UEdGraphNode* SpawnNode(UEdGraph* Graph, const TCHAR* ClassPath, int32 X, int32 Y)
	{
		UClass* Class = FindObject<UClass>(nullptr, ClassPath);
		if (!Class)
		{
			return nullptr;
		}
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, Class, NAME_None, RF_Transactional);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	// 设置节点内部 FAnimNode_xxx 结构里的对象属性
	bool SetNodeObject(UEdGraphNode* Node, const TCHAR* Field, UObject* Value)
	{
		const FStructProperty* NodeProp = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
		if (!NodeProp)
		{
			return false;
		}
		void* NodeData = NodeProp->ContainerPtrToValuePtr<void>(Node);
		const FObjectPropertyBase* Prop = FindFProperty<FObjectPropertyBase>(NodeProp->Struct, Field);
		if (!Prop)
		{
			return false;
		}
		Prop->SetObjectPropertyValue_InContainer(NodeData, Value);
		return true;
	}

	// 设置刚体节点的模拟空间惯性：WorldAlpha 把角色的世界空间加速度按比例传进局部模拟，转身时头发和尾巴才会有滞后
	bool SetRigidBodyInertia(UEdGraphNode* Node, float WorldAlpha)
	{
		const FStructProperty* NodeProp = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
		if (!NodeProp)
		{
			return false;
		}
		void* NodeData = NodeProp->ContainerPtrToValuePtr<void>(Node);
		const FStructProperty* SettingsProp = FindFProperty<FStructProperty>(NodeProp->Struct, TEXT("SimSpaceSettings"));
		if (!SettingsProp)
		{
			return false;
		}
		void* Settings = SettingsProp->ContainerPtrToValuePtr<void>(NodeData);
		auto SetFloat = [Settings](const FStructProperty* Owner, const TCHAR* Name, float Value)
		{
			const FFloatProperty* Prop = FindFProperty<FFloatProperty>(Owner->Struct, Name);
			if (!Prop)
			{
				return false;
			}
			Prop->SetPropertyValue_InContainer(Settings, Value);
			return true;
		};
		// 角速度上限约 3 圈/秒、角加速度上限约 5 圈/秒²：急转身时头发和尾巴摆过去而不是瞬移，也不会甩飞
		return SetFloat(SettingsProp, TEXT("WorldAlpha"), WorldAlpha)
			&& SetFloat(SettingsProp, TEXT("MaxAngularVelocity"), 20.f)
			&& SetFloat(SettingsProp, TEXT("MaxAngularAcceleration"), 30.f)
			&& SetFloat(SettingsProp, TEXT("MaxLinearVelocity"), 1500.f)
			&& SetFloat(SettingsProp, TEXT("MaxLinearAcceleration"), 4000.f);
	}

	UEdGraphPin* FindPosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			const UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
			if (Pin->Direction == Direction && Struct &&
				(Struct->GetFName() == TEXT("PoseLink") || Struct->GetFName() == TEXT("ComponentSpacePoseLink")))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool Connect(const UEdGraphSchema* Schema, UEdGraphPin* From, UEdGraphPin* To)
	{
		return From && To && Schema->TryCreateConnection(From, To);
	}

	// 按名字找重载前的 UFUNCTION（同名同参数签名由调用方保证）
	UK2Node_CallFunction* SpawnCall(UEdGraph* Graph, UClass* ParentClass, const TCHAR* FunctionName, int32 X, int32 Y)
	{
		UFunction* Function = ParentClass ? ParentClass->FindFunctionByName(FName(FunctionName)) : nullptr;
		if (!Function)
		{
			UE_LOG(LogTemp, Error, TEXT("PBZ 找不到函数 %s::%s"), ParentClass ? *ParentClass->GetName() : TEXT("null"), FunctionName);
			return nullptr;
		}
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->SetFromFunction(Function);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	UEdGraphPin* InPin(UK2Node_CallFunction* Node, const FName& Name)
	{
		return Node ? Node->FindPin(Name, EGPD_Input) : nullptr;
	}

	UEdGraphPin* OutPin(UK2Node_CallFunction* Node)
	{
		return Node ? Node->GetReturnValuePin() : nullptr;
	}

	// 可写（非连接）引脚直接写默认值
	bool SetPinValue(UK2Node_CallFunction* Node, const TCHAR* Name, const FString& Value)
	{
		UEdGraphPin* Pin = InPin(Node, Name);
		if (!Pin)
		{
			return false;
		}
		Pin->DefaultValue = Value;
		return true;
	}

}

UPhysicsAsset* UPbzModEditorLibrary::CreateEmptyPhysicsAsset(const FString& PackagePath)
{
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();
	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(Package, FName(*FPackageName::GetShortName(PackagePath)), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	Asset->MarkPackageDirty();
	return Asset;
}

UPhysicsAsset* UPbzModEditorLibrary::CreatePhysicsAssetFromMesh(const FString& PackagePath, USkeletalMesh* Mesh, float MinBoneSize)
{
	if (!Mesh)
	{
		return nullptr;
	}
	UPhysicsAsset* Asset = CreateEmptyPhysicsAsset(PackagePath);
	FPhysAssetCreateParams Params;
	Params.MinBoneSize = MinBoneSize;
	Params.MinWeldSize = KINDA_SMALL_NUMBER;
	Params.GeomType = EFG_Sphyl;
	Params.VertWeight = EVW_DominantWeight;
	Params.bAutoOrientToBone = true;
	Params.bCreateConstraints = true;
	Params.bWalkPastSmall = false;
	Params.bBodyForAll = false;
	Params.bDisableCollisionsByDefault = true;
	Params.AngularConstraintMode = ACM_Limited;
	FText Error;
	if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(Asset, Mesh, Params, Error, /*bSetToMesh*/ false, /*bShowProgress*/ false))
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ CreateFromSkeletalMesh failed: %s"), *Error.ToString());
		return nullptr;
	}
	Asset->SetPreviewMesh(Mesh);
	return Asset;
}

TArray<FName> UPbzModEditorLibrary::GetPhysicsBodyBones(UPhysicsAsset* PhysicsAsset)
{
	TArray<FName> Out;
	if (PhysicsAsset)
	{
		for (const TObjectPtr<USkeletalBodySetup>& Body : PhysicsAsset->SkeletalBodySetups)
		{
			if (Body)
			{
				Out.Add(Body->BoneName);
			}
		}
	}
	return Out;
}

bool UPbzModEditorLibrary::ConfigureBody(UPhysicsAsset* PhysicsAsset, FName BoneName, bool bSimulated, bool bEnableGravity, float LinearDamping, float AngularDamping, bool bEnableCollision)
{
	const int32 Index = PhysicsAsset ? PhysicsAsset->FindBodyIndex(BoneName) : INDEX_NONE;
	if (Index == INDEX_NONE)
	{
		return false;
	}
	USkeletalBodySetup* Body = PhysicsAsset->SkeletalBodySetups[Index];
	Body->Modify();
	Body->PhysicsType = bSimulated ? EPhysicsType::PhysType_Simulated : EPhysicsType::PhysType_Kinematic;
	Body->CollisionReponse = bEnableCollision ? EBodyCollisionResponse::BodyCollision_Enabled : EBodyCollisionResponse::BodyCollision_Disabled;
	Body->DefaultInstance.SetEnableGravity(bEnableGravity);
	Body->DefaultInstance.LinearDamping = LinearDamping;
	Body->DefaultInstance.AngularDamping = AngularDamping;
	PhysicsAsset->MarkPackageDirty();
	return true;
}

bool UPbzModEditorLibrary::SetBodyPairCollision(UPhysicsAsset* PhysicsAsset, FName BoneA, FName BoneB, bool bEnable)
{
	const int32 A = PhysicsAsset ? PhysicsAsset->FindBodyIndex(BoneA) : INDEX_NONE;
	const int32 B = PhysicsAsset ? PhysicsAsset->FindBodyIndex(BoneB) : INDEX_NONE;
	if (A == INDEX_NONE || B == INDEX_NONE || A == B)
	{
		return false;
	}
	PhysicsAsset->Modify();
	if (bEnable)
	{
		PhysicsAsset->EnableCollision(A, B);
	}
	else
	{
		PhysicsAsset->DisableCollision(A, B);
	}
	PhysicsAsset->MarkPackageDirty();
	// 回读确认：碰撞关闭表里没有这一对才算打开
	return PhysicsAsset->CollisionDisableTable.Contains(FRigidBodyIndexPair(A, B)) != bEnable;
}

bool UPbzModEditorLibrary::ConfigureJoint(UPhysicsAsset* PhysicsAsset, FName ChildBone, float SwingLimitDeg, float TwistLimitDeg, float DriveStiffness, float DriveDamping)
{
	if (!PhysicsAsset)
	{
		return false;
	}
	for (const TObjectPtr<UPhysicsConstraintTemplate>& Template : PhysicsAsset->ConstraintSetup)
	{
		if (!Template || Template->DefaultInstance.ConstraintBone1 != ChildBone)
		{
			continue;
		}
		Template->Modify();
		FConstraintInstance& CI = Template->DefaultInstance;
		CI.SetLinearXMotion(ELinearConstraintMotion::LCM_Locked);
		CI.SetLinearYMotion(ELinearConstraintMotion::LCM_Locked);
		CI.SetLinearZMotion(ELinearConstraintMotion::LCM_Locked);
		CI.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, SwingLimitDeg);
		CI.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, SwingLimitDeg);
		CI.SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, TwistLimitDeg);
		CI.SetDisableCollision(true);
		CI.SetAngularDriveMode(EAngularDriveMode::SLERP);
		CI.SetOrientationDriveSLERP(DriveStiffness > 0.f);
		CI.SetAngularDriveParams(DriveStiffness, DriveDamping, 0.f);
		Template->SetDefaultProfile(CI);
		PhysicsAsset->MarkPackageDirty();
		return true;
	}
	return false;
}

FString UPbzModEditorLibrary::BuildRetargetPostProcessGraph(UAnimBlueprint* AnimBlueprint, UObject* IKRetargeter, UPhysicsAsset* SecondaryPhysics, float WorldAlpha)
{
	TArray<FString> Log;
	auto Fail = [&Log](const FString& Message)
	{
		Log.Insert(TEXT("FAIL"), 0);
		Log.Add(TEXT("ERROR: ") + Message);
		return FString::Join(Log, TEXT("\n"));
	};
	if (!AnimBlueprint || !IKRetargeter)
	{
		return Fail(TEXT("null AnimBlueprint or IKRetargeter"));
	}
	FModuleManager::Get().LoadModule(TEXT("IKRigDeveloper"));

	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : Graphs)
	{
		if (G && G->GetFName() == TEXT("AnimGraph"))
		{
			Graph = G;
			break;
		}
	}
	if (!Graph)
	{
		return Fail(TEXT("AnimGraph not found"));
	}
	UEdGraphNode* Root = nullptr;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (N && N->GetClass()->GetFName() == TEXT("AnimGraphNode_Root"))
		{
			Root = N;
			break;
		}
	}
	if (!Root)
	{
		return Fail(TEXT("Output Pose node not found"));
	}
	for (UEdGraphNode* N : TArray<UEdGraphNode*>(Graph->Nodes))
	{
		if (N && N != Root)
		{
			FBlueprintEditorUtils::RemoveNode(AnimBlueprint, N, true);
		}
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	UEdGraphNode* Retarget = SpawnNode(Graph, TEXT("/Script/IKRigDeveloper.AnimGraphNode_RetargetPoseFromMesh"), -900, 0);
	if (!Retarget || !SetNodeObject(Retarget, TEXT("IKRetargeterAsset"), IKRetargeter))
	{
		return Fail(TEXT("create RetargetPoseFromMesh node"));
	}
	UEdGraphPin* Last = FindPosePin(Retarget, EGPD_Output);
	if (SecondaryPhysics)
	{
		UEdGraphNode* ToComponent = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_LocalToComponentSpace"), -650, 0);
		UEdGraphNode* Rigid = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_RigidBody"), -450, 0);
		UEdGraphNode* ToLocal = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_ComponentToLocalSpace"), -200, 0);
		if (!ToComponent || !Rigid || !ToLocal || !SetNodeObject(Rigid, TEXT("OverridePhysicsAsset"), SecondaryPhysics)
			|| !SetRigidBodyInertia(Rigid, WorldAlpha))
		{
			return Fail(TEXT("create physics nodes"));
		}
		if (!Connect(Schema, Last, FindPosePin(ToComponent, EGPD_Input)) ||
			!Connect(Schema, FindPosePin(ToComponent, EGPD_Output), FindPosePin(Rigid, EGPD_Input)) ||
			!Connect(Schema, FindPosePin(Rigid, EGPD_Output), FindPosePin(ToLocal, EGPD_Input)))
		{
			return Fail(TEXT("connect physics nodes"));
		}
		Last = FindPosePin(ToLocal, EGPD_Output);
	}
	if (!Connect(Schema, Last, FindPosePin(Root, EGPD_Input)))
	{
		return Fail(TEXT("connect to Output Pose"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::None, &Results);
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		Log.Add(Message->ToText().ToString());
	}
	Log.Add(FString::Printf(TEXT("nodes=%d status=%d errors=%d warnings=%d"), Graph->Nodes.Num(), static_cast<int32>(AnimBlueprint->Status), Results.NumErrors, Results.NumWarnings));
	AnimBlueprint->MarkPackageDirty();
	const bool bOk = AnimBlueprint->Status == BS_UpToDate || AnimBlueprint->Status == BS_UpToDateWithWarnings;
	Log.Insert(bOk ? TEXT("OK") : TEXT("FAIL"), 0);
	return FString::Join(Log, TEXT("\n"));
}

bool UPbzModEditorLibrary::SetPostProcessAnimBlueprint(USkeletalMesh* Mesh, UAnimBlueprint* AnimBlueprint)
{
	if (!Mesh)
	{
		return false;
	}
	Mesh->Modify();
	Mesh->SetPostProcessAnimBlueprint(AnimBlueprint ? TSubclassOf<UAnimInstance>(AnimBlueprint->GeneratedClass.Get()) : nullptr);
	Mesh->MarkPackageDirty();
	return true;
}

FString UPbzModEditorLibrary::BuildBlinkEventGraph(UAnimBlueprint* AnimBlueprint, FName MorphTargetName, float IntervalSec, float DurationSec, float PhaseSec, float JitterSec)
{
	TArray<FString> Log;
	auto Fail = [&Log](const FString& Message)
	{
		Log.Insert(TEXT("FAIL"), 0);
		Log.Add(TEXT("ERROR: ") + Message);
		return FString::Join(Log, TEXT("\n"));
	};
	if (!AnimBlueprint || MorphTargetName.IsNone())
	{
		return Fail(TEXT("null AnimBlueprint or empty morph target name"));
	}
	if (IntervalSec <= 0.f || DurationSec <= 0.f || DurationSec >= IntervalSec)
	{
		return Fail(TEXT("need 0 < duration < interval"));
	}
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(AnimBlueprint);
	if (!Graph)
	{
		return Fail(TEXT("event graph not found"));
	}
	const UEdGraphSchema* Schema = Graph->GetSchema();

	// Update Animation 事件（已有就复用，重复继承会让蓝图编译报错）
	UK2Node_Event* EventNode = nullptr;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		UK2Node_Event* Existing = Cast<UK2Node_Event>(N);
		if (Existing && Existing->GetFunctionName() == TEXT("BlueprintUpdateAnimation"))
		{
			EventNode = Existing;
			break;
		}
	}
	if (!EventNode)
	{
		EventNode = NewObject<UK2Node_Event>(Graph, NAME_None, RF_Transactional);
		Graph->AddNode(EventNode, false, false);
		EventNode->CreateNewGuid();
		EventNode->EventReference.SetExternalMember(TEXT("BlueprintUpdateAnimation"), UAnimInstance::StaticClass());
		EventNode->bOverrideFunction = true;
		EventNode->NodePosX = -1500;
		EventNode->NodePosY = 360;
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();
	}

	UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph, NAME_None, RF_Transactional);
	Graph->AddNode(SelfNode, false, false);
	SelfNode->CreateNewGuid();
	SelfNode->NodePosX = -1500;
	SelfNode->NodePosY = 520;
	SelfNode->PostPlacedNewNode();
	SelfNode->AllocateDefaultPins();
	UEdGraphPin* SelfPin = SelfNode->FindPin(TEXT("self"), EGPD_Output);
	if (!SelfPin)
	{
		return Fail(TEXT("Self node has no output pin"));
	}

	int32 X = -1250;
	int32 Row = 0;
	// 纯函数链：每个节点接上一个节点的返回值，常量写在未连线的输入引脚上
	auto Chain = [&](UClass* ParentClass, const TCHAR* FunctionName, const TCHAR* InPinName, UEdGraphPin* From) -> UK2Node_CallFunction*
	{
		UK2Node_CallFunction* Node = SpawnCall(Graph, ParentClass, FunctionName, X, Row * 70);
		Row++;
		X += 250;
		if (!Node)
		{
			Log.Add(FString::Printf(TEXT("ERROR: 创建节点 %s 失败"), FunctionName));
			return nullptr;
		}
		if (From && !Connect(Schema, From, InPin(Node, InPinName)))
		{
			Log.Add(FString::Printf(TEXT("ERROR: 连线 %s.%s 失败"), FunctionName, InPinName));
			return nullptr;
		}
		return Node;
	};
	const float HalfDur = FMath::Max(DurationSec * 0.5f, 1e-3f);
	// 抖动是加在时间轴上的：rad 频率 * 抖动幅度 必须明显小于 1，否则相位会停滞甚至倒退，
	// 结果是两次眨眼挤在一起（看起来像抽搐）。这里限幅并记录实际用到的抖动幅度。
	constexpr double JitterRate = 0.31;
	double UseJitter = FMath::Min<double>(JitterSec, 0.8 / JitterRate);
	if (UseJitter < JitterSec - 1e-4)
	{
		Log.Add(FString::Printf(TEXT("jitter 限幅 %.2fs -> %.2fs（相位不能倒退）"), JitterSec, UseJitter));
	}

	// t = GetGameTimeInSeconds(self)
	UK2Node_CallFunction* Time = Chain(UKismetSystemLibrary::StaticClass(), TEXT("GetGameTimeInSeconds"), TEXT("WorldContextObject"), SelfPin);
	if (!Time)
	{
		return Fail(TEXT("Get Game Time in Seconds"));
	}
	// a2 = t + jitter*sin(0.7*t) + 相位偏移 ：正弦抖动让间隔不是死板的定值
	UK2Node_CallFunction* Slow = Chain(UKismetMathLibrary::StaticClass(), TEXT("Multiply_DoubleDouble"), TEXT("A"), OutPin(Time));
	if (!Slow || !SetPinValue(Slow, TEXT("B"), FString::SanitizeFloat(JitterRate))) { return Fail(TEXT("jitter 频率*t")); }
	UK2Node_CallFunction* Wave = Chain(UKismetMathLibrary::StaticClass(), TEXT("Sin"), TEXT("A"), OutPin(Slow));
	if (!Wave) { return Fail(TEXT("sin(0.7t)")); }
	UK2Node_CallFunction* Jitter = Chain(UKismetMathLibrary::StaticClass(), TEXT("Multiply_DoubleDouble"), TEXT("A"), OutPin(Wave));
	if (!Jitter || !SetPinValue(Jitter, TEXT("B"), FString::SanitizeFloat(UseJitter))) { return Fail(TEXT("jitter")); }
	UK2Node_CallFunction* ShiftA = Chain(UKismetMathLibrary::StaticClass(), TEXT("Add_DoubleDouble"), TEXT("A"), OutPin(Time));
	if (!ShiftA || !Connect(Schema, OutPin(Jitter), InPin(ShiftA, TEXT("B")))) { return Fail(TEXT("t+jitter")); }
	UK2Node_CallFunction* ShiftB = Chain(UKismetMathLibrary::StaticClass(), TEXT("Add_DoubleDouble"), TEXT("A"), OutPin(ShiftA));
	if (!ShiftB || !SetPinValue(ShiftB, TEXT("B"), FString::SanitizeFloat(PhaseSec))) { return Fail(TEXT("phase offset")); }
	// phase = fmod(a2, interval)：越小越靠近眨眼中心
	UK2Node_CallFunction* Phase = Chain(UKismetMathLibrary::StaticClass(), TEXT("Percent_FloatFloat"), TEXT("A"), OutPin(ShiftB));
	if (!Phase || !SetPinValue(Phase, TEXT("B"), FString::SanitizeFloat(IntervalSec))) { return Fail(TEXT("fmod")); }
	// s = clamp(1 - |phase - dur/2| / (dur/2), 0, 1)
	UK2Node_CallFunction* Offset = Chain(UKismetMathLibrary::StaticClass(), TEXT("Subtract_DoubleDouble"), TEXT("A"), OutPin(Phase));
	if (!Offset || !SetPinValue(Offset, TEXT("B"), FString::SanitizeFloat(HalfDur))) { return Fail(TEXT("phase-dur/2")); }
	UK2Node_CallFunction* AbsNode = Chain(UKismetMathLibrary::StaticClass(), TEXT("Abs"), TEXT("A"), OutPin(Offset));
	if (!AbsNode) { return Fail(TEXT("abs")); }
	UK2Node_CallFunction* Ratio = Chain(UKismetMathLibrary::StaticClass(), TEXT("Divide_DoubleDouble"), TEXT("A"), OutPin(AbsNode));
	if (!Ratio || !SetPinValue(Ratio, TEXT("B"), FString::SanitizeFloat(HalfDur))) { return Fail(TEXT("归一化")); }
	UK2Node_CallFunction* Triangle = Chain(UKismetMathLibrary::StaticClass(), TEXT("Subtract_DoubleDouble"), TEXT("B"), OutPin(Ratio));
	if (!Triangle || !SetPinValue(Triangle, TEXT("A"), TEXT("1.0"))) { return Fail(TEXT("三角窗")); }
	UK2Node_CallFunction* Clamped = Chain(UKismetMathLibrary::StaticClass(), TEXT("FClamp"), TEXT("Value"), OutPin(Triangle));
	if (!Clamped || !SetPinValue(Clamped, TEXT("Min"), TEXT("0.0")) || !SetPinValue(Clamped, TEXT("Max"), TEXT("1.0"))) { return Fail(TEXT("clamp")); }
	// w = 0.5 - 0.5*cos(π*s)：s=0 时 0（睁眼），s=1 时 1（闭严），正弦软起合，眨眼看起来才自然。
	// 注意是 π 不是 2π：用 2π 会在一个周期里出现两次闭合（半个窗口各一次），像抽搐
	UK2Node_CallFunction* PiS = Chain(UKismetMathLibrary::StaticClass(), TEXT("Multiply_DoubleDouble"), TEXT("A"), OutPin(Clamped));
	if (!PiS || !SetPinValue(PiS, TEXT("B"), TEXT("3.14159265358979"))) { return Fail(TEXT("pi*s")); }
	UK2Node_CallFunction* CosNode = Chain(UKismetMathLibrary::StaticClass(), TEXT("Cos"), TEXT("A"), OutPin(PiS));
	if (!CosNode) { return Fail(TEXT("cos")); }
	UK2Node_CallFunction* HalfCos = Chain(UKismetMathLibrary::StaticClass(), TEXT("Multiply_DoubleDouble"), TEXT("A"), OutPin(CosNode));
	if (!HalfCos || !SetPinValue(HalfCos, TEXT("B"), TEXT("0.5"))) { return Fail(TEXT("0.5*cos")); }
	UK2Node_CallFunction* Weight = Chain(UKismetMathLibrary::StaticClass(), TEXT("Subtract_DoubleDouble"), TEXT("B"), OutPin(HalfCos));
	if (!Weight || !SetPinValue(Weight, TEXT("A"), TEXT("0.5"))) { return Fail(TEXT("weight")); }
	// Set Morph Target：写组件上的 morph 权重（UAnimInstance::SetMorphTarget -> USkeletalMeshComponent::SetMorphTarget）
	// 引擎的数学节点现在都是 double，Value 是 float 引脚，连线时编译器会自动补转换（不再手动加已废弃的转换节点）
	UK2Node_CallFunction* SetMorph = SpawnCall(Graph, UAnimInstance::StaticClass(), TEXT("SetMorphTarget"), X, 0);
	if (!SetMorph
		|| !SetPinValue(SetMorph, TEXT("MorphTargetName"), MorphTargetName.ToString())
		|| !Connect(Schema, OutPin(Weight), InPin(SetMorph, TEXT("Value"))))
	{
		return Fail(TEXT("Set Morph Target 节点或连线失败"));
	}
	if (UEdGraphPin* MorphSelf = InPin(SetMorph, UEdGraphSchema_K2::PN_Self))
	{
		if (!Connect(Schema, SelfPin, MorphSelf))
		{
			return Fail(TEXT("Set Morph Target 的 self 连线失败"));
		}
	}
	if (!Connect(Schema, EventNode->GetThenPin(), SetMorph->GetExecPin()))
	{
		return Fail(TEXT("Update Animation -> Set Morph Target 执行线失败"));
	}

	// 回读：曲线名、Value 连线、执行线都真的接上了
	const FString ReadName = SetMorph->FindPin(TEXT("MorphTargetName"), EGPD_Input)->DefaultValue;
	const UEdGraphPin* ValuePin = InPin(SetMorph, TEXT("Value"));
	const bool bWired = ValuePin && ValuePin->LinkedTo.Num() > 0 && SetMorph->GetExecPin()->LinkedTo.Num() > 0;
	Log.Add(FString::Printf(TEXT("readback morph=%s value_linked=%s exec_linked=%s"), *ReadName,
		ValuePin && ValuePin->LinkedTo.Num() > 0 ? TEXT("yes") : TEXT("no"),
		SetMorph->GetExecPin()->LinkedTo.Num() > 0 ? TEXT("yes") : TEXT("no")));
	if (ReadName != MorphTargetName.ToString() || !bWired)
	{
		return Fail(TEXT("眨眼节点回读不一致"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::None, &Results);
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		Log.Add(Message->ToText().ToString());
	}
	Log.Add(FString::Printf(TEXT("morph=%s interval=%.2fs duration=%.2fs jitter=%.2fs nodes=%d status=%d errors=%d warnings=%d"),
		*MorphTargetName.ToString(), IntervalSec, DurationSec, UseJitter, Graph->Nodes.Num(),
		static_cast<int32>(AnimBlueprint->Status), Results.NumErrors, Results.NumWarnings));
	AnimBlueprint->MarkPackageDirty();
	const bool bOk = AnimBlueprint->Status == BS_UpToDate || AnimBlueprint->Status == BS_UpToDateWithWarnings;
	Log.Insert(bOk ? TEXT("OK") : TEXT("FAIL"), 0);
	return FString::Join(Log, TEXT("\n"));
}

TArray<FName> UPbzModEditorLibrary::GetMorphTargetNames(USkeletalMesh* Mesh)
{
	TArray<FName> Out;
	if (Mesh)
	{
		for (const TObjectPtr<UMorphTarget>& Target : Mesh->GetMorphTargets())
		{
			if (Target)
			{
				Out.Add(Target->GetFName());
			}
		}
	}
	return Out;
}

FString UPbzModEditorLibrary::DescribeBlinkEventGraph(UAnimBlueprint* AnimBlueprint)
{
	TArray<FString> Lines;
	if (!AnimBlueprint)
	{
		return TEXT("null blueprint");
	}
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(AnimBlueprint);
	if (!Graph)
	{
		return TEXT("event graph not found");
	}
	Lines.Add(FString::Printf(TEXT("graph=%s nodes=%d"), *Graph->GetName(), Graph->Nodes.Num()));
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (!Call)
		{
			continue;
		}
		TArray<FString> Consts;
		int32 LinkedInputs = 0;
		for (UEdGraphPin* Pin : Call->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				++LinkedInputs;
			}
			else if (!Pin->DefaultValue.IsEmpty())
			{
				Consts.Add(FString::Printf(TEXT("%s=%s"), *Pin->PinName.ToString(), *Pin->DefaultValue));
			}
		}
		Lines.Add(FString::Printf(TEXT("%s linked=%d exec=%s {%s}"), *Call->GetFunctionName().ToString(), LinkedInputs,
			Call->GetExecPin() && Call->GetExecPin()->LinkedTo.Num() > 0 ? TEXT("yes") : TEXT("no"),
			*FString::Join(Consts, TEXT(", "))));
	}
	return FString::Join(Lines, TEXT("\n"));
}

FString UPbzModEditorLibrary::DescribeMorphTargets(USkeletalMesh* Mesh)
{
	TArray<FString> Lines;
	if (!Mesh)
	{
		return TEXT("null mesh");
	}
	for (const TObjectPtr<UMorphTarget>& Target : Mesh->GetMorphTargets())
	{
		if (!Target)
		{
			continue;
		}
		Lines.Add(FString::Printf(TEXT("%s lods=%d"), *Target->GetName(), Target->GetMorphLODModels().Num()));
		for (int32 Lod = 0; Lod < Target->GetMorphLODModels().Num(); ++Lod)
		{
			const FMorphTargetLODModel& Model = Target->GetMorphLODModels()[Lod];
			int32 Moved = 0;
			double MaxLen = 0.0;
			for (const FMorphTargetDelta& Delta : Model.Vertices)
			{
				const double Len = Delta.PositionDelta.Length();
				if (Len > 1e-4)
				{
					++Moved;
					MaxLen = FMath::Max(MaxLen, Len);
				}
			}
			Lines.Add(FString::Printf(TEXT("  lod%d deltas=%d moved=%d max_delta_cm=%.3f"), Lod, Model.Vertices.Num(), Moved, MaxLen));
		}
	}
	return FString::Join(Lines, TEXT("\n"));
}

TArray<FName> UPbzModEditorLibrary::GetMeshBoneNames(USkeletalMesh* Mesh)
{
	TArray<FName> Out;
	if (Mesh)
	{
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		for (int32 i = 0; i < Ref.GetNum(); ++i)
		{
			Out.Add(Ref.GetBoneName(i));
		}
	}
	return Out;
}

TArray<FTransform> UPbzModEditorLibrary::LocalPosesToComponentSpace(USkeletalMesh* Mesh, const TArray<FName>& BoneNames, const TArray<FTransform>& LocalTransforms, int32 NumFrames)
{
	TArray<FTransform> Out;
	if (!Mesh || NumFrames <= 0 || LocalTransforms.Num() != BoneNames.Num() * NumFrames)
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ LocalPosesToComponentSpace: bad input (%d transforms, %d bones, %d frames)"), LocalTransforms.Num(), BoneNames.Num(), NumFrames);
		return Out;
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const int32 NumBones = Ref.GetNum();
	TArray<int32> InputIndex;
	InputIndex.Init(INDEX_NONE, NumBones);
	for (int32 i = 0; i < BoneNames.Num(); ++i)
	{
		const int32 Bone = Ref.FindBoneIndex(BoneNames[i]);
		if (Bone != INDEX_NONE)
		{
			InputIndex[Bone] = i;
		}
	}
	Out.SetNum(NumBones * NumFrames);
	for (int32 F = 0; F < NumFrames; ++F)
	{
		for (int32 B = 0; B < NumBones; ++B)
		{
			const FTransform Local = InputIndex[B] != INDEX_NONE ? LocalTransforms[F * BoneNames.Num() + InputIndex[B]] : Ref.GetRefBonePose()[B];
			const int32 Parent = Ref.GetParentIndex(B);   // 父骨骼的序号总是小于子骨骼
			Out[F * NumBones + B] = Parent == INDEX_NONE ? Local : Local * Out[F * NumBones + Parent];
		}
	}
	return Out;
}

TArray<FTransform> UPbzModEditorLibrary::RetargetComponentPoses(UObject* IKRetargeter, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, const TArray<FTransform>& SourceComponentPoses, int32 NumFrames)
{
	TArray<FTransform> Out;
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(IKRetargeter);
	if (!Retargeter || !SourceMesh || !TargetMesh || NumFrames <= 0 || SourceComponentPoses.Num() != SourceMesh->GetRefSkeleton().GetNum() * NumFrames)
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ RetargetComponentPoses: bad input"));
		return Out;
	}
	const FReferenceSkeleton& SrcRef = SourceMesh->GetRefSkeleton();
	const FReferenceSkeleton& TgtRef = TargetMesh->GetRefSkeleton();
	FRetargetProfile Profile;
	Profile.FillProfileWithAssetSettings(Retargeter);
	FIKRetargetProcessor Processor;
	Processor.Initialize(SourceMesh, TargetMesh, Retargeter, Profile);
	if (!Processor.IsInitialized())
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ RetargetComponentPoses: retarget processor failed to initialize"));
		return Out;
	}
	// 处理器内部骨骼顺序按名字对回网格顺序
	const TArray<FName>& ProcSrc = Processor.GetSkeleton(ERetargetSourceOrTarget::Source).BoneNames;
	const TArray<FName>& ProcTgt = Processor.GetSkeleton(ERetargetSourceOrTarget::Target).BoneNames;
	TArray<int32> SrcMap;
	for (const FName& Name : ProcSrc)
	{
		SrcMap.Add(SrcRef.FindBoneIndex(Name));
	}
	TArray<int32> TgtMap;
	for (int32 i = 0; i < TgtRef.GetNum(); ++i)
	{
		TgtMap.Add(ProcTgt.IndexOfByKey(TgtRef.GetBoneName(i)));
	}
	Processor.OnPlaybackReset();
	TArray<FTransform> SourcePose;
	SourcePose.SetNum(ProcSrc.Num());
	Out.Reserve(TgtRef.GetNum() * NumFrames);
	for (int32 F = 0; F < NumFrames; ++F)
	{
		for (int32 i = 0; i < ProcSrc.Num(); ++i)
		{
			SourcePose[i] = SrcMap[i] != INDEX_NONE ? SourceComponentPoses[F * SrcRef.GetNum() + SrcMap[i]] : FTransform::Identity;
			SourcePose[i].SetScale3D(FVector::OneVector);
		}
		Processor.ApplySourceScaleToPose(SourcePose);
		const TArray<FTransform>& TargetPose = Processor.RunRetargeter(SourcePose, Profile, 1.f / 30.f);
		for (int32 i = 0; i < TgtRef.GetNum(); ++i)
		{
			Out.Add(TgtMap[i] != INDEX_NONE ? TargetPose[TgtMap[i]] : FTransform::Identity);
		}
	}
	return Out;
}
