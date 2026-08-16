// Copyright (c) Jared Taylor

#include "RagdollComponent.h"

#include "PhysicalRagdoll.h"
#include "PhysicalRagdollTags.h"
#include "RagdollPhysicalAnimationComponent.h"
#include "RagdollStatics.h"
#include "CollisionQueryParams.h"
#include "DrawDebugHelpers.h"
#include "TimerManager.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Curves/CurveFloat.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

#if WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(RagdollComponent)

static constexpr float GRagdollWeightTolerance = 0.001f;

namespace Ragdoll
{
	static TAutoConsoleVariable<int32> CVarRagdollEnable(
		TEXT("p.Ragdoll.Enable"),
		1,
		TEXT("Whether the physical layer runs at all. 0 takes it off every character immediately and holds it ")
		TEXT("off; 1 puts back whatever each was running. Suspensions gameplay asked for are untouched."),
		ECVF_Cheat);

	static TAutoConsoleVariable<int32> CVarRagdollDebugBodies(
		TEXT("p.Ragdoll.DebugBodies"),
		0,
		TEXT("Log every body's simulation state and blend weight each time a physical profile settles"),
		ECVF_Cheat);

	static TAutoConsoleVariable<int32> CVarRagdollDebugMotion(
		TEXT("p.Ragdoll.DebugMotion"),
		0,
		TEXT("Draw the bias passed to AddPhysicalBias, and report when nothing is being applied"),
		ECVF_Cheat);

	static TAutoConsoleVariable<float> CVarRagdollTestBias(
		TEXT("p.Ragdoll.TestBias"),
		0.f,
		TEXT("Constantly bias the driven bodies along the actor's forward axis, in cm/s2, ignoring movement.\n")
		TEXT("Stand still and set this: positive should visibly pitch the torso forward. If it does nothing,\n")
		TEXT("the force is not reaching the bodies and no amount of tuning will help."),
		ECVF_Cheat);

	/** Any debug output has to hold the tick open, because a settled layer stops ticking entirely */
	static bool IsRagdollDebugActive()
	{
		return CVarRagdollDebugBodies.GetValueOnGameThread() > 0
			|| CVarRagdollDebugMotion.GetValueOnGameThread() > 0
			|| !FMath::IsNearlyZero(CVarRagdollTestBias.GetValueOnGameThread());
	}

	/** Toggling a cvar on an already-sleeping component would otherwise do nothing until something else woke it */
	static FSimpleMulticastDelegate GRagdollDebugCVarChanged;

	static void BindRagdollDebugCVarCallbacks()
	{
		static bool bBound = false;
		if (bBound)
		{
			return;
		}
		bBound = true;

		const FConsoleVariableDelegate OnChanged = FConsoleVariableDelegate::CreateLambda(
			[](IConsoleVariable*) { GRagdollDebugCVarChanged.Broadcast(); });

		CVarRagdollEnable.AsVariable()->SetOnChangedCallback(OnChanged);
		CVarRagdollDebugBodies.AsVariable()->SetOnChangedCallback(OnChanged);
		CVarRagdollDebugMotion.AsVariable()->SetOnChangedCallback(OnChanged);
		CVarRagdollTestBias.AsVariable()->SetOnChangedCallback(OnChanged);
	}
}

URagdollComponent::URagdollComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bAllowTickOnDedicatedServer = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	PhysicalAnimationComponentClass = URagdollPhysicalAnimationComponent::StaticClass();
	
	// Ragdoll defaults
	RagdollSettings.PhysicalAnimData.bIsLocalSimulation = false;
	RagdollSettings.PhysicalAnimData.OrientationStrength = 30.f;
	
	// Set up a ready-to-go implementation for always-on profile
	{
		FRagdollPhysicalProfile& Profile = PhysicalProfiles.Add(FPhysicalRagdollTags::Ragdoll_Profile);
		Profile.StrengthMultiplier = 2.f;

		FRagdollBoneGroup& Bone = Profile.BoneGroups.Add_GetRef(FRagdollBoneGroup(TEXT("spine_01")));
		Bone.BlendWeight = 0.6f;
		Bone.PhysicalAnimData.bIsLocalSimulation = false;
		Bone.PhysicalAnimData.OrientationStrength = 400.f;
		Bone.PhysicalAnimData.AngularVelocityStrength = 10.f;
		Bone.PhysicalAnimData.PositionStrength = 5.f;
		Bone.PhysicalAnimData.VelocityStrength = 5.f;
	
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("spine_05"), true, false, 0.35f));
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("neck_01"), true, false, 0.2f));
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("lowerarm_l"), true, true));
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("lowerarm_r"), true, true));
	}
}

void URagdollComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	Ragdoll::BindRagdollDebugCVarCallbacks();
	DebugCVarChangedHandle = Ragdoll::GRagdollDebugCVarChanged.AddWeakLambda(this, [this]
	{
		ApplyEnabledCVar();

		if (CurrentState == ERagdollState::Physical)
		{
			Wake();
		}
	});

	CacheReferences();
	ApplyEnabledCVar();

	if (AutoPhysicalProfile.IsValid())
	{
		SetPhysicalProfile(AutoPhysicalProfile);
	}
}

void URagdollComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Ragdoll::GRagdollDebugCVarChanged.Remove(DebugCVarChangedHandle);
	DebugCVarChangedHandle.Reset();

	Super::EndPlay(EndPlayReason);
}

void URagdollComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bQueryStateSuspension)
	{
		TickStateSuspension();
	}

	switch (CurrentState)
	{
	case ERagdollState::Physical:
		TickPhysical(DeltaTime);
		break;
	case ERagdollState::Ragdoll:
		TickRagdoll(DeltaTime);
		break;
	case ERagdollState::Recovery:
		TickRecovery(DeltaTime);
		break;
	default:
		// The query is the only thing that can bring the layer back, so it has to keep running
		if (!bQueryStateSuspension)
		{
			Sleep();
		}
		break;
	}
}

bool URagdollComponent::ShouldSuspendPhysicalLayer_Implementation(ERagdollSuspendUrgency& OutUrgency) const
{
	OutUrgency = ERagdollSuspendUrgency::Blend;
	return false;
}

void URagdollComponent::TickStateSuspension()
{
	ERagdollSuspendUrgency Urgency = ERagdollSuspendUrgency::Blend;
	const bool bShouldSuspend = ShouldSuspendPhysicalLayer(Urgency);

	const FGameplayTag& StateTag = FPhysicalRagdollTags::Ragdoll_Suspend_State;
	const ERagdollSuspendUrgency* Existing = SuspendReasons.Find(StateTag);

	if (bShouldSuspend)
	{
		// Re-suspending at a higher urgency has to take effect, not be swallowed as already suspended
		if (!Existing || *Existing != Urgency)
		{
			SuspendPhysicalLayer(StateTag, Urgency);
		}
	}
	else if (Existing)
	{
		ResumePhysicalLayer(StateTag);
	}
}

bool URagdollComponent::IsRagdollRunnable() const
{
	return GetNetMode() != NM_DedicatedServer && Ragdoll::CVarRagdollEnable.GetValueOnGameThread() > 0;
}

void URagdollComponent::ApplyEnabledCVar()
{
	const FGameplayTag& DisabledTag = FPhysicalRagdollTags::Ragdoll_Suspend_Disabled;

	if (Ragdoll::CVarRagdollEnable.GetValueOnGameThread() > 0)
	{
		ResumePhysicalLayer(DisabledTag);
	}
	else if (!SuspendReasons.Contains(DisabledTag))
	{
		SuspendPhysicalLayer(DisabledTag, ERagdollSuspendUrgency::Immediate);
	}
}

