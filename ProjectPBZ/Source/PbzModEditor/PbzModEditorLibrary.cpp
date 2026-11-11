#include "PbzModEditorLibrary.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "AnimGraphNode_Base.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
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
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
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

	// 动画图节点内部 FAnimNode_xxx 的任意字段：用 UE 文本格式写入（如 (BoneName="hand_l")），再导出回读，写不进去就返回 false。
	// 字段若在节点上有同名引脚（包括默认隐藏的可选引脚，如 CopyBone 的 bCopyTranslation、TwoBoneIK 的 JointTargetLocation），
	// 动画蓝图编译器会把「未连线引脚的默认值」写回结构体（AnimBlueprintCompiler.cpp GatherFoldRecordsForAnimationNode），
	// 只改结构体会被旧的引脚默认值覆盖 —— 这就是上一版 CopyBone 三个开关烤进资产后仍是 false 的原因。所以这里两边一起写。
	bool SetNodeText(UEdGraphNode* Node, const TCHAR* Field, const FString& Text, FString* OutReadback = nullptr)
	{
		const FStructProperty* NodeProp = Node ? FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node")) : nullptr;
		if (!NodeProp)
		{
			return false;
		}
		void* NodeData = NodeProp->ContainerPtrToValuePtr<void>(Node);
		const FProperty* Prop = FindFProperty<FProperty>(NodeProp->Struct, Field);
		if (!Prop)
		{
			return false;
		}
		void* Ptr = Prop->ContainerPtrToValuePtr<void>(NodeData);
		if (!Prop->ImportText_Direct(*Text, Ptr, Node, PPF_None, GLog))
		{
			return false;
		}
		FString Back;
		Prop->ExportText_Direct(Back, Ptr, nullptr, Node, PPF_None);
		if (UEdGraphPin* Pin = Node->FindPin(Prop->GetFName(), EGPD_Input))
		{
			FString PinText;
			if (!FBlueprintEditorUtils::PropertyValueToString_Direct(Prop, static_cast<const uint8*>(Ptr), PinText, Node))
			{
				return false;
			}
			Node->GetSchema()->TrySetDefaultValue(*Pin, PinText, false);
			Back += FString::Printf(TEXT("{pin=%s}"), *Pin->GetDefaultAsString());
		}
		if (OutReadback)
		{
			*OutReadback = Back;
		}
		return true;
	}

	FString GetNodeText(const UEdGraphNode* Node, const TCHAR* Field)
	{
		const FStructProperty* NodeProp = Node ? FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node")) : nullptr;
		const FProperty* Prop = NodeProp ? FindFProperty<FProperty>(NodeProp->Struct, Field) : nullptr;
		if (!Prop)
		{
			return TEXT("<missing>");
		}
		FString Out;
		Prop->ExportText_Direct(Out, Prop->ContainerPtrToValuePtr<void>(NodeProp->ContainerPtrToValuePtr<void>(const_cast<UEdGraphNode*>(Node))), nullptr, nullptr, PPF_None);
		// 有同名引脚时，编译器用的是引脚默认值（未连线时），一并输出
		if (const UEdGraphPin* Pin = const_cast<UEdGraphNode*>(Node)->FindPin(Prop->GetFName(), EGPD_Input))
		{
			Out += Pin->LinkedTo.Num() ? TEXT("{pin=linked}") : FString::Printf(TEXT("{pin=%s}"), *Pin->GetDefaultAsString());
		}
		return Out;
	}

	UK2Node_Variable* SpawnVariableNode(UEdGraph* Graph, bool bSet, FName Variable, int32 X, int32 Y)
	{
		UK2Node_Variable* Node = bSet ? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(Graph, NAME_None, RF_Transactional))
			: static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(Graph, NAME_None, RF_Transactional));
		Node->VariableReference.SetSelfMember(Variable);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	void CollectCompileLog(UAnimBlueprint* AnimBlueprint, TArray<FString>& Log, int32& OutErrors)
	{
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::None, &Results);
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			Log.Add(Message->ToText().ToString());
		}
		OutErrors = Results.NumErrors;
	}

}

// ---- 离线预览用的主动画实例：不跑图，直接输出外部给定的局部姿势 ----
namespace
{
	struct FPbzPoseFeedProxy : public FAnimInstanceProxy
	{
		explicit FPbzPoseFeedProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance), Owner(Cast<UPbzPoseFeedInstance>(Instance)) {}

		virtual bool Evaluate(FPoseContext& Output) override
		{
			const TArray<FTransform>* Pose = Owner ? &Owner->MeshLocalPose : nullptr;
			const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
			for (FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex())
			{
				const int32 MeshIndex = Bones.MakeMeshPoseIndex(Index).GetInt();
				if (Pose && Pose->IsValidIndex(MeshIndex))
				{
					Output.Pose[Index] = (*Pose)[MeshIndex];
				}
			}
			return true;
		}

		UPbzPoseFeedInstance* Owner;
	};
}

