// Copyright (c) Jared Taylor

#include "RagdollComponent.h"

#include "RagdollAssets.h"

#include "PhysicalRagdoll.h"
#include "PhysicalRagdollTags.h"
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
#include "PhysicsControlComponent.h"
#include "PhysicsControlHelpers.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

#if WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#endif

#include "ProfilingDebugging/CpuProfilerTrace.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RagdollComponent)

static constexpr float GRagdollWeightTolerance = 0.001f;

/** Set every control and body modifier this component owns belongs to, so they can be torn down together */
static const FName GRagdollOperatorSet(TEXT("Ragdoll"));

namespace Ragdoll
{
	static TAutoConsoleVariable<int32> CVarRagdollEnable(
		TEXT("p.Ragdoll.Enable"),
		1,
		TEXT("Whether the physical layer runs at all. 0 takes it off every character immediately and holds it ")
		TEXT("off; 1 puts back whatever each was running. Suspensions gameplay asked for are untouched."),
		ECVF_Cheat);

#if !UE_BUILD_SHIPPING
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

#endif

	static TAutoConsoleVariable<bool> CVarRagdollAllowUnscopedPush(
		TEXT("p.Ragdoll.AllowUnscopedPush"),
		false,
		TEXT("Allow AddPhysicalBias and AddPhysicalTorque to be called with no bone, covering every group the\n")
		TEXT("active profile drives. Off by default: an unscoped push goes through the arms and legs as well,\n")
		TEXT("and reads as a body being shaken rather than leaning."),
		ECVF_Default);

#if !UE_BUILD_SHIPPING
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
#endif
}

URagdollComponent::URagdollComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bAllowTickOnDedicatedServer = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	PhysicsControlComponentClass = UPhysicsControlComponent::StaticClass();

	// Ragdoll defaults
	RagdollSettings.ControlData.AngularStrength = 0.87f;
	
	// Set up a ready-to-go implementation for always-on profile
	{
		FRagdollPhysicalProfile& Profile = PhysicalProfiles.Add(FPhysicalRagdollTags::Ragdoll_Profile);
		Profile.StrengthMultiplier = 2.f;

		FRagdollBoneGroup& Bone = Profile.BoneGroups.Add_GetRef(FRagdollBoneGroup(TEXT("spine_01")));
		Bone.BlendWeight = 0.6f;
		Bone.ControlType = EPhysicsControlType::WorldSpace;
		Bone.AngularStiffness = 404.f;
		Bone.AngularDamping = 10.f;
		Bone.LinearStiffness = 5.f;
		Bone.LinearDamping = 5.f;
	
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("spine_05"), true, false, 0.35f));
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("neck_01"), true, false, 0.2f));
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("lowerarm_l"), true, true));
		Profile.BoneOverrides.Add(FRagdollBoneOverride(TEXT("lowerarm_r"), true, true));
	}
}

void URagdollComponent::BeginPlay()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::BeginPlay);

	Super::BeginPlay();

	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	Ragdoll::BindRagdollDebugCVarCallbacks();
	DebugCVarChangedHandle = Ragdoll::GRagdollDebugCVarChanged.AddWeakLambda(this, [this]
	{
		ApplyEnabledCVar();

		if (CurrentState == ERagdollState::Physical)
		{
			Wake();
		}
	});
#endif

	CacheReferences();
	ApplyEnabledCVar();

	if (AutoPhysicalProfile.IsValid())
	{
		SetPhysicalProfile(AutoPhysicalProfile);
	}
}

void URagdollComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::EndPlay);

#if !UE_BUILD_SHIPPING
	Ragdoll::GRagdollDebugCVarChanged.Remove(DebugCVarChangedHandle);
	DebugCVarChangedHandle.Reset();
#endif

	Super::EndPlay(EndPlayReason);
}

void URagdollComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TickComponent);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TickStateSuspension);

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
	if (IsRunningDedicatedServer() || GetNetMode() == NM_DedicatedServer)
	{
		return false;
	}
	
	if (Ragdoll::CVarRagdollEnable.GetValueOnGameThread() <= 0)
	{
		return false;
	}
	
	return true;
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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::CalculateLODScale);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SuspendPhysicalLayer);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ResumePhysicalLayer);

	if (SuspendReasons.Remove(Reason) == 0 || SuspendReasons.Num() > 0)
	{
		return;
	}

	ApplyPendingPhysicalProfile();
	Wake();
}

void URagdollComponent::ResumePhysicalLayerAll()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ResumePhysicalLayerAll);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SuspendImmediately);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetPhysicalProfile);

	if (!IsRagdollRunnable())
	{
		return;
	}

	if (!ProfileTag.IsValid())
	{
		ClearPhysicalProfile();
		return;
	}

	if (PhysicalProfileSource == ERagdollTuningSource::Asset)
	{
		const TObjectPtr<URagdollProfileAsset>* Asset = PhysicalProfileAssets.Find(ProfileTag);
		URagdollProfileAsset* ProfileAsset = Asset ? *Asset : nullptr;
		if (!ProfileAsset)
		{
			UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s has no physical profile asset for '%s'"),
				*GetNameSafe(GetOwner()), *ProfileTag.ToString());
			return;
		}

		ActiveProfileAsset = ProfileAsset;
		ActiveProfileRevision = ProfileAsset->GetRevision();
		SetupPhysical(ProfileTag, ProfileAsset->Profile);
		return;
	}

	ActiveProfileAsset = nullptr;

	const FRagdollPhysicalProfile* Profile = PhysicalProfiles.Find(ProfileTag);
	if (!Profile)
	{
		FString Keys;
		for (const TPair<FGameplayTag, FRagdollPhysicalProfile>& Pair : PhysicalProfiles)
		{
			Keys += FString::Printf(TEXT("'%s'(valid=%d) "), *Pair.Key.ToString(), Pair.Key.IsValid() ? 1 : 0);
		}

		UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s has no physical profile for '%s' (valid=%d). PhysicalProfiles holds %d: %s"),
			*GetNameSafe(GetOwner()), *ProfileTag.ToString(), ProfileTag.IsValid() ? 1 : 0,
			PhysicalProfiles.Num(), Keys.IsEmpty() ? TEXT("<empty>") : *Keys);
		return;
	}

	SetupPhysical(ProfileTag, *Profile);
}