float URagdollComponent::CalculateLODScale() const
{
	if (!Mesh)
	{
		return 0.f;
	}

	// The pawn the player is driving is exempt from all of it, before any check gets a chance to run
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (Pawn && Pawn->IsLocallyControlled())
	{
		return 1.f;
	}

	if (LOD.LODThreshold >= 0 && Mesh->GetPredictedLODLevel() > LOD.LODThreshold)
	{
		return 0.f;
	}

	if (LOD.bDisableWhenNotRendered && !Mesh->WasRecentlyRendered(LOD.NotRenderedThreshold))
	{
		return 0.f;
	}

	float Scale = LOD.RemoteScale;

	if (LOD.RemoteCullDistance > 0.f)
	{
		const UWorld* World = GetWorld();
		const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (!PC)
		{
			return Scale;
		}

		FVector ViewLocation;
		FRotator ViewRotation;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

		const float Distance = FVector::Dist(ViewLocation, GetOwner()->GetActorLocation());
		if (Distance >= LOD.RemoteCullDistance)
		{
			return 0.f;
		}

		if (LOD.RemoteFadeDistance < LOD.RemoteCullDistance && Distance > LOD.RemoteFadeDistance)
		{
			const float FadeAlpha = (Distance - LOD.RemoteFadeDistance) / (LOD.RemoteCullDistance - LOD.RemoteFadeDistance);
			Scale *= 1.f - FMath::Clamp(FadeAlpha, 0.f, 1.f);
		}
	}

	return Scale;
}

ERagdollSuspendUrgency URagdollComponent::GetSuspendUrgency() const
{
	ERagdollSuspendUrgency Urgency = ERagdollSuspendUrgency::Blend;
	for (const TPair<FGameplayTag, ERagdollSuspendUrgency>& Pair : SuspendReasons)
	{
		Urgency = FMath::Max(Urgency, Pair.Value);
	}
	return Urgency;
}

void URagdollComponent::SuspendPhysicalLayer(FGameplayTag Reason, ERagdollSuspendUrgency Urgency)
{
	if (!Reason.IsValid())
	{
		return;
	}

	SuspendReasons.Add(Reason, Urgency);

	// Hold onto the profile so lifting the suspension puts back exactly what was running
	if (CurrentState == ERagdollState::Physical && ActiveProfileTag.IsValid())
	{
		PendingProfileTag = ActiveProfileTag;
	}

	if (Urgency == ERagdollSuspendUrgency::Immediate)
	{
		SuspendImmediately();
		return;
	}

	Wake();
}

void URagdollComponent::ResumePhysicalLayer(FGameplayTag Reason)
{
	if (SuspendReasons.Remove(Reason) == 0 || SuspendReasons.Num() > 0)
	{
		return;
	}

	ApplyPendingPhysicalProfile();
	Wake();
}

void URagdollComponent::ResumePhysicalLayerAll()
{
	if (SuspendReasons.Num() == 0)
	{
		return;
	}

	SuspendReasons.Reset();
	ApplyPendingPhysicalProfile();
	Wake();
}

void URagdollComponent::SuspendImmediately()
{
	if (CurrentState != ERagdollState::Physical)
	{
		return;
	}

	TeardownPhysical();
	SetState(ERagdollState::None);
}

// ============================================================================
// Physical API
// ============================================================================

void URagdollComponent::SetPhysicalProfile(FGameplayTag ProfileTag)
{
	if (!IsRagdollRunnable())
	{
		return;
	}

	if (!ProfileTag.IsValid())
	{
		ClearPhysicalProfile();
		return;
	}

	const FRagdollPhysicalProfile* Profile = PhysicalProfiles.Find(ProfileTag);
	if (!Profile)
	{
		UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s has no physical profile for %s"),
			*GetNameSafe(GetOwner()), *ProfileTag.ToString());
		return;
	}

	SetupPhysical(ProfileTag, *Profile);
}

void URagdollComponent::SetPhysicalProfileWithSettings(FGameplayTag ProfileTag, const FRagdollPhysicalProfile& Profile)
{
	SetupPhysical(ProfileTag, Profile);
}

void URagdollComponent::ClearPhysicalProfile()
{
	if (CurrentState == ERagdollState::Ragdoll || CurrentState == ERagdollState::Recovery)
	{
		PendingProfileTag = FGameplayTag::EmptyTag;
		return;
	}

	if (CurrentState != ERagdollState::Physical)
	{
		return;
	}

	const FGameplayTag OldProfileTag = ActiveProfileTag;
	ActiveProfileTag = FGameplayTag::EmptyTag;
	ActiveProfile.BoneGroups.Reset();
	ResolvedBoneOverrides.Reset();

	Wake();

	OnPhysicalProfileChanged.Broadcast(OldProfileTag, FGameplayTag::EmptyTag);
}

void URagdollComponent::SetPhysicalAlpha(float NewAlpha)
{
	NewAlpha = FMath::Clamp(NewAlpha, 0.f, 1.f);
	if (FMath::IsNearlyEqual(PhysicalAlpha, NewAlpha))
	{
		return;
	}

	PhysicalAlpha = NewAlpha;

	if (CurrentState == ERagdollState::Physical)
	{
		Wake();
	}
}

void URagdollComponent::SetPhysicalStrength(float NewStrength)
{
	NewStrength = FMath::Max(NewStrength, 0.f);
	if (FMath::IsNearlyEqual(PhysicalStrength, NewStrength))
	{
		return;
	}

	PhysicalStrength = NewStrength;

	if (CurrentState == ERagdollState::Physical)
	{
		Wake();
	}
}

void URagdollComponent::SetPhysicalBlendRate(float NewBlendRate)
{
	if (FMath::IsNearlyEqual(PhysicalBlendRateOverride, NewBlendRate))
	{
		return;
	}

	PhysicalBlendRateOverride = NewBlendRate;

	if (CurrentState == ERagdollState::Physical)
	{
		Wake();
	}
}

float URagdollComponent::GetPhysicalBlendRate() const
{
	return PhysicalBlendRateOverride >= 0.f ? PhysicalBlendRateOverride : ActiveProfile.BoneBlendRate;
}

// ============================================================================
// Ragdoll API
// ============================================================================

void URagdollComponent::RagdollDeath(FVector Impulse)
{
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		if (UCharacterMovementComponent* CMC = Character->GetCharacterMovement())
		{
			CMC->StopMovementImmediately();
			CMC->DisableMovement();
		}
		bRestoreMovementOnEnd = true;
	}

	StartRagdoll(Impulse);
}

void URagdollComponent::StartRagdoll(FVector Impulse)
{
	SetupRagdoll(Impulse);
}

void URagdollComponent::StartRagdollWithSettings(const FRagdollSettings& Settings, FVector Impulse)
{
	RagdollSettings = Settings;
	SetupRagdoll(Impulse);
}

void URagdollComponent::StopRagdoll()
{
	if (CurrentState != ERagdollState::Ragdoll)
	{
		return;
	}

	TeardownRagdoll();
	SetState(ERagdollState::None);
	ApplyPendingPhysicalProfile();
}

// ============================================================================
// Recovery API
// ============================================================================

void URagdollComponent::StartRecovery()
{
	SetupRecovery();
}

void URagdollComponent::StopRecovery()
{
	if (CurrentState != ERagdollState::Recovery)
	{
		return;
	}

	TeardownRecovery();
	SetState(ERagdollState::None);
	ApplyPendingPhysicalProfile();
}

// ============================================================================
// Query
// ============================================================================

float URagdollComponent::GetBlendAlpha() const
{
	switch (CurrentState)
	{
	case ERagdollState::Ragdoll:
		return RagdollWeight;
	case ERagdollState::Physical:
		{
			float MaxWeight = 0.f;
			for (const TPair<FName, float>& Pair : BoneWeights)
			{
				MaxWeight = FMath::Max(MaxWeight, Pair.Value);
			}
			return MaxWeight;
		}
	default:
		return 0.f;
	}
}

// ============================================================================
// Debug
// ============================================================================

static URagdollComponent* FindDebugRagdollComponent(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (const APlayerController* PC = It->Get())
		{
			if (const APawn* Pawn = PC->GetPawn())
			{
				if (URagdollComponent* Comp = Pawn->FindComponentByClass<URagdollComponent>())
				{
					return Comp;
				}
			}
		}
	}
	return nullptr;
}

