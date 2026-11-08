#include "PbzModEditorLibrary.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/SkeletalMesh.h"
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