void URagdollComponent::RefreshProfileFromAsset()
{
	URagdollProfileAsset* Asset = ActiveProfileAsset.Get();
	if (!Asset || Asset->GetRevision() == ActiveProfileRevision)
	{
		return;
	}

	// The drives are built once, so an edited spring only reaches the solver by building them again
	ActiveProfileRevision = Asset->GetRevision();
	SetupPhysical(ActiveProfileTag, Asset->Profile);
}

void URagdollComponent::SetPhysicalProfileWithSettings(FGameplayTag ProfileTag, const FRagdollPhysicalProfile& Profile)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetPhysicalProfileWithSettings);

	SetupPhysical(ProfileTag, Profile);
}

void URagdollComponent::ClearPhysicalProfile()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ClearPhysicalProfile);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::RagdollDeath);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::StartRagdollWithSettings);

	RagdollSettings = Settings;
	SetupRagdoll(Impulse);
}

void URagdollComponent::StopRagdoll()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::StopRagdoll);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::StartRecovery);

	SetupRecovery();
}

void URagdollComponent::StopRecovery()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::StopRecovery);

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

#if !UE_BUILD_SHIPPING
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
#endif

#if !UE_BUILD_SHIPPING
void URagdollComponent::DrawMotionDebug() const
{
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
}
#endif

void URagdollComponent::DebugDumpBodies() const
{
#if !UE_BUILD_SHIPPING
	if (!Mesh)
	{
		UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: no mesh"));
		return;
	}

	UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: state=%d profile=%s bones=%d alpha=%.2f strength=%.2f lod=%.2f"),
		static_cast<int32>(CurrentState), *ActiveProfileTag.ToString(), BoneWeights.Num(), PhysicalAlpha,
		CurrentStrength, LODScale);

	// Physics blending is skipped wholesale without physics collision on the mesh, so it is the first thing to rule out
	UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: mesh collision=%d hasPhysics=%d profile=%s control=%s controls=%d modifiers=%d"),
		static_cast<int32>(Mesh->GetCollisionEnabled()),
		CollisionEnabledHasPhysics(Mesh->GetCollisionEnabled()) ? 1 : 0,
		*Mesh->GetCollisionProfileName().ToString(),
		*GetNameSafe(PhysicsControl),
		PhysicsControl ? PhysicsControl->GetControlNamesInSet(GRagdollOperatorSet).Num() : -1,
		PhysicsControl ? PhysicsControl->GetBodyModifierNamesInSet(GRagdollOperatorSet).Num() : -1);

	for (const FBodyInstance* Body : Mesh->Bodies)
	{
		const UBodySetup* BodySetup = Body ? Body->GetBodySetup() : nullptr;
		if (!BodySetup)
		{
			continue;
		}

		const FName BoneName = BodySetup->BoneName;
		const FName OperatorName = MakeOperatorName(BoneName);

		FPhysicsControlData ControlData;
		const bool bHasControl = PhysicsControl && PhysicsControl->GetControlData(OperatorName, ControlData);

		/**
		 * Deviation from the pose the motors are supposed to be holding, read from the same buffer the
		 * control component takes its targets from. Small means the motors are working, large and steady
		 * means they are not reaching the bodies at all, and large and changing means they are unstable.
		 */
		float ErrorDegrees = -1.f;
		float SpinRate = 0.f;
		const int32 BoneIndex = Mesh->GetBoneIndex(BoneName);
		const TArray<FTransform>& Animated = Mesh->GetEditableComponentSpaceTransforms();
		if (Animated.IsValidIndex(BoneIndex))
		{
			const FQuat Target = (Animated[BoneIndex] * Mesh->GetComponentTransform()).GetRotation();
			const FQuat Current = Body->GetUnrealWorldTransform().GetRotation();
			ErrorDegrees = FMath::RadiansToDegrees((Target.Inverse() * Current).GetNormalized().GetAngle());
			SpinRate = Body->GetUnrealWorldAngularVelocityInRadians().Size();
		}

		UE_LOG(LogPhysicalRagdoll, Display, TEXT("DumpBodies: %s sim=%d weight=%.3f shapeCollision=%d control=%d enabled=%d angular=%.2f linear=%.2f error=%.1fdeg spin=%.2frad/s"),
			*BoneName.ToString(),
			Body->IsInstanceSimulatingPhysics() ? 1 : 0,
			Body->PhysicsBlendWeight,
			BodySetup->AggGeom.GetElementCount() > 0 ? static_cast<int32>(Body->GetShapeCollisionEnabled(0)) : -1,
			bHasControl ? 1 : 0,
			bHasControl && ControlData.bEnabled ? 1 : 0,
			bHasControl ? ControlData.AngularStrength : 0.f,
			bHasControl ? ControlData.LinearStrength : 0.f,
			ErrorDegrees, SpinRate);
	}

	if (PhysicsControl)
	{
		PhysicsControl->LogControlsAndBodyModifiers();
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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetState);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::CacheReferences);

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

	if (!PhysicsControl)
	{
		PhysicsControl = Owner->FindComponentByClass<UPhysicsControlComponent>();

		if (!PhysicsControl && PhysicsControlComponentClass)
		{
			PhysicsControl = NewObject<UPhysicsControlComponent>(Owner, PhysicsControlComponentClass,
				TEXT("RagdollPhysicsControl"));
			PhysicsControl->SetupAttachment(Owner->GetRootComponent());
			PhysicsControl->RegisterComponent();
		}

		if (PhysicsControl)
		{
			// Weights and strengths are written from our tick, and the control component reads them on
			// its own, so it has to come second within the tick group
			PhysicsControl->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
		}

		if (Mesh)
		{
			// The animated pose is only in the mesh's buffer once it has evaluated
			PrimaryComponentTick.AddPrerequisite(Mesh, Mesh->PrimaryComponentTick);
		}
	}
}

bool URagdollComponent::HasValidPhysics() const
{
	return Mesh && PhysicsControl && Mesh->GetPhysicsAsset();
}

// ============================================================================
// Controls and Body Modifiers
// ============================================================================

FName URagdollComponent::MakeOperatorName(FName BoneName)
{
	return FName(*(TEXT("Ragdoll_") + BoneName.ToString()));
}