static FAutoConsoleCommandWithWorldAndArgs GDebugRagdollDeathCmd(
	TEXT("p.Ragdoll.Death"),
	TEXT("Toggle ragdoll on the player's character"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (URagdollComponent* Comp = FindDebugRagdollComponent(World))
		{
			Comp->DebugRagdollDeath();
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GDebugRagdollProfileCmd(
	TEXT("p.Ragdoll.Profile"),
	TEXT("Apply a physical profile by tag on the player's character. No argument clears it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (URagdollComponent* Comp = FindDebugRagdollComponent(World))
		{
			const FGameplayTag ProfileTag = Args.Num() > 0
				? FGameplayTag::RequestGameplayTag(FName(*Args[0]), false)
				: FGameplayTag::EmptyTag;
			Comp->SetPhysicalProfile(ProfileTag);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GDebugRagdollDumpBodiesCmd(
	TEXT("p.Ragdoll.DumpBodies"),
	TEXT("Log every body's simulation state and physics blend weight on the player's character"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (const URagdollComponent* Comp = FindDebugRagdollComponent(World))
		{
			Comp->DebugDumpBodies();
		}
	}));

void URagdollComponent::DrawMotionDebug() const
{
#if !UE_BUILD_SHIPPING
	if (!Mesh || !GEngine)
	{
		return;
	}

	// GFrameCounter has already advanced if the bias came from an earlier tick group this frame
	const bool bAppliedThisFrame = LastBiasFrame + 1 >= GFrameCounter && LastBiasFrame != 0;
	const FVector Origin = Mesh->GetComponentLocation() + FVector(0.f, 0.f, 120.f);

	if (!bAppliedThisFrame)
	{
		GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this), 0.f, FColor::Red,
			TEXT("Ragdoll: AddPhysicalBias is not being called - the motion lean cannot do anything"));
		return;
	}

	const FVector Applied = LastAppliedBias;
	GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this), 0.f, FColor::Green,
		FString::Printf(TEXT("Ragdoll: bias %.0f cm/s2, forward dot %.2f (positive leans into travel)"),
			Applied.Size(), FVector::DotProduct(Applied.GetSafeNormal(), GetOwner()->GetActorForwardVector())));

	DrawDebugDirectionalArrow(GetWorld(), Origin, Origin + Applied.GetSafeNormal() * 100.f,
		20.f, FColor::Green, false, -1.f, 0, 2.f);

	// Where the radial force is actually coming from, and how far it reaches
	for (const FVector& BiasOrigin : LastBiasOrigins)
	{
		DrawDebugSphere(GetWorld(), BiasOrigin, 8.f, 8, FColor::Orange, false, -1.f, 0, 1.f);
		DrawDebugDirectionalArrow(GetWorld(), BiasOrigin, BiasOrigin + Applied.GetSafeNormal() * 80.f,
			16.f, FColor::Orange, false, -1.f, 0, 3.f);
	}

	// Every body the profile touches, so it is obvious which are driven, which are asleep, and how hard
	for (const TPair<FName, float>& Pair : BoneWeights)
	{
		const FBodyInstance* BI = Mesh->GetBodyInstance(Pair.Key);
		if (!BI)
		{
			continue;
		}

		const FVector BoneLocation = Mesh->GetBoneLocation(Pair.Key);
		const bool bAsleep = BI->IsInstanceSimulatingPhysics() && !BI->IsInstanceAwake();
		const FColor Color = bAsleep ? FColor::Red : FColor::MakeRedToGreenColorFromScalar(Pair.Value);

		DrawDebugSphere(GetWorld(), BoneLocation, 4.f + Pair.Value * 5.f, 8, Color, false, -1.f, 0, 0.5f);

		if (bAsleep)
		{
			DrawDebugString(GetWorld(), BoneLocation, TEXT("asleep"), nullptr, FColor::Red, 0.f, true, 1.f);
		}
	}
#endif
}

void URagdollComponent::DebugDumpBodies() const
{
#if !UE_BUILD_SHIPPING
	if (!Mesh)
	{
		UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: no mesh"));
		return;
	}

	UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: state=%d profile=%s bones=%d alpha=%.2f"),
		static_cast<int32>(CurrentState), *ActiveProfileTag.ToString(), BoneWeights.Num(), PhysicalAlpha);

	for (const FBodyInstance* Body : Mesh->Bodies)
	{
		if (Body && Body->BodySetup.Get())
		{
			UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: %s sim=%d weight=%.3f"),
				*Body->BodySetup.Get()->BoneName.ToString(),
				Body->IsInstanceSimulatingPhysics() ? 1 : 0,
				Body->PhysicsBlendWeight);
		}
	}
#endif
}

void URagdollComponent::DebugRagdollDeath(float ImpulseScaleXY, float ImpulseScaleZ)
{
#if !UE_BUILD_SHIPPING
	if (CurrentState == ERagdollState::Ragdoll)
	{
		StartRecovery();
		return;
	}

	if (CurrentState == ERagdollState::Recovery)
	{
		StopRecovery();
		return;
	}

	const float Angle = FMath::FRandRange(0.f, 2.f * UE_PI);
	const FVector Impulse(FMath::Cos(Angle) * ImpulseScaleXY, FMath::Sin(Angle) * ImpulseScaleXY, 0.3f * ImpulseScaleZ);
	RagdollDeath(Impulse);
#endif
}

#if WITH_EDITOR
namespace Ragdoll
{
	static const USkeletalMeshComponent* FindMeshOnActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		if (const ACharacter* Character = Cast<ACharacter>(Actor))
		{
			if (const USkeletalMeshComponent* CharacterMesh = Character->GetMesh())
			{
				return CharacterMesh;
			}
		}

		return Actor->FindComponentByClass<USkeletalMeshComponent>();
	}

	/** Blueprint-added components live on the construction script rather than on the class default */
	static const USkeletalMeshComponent* FindMeshOnConstructionScript(const UClass* Class)
	{
		for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
		{
			const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Current);
			const USimpleConstructionScript* SCS = BPClass ? BPClass->SimpleConstructionScript : nullptr;
			if (!SCS)
			{
				continue;
			}

			for (const USCS_Node* Node : SCS->GetAllNodes())
			{
				if (const USkeletalMeshComponent* Template = Node ? Cast<USkeletalMeshComponent>(Node->ComponentTemplate) : nullptr)
				{
					return Template;
				}
			}
		}

		return nullptr;
	}
}

const UPhysicsAsset* URagdollComponent::GetEditorPhysicsAsset() const
{
	if (const UPhysicsAsset* SourceAsset = ProfileSourcePhysicsAsset.LoadSynchronous())
	{
		return SourceAsset;
	}

	const USkeletalMeshComponent* MeshComp = Mesh;

	if (!MeshComp)
	{
		MeshComp = Ragdoll::FindMeshOnActor(GetOwner());
	}

	if (!MeshComp)
	{
		// A component template is outered to the generated class rather than to an actor
		const UClass* OwnerClass = GetTypedOuter<UClass>();
		if (!OwnerClass)
		{
			if (const UBlueprint* Blueprint = GetTypedOuter<UBlueprint>())
			{
				OwnerClass = Blueprint->GeneratedClass;
			}
		}

		if (OwnerClass)
		{
			MeshComp = Ragdoll::FindMeshOnActor(Cast<AActor>(OwnerClass->GetDefaultObject()));

			if (!MeshComp)
			{
				MeshComp = Ragdoll::FindMeshOnConstructionScript(OwnerClass);
			}
		}
	}

	if (!MeshComp)
	{
		return nullptr;
	}

	if (const UPhysicsAsset* PhysAsset = MeshComp->GetPhysicsAsset())
	{
		return PhysAsset;
	}

	const USkeletalMesh* SkelMesh = MeshComp->GetSkeletalMeshAsset();
	return SkelMesh ? SkelMesh->GetPhysicsAsset() : nullptr;
}