FAnimInstanceProxy* UPbzPoseFeedInstance::CreateAnimInstanceProxy()
{
	return new FPbzPoseFeedProxy(this);
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

FString UPbzModEditorLibrary::DescribePhysicsBodies(UPhysicsAsset* PhysicsAsset)
{
	TArray<FString> Lines;
	if (!PhysicsAsset)
	{
		return TEXT("null physics asset");
	}
	// 关节：ConstraintBone1（子骨）→ swing/twist 上限与驱动刚度/阻尼
	TMap<FName, const FConstraintInstance*> JointOf;
	for (const TObjectPtr<UPhysicsConstraintTemplate>& Template : PhysicsAsset->ConstraintSetup)
	{
		if (Template)
		{
			JointOf.Add(Template->DefaultInstance.ConstraintBone1, &Template->DefaultInstance);
		}
	}
	Lines.Add(FString::Printf(TEXT("bodies=%d joints=%d"), PhysicsAsset->SkeletalBodySetups.Num(), PhysicsAsset->ConstraintSetup.Num()));
	for (const TObjectPtr<USkeletalBodySetup>& Body : PhysicsAsset->SkeletalBodySetups)
	{
		if (!Body)
		{
			continue;
		}
		const FBodyInstance& BI = Body->DefaultInstance;
		const bool bSim = Body->PhysicsType == EPhysicsType::PhysType_Simulated;
		const bool bCollide = Body->CollisionReponse == EBodyCollisionResponse::BodyCollision_Enabled;
		// bEnableGravity 是 FBodyInstance 的 public 位域，直接读；这是判断“尾巴会不会被重力拉直”的唯一依据
		const bool bGrav = BI.bEnableGravity;
		const float Lin = BI.LinearDamping;
		const float Ang = BI.AngularDamping;
		FString Joint = TEXT("-");
		if (const FConstraintInstance* const* CI = JointOf.Find(Body->BoneName))
		{
			const float Swing1 = (*CI)->GetAngularSwing1Limit();
			const float Swing2 = (*CI)->GetAngularSwing2Limit();
			const float Twist = (*CI)->GetAngularTwistLimit();
			float Stiff = 0.f, Damp = 0.f, ForceLimit = 0.f;
			(*CI)->GetAngularDriveParams(Stiff, Damp, ForceLimit);
			Joint = FString::Printf(TEXT("swing=%.1f/%.1f twist=%.1f stiff=%.0f damp=%.0f"), Swing1, Swing2, Twist, Stiff, Damp);
		}
		Lines.Add(FString::Printf(TEXT("%s sim=%d grav=%d collide=%d lin=%.1f ang=%.1f %s"),
			*Body->BoneName.ToString(), bSim ? 1 : 0, bGrav ? 1 : 0, bCollide ? 1 : 0, Lin, Ang, *Joint));
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

// ---------------------------------------------------------------------------------------------
// 魂身体后处理：双手握持修正（见头文件注释）
// ---------------------------------------------------------------------------------------------
namespace
{
	const FName GripAlphaVar(TEXT("GripAlpha"));
	const FName GripOffsetVar(TEXT("GripOffset"));
}

FString UPbzModEditorLibrary::BuildSoulGripPostProcess(UAnimBlueprint* AnimBlueprint, USkeletalMesh* SoulMesh, float FullRadius, float ZeroRadius, float MaxReach, float MinHandGap)
{
	TArray<FString> Log;
	auto Fail = [&Log](const FString& Message)
	{
		Log.Insert(TEXT("FAIL"), 0);
		Log.Add(TEXT("ERROR: ") + Message);
		return FString::Join(Log, TEXT("\n"));
	};
	if (!AnimBlueprint || !SoulMesh)
	{
		return Fail(TEXT("null AnimBlueprint or SoulMesh"));
	}
	if (!(FullRadius > 0.f && ZeroRadius > FullRadius && MaxReach > 0.5f && MaxReach <= 1.f && MinHandGap >= 0.f))
	{
		return Fail(TEXT("need 0 < full_radius < zero_radius, 0.5 < max_reach <= 1, min_hand_gap >= 0"));
	}
	// 左臂长度：适配后空身体参考姿势里 lowerarm_l、hand_l 的局部平移长度（与运行时 AnimationScaled 后一致）
	const FReferenceSkeleton& Ref = SoulMesh->GetRefSkeleton();
	for (const TCHAR* Bone : { TEXT("hand_r"), TEXT("hand_l"), TEXT("lowerarm_l"), TEXT("upperarm_l"), TEXT("weapon_r"), TEXT("ik_hand_gun"), TEXT("ik_hand_l") })
	{
		if (Ref.FindBoneIndex(Bone) == INDEX_NONE)
		{
			return Fail(FString::Printf(TEXT("soul mesh has no bone %s"), Bone));
		}
	}
	const int32 Upper = Ref.FindBoneIndex(TEXT("upperarm_l"));
	const int32 Lower = Ref.FindBoneIndex(TEXT("lowerarm_l"));
	const int32 Hand = Ref.FindBoneIndex(TEXT("hand_l"));
	if (Ref.GetParentIndex(Hand) != Lower || Ref.GetParentIndex(Lower) != Upper
		|| Ref.GetParentIndex(Ref.FindBoneIndex(TEXT("ik_hand_l"))) != Ref.FindBoneIndex(TEXT("ik_hand_gun"))
		|| Ref.GetParentIndex(Ref.FindBoneIndex(TEXT("weapon_r"))) != Ref.FindBoneIndex(TEXT("hand_r")))
	{
		return Fail(TEXT("unexpected bone hierarchy (upperarm_l>lowerarm_l>hand_l, ik_hand_gun>ik_hand_l, hand_r>weapon_r)"));
	}
	const double ArmLength = Ref.GetRefBonePose()[Lower].GetTranslation().Size() + Ref.GetRefBonePose()[Hand].GetTranslation().Size();
	const double ReachLength = ArmLength * MaxReach;
	Log.Add(FString::Printf(TEXT("arm_l=%.3fcm reach=%.3fcm full=%.1f zero=%.1f gap=%.1f"), ArmLength, ReachLength, FullRadius, ZeroRadius, MinHandGap));

	// ---- 成员变量：事件图表写、动画图读 ----
	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	const TPair<FName, FEdGraphPinType> Vars[] = { { GripAlphaVar, FloatType }, { GripOffsetVar, VectorType } };
	for (const TPair<FName, FEdGraphPinType>& Var : Vars)
	{
		if (FBlueprintEditorUtils::FindNewVariableIndex(AnimBlueprint, Var.Key) == INDEX_NONE
			&& !FBlueprintEditorUtils::AddMemberVariable(AnimBlueprint, Var.Key, Var.Value))
		{
			return Fail(FString::Printf(TEXT("add member variable %s"), *Var.Key.ToString()));
		}
	}

	// ---- 动画图：InPose → 组件空间 → CopyBone → TwoBoneIK → 局部空间 → 输出 ----
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
	UEdGraphNode* Input = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_LinkedInputPose"), -1400, 0);
	UEdGraphNode* ToComponent = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_LocalToComponentSpace"), -1150, 0);
	UEdGraphNode* Copy = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_CopyBone"), -900, 0);
	UEdGraphNode* Ik = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_TwoBoneIK"), -550, 0);
	UEdGraphNode* ToLocal = SpawnNode(Graph, TEXT("/Script/AnimGraph.AnimGraphNode_ComponentToLocalSpace"), -250, 0);
	if (!Input || !ToComponent || !Copy || !Ik || !ToLocal)
	{
		return Fail(TEXT("spawn anim graph nodes (AnimGraph module classes not found?)"));
	}
	// CopyBone：ik_hand_gun ← hand_r（组件空间，位置+旋转）。ik_hand_l 是它的子骨，跟着到「适配后右手 × 原版右手→左手关系」
	// 注意：bCopyTranslation/bCopyRotation 默认 false，三个都 false 时 CopyBone 直接跳过（上一次失败的原因）
	FString Back;
	TArray<FString> Settings;
	auto Set = [&](UEdGraphNode* Node, const TCHAR* Field, const FString& Text) -> bool
	{
		if (!SetNodeText(Node, Field, Text, &Back))
		{
			Log.Add(FString::Printf(TEXT("ERROR: set %s.%s = %s"), *Node->GetClass()->GetName(), Field, *Text));
			return false;
		}
		Settings.Add(FString::Printf(TEXT("%s=%s"), Field, *Back));
		return true;
	};
	const FString Pole = TEXT("(X=0.000000,Y=-15.000000,Z=0.000000)");   // lowerarm_l 局部 -Y = 肘的外凸方向（各动作实测均值）
	if (!Set(Copy, TEXT("SourceBone"), TEXT("(BoneName=\"hand_r\")")) || !Set(Copy, TEXT("TargetBone"), TEXT("(BoneName=\"ik_hand_gun\")"))
		|| !Set(Copy, TEXT("bCopyTranslation"), TEXT("True")) || !Set(Copy, TEXT("bCopyRotation"), TEXT("True"))
		|| !Set(Copy, TEXT("bCopyScale"), TEXT("False")) || !Set(Copy, TEXT("ControlSpace"), TEXT("BCS_ComponentSpace"))
		|| !Set(Ik, TEXT("IKBone"), TEXT("(BoneName=\"hand_l\")"))
		|| !Set(Ik, TEXT("EffectorLocationSpace"), TEXT("BCS_BoneSpace"))
		|| !Set(Ik, TEXT("EffectorTarget"), TEXT("(bUseSocket=False,BoneReference=(BoneName=\"ik_hand_l\"))"))
		|| !Set(Ik, TEXT("bTakeRotationFromEffectorSpace"), TEXT("True"))
		|| !Set(Ik, TEXT("bMaintainEffectorRelRot"), TEXT("False"))
		|| !Set(Ik, TEXT("bAllowStretching"), TEXT("False"))
		|| !Set(Ik, TEXT("JointTargetLocationSpace"), TEXT("BCS_BoneSpace"))
		|| !Set(Ik, TEXT("JointTarget"), TEXT("(bUseSocket=False,BoneReference=(BoneName=\"lowerarm_l\"))"))
		|| !Set(Ik, TEXT("JointTargetLocation"), Pole))
	{
		return Fail(TEXT("configure CopyBone / TwoBoneIK"));
	}
	Log.Add(TEXT("nodes: ") + FString::Join(Settings, TEXT("; ")));
	if (!Connect(Schema, FindPosePin(Input, EGPD_Output), FindPosePin(ToComponent, EGPD_Input))
		|| !Connect(Schema, FindPosePin(ToComponent, EGPD_Output), FindPosePin(Copy, EGPD_Input))
		|| !Connect(Schema, FindPosePin(Copy, EGPD_Output), FindPosePin(Ik, EGPD_Input))
		|| !Connect(Schema, FindPosePin(Ik, EGPD_Output), FindPosePin(ToLocal, EGPD_Input))
		|| !Connect(Schema, FindPosePin(ToLocal, EGPD_Output), FindPosePin(Root, EGPD_Input)))
	{
		return Fail(TEXT("connect pose pins"));
	}
	// 先编译一次：变量进骨架类，下面的变量节点才能解析出引脚
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	int32 Errors = 0;
	CollectCompileLog(AnimBlueprint, Log, Errors);
	UEdGraphPin* AlphaPin = Ik->FindPin(TEXT("Alpha"), EGPD_Input);
	UEdGraphPin* EffectorPin = Ik->FindPin(TEXT("EffectorLocation"), EGPD_Input);
	if (!AlphaPin || !EffectorPin)
	{
		TArray<FString> Names;
		for (UEdGraphPin* Pin : Ik->Pins)
		{
			Names.Add(Pin->PinName.ToString());
		}
		return Fail(TEXT("TwoBoneIK has no Alpha/EffectorLocation pin: ") + FString::Join(Names, TEXT(",")));
	}
	UK2Node_Variable* GetAlpha = SpawnVariableNode(Graph, false, GripAlphaVar, -800, 220);
	UK2Node_Variable* GetOffset = SpawnVariableNode(Graph, false, GripOffsetVar, -800, 300);
	if (!Connect(Schema, GetAlpha->FindPin(GripAlphaVar, EGPD_Output), AlphaPin)
		|| !Connect(Schema, GetOffset->FindPin(GripOffsetVar, EGPD_Output), EffectorPin))
	{
		return Fail(TEXT("connect GripAlpha/GripOffset to TwoBoneIK"));
	}

	// ---- 事件图表：每帧按上一帧姿势算 GripAlpha / GripOffset ----
	UEdGraph* Event = FBlueprintEditorUtils::FindEventGraph(AnimBlueprint);
	if (!Event)
	{
		return Fail(TEXT("event graph not found"));
	}
	for (UEdGraphNode* N : TArray<UEdGraphNode*>(Event->Nodes))
	{
		if (N)
		{
			FBlueprintEditorUtils::RemoveNode(AnimBlueprint, N, true);
		}
	}
	const UEdGraphSchema* ES = Event->GetSchema();
	UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Event, NAME_None, RF_Transactional);
	Event->AddNode(EventNode, false, false);
	EventNode->CreateNewGuid();
	EventNode->EventReference.SetExternalMember(TEXT("BlueprintUpdateAnimation"), UAnimInstance::StaticClass());
	EventNode->bOverrideFunction = true;
	EventNode->NodePosX = -2400;
	EventNode->PostPlacedNewNode();
	EventNode->AllocateDefaultPins();

	int32 Col = 0;
	TArray<FString> Missing;
	UClass* Math = UKismetMathLibrary::StaticClass();
	using FIn = TPair<const TCHAR*, UEdGraphPin*>;
	using FConst = TPair<const TCHAR*, FString>;
	// 纯函数节点：Ins = {引脚名, 来源引脚}，Consts = {引脚名, 常量}
	auto Call = [&](UClass* Owner, const TCHAR* Fn, const TArray<FIn>& Ins, const TArray<FConst>& Consts) -> UK2Node_CallFunction*
	{
		UK2Node_CallFunction* Node = SpawnCall(Event, Owner, Fn, -2100 + (Col % 12) * 260, 200 + (Col / 12) * 220);
		++Col;
		if (!Node)
		{
			Missing.Add(Fn);
			return nullptr;
		}
		for (const FIn& In : Ins)
		{
			if (!In.Value || !Connect(ES, In.Value, Node->FindPin(In.Key, EGPD_Input)))
			{
				Missing.Add(FString::Printf(TEXT("%s.%s"), Fn, In.Key));
			}
		}
		for (const FConst& C : Consts)
		{
			if (!SetPinValue(Node, C.Key, C.Value))
			{
				Missing.Add(FString::Printf(TEXT("%s.%s=const"), Fn, C.Key));
			}
		}
		return Node;
	};
	auto Ret = [](UK2Node_CallFunction* N) -> UEdGraphPin* { return N ? N->GetReturnValuePin() : nullptr; };
	auto Out = [](UK2Node_CallFunction* N, const TCHAR* Name) -> UEdGraphPin* { return N ? N->FindPin(Name, EGPD_Output) : nullptr; };
	auto Num = [](double V) { return FString::SanitizeFloat(V); };

	UK2Node_CallFunction* Comp = Call(UAnimInstance::StaticClass(), TEXT("GetOwningComponent"), {}, {});
	auto Socket = [&](const TCHAR* Bone, const TCHAR* Space)
	{
		return Call(USceneComponent::StaticClass(), TEXT("GetSocketTransform"), { FIn(TEXT("self"), Ret(Comp)) },
			{ FConst(TEXT("InSocketName"), Bone), FConst(TEXT("TransformSpace"), Space) });
	};
	// 动画局部关系（不受本后处理的 IK 影响）：W = weapon_r 相对 hand_r，Lc = ik_hand_l 相对 ik_hand_gun（= 原版右手→左手）
	UK2Node_CallFunction* W = Socket(TEXT("weapon_r"), TEXT("RTS_ParentBoneSpace"));
	UK2Node_CallFunction* Lc = Socket(TEXT("ik_hand_l"), TEXT("RTS_ParentBoneSpace"));
	UK2Node_CallFunction* Hr = Socket(TEXT("hand_r"), TEXT("RTS_Component"));
	UK2Node_CallFunction* Ua = Socket(TEXT("upperarm_l"), TEXT("RTS_Component"));
	// 左手目标在刀的局部空间：p = W⁻¹ · Lc.平移；刀身沿 +Z（TrailEfx 插槽 Z=85.6），rho = 离刀轴的径向距离
	UK2Node_CallFunction* LcT = Call(Math, TEXT("BreakTransform"), { FIn(TEXT("InTransform"), Ret(Lc)) }, {});
	UK2Node_CallFunction* P = Call(Math, TEXT("InverseTransformLocation"), { FIn(TEXT("T"), Ret(W)), FIn(TEXT("Location"), Out(LcT, TEXT("Location"))) }, {});
	UK2Node_CallFunction* PB = Call(Math, TEXT("BreakVector"), { FIn(TEXT("InVec"), Ret(P)) }, {});
	UK2Node_CallFunction* X2 = Call(Math, TEXT("Multiply_DoubleDouble"), { FIn(TEXT("A"), Out(PB, TEXT("X"))), FIn(TEXT("B"), Out(PB, TEXT("X"))) }, {});
	UK2Node_CallFunction* Y2 = Call(Math, TEXT("Multiply_DoubleDouble"), { FIn(TEXT("A"), Out(PB, TEXT("Y"))), FIn(TEXT("B"), Out(PB, TEXT("Y"))) }, {});
	UK2Node_CallFunction* R2 = Call(Math, TEXT("Add_DoubleDouble"), { FIn(TEXT("A"), Ret(X2)), FIn(TEXT("B"), Ret(Y2)) }, {});
	UK2Node_CallFunction* Rho = Call(Math, TEXT("Sqrt"), { FIn(TEXT("A"), Ret(R2)) }, {});
	// alpha = clamp((Zero - rho) / (Zero - Full), 0, 1)
	UK2Node_CallFunction* AlphaRaw = Call(Math, TEXT("Subtract_DoubleDouble"), { FIn(TEXT("B"), Ret(Rho)) }, { FConst(TEXT("A"), Num(ZeroRadius)) });
	UK2Node_CallFunction* AlphaDiv = Call(Math, TEXT("Divide_DoubleDouble"), { FIn(TEXT("A"), Ret(AlphaRaw)) }, { FConst(TEXT("B"), Num(ZeroRadius - FullRadius)) });
	UK2Node_CallFunction* Alpha = Call(Math, TEXT("FClamp"), { FIn(TEXT("Value"), Ret(AlphaDiv)) }, { FConst(TEXT("Min"), TEXT("0.0")), FConst(TEXT("Max"), TEXT("1.0")) });
	// 组件空间：目标 T = Hr · Lc.平移；刀轴 A = Hr 方向变换 (W 方向变换 Z)；肩 S = upperarm_l
	UK2Node_CallFunction* T = Call(Math, TEXT("TransformLocation"), { FIn(TEXT("T"), Ret(Hr)), FIn(TEXT("Location"), Out(LcT, TEXT("Location"))) }, {});
	UK2Node_CallFunction* AxW = Call(Math, TEXT("TransformDirection"), { FIn(TEXT("T"), Ret(W)) }, { FConst(TEXT("Direction"), TEXT("0.000000,0.000000,1.000000")) });
	UK2Node_CallFunction* A = Call(Math, TEXT("TransformDirection"), { FIn(TEXT("T"), Ret(Hr)), FIn(TEXT("Direction"), Ret(AxW)) }, {});
	UK2Node_CallFunction* UaT = Call(Math, TEXT("BreakTransform"), { FIn(TEXT("InTransform"), Ret(Ua)) }, {});
	UK2Node_CallFunction* D = Call(Math, TEXT("Subtract_VectorVector"), { FIn(TEXT("A"), Ret(T)), FIn(TEXT("B"), Out(UaT, TEXT("Location"))) }, {});
	// 最小 s ≥ 0 使 |D + s·A| ≤ Reach：b = D·A，c = |D|² - Reach²，s = max(0, -b - sqrt(max(b² - c, 0)))
	// （c ≤ 0 即本来够得到时 -b - √(b²-c) ≤ 0 → s = 0；连轴上最近点都够不到时取最近点 -b）
	UK2Node_CallFunction* B = Call(Math, TEXT("Dot_VectorVector"), { FIn(TEXT("A"), Ret(D)), FIn(TEXT("B"), Ret(A)) }, {});
	UK2Node_CallFunction* D2 = Call(Math, TEXT("VSizeSquared"), { FIn(TEXT("A"), Ret(D)) }, {});
	UK2Node_CallFunction* C = Call(Math, TEXT("Subtract_DoubleDouble"), { FIn(TEXT("A"), Ret(D2)) }, { FConst(TEXT("B"), Num(ReachLength * ReachLength)) });
	UK2Node_CallFunction* B2 = Call(Math, TEXT("Multiply_DoubleDouble"), { FIn(TEXT("A"), Ret(B)), FIn(TEXT("B"), Ret(B)) }, {});
	UK2Node_CallFunction* Disc = Call(Math, TEXT("Subtract_DoubleDouble"), { FIn(TEXT("A"), Ret(B2)), FIn(TEXT("B"), Ret(C)) }, {});
	UK2Node_CallFunction* DiscP = Call(Math, TEXT("FMax"), { FIn(TEXT("A"), Ret(Disc)) }, { FConst(TEXT("B"), TEXT("0.0")) });
	UK2Node_CallFunction* Root2 = Call(Math, TEXT("Sqrt"), { FIn(TEXT("A"), Ret(DiscP)) }, {});
	UK2Node_CallFunction* NegB = Call(Math, TEXT("Multiply_DoubleDouble"), { FIn(TEXT("A"), Ret(B)) }, { FConst(TEXT("B"), TEXT("-1.0")) });
	UK2Node_CallFunction* SRaw = Call(Math, TEXT("Subtract_DoubleDouble"), { FIn(TEXT("A"), Ret(NegB)), FIn(TEXT("B"), Ret(Root2)) }, {});
	// 滑动上限：左手不越过右手（右手在刀轴上的位置 zr = (W⁻¹·0).z，左手 p.z）：smax = max(0, zr - p.z - gap)
	UK2Node_CallFunction* Zr = Call(Math, TEXT("InverseTransformLocation"), { FIn(TEXT("T"), Ret(W)) }, { FConst(TEXT("Location"), TEXT("0.000000,0.000000,0.000000")) });
	UK2Node_CallFunction* ZrB = Call(Math, TEXT("BreakVector"), { FIn(TEXT("InVec"), Ret(Zr)) }, {});
	UK2Node_CallFunction* Gap = Call(Math, TEXT("Subtract_DoubleDouble"), { FIn(TEXT("A"), Out(ZrB, TEXT("Z"))), FIn(TEXT("B"), Out(PB, TEXT("Z"))) }, {});
	UK2Node_CallFunction* SMaxRaw = Call(Math, TEXT("Subtract_DoubleDouble"), { FIn(TEXT("A"), Ret(Gap)) }, { FConst(TEXT("B"), Num(MinHandGap)) });
	UK2Node_CallFunction* SMax = Call(Math, TEXT("FMax"), { FIn(TEXT("A"), Ret(SMaxRaw)) }, { FConst(TEXT("B"), TEXT("0.0")) });
	UK2Node_CallFunction* SLow = Call(Math, TEXT("FMax"), { FIn(TEXT("A"), Ret(SRaw)) }, { FConst(TEXT("B"), TEXT("0.0")) });
	UK2Node_CallFunction* S = Call(Math, TEXT("FMin"), { FIn(TEXT("A"), Ret(SLow)), FIn(TEXT("B"), Ret(SMax)) }, {});
	// 偏移（ik_hand_l 骨骼空间）：ik_hand_gun 已被 CopyBone 对齐到 hand_r，刀轴在 ik_hand_gun 空间就是 W 的 Z，再转进 ik_hand_l 空间
	UK2Node_CallFunction* AxL = Call(Math, TEXT("InverseTransformDirection"), { FIn(TEXT("T"), Ret(Lc)), FIn(TEXT("Direction"), Ret(AxW)) }, {});
	UK2Node_CallFunction* Offset = Call(Math, TEXT("Multiply_VectorFloat"), { FIn(TEXT("A"), Ret(AxL)), FIn(TEXT("B"), Ret(S)) }, {});
	if (!Missing.IsEmpty())
	{
		return Fail(TEXT("event graph nodes/pins: ") + FString::Join(Missing, TEXT(", ")));
	}
	UK2Node_Variable* SetAlpha = SpawnVariableNode(Event, true, GripAlphaVar, -2100, -200);
	UK2Node_Variable* SetOffset = SpawnVariableNode(Event, true, GripOffsetVar, -1800, -200);
	if (!Connect(ES, Ret(Alpha), SetAlpha->FindPin(GripAlphaVar, EGPD_Input))
		|| !Connect(ES, Ret(Offset), SetOffset->FindPin(GripOffsetVar, EGPD_Input))
		|| !Connect(ES, EventNode->GetThenPin(), SetAlpha->GetExecPin())
		|| !Connect(ES, SetAlpha->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), SetOffset->GetExecPin()))
	{
		return Fail(TEXT("connect variable setters / exec chain"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	CollectCompileLog(AnimBlueprint, Log, Errors);
	Log.Add(FString::Printf(TEXT("anim_nodes=%d event_nodes=%d status=%d errors=%d"), Graph->Nodes.Num(), Event->Nodes.Num(), static_cast<int32>(AnimBlueprint->Status), Errors));
	AnimBlueprint->MarkPackageDirty();
	const bool bOk = Errors == 0 && (AnimBlueprint->Status == BS_UpToDate || AnimBlueprint->Status == BS_UpToDateWithWarnings);
	Log.Insert(bOk ? TEXT("OK") : TEXT("FAIL"), 0);
	return FString::Join(Log, TEXT("\n"));
}

FString UPbzModEditorLibrary::DescribeSoulGripPostProcess(UAnimBlueprint* AnimBlueprint)
{
	TArray<FString> Lines;
	if (!AnimBlueprint)
	{
		return TEXT("null blueprint");
	}
	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph || Graph->GetFName() != TEXT("AnimGraph"))
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const FString Cls = Node->GetClass()->GetName();
			TArray<FString> Linked;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
				{
					Linked.Add(FString::Printf(TEXT("%s<-%s"), *Pin->PinName.ToString(), *Pin->LinkedTo[0]->GetOwningNode()->GetClass()->GetName()));
				}
			}
			FString Detail;
			if (Cls == TEXT("AnimGraphNode_CopyBone"))
			{
				for (const TCHAR* F : { TEXT("SourceBone"), TEXT("TargetBone"), TEXT("bCopyTranslation"), TEXT("bCopyRotation"), TEXT("bCopyScale"), TEXT("ControlSpace") })
				{
					Detail += FString::Printf(TEXT(" %s=%s"), F, *GetNodeText(Node, F));
				}
			}
			else if (Cls == TEXT("AnimGraphNode_TwoBoneIK"))
			{
				for (const TCHAR* F : { TEXT("IKBone"), TEXT("EffectorLocationSpace"), TEXT("EffectorTarget"), TEXT("bTakeRotationFromEffectorSpace"), TEXT("bAllowStretching"), TEXT("JointTargetLocationSpace"), TEXT("JointTarget"), TEXT("JointTargetLocation") })
				{
					Detail += FString::Printf(TEXT(" %s=%s"), F, *GetNodeText(Node, F));
				}
			}
			Lines.Add(FString::Printf(TEXT("anim %s [%s]%s"), *Cls, *FString::Join(Linked, TEXT(",")), *Detail));
		}
	}
	if (UEdGraph* Event = FBlueprintEditorUtils::FindEventGraph(AnimBlueprint))
	{
		int32 Calls = 0;
		int32 Unlinked = 0;
		TArray<FString> Consts;
		for (UEdGraphNode* Node : Event->Nodes)
		{
			if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
			{
				const UEdGraphPin* Value = SetNode->FindPin(SetNode->GetVarName(), EGPD_Input);
				Lines.Add(FString::Printf(TEXT("event Set %s exec=%s value_linked=%s"), *SetNode->GetVarNameString(),
					SetNode->GetExecPin()->LinkedTo.Num() ? TEXT("yes") : TEXT("no"), Value && Value->LinkedTo.Num() ? TEXT("yes") : TEXT("no")));
				continue;
			}
			UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			if (!CallNode)
			{
				continue;
			}
			++Calls;
			for (UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->LinkedTo.Num() > 0)
				{
					continue;
				}
				++Unlinked;
				if (!Pin->DefaultValue.IsEmpty())
				{
					Consts.Add(FString::Printf(TEXT("%s.%s=%s"), *CallNode->GetFunctionName().ToString(), *Pin->PinName.ToString(), *Pin->DefaultValue));
				}
			}
		}
		Lines.Add(FString::Printf(TEXT("event calls=%d unlinked_inputs=%d"), Calls, Unlinked));
		Lines.Add(TEXT("event consts: ") + FString::Join(Consts, TEXT("; ")));
	}
	for (const FBPVariableDescription& Var : AnimBlueprint->NewVariables)
	{
		Lines.Add(FString::Printf(TEXT("var %s type=%s/%s"), *Var.VarName.ToString(), *Var.VarType.PinCategory.ToString(), *Var.VarType.PinSubCategory.ToString()));
	}
	Lines.Add(FString::Printf(TEXT("status=%d"), static_cast<int32>(AnimBlueprint->Status)));
	return FString::Join(Lines, TEXT("\n"));
}