void URagdollComponent::EnsureBodyModifierForBone(FName BoneName) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::EnsureBodyModifierForBone);

	const FName Name = MakeOperatorName(BoneName);
	if (PhysicsControl->GetBodyModifierExists(Name))
	{
		return;
	}

	FPhysicsControlModifierData ModifierData;
	ModifierData.MovementType = EPhysicsMovementType::Default;
	ModifierData.PhysicsBlendWeight = 0.f;

	// The modifier rewrites every shape's collision each tick, so it has to start from what the body
	// already has or simply owning a modifier would change the body's collision
	const FBodyInstance* BI = Mesh->GetBodyInstance(BoneName);
	const UBodySetup* BodySetup = BI ? BI->GetBodySetup() : nullptr;
	if (BodySetup && BodySetup->AggGeom.GetElementCount() > 0)
	{
		ModifierData.CollisionType = BI->GetShapeCollisionEnabled(0);
	}

	PhysicsControl->CreateNamedBodyModifier(Name, Mesh, BoneName, GRagdollOperatorSet, ModifierData);
}

float URagdollComponent::CalculateLoadScale(FName BoneName, float MaxScale) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::CalculateLoadScale);

	const FBodyInstance* Own = Mesh ? Mesh->GetBodyInstance(BoneName) : nullptr;
	if (!Own)
	{
		return 1.f;
	}

	// Inertia about the joint, so mass out along the chain counts for what it is really worth
	const FVector Pivot = Mesh->GetBoneLocation(BoneName, EBoneSpaces::WorldSpace);

	// Parallel axis: a body's own spin inertia plus its mass at its distance from the joint. Without the
	// first term any body whose centre of mass sits on its bone reads as weightless.
	auto InertiaAbout = [&Pivot](const FBodyInstance* Body) -> double
	{
		const FVector Tensor = Body->GetBodyInertiaTensor();
		const double Spin = (Tensor.X + Tensor.Y + Tensor.Z) / 3.0;
		return Spin + Body->GetBodyMass() * FVector::DistSquared(Body->GetCOMPosition(), Pivot);
	};

	double Load = 0.0;
	URagdollStatics::ForEach(Mesh, BoneName, true, [&Load, &InertiaAbout](const FBodyInstance* Body)
	{
		Load += InertiaAbout(Body);
		return true;
	});

	const double Self = InertiaAbout(Own);
	if (Self <= UE_KINDA_SMALL_NUMBER || Load <= Self)
	{
		return 1.f;
	}

	// Strength is a frequency and stiffness its square, so the inertia ratio enters as its root
	return FMath::Clamp(FMath::Sqrt(static_cast<float>(Load / Self)), 1.f, MaxScale);
}

FPhysicsControlData URagdollComponent::ResolveGroupSpring(const FRagdollBoneGroup& Group, FName BoneName) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ResolveGroupSpring);

	FPhysicsControlData Data;
	Data.bUseSkeletalAnimation = Group.bUseSkeletalAnimation;
	Data.bUseAccelerationDriveMode = Group.bUseAccelerationDriveMode;
	Data.bDisableCollision = Group.bDisableCollision;
	Data.MaxForce = Group.LinearMaxForce;

	// World space drives every body against its own target, so nothing hangs off the spring to correct for
	const float LoadScale = Group.bScaleStrengthByLoad && Group.ControlType == EPhysicsControlType::ParentSpace
		? CalculateLoadScale(BoneName, Group.MaxLoadScale)
		: 1.f;

	/**
	 * Acceleration drive divides the whole spring by the body's own inertia while the joint is holding the
	 * chain, so stiffness and damping both take the full load, not its root. Scaling only one of them
	 * changes the damping ratio, which is what leaves a loaded joint ringing.
	 */
	const float Load = FMath::Square(LoadScale);

	float Stiffness = Group.AngularStiffness * Load;
	float Damping = Group.AngularDamping * Load;
	Data.MaxTorque = Group.AngularMaxTorque;

	// Past a fraction of the physics rate the drive overcorrects every step and rings instead of holding
	if (Group.MaxDriveFrequency > 0.f)
	{
		Stiffness = FMath::Min(Stiffness, FMath::Square(Group.MaxDriveFrequency * UE_TWO_PI));
	}

	/**
	 * Both paths take a strength and a ratio, so the spring goes back through them rather than arriving as
	 * extra damping. The layer strength multiplies the strength alone, and a spring scaled that way only
	 * stays at the same damping ratio while the damping is expressed as one: put in as extra damping it
	 * would stay put while the stiffness moved, and the body would go sluggish every time the layer eased
	 * off.
	 */
	const float Frequency = FMath::Sqrt(Stiffness);

	Data.AngularStrength = Frequency / UE_TWO_PI;
	Data.AngularDampingRatio = Frequency > UE_KINDA_SMALL_NUMBER ? Damping / (2.f * Frequency) : 0.f;
	Data.AngularExtraDamping = 0.f;

	const float LinearStiffness = Group.LinearStiffness * Load;
	const float LinearFrequency = FMath::Sqrt(LinearStiffness);

	Data.LinearStrength = LinearFrequency / UE_TWO_PI;
	Data.LinearDampingRatio = LinearFrequency > UE_KINDA_SMALL_NUMBER
		? Group.LinearDamping * Load / (2.f * LinearFrequency)
		: 0.f;
	Data.LinearExtraDamping = 0.f;

	return Data;
}