TArray<FString> URagdollComponent::GetPhysicalAnimationProfileOptions() const
{
	TArray<FString> Options { TEXT("None") };

	if (const UPhysicsAsset* PhysAsset = GetEditorPhysicsAsset())
	{
		for (const FName& ProfileName : PhysAsset->GetPhysicalAnimationProfileNames())
		{
			Options.Add(ProfileName.ToString());
		}
	}

	return Options;
}

TArray<FString> URagdollComponent::GetConstraintProfileOptions() const
{
	TArray<FString> Options { TEXT("None") };

	if (const UPhysicsAsset* PhysAsset = GetEditorPhysicsAsset())
	{
		for (const FName& ProfileName : PhysAsset->GetConstraintProfileNames())
		{
			Options.Add(ProfileName.ToString());
		}
	}

	return Options;
}
#endif

// ============================================================================
// State Management
// ============================================================================

void URagdollComponent::SetState(ERagdollState NewState)
{
	if (CurrentState == NewState)
	{
		return;
	}

	const ERagdollState OldState = CurrentState;
	CurrentState = NewState;

	if (NewState == ERagdollState::None)
	{
		RestoreCollisionEnabled();
		RestoreCollisionProfile();
		RestoreConstraintProfile();
		Sleep();

		if (bRestoreMovementOnEnd)
		{
			bRestoreMovementOnEnd = false;
			if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
			{
				if (UCharacterMovementComponent* CMC = Character->GetCharacterMovement())
				{
					CMC->SetMovementMode(MOVE_Walking);
				}
				if (UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
				{
					Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				}
			}
		}
	}
	else
	{
		Wake();
	}

	OnRagdollStateChanged.Broadcast(OldState, NewState);
}

void URagdollComponent::CacheReferences()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (!Mesh)
	{
		if (const ACharacter* Character = Cast<ACharacter>(Owner))
		{
			Mesh = Character->GetMesh();
		}
		if (!Mesh)
		{
			Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		}
	}

	if (Mesh && !bHasCachedMeshTransform)
	{
		CachedMeshRelativeTransform = Mesh->GetRelativeTransform();
		bHasCachedMeshTransform = true;
	}

	if (!PhysicalAnimation)
	{
		PhysicalAnimation = Owner->FindComponentByClass<UPhysicalAnimationComponent>();

		if (!PhysicalAnimation && PhysicalAnimationComponentClass)
		{
			PhysicalAnimation = NewObject<UPhysicalAnimationComponent>(Owner, PhysicalAnimationComponentClass,
				TEXT("RagdollPhysicalAnimation"));
			PhysicalAnimation->RegisterComponent();
		}

		if (PhysicalAnimation && !PhysicalAnimation->IsA<URagdollPhysicalAnimationComponent>())
		{
			UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s uses %s rather than a URagdollPhysicalAnimationComponent, which is not guarded against the engine crash when bone transforms are empty during tick"),
				*GetNameSafe(Owner), *GetNameSafe(PhysicalAnimation->GetClass()));
		}
	}
}

bool URagdollComponent::HasValidPhysics() const
{
	return Mesh && PhysicalAnimation && Mesh->GetPhysicsAsset();
}

// ============================================================================
// Physical Layer
// ============================================================================

void URagdollComponent::SetupPhysical(FGameplayTag ProfileTag, const FRagdollPhysicalProfile& Profile)
{
	if (!IsRagdollRunnable())
	{
		return;
	}

	// Suspended means something more important owns the character, so remember the request for later
	if (IsPhysicalSuspended())
	{
		PendingProfileTag = ProfileTag;
		return;
	}

	// Ragdoll owns the mesh, so hold the request until recovery completes
	if (CurrentState == ERagdollState::Ragdoll || CurrentState == ERagdollState::Recovery)
	{
		PendingProfileTag = ProfileTag;
		return;
	}

	CacheReferences();
	if (!HasValidPhysics())
	{
		return;
	}

	const FGameplayTag OldProfileTag = ActiveProfileTag;
	ActiveProfileTag = ProfileTag;
	ActiveProfile = Profile;

	EnsurePhysicsCollision();
	ResolveBoneOverrides();

	PhysicalAnimation->SetSkeletalMeshComponent(Mesh);

	for (const FRagdollBoneGroup& Group : ActiveProfile.BoneGroups)
	{
		WarnIfGroupUnanchored(Group);

		if (!Group.PhysicalAnimationProfile.IsNone())
		{
			WarnIfPhysicalAnimationProfileMissing(Group.RootBone, Group.PhysicalAnimationProfile);
			PhysicalAnimation->ApplyPhysicalAnimationProfileBelow(Group.RootBone, Group.PhysicalAnimationProfile, Group.bIncludeRootBone);
		}
		else
		{
			PhysicalAnimation->ApplyPhysicalAnimationSettingsBelow(Group.RootBone, Group.PhysicalAnimData, Group.bIncludeRootBone);
		}
	}

	ApplyConstraintProfile(ActiveProfile.ConstraintProfile);

	// Bones held off physics get no motor either, so they cost nothing while the group runs
	for (const FRagdollBoneOverride& Override : ActiveProfile.BoneOverrides)
	{
		if (Override.bDisablePhysics && Override.BoneName != NAME_None)
		{
			PhysicalAnimation->ApplyPhysicalAnimationSettingsBelow(Override.BoneName, FPhysicalAnimationData(), Override.bIncludeSelf);
		}
	}

	if (CurrentState != ERagdollState::Physical)
	{
		// Motors drive toward the animated pose, so they can come up instantly; only the blend weight ramps
		CurrentStrength = ActiveProfile.StrengthMultiplier * PhysicalStrength;
		PhysicalAnimation->SetStrengthMultiplyer(CurrentStrength);
		SetState(ERagdollState::Physical);
	}
	else
	{
		Wake();
	}

	if (OldProfileTag != ActiveProfileTag)
	{
		OnPhysicalProfileChanged.Broadcast(OldProfileTag, ActiveProfileTag);
	}
}

void URagdollComponent::TeardownPhysical()
{
	if (Mesh)
	{
		for (const TPair<FName, float>& Pair : BoneWeights)
		{
			URagdollStatics::SetBlendWeight(Mesh, Pair.Key, 0.f);
		}
		URagdollStatics::FinalizeMeshPhysics(Mesh);
	}

	BoneWeights.Reset();
	ResolvedBoneOverrides.Reset();
	ActiveProfileTag = FGameplayTag::EmptyTag;
	CurrentStrength = 0.f;

	if (PhysicalAnimation)
	{
		PhysicalAnimation->SetStrengthMultiplyer(1.f);
		PhysicalAnimation->SetSkeletalMeshComponent(nullptr);
	}
}

void URagdollComponent::ResolveBoneOverrides()
{
	ResolvedBoneOverrides.Reset();

	for (const FRagdollBoneOverride& Override : ActiveProfile.BoneOverrides)
	{
		if (Override.BoneName == NAME_None || (!Override.bDisablePhysics && FMath::IsNearlyEqual(Override.BlendWeightScalar, 1.f)))
		{
			continue;
		}

		URagdollStatics::ForEach(Mesh, Override.BoneName, Override.bIncludeSelf, [this, &Override](const FBodyInstance* BI)
		{
			const FName BoneName = URagdollStatics::GetBoneName(Mesh, BI);
			FRagdollBoneOverride& Resolved = ResolvedBoneOverrides.FindOrAdd(BoneName);

			// Overlapping overrides take the most restrictive result
			Resolved.bDisablePhysics |= Override.bDisablePhysics;
			Resolved.BlendWeightScalar = FMath::Min(Resolved.BlendWeightScalar, Override.BlendWeightScalar);
			return true;
		});
	}
}