TArray<FTransform> UPbzModEditorLibrary::EvaluatePostProcessPoses(USkeletalMesh* Mesh, UClass* PostProcessClass, const TArray<FName>& BoneNames, const TArray<FTransform>& LocalTransforms, int32 NumFrames, int32 TicksPerFrame, FName AlphaVar, FName OffsetVar)
{
	TArray<FTransform> Out;
	if (!Mesh || !PostProcessClass || !PostProcessClass->IsChildOf(UAnimInstance::StaticClass()) || NumFrames <= 0 || TicksPerFrame <= 0
		|| LocalTransforms.Num() != BoneNames.Num() * NumFrames)
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ EvaluatePostProcessPoses: bad input"));
		return Out;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext(false).World() : nullptr;
	UWorld* TempWorld = nullptr;
	if (!World)
	{
		TempWorld = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("PbzGripPreview"));
		World = TempWorld;
	}
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ EvaluatePostProcessPoses: no world"));
		return Out;
	}
	USkeletalMeshComponent* Comp = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	Comp->SetUpdateAnimationInEditor(true);
	Comp->bEnableUpdateRateOptimizations = false;
	Comp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Comp->SetSkeletalMeshAsset(Mesh);
	Comp->SetAnimationMode(EAnimationMode::AnimationBlueprint, false);
	Comp->SetAnimInstanceClass(UPbzPoseFeedInstance::StaticClass());
	Comp->SetOverridePostProcessAnimBP(PostProcessClass, false);
	Comp->RegisterComponentWithWorld(World);
	Comp->InitAnim(true);
	UPbzPoseFeedInstance* Feed = Cast<UPbzPoseFeedInstance>(Comp->GetAnimInstance());
	UAnimInstance* Post = Comp->GetPostProcessInstance();
	auto Cleanup = [&]()
	{
		Comp->UnregisterComponent();
		Comp->MarkAsGarbage();
		if (TempWorld)
		{
			TempWorld->DestroyWorld(false);
		}
	};
	if (!Feed || !Post || Post->GetClass() != PostProcessClass)
	{
		UE_LOG(LogTemp, Error, TEXT("PBZ EvaluatePostProcessPoses: instances not created (feed=%d post=%s)"), Feed != nullptr ? 1 : 0, Post ? *Post->GetClass()->GetName() : TEXT("null"));
		Cleanup();
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
	const FFloatProperty* AlphaProp = AlphaVar.IsNone() ? nullptr : FindFProperty<FFloatProperty>(Post->GetClass(), AlphaVar);
	const FStructProperty* OffsetProp = OffsetVar.IsNone() ? nullptr : FindFProperty<FStructProperty>(Post->GetClass(), OffsetVar);
	Out.Reserve((NumBones + 1) * NumFrames);
	for (int32 F = 0; F < NumFrames; ++F)
	{
		Feed->MeshLocalPose.SetNum(NumBones);
		for (int32 B = 0; B < NumBones; ++B)
		{
			Feed->MeshLocalPose[B] = InputIndex[B] != INDEX_NONE ? LocalTransforms[F * BoneNames.Num() + InputIndex[B]] : Ref.GetRefBonePose()[B];
		}
		// 事件图表读的是上一次求值后的骨骼（和游戏里一样滞后一帧），每帧多 tick 几次让它收敛到本帧
		for (int32 Tick = 0; Tick < TicksPerFrame; ++Tick)
		{
			Comp->TickAnimation(1.f / 30.f, false);
			Comp->RefreshBoneTransforms(nullptr);
		}
		const TArray<FTransform>& CS = Comp->GetComponentSpaceTransforms();
		if (CS.Num() != NumBones)
		{
			UE_LOG(LogTemp, Error, TEXT("PBZ EvaluatePostProcessPoses: component space has %d bones, mesh %d"), CS.Num(), NumBones);
			Cleanup();
			return TArray<FTransform>();
		}
		Out.Append(CS);
		FTransform Diag = FTransform::Identity;
		Diag.SetTranslation(FVector(AlphaProp ? AlphaProp->GetPropertyValue_InContainer(Post) : -1.f, 0, 0));
		if (OffsetProp && OffsetProp->Struct == TBaseStructure<FVector>::Get())
		{
			Diag.SetScale3D(*OffsetProp->ContainerPtrToValuePtr<FVector>(Post));
		}
		Out.Add(Diag);
	}
	Cleanup();
	return Out;
}