void URagdollComponent::SetupJointDriveForBone(FName BoneName, const FPhysicsControlData& ControlData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetupJointDriveForBone);

	const int32 Index = Mesh ? Mesh->FindConstraintIndex(BoneName) : INDEX_NONE;
	FConstraintInstance* Constraint = Index != INDEX_NONE ? Mesh->GetConstraintInstanceByIndex(static_cast<uint32>(Index)) : nullptr;
	if (!Constraint)
	{
		return;
	}

	/**
	 * The damping ratio takes the load scale as well as the strength does. Acceleration drive normalises
	 * by the body's own inertia while the joint is carrying the whole chain, which divides the ratio by
	 * the same load the strength multiplies in, so a critically damped number arrives badly under-damped
	 * and the joint rings however high the damping is set.
	 */
	float Spring, Damping;
	UE::PhysicsControl::ConvertStrengthToSpringParams(Spring, Damping,
		ControlData.AngularStrength, ControlData.AngularDampingRatio, ControlData.AngularExtraDamping);

	/**
	 * Chaos discards the SLERP drive on any joint with a locked angular axis and looks for the twist and
	 * swing drives instead, so a joint left on SLERP alone ends up with no drive at all and hangs loose
	 * inside its limits however high the strength is set.
	 */
	const bool bLockedAxis = Constraint->GetAngularTwistMotion() == ACM_Locked
		|| Constraint->GetAngularSwing1Motion() == ACM_Locked
		|| Constraint->GetAngularSwing2Motion() == ACM_Locked;

	Constraint->SetAngularDriveMode(bLockedAxis ? EAngularDriveMode::TwistAndSwing : EAngularDriveMode::SLERP);
	Constraint->SetAngularDriveAccelerationMode(ControlData.bUseAccelerationDriveMode);
	Constraint->SetAngularDriveParams(Spring, Damping, ControlData.MaxTorque);
	Constraint->SetOrientationDriveSLERP(true);
	Constraint->SetAngularVelocityDriveSLERP(true);
	Constraint->SetOrientationDriveTwistAndSwing(true, true);
	Constraint->SetAngularVelocityDriveTwistAndSwing(true, true);

	JointDriveParams.Add(BoneName, FVector(Spring, Damping, ControlData.MaxTorque));
}

void URagdollComponent::ApplyJointDriveStrength(float Multiplier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ApplyJointDriveStrength);

	if (JointDriveParams.IsEmpty() || !Mesh || FMath::IsNearlyEqual(AppliedJointDriveStrength, Multiplier))
	{
		return;
	}

	AppliedJointDriveStrength = Multiplier;

	for (const TPair<FName, FVector>& Pair : JointDriveParams)
	{
		const int32 Index = Mesh->FindConstraintIndex(Pair.Key);
		FConstraintInstance* Constraint = Index != INDEX_NONE ? Mesh->GetConstraintInstanceByIndex(static_cast<uint32>(Index)) : nullptr;
		if (!Constraint)
		{
			continue;
		}

		// Strength is a frequency, so stiffness takes its square where damping takes it once
		Constraint->SetAngularDriveParams(Pair.Value.X * FMath::Square(Multiplier),
			Pair.Value.Y * Multiplier, Pair.Value.Z);
	}
}

void URagdollComponent::SetBodyIterationCounts(FBodyInstance* Body, int32 Position, int32 Velocity)
{
	// The flag that makes the per-body counts count at all has no setter, only a UPROPERTY
	static const FBoolProperty* OverrideProperty = CastField<FBoolProperty>(
		FBodyInstance::StaticStruct()->FindPropertyByName(TEXT("bOverrideIterationCounts")));

	if (!Body || !OverrideProperty)
	{
		return;
	}

	OverrideProperty->SetPropertyValue_InContainer(Body, Position >= 0 || Velocity >= 0);

	Body->SetPositionSolverIterationCount(static_cast<uint8>(FMath::Max(Position, 0)));
	Body->SetVelocitySolverIterationCount(static_cast<uint8>(FMath::Max(Velocity, 0)));
}

void URagdollComponent::RestoreBodyIterationCounts()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::RestoreBodyIterationCounts);

	for (const TPair<FName, FIntPoint>& Pair : RestoreIterationCounts)
	{
		SetBodyIterationCounts(Mesh ? Mesh->GetBodyInstance(Pair.Key) : nullptr, Pair.Value.X, Pair.Value.Y);
	}

	RestoreIterationCounts.Reset();
}

void URagdollComponent::UpdateJointDriveVelocities(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::UpdateJointDriveVelocities);

	if (DrivenJoints.IsEmpty() || DeltaTime <= 0.f || !Mesh)
	{
		return;
	}

	const TArrayView<const FTransform> LocalPose = Mesh->GetBoneSpaceTransformsView();

	for (const FName& BoneName : DrivenJoints)
	{
		const int32 BoneIndex = Mesh->GetBoneIndex(BoneName);
		if (!LocalPose.IsValidIndex(BoneIndex))
		{
			continue;
		}

		const FQuat Target = LocalPose[BoneIndex].GetRotation();

		FQuat* Previous = PreviousJointTargets.Find(BoneName);
		if (!Previous)
		{
			PreviousJointTargets.Add(BoneName, Target);
			continue;
		}

		const int32 Index = Mesh->FindConstraintIndex(BoneName);
		if (FConstraintInstance* Constraint = Index != INDEX_NONE ? Mesh->GetConstraintInstanceByIndex(static_cast<uint32>(Index)) : nullptr)
		{
			const FQuat Delta = (Target * Previous->Inverse()).GetShortestArcWith(FQuat::Identity);

			// The velocity target shares the orientation target's frame, so it is carried into it the same way
			const FQuat JointFrame = Constraint->GetRefFrame(EConstraintFrame::Frame2).GetRotation();
			const FQuat Local = JointFrame.Inverse() * Delta * JointFrame;

			// Capped, since one fast frame of animation would otherwise arrive as a kick
			const FVector Velocity = Local.ToRotationVector() / (DeltaTime * UE_TWO_PI);

			Constraint->SetAngularVelocityTarget(Velocity.GetClampedToMaxSize(MaxJointDriveVelocity));
		}

		*Previous = Target;
	}
}

void URagdollComponent::ClearJointDriveForBone(FName BoneName) const
{
	const int32 Index = Mesh ? Mesh->FindConstraintIndex(BoneName) : INDEX_NONE;
	FConstraintInstance* Constraint = Index != INDEX_NONE ? Mesh->GetConstraintInstanceByIndex(static_cast<uint32>(Index)) : nullptr;
	if (!Constraint)
	{
		return;
	}

	// Reinitialised from the asset, since zeroing the params would throw away any drives it authored
	Mesh->SetConstraintProfile(BoneName, NAME_None, true);
}