void URagdollComponent::GatherTargetWeights(TMap<FName, float>& OutTargets) const
{
	for (const FRagdollBoneGroup& Group : ActiveProfile.BoneGroups)
	{
		const float GroupWeight = FMath::Clamp(Group.BlendWeight * PhysicalAlpha * LODScale, 0.f, 1.f);

		URagdollStatics::ForEach(Mesh, Group.RootBone, Group.bIncludeRootBone,
			[this, GroupWeight, &OutTargets](const FBodyInstance* BI)
		{
			const FName BoneName = URagdollStatics::GetBoneName(Mesh, BI);

			float Weight = GroupWeight;
			if (const FRagdollBoneOverride* Override = ResolvedBoneOverrides.Find(BoneName))
			{
				Weight = Override->bDisablePhysics ? 0.f : Weight * Override->BlendWeightScalar;
			}

			// Later groups win where they overlap, so a group can refine a broader one
			OutTargets.Add(BoneName, Weight);
			return true;
		});
	}
}

void URagdollComponent::AddPhysicalBias(FVector Bias, FName BoneName, float HeightOffset)
{
	if (!IsRagdollRunnable())
	{
		return;
	}

	LastBiasFrame = GFrameCounter;

#if !UE_BUILD_SHIPPING
	LastAppliedBias = Bias;
	LastBiasOrigins.Reset();
#endif

	if (CurrentState != ERagdollState::Physical || Bias.IsNearlyZero() || !Mesh)
	{
		return;
	}

	auto ApplyAt = [this, &Bias, HeightOffset](FName Bone, bool bIncludeSelf)
	{
		URagdollStatics::ForEach(Mesh, Bone, bIncludeSelf, [this, &Bias, HeightOffset](FBodyInstance* BI)
		{
			if (!BI->IsInstanceSimulatingPhysics())
			{
				return true;
			}

			// A settled body is asleep, and forces do not wake it, so every bias after the first is lost
			if (!BI->IsInstanceAwake())
			{
				BI->WakeInstance();
			}

			FVector Applied = Bias;

			if (bRestoreLinearDrift && PhysicalAnimation)
			{
				// Local simulation drives orientation only, so nothing holds a body's position. Without
				// this the bias is an open-ended acceleration and the body walks away from its pose.
				const FName BodyBone = URagdollStatics::GetBoneName(Mesh, BI);
				const FVector Target = PhysicalAnimation->GetBodyTargetTransform(BodyBone).GetLocation();
				const FVector Drift = BI->GetUnrealWorldTransform().GetLocation() - Target;

				Applied -= Drift * LinearRestoreStiffness;

				// Damped along the drift only, so travelling with the animation is left alone
				const FVector DriftDir = Drift.GetSafeNormal();
				if (!DriftDir.IsNearlyZero())
				{
					const float DriftSpeed = FVector::DotProduct(BI->GetUnrealWorldVelocity(), DriftDir);
					Applied -= DriftDir * DriftSpeed * LinearRestoreDamping;
				}
			}

			// Above the centre of mass, so the push has a lever arm to pitch the body with rather than
			// just translating it. Mass scaled, since AddForceAtPosition has no acceleration mode.
			const FVector Position = BI->GetCOMPosition() + FVector::UpVector * HeightOffset;
			BI->AddForceAtPosition(Applied * BI->GetBodyMass(), Position, true, false);

#if !UE_BUILD_SHIPPING
			LastBiasOrigins.Add(Position);
#endif
			return true;
		});
	};

	if (BoneName != NAME_None)
	{
		ApplyAt(BoneName, true);
		return;
	}

	for (const FRagdollBoneGroup& Group : ActiveProfile.BoneGroups)
	{
		ApplyAt(Group.RootBone, Group.bIncludeRootBone);
	}
}

void URagdollComponent::AddPhysicalTorque(FVector Torque, FName BoneName)
{
	LastBiasFrame = GFrameCounter;

	if (!IsRagdollRunnable() || CurrentState != ERagdollState::Physical || Torque.IsNearlyZero() || !Mesh)
	{
		return;
	}

	auto ApplyAt = [this, &Torque](FName Bone, bool bIncludeSelf)
	{
		URagdollStatics::ForEach(Mesh, Bone, bIncludeSelf, [this, &Torque](FBodyInstance* BI)
		{
			if (!BI->IsInstanceSimulatingPhysics())
			{
				return true;
			}

			if (!BI->IsInstanceAwake())
			{
				BI->WakeInstance();
			}

			BI->AddTorqueInRadians(Torque, true, true);
			return true;
		});
	};

	if (BoneName != NAME_None)
	{
		ApplyAt(BoneName, true);
		return;
	}

	for (const FRagdollBoneGroup& Group : ActiveProfile.BoneGroups)
	{
		ApplyAt(Group.RootBone, Group.bIncludeRootBone);
	}
}

void URagdollComponent::WakeBodiesBelow(FName BoneName, bool bIncludeSelf) const
{
	URagdollStatics::ForEach(Mesh, BoneName, bIncludeSelf, [](FBodyInstance* BI)
	{
		if (BI->IsInstanceSimulatingPhysics() && !BI->IsInstanceAwake())
		{
			BI->WakeInstance();
		}
		return true;
	});
}

void URagdollComponent::TickPhysical(float DeltaTime)
{
	if (!HasValidPhysics())
	{
		TeardownPhysical();
		SetState(ERagdollState::None);
		return;
	}

	LODScale = CalculateLODScale();

	TMap<FName, float> Targets;
	Targets.Reserve(BoneWeights.Num());

	// A suspension drives everything to zero without discarding the profile, so it can come straight back
	if (!IsPhysicalSuspended())
	{
		GatherTargetWeights(Targets);
	}

	// Bones the profile no longer covers fade out rather than snapping off
	for (const TPair<FName, float>& Pair : BoneWeights)
	{
		if (!Targets.Contains(Pair.Key))
		{
			Targets.Add(Pair.Key, 0.f);
		}
	}

	const float BlendRate = IsPhysicalSuspended() && GetSuspendUrgency() == ERagdollSuspendUrgency::Fast
		? FastSuspendBlendRate
		: GetPhysicalBlendRate();
	const float BlendAlpha = 1.f - FMath::Exp(-FMath::Max(BlendRate, 0.f) * DeltaTime);
	bool bConverged = true;

	for (const TPair<FName, float>& Pair : Targets)
	{
		float& Current = BoneWeights.FindOrAdd(Pair.Key);
		Current = BlendRate > 0.f ? FMath::Lerp(Current, Pair.Value, BlendAlpha) : Pair.Value;

		if (FMath::IsNearlyEqual(Current, Pair.Value, GRagdollWeightTolerance))
		{
			Current = Pair.Value;
		}
		else
		{
			bConverged = false;
		}

		URagdollStatics::SetBlendWeight(Mesh, Pair.Key, Current);
	}

	URagdollStatics::FinalizeMeshPhysics(Mesh);

#if !UE_BUILD_SHIPPING
	if (const float TestBias = Ragdoll::CVarRagdollTestBias.GetValueOnGameThread(); !FMath::IsNearlyZero(TestBias))
	{
		AddPhysicalBias(GetOwner()->GetActorForwardVector() * TestBias);
	}

	if (Ragdoll::CVarRagdollDebugMotion.GetValueOnGameThread() > 0)
	{
		DrawMotionDebug();
	}
#endif

	// Drop bones that have finished fading out
	for (auto It = BoneWeights.CreateIterator(); It; ++It)
	{
		if (It->Value <= 0.f && !Targets.FindChecked(It->Key))
		{
			It.RemoveCurrent();
		}
	}

	const float TargetStrength = ActiveProfile.StrengthMultiplier * PhysicalStrength;
	CurrentStrength = BlendRate > 0.f
		? FMath::Lerp(CurrentStrength, TargetStrength, BlendAlpha)
		: TargetStrength;

	if (FMath::IsNearlyEqual(CurrentStrength, TargetStrength, GRagdollWeightTolerance))
	{
		CurrentStrength = TargetStrength;
	}
	else
	{
		bConverged = false;
	}
	PhysicalAnimation->SetStrengthMultiplyer(CurrentStrength);

	if (BoneWeights.Num() == 0 && (ActiveProfile.BoneGroups.Num() == 0 || IsPhysicalSuspended()))
	{
		TeardownPhysical();
		SetState(ERagdollState::None);
		return;
	}

	if (bConverged)
	{
#if !UE_BUILD_SHIPPING
		if (Ragdoll::CVarRagdollDebugBodies.GetValueOnGameThread() > 0 && !bLoggedConvergedBodies)
		{
			bLoggedConvergedBodies = true;
			DebugDumpBodies();
		}

		if (Ragdoll::IsRagdollDebugActive())
		{
			return;
		}
#endif

		// Anything driving the layer needs the per-frame maintenance above to keep working, so only
		// sleep once nothing has pushed on it for a couple of frames
		if (GFrameCounter <= LastBiasFrame + 2)
		{
			return;
		}

		Sleep();
	}
	else
	{
		bLoggedConvergedBodies = false;
	}
}

void URagdollComponent::ApplyPendingPhysicalProfile()
{
	if (!PendingProfileTag.IsValid())
	{
		return;
	}

	const FGameplayTag ProfileTag = PendingProfileTag;
	PendingProfileTag = FGameplayTag::EmptyTag;
	SetPhysicalProfile(ProfileTag);
}

// ============================================================================
// Ragdoll
// ============================================================================

void URagdollComponent::SetupRagdoll(const FVector& Impulse)
{
	if (!IsRagdollRunnable())
	{
		return;
	}

	CacheReferences();
	if (!HasValidPhysics())
	{
		return;
	}

	if (CurrentState == ERagdollState::Physical)
	{
		if (bRestorePhysicalProfileAfterRecovery)
		{
			PendingProfileTag = ActiveProfileTag;
		}

		// Enter at the weight the physical layer had reached so there is no pop
		RagdollWeight = GetBlendAlpha();
		BoneWeights.Reset();
		ResolvedBoneOverrides.Reset();
		ActiveProfileTag = FGameplayTag::EmptyTag;
	}
	else
	{
		if (CurrentState == ERagdollState::Recovery)
		{
			TeardownRecovery();
		}
		RagdollWeight = 0.f;
	}

	ApplySimulationCollisionProfile();
	EnsurePhysicsCollision();

	// Detach so capsule tracking doesn't feed back into the simulation
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

	PhysicalAnimation->SetSkeletalMeshComponent(Mesh);

	if (!RagdollSettings.PhysicalAnimationProfile.IsNone())
	{
		WarnIfPhysicalAnimationProfileMissing(RagdollSettings.SimulationRootBone, RagdollSettings.PhysicalAnimationProfile);
		PhysicalAnimation->ApplyPhysicalAnimationProfileBelow(
			RagdollSettings.SimulationRootBone,
			RagdollSettings.PhysicalAnimationProfile,
			RagdollSettings.bIncludeSimulationRoot);
	}
	else
	{
		PhysicalAnimation->ApplyPhysicalAnimationSettingsBelow(
			RagdollSettings.SimulationRootBone,
			RagdollSettings.PhysicalAnimData,
			RagdollSettings.bIncludeSimulationRoot);
	}

	ApplyConstraintProfile(RagdollSettings.ConstraintProfile);

	Mesh->SetAllBodiesBelowSimulatePhysics(
		RagdollSettings.SimulationRootBone, true,
		RagdollSettings.bIncludeSimulationRoot);

	RagdollMotorStrength = 1.f;
	PhysicalAnimation->SetStrengthMultiplyer(RagdollMotorStrength);
	bRagdollBlendComplete = false;

	if (RagdollSettings.ImpulseStrength > UE_KINDA_SMALL_NUMBER)
	{
		const FVector AppliedImpulse = Impulse.IsNearlyZero()
			? FVector(0.f, 0.f, -RagdollSettings.ImpulseStrength * 0.1f)
			: Impulse.GetSafeNormal() * RagdollSettings.ImpulseStrength;

		Mesh->AddImpulseToAllBodiesBelow(AppliedImpulse, RagdollSettings.SimulationRootBone, true,
			RagdollSettings.bIncludeSimulationRoot);
	}

	TArray<FRagdollSeparatedBone> ActiveBones;
	GetActiveSeparatedBones(ActiveBones);
	for (const FRagdollSeparatedBone& SepBone : ActiveBones)
	{
		if (SepBone.BoneName == NAME_None)
		{
			continue;
		}

		const FVector BoneLocation = Mesh->GetBoneLocation(SepBone.BoneName);
		const FVector BoneImpulse = Impulse.IsNearlyZero()
			? FVector::ZeroVector
			: Impulse.GetSafeNormal() * RagdollSettings.ImpulseStrength * SepBone.ImpulseScale;

		Mesh->BreakConstraint(BoneImpulse, BoneLocation, SepBone.BoneName);
	}

	SetState(ERagdollState::Ragdoll);
}

void URagdollComponent::TeardownRagdoll()
{
	// Bone positions are still valid, so place the capsule before the simulation stops
	UpdateCapsuleToFollowMesh();

	if (Mesh)
	{
		Mesh->SetAllBodiesBelowSimulatePhysics(RagdollSettings.SimulationRootBone, false, RagdollSettings.bIncludeSimulationRoot);
		Mesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollSettings.SimulationRootBone, 0.f, false, RagdollSettings.bIncludeSimulationRoot);
	}

	if (PhysicalAnimation)
	{
		PhysicalAnimation->SetStrengthMultiplyer(1.f);
		PhysicalAnimation->SetSkeletalMeshComponent(nullptr);
	}

	SnapMeshToCapsule();

	RagdollWeight = 0.f;
	RagdollMotorStrength = 1.f;
	bRagdollBlendComplete = false;
}

void URagdollComponent::TickRagdoll(float DeltaTime)
{
	if (!HasValidPhysics())
	{
		StopRagdoll();
		return;
	}

	RagdollWeight = RagdollSettings.BlendIn.Interp(RagdollWeight, 1.f, DeltaTime);
	Mesh->SetAllBodiesBelowPhysicsBlendWeight(
		RagdollSettings.SimulationRootBone, RagdollWeight,
		false, RagdollSettings.bIncludeSimulationRoot);

	RagdollMotorStrength = RagdollSettings.MotorDecay.Interp(RagdollMotorStrength, 0.f, DeltaTime);
	PhysicalAnimation->SetStrengthMultiplyer(RagdollMotorStrength);

	if (!bRagdollBlendComplete
		&& FMath::IsNearlyEqual(RagdollWeight, 1.f, GRagdollWeightTolerance)
		&& FMath::IsNearlyZero(RagdollMotorStrength, GRagdollWeightTolerance))
	{
		bRagdollBlendComplete = true;
		OnRagdollBlendComplete.Broadcast();
	}

	UpdateCapsuleToFollowMesh();
}

// ============================================================================
// Recovery
// ============================================================================