void URagdollComponent::CreateDriveForBone(FName BoneName, const FPhysicsControlData& ControlData,
	EPhysicsControlType ControlType, FName ConstraintProfile) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::CreateDriveForBone);

	EnsureBodyModifierForBone(BoneName);

	const FName Name = MakeOperatorName(BoneName);
	if (PhysicsControl->GetControlExists(Name))
	{
		PhysicsControl->DestroyControl(Name, true, false);
	}

	// A joint drive is expressed relative to the parent body, so reading one implies parent space
	const bool bParentSpace = ControlType == EPhysicsControlType::ParentSpace || !ConstraintProfile.IsNone();
	const FName ParentBoneName = bParentSpace
		? UE::PhysicsControl::GetPhysicalParentBone(Mesh, BoneName)
		: NAME_None;
	UPrimitiveComponent* ParentComponent = ParentBoneName.IsNone() ? nullptr : Mesh.Get();

	FPhysicsControlData Data = ControlData;

	if (!ConstraintProfile.IsNone() && ParentComponent)
	{
		FConstraintProfileProperties ProfileProperties;
		if (Mesh->GetConstraintProfilePropertiesOrDefault(ProfileProperties, BoneName, ConstraintProfile))
		{
			UE::PhysicsControl::ConvertConstraintProfileToControlData(Data, ProfileProperties);

			// A joint drive has no animation target velocity, so its damping acts against the world
			Data.AngularTargetVelocityMultiplier = 0.f;
			Data.LinearTargetVelocityMultiplier = 0.f;
		}
	}

	PhysicsControl->CreateNamedControl(Name, ParentComponent, ParentBoneName, Mesh, BoneName,
		Data, FPhysicsControlTarget(), GRagdollOperatorSet);
}

void URagdollComponent::DestroyDriveForBone(FName BoneName) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::DestroyDriveForBone);

	if (!PhysicsControl)
	{
		return;
	}

	const FName Name = MakeOperatorName(BoneName);
	if (PhysicsControl->GetControlExists(Name))
	{
		PhysicsControl->DestroyControl(Name, true, false);
	}
	if (PhysicsControl->GetBodyModifierExists(Name))
	{
		PhysicsControl->DestroyBodyModifier(Name, true, false);
	}
}

void URagdollComponent::DestroyAllDrives() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::DestroyAllDrives);

	if (!PhysicsControl)
	{
		return;
	}

	if (PhysicsControl->GetControlNamesInSet(GRagdollOperatorSet).Num() > 0)
	{
		PhysicsControl->DestroyControlsInSet(GRagdollOperatorSet);
	}
	if (PhysicsControl->GetBodyModifierNamesInSet(GRagdollOperatorSet).Num() > 0)
	{
		PhysicsControl->DestroyBodyModifiersInSet(GRagdollOperatorSet);
	}
}

void URagdollComponent::ApplyBoneWeight(FName BoneName, float Weight) const
{
	const FName Name = MakeOperatorName(BoneName);

	/**
	 * Always Default, which the control component reads as "leave this body alone". Asking it for
	 * Simulated makes it call SetInstanceSimulatePhysics every tick, which re-asserts the particle's
	 * state and wakes it, so the bodies never settle and carry a permanent tremor. SetBlendWeight below
	 * owns whether the body simulates, and only touches it when that actually changes.
	 */
	PhysicsControl->SetBodyModifierMovementType(Name, EPhysicsMovementType::Default, true, false);
	PhysicsControl->SetBodyModifierPhysicsBlendWeight(Name, Weight, true, false);

	// The control component applies its modifiers on its own tick and never does the mesh level
	// bookkeeping FinalizeMeshPhysics needs, so the weight goes straight to the body as well
	URagdollStatics::SetBlendWeight(Mesh, BoneName, Weight);
}

void URagdollComponent::ApplyStrengthMultiplier(float Multiplier) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ApplyStrengthMultiplier);

	// Addressing an empty set logs a warning, and a profile with no groups is legitimate
	if (PhysicsControl->GetControlNamesInSet(GRagdollOperatorSet).IsEmpty())
	{
		return;
	}

	FPhysicsControlMultiplier Mult;
	Mult.LinearStrengthMultiplier = FVector(Multiplier);
	Mult.AngularStrengthMultiplier = Multiplier;

	PhysicsControl->SetControlMultipliersInSet(GRagdollOperatorSet, Mult);
}

// ============================================================================
// Physical Layer
// ============================================================================

void URagdollComponent::SetupPhysical(FGameplayTag ProfileTag, const FRagdollPhysicalProfile& Profile)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetupPhysical);

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
	RuntimeBoneOverrides.Reset();
	GatherRuntimeBoneOverrides(RuntimeBoneOverrides);
	ResolveBoneOverrides();

	// The drive definition changes wholesale, so start from nothing rather than reconcile it. Modifiers
	// are left alone, since a bone the new profile has dropped still needs one to fade its weight out.
	if (PhysicsControl->GetControlNamesInSet(GRagdollOperatorSet).Num() > 0)
	{
		PhysicsControl->DestroyControlsInSet(GRagdollOperatorSet);
	}

	LeanedBones.Reset();
	PendingLeanBones.Reset();

	for (const FName& JointName : DrivenJoints)
	{
		ClearJointDriveForBone(JointName);
	}
	DrivenJoints.Reset();
	PreviousJointTargets.Reset();
	JointDriveParams.Reset();
	AppliedJointDriveStrength = -1.f;
	HeldLeans.Reset();
	RestoreBodyIterationCounts();

	// The engine writes the joint targets from the animated pose, which is the whole point of driving them
	if (!bRestoreUpdateJointsFromAnimation)
	{
		bool bWantsJointDrives = false;
		for (const FRagdollBoneGroup& Group : ActiveProfile.BoneGroups)
		{
			bWantsJointDrives |= Group.bUseJointDrives;
		}

		if (bWantsJointDrives && !Mesh->bUpdateJointsFromAnimation)
		{
			Mesh->bUpdateJointsFromAnimation = true;
			bRestoreUpdateJointsFromAnimation = true;
		}
	}

	for (const FRagdollBoneGroup& Group : ActiveProfile.BoneGroups)
	{
		WarnIfGroupUnanchored(Group);

		URagdollStatics::ForEach(Mesh, Group.RootBone, Group.bIncludeRootBone, [this, &Group](FBodyInstance* BI)
		{
			const FName BoneName = URagdollStatics::GetBoneName(Mesh, BI);

			// Bones held off physics get no control either, so they cost nothing while the group runs
			const FRagdollBoneOverride* Override = ResolvedBoneOverrides.Find(BoneName);
			if (Override && Override->bDisablePhysics)
			{
				return true;
			}

			// Chaos rebuilds a body's particle when it starts simulating, which would leave the control's
			// constraint holding a dead one, so the body has to be dynamic before the control is made
			if (!BI->IsInstanceSimulatingPhysics())
			{
				BI->SetInstanceSimulatePhysics(true, true, true);
			}

			if (Group.PositionSolverIterations > 0 || Group.VelocitySolverIterations > 0)
			{
				// Negative means the body was on the scene default, which is what it has to go back to
				RestoreIterationCounts.FindOrAdd(BoneName,
					FIntPoint(BI->GetPositionSolverIterationCount(), BI->GetVelocitySolverIterationCount()));

				SetBodyIterationCounts(BI, Group.PositionSolverIterations, Group.VelocitySolverIterations);
			}

			const FPhysicsControlData Spring = ResolveGroupSpring(Group, BoneName);

			// A joint drive holds a body against its parent, so it cannot serve a group asking for world space
			if (Group.bUseJointDrives && Group.ControlType == EPhysicsControlType::ParentSpace)
			{
				EnsureBodyModifierForBone(BoneName);
				SetupJointDriveForBone(BoneName, Spring);
				DrivenJoints.Add(BoneName);
				return true;
			}

			CreateDriveForBone(BoneName, Spring, Group.ControlType, Group.ControlDataConstraintProfile);
			return true;
		});
	}

	ApplyConstraintProfile(ActiveProfile.ConstraintProfile);

	if (CurrentState != ERagdollState::Physical)
	{
		// Controls drive toward the animated pose, so they can come up instantly; only the weight ramps
		CurrentStrength = ActiveProfile.StrengthMultiplier * PhysicalStrength;
	}

	// The controls above were created at full strength, so the running value has to land on them before
	// the control component's tick sees them
	ApplyStrengthMultiplier(CurrentStrength);
	ApplyJointDriveStrength(CurrentStrength);

	if (CurrentState != ERagdollState::Physical)
	{
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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TeardownPhysical);

	LeanedBones.Reset();
	PendingLeanBones.Reset();

	for (const FName& JointName : DrivenJoints)
	{
		ClearJointDriveForBone(JointName);
	}
	DrivenJoints.Reset();
	PreviousJointTargets.Reset();
	JointDriveParams.Reset();
	AppliedJointDriveStrength = -1.f;
	HeldLeans.Reset();
	RestoreBodyIterationCounts();

	if (Mesh && bRestoreUpdateJointsFromAnimation)
	{
		bRestoreUpdateJointsFromAnimation = false;
		Mesh->bUpdateJointsFromAnimation = false;
	}

	if (Mesh)
	{
		for (const TPair<FName, float>& Pair : BoneWeights)
		{
			URagdollStatics::SetBlendWeight(Mesh, Pair.Key, 0.f);
		}

		URagdollStatics::FinalizeMeshPhysics(Mesh);
	}

	DestroyAllDrives();

	BoneWeights.Reset();
	ResolvedBoneOverrides.Reset();
	ActiveProfileTag = FGameplayTag::EmptyTag;
	CurrentStrength = 0.f;
}

void URagdollComponent::ResolveBoneOverrides()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ResolveBoneOverrides);

	ResolvedBoneOverrides.Reset();

	auto Resolve = [this](const FRagdollBoneOverride& Override)
	{
		if (Override.BoneName == NAME_None || (!Override.bDisablePhysics && FMath::IsNearlyEqual(Override.BlendWeightScalar, 1.f)))
		{
			return;
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
	};

	for (const FRagdollBoneOverride& Override : ActiveProfile.BoneOverrides)
	{
		Resolve(Override);
	}

	for (const FRagdollRuntimeBoneOverride& Runtime : RuntimeBoneOverrides)
	{
		if (!Runtime.ProfileTag.IsValid() || Runtime.ProfileTag == ActiveProfileTag)
		{
			Resolve(Runtime.Override);
		}
	}
}

void URagdollComponent::RefreshBoneOverrides()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::RefreshBoneOverrides);

	TArray<FRagdollRuntimeBoneOverride> Gathered;
	GatherRuntimeBoneOverrides(Gathered);

	if (Gathered == RuntimeBoneOverrides)
	{
		return;
	}

	RuntimeBoneOverrides = MoveTemp(Gathered);

	// A bone the overrides just dropped has no control to disable, so the drives are built again rather
	// than reconciled. The bone weights blend, so the change still arrives as a fade.
	if (CurrentState == ERagdollState::Physical && ActiveProfileTag.IsValid())
	{
		SetupPhysical(ActiveProfileTag, ActiveProfile);
	}
}

void URagdollComponent::GatherTargetWeights(TMap<FName, float>& OutTargets) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::GatherTargetWeights);

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

bool URagdollComponent::AllowUnscopedPush(const TCHAR* FunctionName)
{
	if (Ragdoll::CVarRagdollAllowUnscopedPush.GetValueOnGameThread())
	{
		return true;
	}

	if (!bWarnedUnscopedPush)
	{
		bWarnedUnscopedPush = true;
		UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s called %s with no bone. That covers every group the ")
			TEXT("profile drives, arms and legs included. Pass the bone to push from, or set ")
			TEXT("p.Ragdoll.AllowUnscopedPush=1 under [ConsoleVariables]."),
			*GetNameSafe(GetOwner()), FunctionName);
	}

	return false;
}

void URagdollComponent::AddPhysicalBias(FVector Bias, FName BoneName, float HeightOffset)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::AddPhysicalBias);

	if (!IsRagdollRunnable())
	{
		return;
	}

	if (BoneName == NAME_None && !AllowUnscopedPush(TEXT("AddPhysicalBias")))
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

			const FVector& Applied = Bias;

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::AddPhysicalTorque);

	if (BoneName == NAME_None && !AllowUnscopedPush(TEXT("AddPhysicalTorque")))
	{
		return;
	}

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