void URagdollComponent::SetupRecovery()
{
	if (CurrentState != ERagdollState::Ragdoll)
	{
		return;
	}

	CacheReferences();
	if (!Mesh)
	{
		return;
	}

	// Determine the side while physics is still active, so bone transforms reflect the ragdoll pose
	const ERagdollRecoverySide Side = DetermineRecoverySide();
	UAnimMontage* Montage = GetRecoveryMontage(Side);

	UpdateCapsuleToFollowMesh();

	Mesh->SetAllBodiesBelowSimulatePhysics(RagdollSettings.SimulationRootBone, false, RagdollSettings.bIncludeSimulationRoot);
	Mesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollSettings.SimulationRootBone, 0.f, false, RagdollSettings.bIncludeSimulationRoot);

	if (PhysicalAnimation)
	{
		PhysicalAnimation->SetStrengthMultiplyer(1.f);
		PhysicalAnimation->SetSkeletalMeshComponent(nullptr);
	}

	RagdollWeight = 0.f;
	RagdollMotorStrength = 1.f;
	bRagdollBlendComplete = false;

	ACharacter* Character = Cast<ACharacter>(GetOwner());
	USceneComponent* Root = Character ? Character->GetRootComponent() : nullptr;
	if (Root && Mesh->GetAttachParent() != Root)
	{
		Mesh->AttachToComponent(Root, FAttachmentTransformRules::KeepWorldTransform);
	}

	RecoveryStartLocation = Mesh->GetRelativeLocation();
	RecoveryStartRotation = Mesh->GetRelativeRotation().Quaternion();
	RecoveryTargetLocation = bHasCachedMeshTransform ? CachedMeshRelativeTransform.GetLocation() : FVector::ZeroVector;
	RecoveryTargetRotation = bHasCachedMeshTransform ? CachedMeshRelativeTransform.GetRotation() : FQuat::Identity;

	RecoveryCapsuleStartZ = Character ? Character->GetActorLocation().Z : 0.f;
	RecoveryCapsuleTargetZ = RecoveryCapsuleStartZ;

	if (Character)
	{
		if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			const FVector TraceStart = Character->GetActorLocation();
			const FVector TraceEnd = TraceStart - FVector(0.f, 0.f, HalfHeight + 200.f);

			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollRecoveryFloorTrace), false, Character);
			if (GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params))
			{
				RecoveryCapsuleTargetZ = Hit.ImpactPoint.Z + HalfHeight + UCharacterMovementComponent::MAX_FLOOR_DIST;
			}
		}
	}

	RecoveryElapsedTime = 0.f;
	RecoveryAlpha = 0.f;

	if (Montage)
	{
		UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
		if (!AnimInstance)
		{
			SnapMeshToCapsule();
			SetState(ERagdollState::None);
			ApplyPendingPhysicalProfile();
			return;
		}

		RecoveryTotalDuration = Montage->GetPlayLength();
		ActiveRecoveryMontage = Montage;
		AnimInstance->Montage_Play(Montage);

		FOnMontageEnded EndDelegate;
		EndDelegate.BindUObject(this, &URagdollComponent::OnRecoveryMontageEnded);
		AnimInstance->Montage_SetEndDelegate(EndDelegate, Montage);
	}
	else
	{
		RecoveryTotalDuration = RecoverySettings.FallbackDuration;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(RecoveryTimerHandle, this,
				&URagdollComponent::OnRecoveryTimerExpired, FMath::Max(RecoveryTotalDuration, UE_KINDA_SMALL_NUMBER), false);
		}
	}

	SetState(ERagdollState::Recovery);
}

void URagdollComponent::TeardownRecovery()
{
	if (ActiveRecoveryMontage && Mesh)
	{
		if (UAnimInstance* AnimInstance = Mesh->GetAnimInstance())
		{
			AnimInstance->Montage_Stop(0.2f, ActiveRecoveryMontage);
		}
	}
	ActiveRecoveryMontage = nullptr;

	if (RecoveryTimerHandle.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(RecoveryTimerHandle);
		}
		RecoveryTimerHandle.Invalidate();
	}

	FinalizeRecoveryTransform();
	RecoveryAlpha = 0.f;
}

void URagdollComponent::TickRecovery(float DeltaTime)
{
	if (!Mesh)
	{
		ActiveRecoveryMontage = nullptr;
		SetState(ERagdollState::None);
		return;
	}

	RecoveryElapsedTime += DeltaTime;

	float Alpha;
	if (RecoverySettings.BlendCurve)
	{
		const float NormalizedTime = RecoveryTotalDuration > UE_KINDA_SMALL_NUMBER
			? FMath::Clamp(RecoveryElapsedTime / RecoveryTotalDuration, 0.f, 1.f)
			: 1.f;
		Alpha = RecoverySettings.BlendCurve->GetFloatValue(NormalizedTime);
	}
	else if (ActiveRecoveryMontage && RecoverySettings.MontageCurveName != NAME_None)
	{
		Alpha = 0.f;
		if (const UAnimInstance* AnimInstance = Mesh->GetAnimInstance())
		{
			float CurveValue = 0.f;
			if (AnimInstance->GetCurveValue(RecoverySettings.MontageCurveName, CurveValue))
			{
				Alpha = CurveValue;
			}
		}
	}
	else
	{
		Alpha = RecoveryTotalDuration > UE_KINDA_SMALL_NUMBER
			? FMath::Clamp(RecoveryElapsedTime / RecoveryTotalDuration, 0.f, 1.f)
			: 1.f;
	}

	RecoveryAlpha = Alpha;

	const float RotationAlpha = FMath::Clamp(Alpha / FMath::Max(RecoverySettings.RotationTimeScale, UE_KINDA_SMALL_NUMBER), 0.f, 1.f);

	const FVector BlendedLocation = FMath::Lerp(RecoveryStartLocation, RecoveryTargetLocation, Alpha);
	const FQuat BlendedRotation = FQuat::Slerp(RecoveryStartRotation, RecoveryTargetRotation, RotationAlpha);
	Mesh->SetRelativeLocationAndRotation(BlendedLocation, BlendedRotation);

	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		FVector CapsulePos = Character->GetActorLocation();
		CapsulePos.Z = FMath::Lerp(RecoveryCapsuleStartZ, RecoveryCapsuleTargetZ, Alpha);
		Character->SetActorLocation(CapsulePos, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void URagdollComponent::OnRecoveryMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	if (CurrentState == ERagdollState::Recovery)
	{
		ActiveRecoveryMontage = nullptr;
		FinishRecovery();
	}
}

void URagdollComponent::OnRecoveryTimerExpired()
{
	RecoveryTimerHandle.Invalidate();
	if (CurrentState == ERagdollState::Recovery)
	{
		FinishRecovery();
	}
}

void URagdollComponent::FinishRecovery()
{
	FinalizeRecoveryTransform();
	RecoveryAlpha = 0.f;
	SetState(ERagdollState::None);
	ApplyPendingPhysicalProfile();
}

void URagdollComponent::FinalizeRecoveryTransform() const
{
	if (!Mesh)
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		FVector CapsulePos = Character->GetActorLocation();
		CapsulePos.Z = RecoveryCapsuleTargetZ;
		Character->SetActorLocation(CapsulePos, false, nullptr, ETeleportType::TeleportPhysics);
	}

	Mesh->SetRelativeLocationAndRotation(RecoveryTargetLocation, RecoveryTargetRotation);
}

ERagdollRecoverySide URagdollComponent::DetermineRecoverySide() const
{
	if (!Mesh)
	{
		return ERagdollRecoverySide::Back;
	}

	const int32 BoneIndex = Mesh->GetBoneIndex(RagdollSettings.SimulationRootBone);
	if (BoneIndex == INDEX_NONE)
	{
		return ERagdollRecoverySide::Back;
	}

	const FTransform BoneTransform = Mesh->GetBoneTransform(BoneIndex);
	const FVector BoneUp = BoneTransform.GetUnitAxis(EAxis::Z);
	const FVector BoneRight = BoneTransform.GetUnitAxis(EAxis::Y);

	const float UpDot = FVector::DotProduct(BoneUp, FVector::UpVector);
	const float RightDot = FVector::DotProduct(BoneRight, FVector::UpVector);

	if (FMath::Abs(UpDot) >= FMath::Abs(RightDot))
	{
		return UpDot >= 0.f ? ERagdollRecoverySide::Back : ERagdollRecoverySide::Front;
	}
	return RightDot >= 0.f ? ERagdollRecoverySide::Left : ERagdollRecoverySide::Right;
}

UAnimMontage* URagdollComponent::GetRecoveryMontage(ERagdollRecoverySide Side) const
{
	if (const TObjectPtr<UAnimMontage>* Found = RecoverySettings.Montages.Find(Side))
	{
		return *Found;
	}
	return nullptr;
}