void URagdollComponent::SetPhysicalLean(FRotator Lean, FName BoneName)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetPhysicalLean);

	if (!IsRagdollRunnable() || CurrentState != ERagdollState::Physical || !Mesh || !PhysicsControl)
	{
		return;
	}

	const bool bJointDriven = DrivenJoints.Contains(BoneName);

	// Nothing to offset unless the active profile drives this bone
	if (BoneName == NAME_None || (!bJointDriven && !PhysicsControl->GetControlExists(MakeOperatorName(BoneName))))
	{
		return;
	}

	LastBiasFrame = GFrameCounter;
	PendingLeanBones.Add(BoneName);

	const FQuat World = Lean.Quaternion();

	// Only when the lean actually moves, so holding one steady lets the bodies settle
	const FQuat* Held = HeldLeans.Find(BoneName);
	const bool bLeanChanged = !Held || !Held->Equals(World, 1.e-4f);
	HeldLeans.Add(BoneName, World);

	// A driven joint is offset from the tick, which is ordered after the animation writes its target
	if (!bJointDriven)
	{
		// A control target lives in the bone's own frame, so the world lean is conjugated into it
		const FQuat BoneWorld = Mesh->GetBoneQuaternion(BoneName, EBoneSpaces::WorldSpace);
		PhysicsControl->SetControlTargetOrientation(MakeOperatorName(BoneName),
			(BoneWorld.Inverse() * World * BoneWorld).Rotator(), 0.f, true, false, true, false);
	}

	if (bLeanChanged)
	{
		WakeBodiesBelow(BoneName, true);
	}
}

void URagdollComponent::EnforceBodySafety()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::EnforceBodySafety);

	if (!Mesh || BoneWeights.IsEmpty())
	{
		return;
	}

	const TArray<FTransform>& Animated = Mesh->GetEditableComponentSpaceTransforms();
	const FTransform& ComponentTM = Mesh->GetComponentTransform();
	const float SeparationSq = FMath::Square(MaxBodySeparation);

	for (const TPair<FName, float>& Pair : BoneWeights)
	{
		FBodyInstance* Body = Mesh->GetBodyInstance(Pair.Key);
		if (!Body || !Body->IsInstanceSimulatingPhysics())
		{
			continue;
		}

		const int32 BoneIndex = Mesh->GetBoneIndex(Pair.Key);
		if (!Animated.IsValidIndex(BoneIndex))
		{
			continue;
		}

		const FTransform Current = Body->GetUnrealWorldTransform_AssumesLocked();
		const FTransform Target = Animated[BoneIndex] * ComponentTM;

		// NaN first: once a body carries one, every distance against it compares false and it is never caught
		const bool bThrown = Current.ContainsNaN()
			|| (MaxBodySeparation > 0.f && FVector::DistSquared(Current.GetLocation(), Target.GetLocation()) > SeparationSq);

		if (bThrown)
		{
			Body->SetBodyTransform(Target, ETeleportType::ResetPhysics);
			Body->SetLinearVelocity(FVector::ZeroVector, false);
			Body->SetAngularVelocityInRadians(FVector::ZeroVector, false);
			continue;
		}

		if (MaxBodyAngularSpeed > 0.f)
		{
			const FVector Spin = Body->GetUnrealWorldAngularVelocityInRadians();
			const float MaxSpin = FMath::DegreesToRadians(MaxBodyAngularSpeed);
			if (Spin.SizeSquared() > FMath::Square(MaxSpin))
			{
				Body->SetAngularVelocityInRadians(Spin.GetSafeNormal() * MaxSpin, false);
			}
		}
	}
}

void URagdollComponent::ApplyHeldLeansToJoints()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ApplyHeldLeansToJoints);

	if (HeldLeans.IsEmpty() || !Mesh)
	{
		return;
	}

	for (const TPair<FName, FQuat>& Pair : HeldLeans)
	{
		if (!DrivenJoints.Contains(Pair.Key))
		{
			continue;
		}

		const int32 Index = Mesh->FindConstraintIndex(Pair.Key);
		FConstraintInstance* Constraint = Index != INDEX_NONE ? Mesh->GetConstraintInstanceByIndex(static_cast<uint32>(Index)) : nullptr;
		if (!Constraint)
		{
			continue;
		}

		/**
		 * The engine writes this target in the joint's own reference frame, measured against the parent
		 * body, so a world-space lean only lands where it was asked for once it has been carried through
		 * both: first into the parent bone it rotates against, then into the joint frame itself.
		 */
		const FQuat ParentWorld = Mesh->GetBoneQuaternion(Constraint->ConstraintBone2, EBoneSpaces::WorldSpace);
		const FQuat JointFrame = Constraint->GetRefFrame(EConstraintFrame::Frame2).GetRotation();
		const FQuat Offset = JointFrame.Inverse() * (ParentWorld.Inverse() * Pair.Value * ParentWorld) * JointFrame;

		const FQuat Animated = Constraint->ProfileInstance.AngularDrive.OrientationTarget.Quaternion();
		Constraint->SetAngularOrientationTarget(Offset * Animated);
	}
}

void URagdollComponent::ClearPhysicalLean()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ClearPhysicalLean);

	if (PhysicsControl)
	{
		for (const FName& BoneName : LeanedBones)
		{
			if (!DrivenJoints.Contains(BoneName))
			{
				PhysicsControl->SetControlTargetOrientation(MakeOperatorName(BoneName), FRotator::ZeroRotator, 0.f,
					true, false, true, false);
			}
		}
	}

	LeanedBones.Reset();
	PendingLeanBones.Reset();
	HeldLeans.Reset();
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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TickPhysical);

	if (!HasValidPhysics())
	{
		TeardownPhysical();
		SetState(ERagdollState::None);
		return;
	}

#if WITH_EDITOR
	// Tuning an asset mid-play is the point of holding the profile in one, and the drives only take an
	// edit by being built again, so it happens before anything this frame reads them
	RefreshProfileFromAsset();
#endif

	RefreshBoneOverrides();

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

		// Bones a new profile dropped keep a modifier so their weight can still fade to zero
		EnsureBodyModifierForBone(Pair.Key);
		ApplyBoneWeight(Pair.Key, Current);
	}

	UpdateJointDriveVelocities(DeltaTime);
	ApplyHeldLeansToJoints();
	EnforceBodySafety();

	URagdollStatics::FinalizeMeshPhysics(Mesh);