void URagdollComponent::GetActiveSeparatedBones(TArray<FRagdollSeparatedBone>& OutBones) const
{
	OutBones = SeparatedBones;
}

// ============================================================================
// Capsule Tracking
// ============================================================================

void URagdollComponent::UpdateCapsuleToFollowMesh() const
{
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!Mesh || !Character)
	{
		return;
	}

	const FVector RootLocation = Mesh->GetBoneLocation(RagdollSettings.SimulationRootBone);
	if (RootLocation.IsNearlyZero())
	{
		return;
	}

	float ZOffset = 0.f;
	if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
	{
		ZOffset = Capsule->GetScaledCapsuleHalfHeight();
	}

	const FVector CapsuleTarget(RootLocation.X, RootLocation.Y, RootLocation.Z + ZOffset);
	Character->SetActorLocation(CapsuleTarget, false, nullptr, ETeleportType::TeleportPhysics);
}

void URagdollComponent::SnapMeshToCapsule() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!Mesh || !Character)
	{
		return;
	}

	USceneComponent* Root = Character->GetRootComponent();
	if (Root && Mesh->GetAttachParent() != Root)
	{
		Mesh->AttachToComponent(Root, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	}

	if (bHasCachedMeshTransform)
	{
		Mesh->SetRelativeTransform(CachedMeshRelativeTransform);
	}
}

// ============================================================================
// Shared Helpers
// ============================================================================

void URagdollComponent::ApplySimulationCollisionProfile()
{
	if (!Mesh || SimulationCollisionProfile == NAME_None)
	{
		return;
	}

	if (!bCollisionProfileChanged)
	{
		OriginalCollisionProfileName = Mesh->GetCollisionProfileName();
		bCollisionProfileChanged = true;
	}

	Mesh->SetCollisionProfileName(SimulationCollisionProfile);
}

void URagdollComponent::RestoreCollisionProfile()
{
	if (!bCollisionProfileChanged || !Mesh)
	{
		return;
	}

	const FName ProfileToRestore = DefaultCollisionProfile != NAME_None
		? DefaultCollisionProfile
		: OriginalCollisionProfileName;

	if (ProfileToRestore != NAME_None)
	{
		Mesh->SetCollisionProfileName(ProfileToRestore);
	}

	bCollisionProfileChanged = false;
	OriginalCollisionProfileName = NAME_None;
}

void URagdollComponent::ApplyConstraintProfile(FName ProfileName)
{
	if (!Mesh)
	{
		return;
	}

	if (ProfileName.IsNone())
	{
		RestoreConstraintProfile();
		return;
	}

	bConstraintProfileChanged = true;
	Mesh->SetConstraintProfileForAll(ProfileName, true);
}

void URagdollComponent::RestoreConstraintProfile()
{
	if (!bConstraintProfileChanged)
	{
		return;
	}

	bConstraintProfileChanged = false;

	if (Mesh)
	{
		Mesh->SetConstraintProfileForAll(DefaultConstraintProfile, true);
	}
}

const FPhysicalAnimationData* URagdollComponent::FindPhysicalAnimationProfileData(FName BoneName, FName ProfileName) const
{
	const UPhysicsAsset* PhysAsset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PhysAsset || ProfileName.IsNone())
	{
		return nullptr;
	}

	const int32 BodyIndex = PhysAsset->FindBodyIndex(BoneName);
	if (!PhysAsset->SkeletalBodySetups.IsValidIndex(BodyIndex))
	{
		return nullptr;
	}

	const USkeletalBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[BodyIndex];
	const FPhysicalAnimationProfile* Profile = BodySetup ? BodySetup->FindPhysicalAnimationProfile(ProfileName) : nullptr;
	return Profile ? &Profile->PhysicalAnimationData : nullptr;
}

void URagdollComponent::WarnIfPhysicalAnimationProfileMissing(FName BoneName, FName ProfileName) const
{
	const UPhysicsAsset* PhysAsset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PhysAsset || ProfileName.IsNone())
	{
		return;
	}

	// A bone without a body carries no profile of its own, and the bodies below it may still have one
	const int32 BodyIndex = PhysAsset->FindBodyIndex(BoneName);
	if (!PhysAsset->SkeletalBodySetups.IsValidIndex(BodyIndex))
	{
		return;
	}

	if (!FindPhysicalAnimationProfileData(BoneName, ProfileName))
	{
		UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s asks for physical animation profile '%s' on '%s', which %s does not define for that body, so it receives no motor drive"),
			*GetNameSafe(GetOwner()), *ProfileName.ToString(), *BoneName.ToString(), *GetNameSafe(PhysAsset));
	}
}

void URagdollComponent::EnsurePhysicsCollision()
{
	if (!bAutoEnablePhysicsCollision || !Mesh)
	{
		return;
	}

	const ECollisionEnabled::Type Current = Mesh->GetCollisionEnabled();
	if (CollisionEnabledHasPhysics(Current))
	{
		return;
	}

	if (!bCollisionEnabledChanged)
	{
		OriginalCollisionEnabled = Current;
		bCollisionEnabledChanged = true;
	}

	// USkeletalMeshComponent::ShouldBlendPhysicsBones() bails without physics collision, so no
	// per-bone blend weight has any effect until this is upgraded
	Mesh->SetCollisionEnabled(CollisionEnabledHasQuery(Current)
		? ECollisionEnabled::QueryAndPhysics
		: ECollisionEnabled::PhysicsOnly);
}

void URagdollComponent::RestoreCollisionEnabled()
{
	if (!bCollisionEnabledChanged)
	{
		return;
	}

	// Restoring the profile resets the collision enabled state along with it
	if (Mesh && !bCollisionProfileChanged)
	{
		Mesh->SetCollisionEnabled(OriginalCollisionEnabled);
	}

	bCollisionEnabledChanged = false;
}

void URagdollComponent::WarnIfGroupUnanchored(const FRagdollBoneGroup& Group) const
{
	if (!Group.bIncludeRootBone || !Mesh)
	{
		return;
	}

	const UPhysicsAsset* PhysAsset = Mesh->GetPhysicsAsset();
	const USkeletalMesh* SkelMesh = Mesh->GetSkeletalMeshAsset();
	if (!PhysAsset || !SkelMesh)
	{
		return;
	}

	const FPhysicalAnimationData* ProfileData = FindPhysicalAnimationProfileData(Group.RootBone, Group.PhysicalAnimationProfile);
	if (!Group.PhysicalAnimationProfile.IsNone() && !ProfileData)
	{
		return;
	}

	if (!(ProfileData ? ProfileData->bIsLocalSimulation : Group.PhysicalAnimData.bIsLocalSimulation))
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
	int32 BoneIndex = RefSkeleton.FindBoneIndex(Group.RootBone);
	if (BoneIndex == INDEX_NONE)
	{
		UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s physical profile references bone '%s', which is not in %s"),
			*GetNameSafe(GetOwner()), *Group.RootBone.ToString(), *GetNameSafe(SkelMesh));
		return;
	}

	while ((BoneIndex = RefSkeleton.GetParentIndex(BoneIndex)) != INDEX_NONE)
	{
		if (PhysAsset->FindBodyIndex(RefSkeleton.GetBoneName(BoneIndex)) != INDEX_NONE)
		{
			return;
		}
	}

	// Local simulation zeroes the linear drive, so a body with no parent body has nothing holding it in place
	UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s physical profile drives '%s' in local simulation, but no ancestor bone has a body in %s, so it has no linear drive and will fall. Start the group below the root body, or clear bIsLocalSimulation and set PositionStrength."),
		*GetNameSafe(GetOwner()), *Group.RootBone.ToString(), *GetNameSafe(PhysAsset));
}

void URagdollComponent::Wake()
{
	PrimaryComponentTick.SetTickFunctionEnable(true);
}

void URagdollComponent::Sleep()
{
	PrimaryComponentTick.SetTickFunctionEnable(false);
}