#if !UE_BUILD_SHIPPING
	if (const float TestBias = Ragdoll::CVarRagdollTestBias.GetValueOnGameThread(); !FMath::IsNearlyZero(TestBias))
	{
		const FName TestBone = ActiveProfile.BoneGroups.Num() > 0 ?
			ActiveProfile.BoneGroups[0].RootBone : NAME_None;
		AddPhysicalBias(GetOwner()->GetActorForwardVector() * TestBias, TestBone);
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
			DestroyDriveForBone(It->Key);
			It.RemoveCurrent();
		}
	}

	for (const FName& BoneName : LeanedBones)
	{
		// A driven joint's target is rewritten from the animated pose every frame, so it clears itself
		if (!PendingLeanBones.Contains(BoneName) && !DrivenJoints.Contains(BoneName))
		{
			PhysicsControl->SetControlTargetOrientation(MakeOperatorName(BoneName), FRotator::ZeroRotator, 0.f,
				true, false, true, false);
		}
	}
	for (auto It = HeldLeans.CreateIterator(); It; ++It)
	{
		if (!PendingLeanBones.Contains(It->Key))
		{
			It.RemoveCurrent();
		}
	}

	LeanedBones = MoveTemp(PendingLeanBones);
	PendingLeanBones.Reset();

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
	ApplyStrengthMultiplier(CurrentStrength);
	ApplyJointDriveStrength(CurrentStrength);

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
#if !UE_BUILD_SHIPPING
		bLoggedConvergedBodies = false;
#endif
	}
}

void URagdollComponent::ApplyPendingPhysicalProfile()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ApplyPendingPhysicalProfile);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetupRagdoll);

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

	// Ragdoll takes the whole subtree, so the physical layer's per-bone drives are replaced outright
	DestroyAllDrives();

	URagdollStatics::ForEach(Mesh, RagdollSettings.SimulationRootBone, RagdollSettings.bIncludeSimulationRoot,
		[this](const FBodyInstance* BI)
	{
		CreateDriveForBone(URagdollStatics::GetBoneName(Mesh, BI), RagdollSettings.ControlData,
			RagdollSettings.ControlType, RagdollSettings.ControlDataConstraintProfile);
		return true;
	});

	ApplyConstraintProfile(RagdollSettings.ConstraintProfile);

	Mesh->SetAllBodiesBelowSimulatePhysics(
		RagdollSettings.SimulationRootBone, true,
		RagdollSettings.bIncludeSimulationRoot);

	if (!PhysicsControl->GetBodyModifierNamesInSet(GRagdollOperatorSet).IsEmpty())
	{
		PhysicsControl->SetBodyModifiersInSetMovementType(GRagdollOperatorSet, EPhysicsMovementType::Simulated);
	}

	RagdollMotorStrength = 1.f;
	ApplyStrengthMultiplier(RagdollMotorStrength);
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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TeardownRagdoll);

	// Bone positions are still valid, so place the capsule before the simulation stops
	UpdateCapsuleToFollowMesh();

	if (Mesh)
	{
		Mesh->SetAllBodiesBelowSimulatePhysics(RagdollSettings.SimulationRootBone, false, RagdollSettings.bIncludeSimulationRoot);
		Mesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollSettings.SimulationRootBone, 0.f, false, RagdollSettings.bIncludeSimulationRoot);
	}

	DestroyAllDrives();

	SnapMeshToCapsule();

	RagdollWeight = 0.f;
	RagdollMotorStrength = 1.f;
	bRagdollBlendComplete = false;
}

void URagdollComponent::TickRagdoll(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TickRagdoll);

	if (!HasValidPhysics())
	{
		StopRagdoll();
		return;
	}

	RagdollWeight = RagdollSettings.BlendIn.Interp(RagdollWeight, 1.f, DeltaTime);
	Mesh->SetAllBodiesBelowPhysicsBlendWeight(
		RagdollSettings.SimulationRootBone, RagdollWeight,
		false, RagdollSettings.bIncludeSimulationRoot);
	if (!PhysicsControl->GetBodyModifierNamesInSet(GRagdollOperatorSet).IsEmpty())
	{
		PhysicsControl->SetBodyModifiersInSetPhysicsBlendWeight(GRagdollOperatorSet, RagdollWeight);
	}

	RagdollMotorStrength = RagdollSettings.MotorDecay.Interp(RagdollMotorStrength, 0.f, DeltaTime);
	ApplyStrengthMultiplier(RagdollMotorStrength);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SetupRecovery);

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

	DestroyAllDrives();

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TeardownRecovery);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::TickRecovery);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::FinishRecovery);

	FinalizeRecoveryTransform();
	RecoveryAlpha = 0.f;
	SetState(ERagdollState::None);
	ApplyPendingPhysicalProfile();
}

void URagdollComponent::FinalizeRecoveryTransform() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::FinalizeRecoveryTransform);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::UpdateCapsuleToFollowMesh);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::SnapMeshToCapsule);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ApplySimulationCollisionProfile);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::RestoreCollisionProfile);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::ApplyConstraintProfile);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::RestoreConstraintProfile);

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

void URagdollComponent::EnsurePhysicsCollision()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::EnsurePhysicsCollision);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::RestoreCollisionEnabled);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollComponent::WarnIfGroupUnanchored);

	if (!Group.bIncludeRootBone || !Mesh)
	{
		return;
	}

	const bool bParentSpace = Group.ControlType == EPhysicsControlType::ParentSpace
		|| !Group.ControlDataConstraintProfile.IsNone();
	if (!bParentSpace)
	{
		return;
	}

	const USkeletalMesh* SkelMesh = Mesh->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return;
	}

	if (SkelMesh->GetRefSkeleton().FindBoneIndex(Group.RootBone) == INDEX_NONE)
	{
		UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s physical profile references bone '%s', which is not in %s"),
			*GetNameSafe(GetOwner()), *Group.RootBone.ToString(), *GetNameSafe(SkelMesh));
		return;
	}

	if (!UE::PhysicsControl::GetPhysicalParentBone(Mesh, Group.RootBone).IsNone())
	{
		return;
	}

	// A parent space control needs a parent body to drive against, so the group silently falls back to world
	UE_LOG(LogPhysicalRagdoll, Warning, TEXT("%s physical profile drives '%s' in parent space, but no ancestor bone has a body in %s, so it falls back to world space. Start the group below the root body, or set ControlType to WorldSpace."),
		*GetNameSafe(GetOwner()), *Group.RootBone.ToString(), *GetNameSafe(Mesh->GetPhysicsAsset()));
}

void URagdollComponent::Wake()
{
	PrimaryComponentTick.SetTickFunctionEnable(true);
}

void URagdollComponent::Sleep()
{
	PrimaryComponentTick.SetTickFunctionEnable(false);
}
