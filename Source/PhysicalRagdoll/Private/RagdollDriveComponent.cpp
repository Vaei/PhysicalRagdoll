// Copyright (c) Jared Taylor

#include "RagdollDriveComponent.h"

#include "PhysicalRagdollTags.h"
#include "RagdollAssets.h"
#include "RagdollStatics.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#if !UE_BUILD_SHIPPING
#include "HAL/IConsoleManager.h"

namespace PhysicalRagdoll
{
	static TAutoConsoleVariable<bool> CVarDebugBaseDrive(
		TEXT("p.Ragdoll.DebugBase"),
		false,
		TEXT("Draw what the base drive is reading off the surface the character is riding, and what it makes of it"),
		ECVF_Cheat);
}
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(RagdollDriveComponent)

URagdollDriveComponent::URagdollDriveComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void URagdollDriveComponent::TickDrives(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::TickDrives);

	ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!Character || !IsRagdollRunnable())
	{
		return;
	}

	const bool bMotionDrive = IsMotionDriveEnabled();
	const bool bBaseDriveEnabled = IsBaseDriveEnabled();
	const bool bImpulse = HasActiveDriveImpulse();

	// Nothing runs for this owner, which is every simulated proxy under a local-only mode, so it must cost
	// nothing beyond this
	if (!bMotionDrive && !bBaseDriveEnabled && !bImpulse)
	{
		if (bDrivesActive)
		{
			bDrivesActive = false;
			if (IsDriveProfile(GetPhysicalProfile()))
			{
				ClearPhysicalProfile();
			}
			ResetDrives();
		}
		return;
	}

	bDrivesActive = true;

	FVector BaseBias = FVector::ZeroVector;
	FVector BaseTorque = FVector::ZeroVector;
	bool bBaseDrive = false;

	const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	const bool bHasInput = Movement && Movement->GetCurrentAcceleration().Size2D() > 1.f;

	/**
	 * Input snaps the layer over to the motion drive and only gives it back slowly, since a body moving
	 * under its own power has no attention left for the deck, and the deck's response has to arrive as it
	 * settles rather than the moment the stick is released.
	 */
	BaseDriveInputAlpha = bHasInput
		? 1.f
		: FMath::FInterpTo(BaseDriveInputAlpha, 0.f, DeltaTime, GetBaseDriveParams().InputReleaseRate);

	if (bBaseDriveEnabled)
	{
		UpdateBaseMotion(DeltaTime);
		UpdateBaseDrive(DeltaTime, 1.f - BaseDriveInputAlpha, BaseBias, BaseTorque);

		// Anything carrying the owner holds the layer, whether or not it is moving right now: the motors
		// have to already be holding the pose when the deck starts to roll
		bBaseDrive = BaseMotionState.ResolvedBase.IsValid() && !bHasInput;
	}

	if (bImpulse)
	{
		UpdateDriveImpulse(DeltaTime);
	}

	// A kick has to land on something, so it borrows the motion drive's profile when neither drive owns one
	FGameplayTag DriveProfile;
	if (bBaseDrive && BaseDriveProfile.IsValid())
	{
		DriveProfile = BaseDriveProfile;
	}
	else if ((bMotionDrive || bImpulse) && MotionDriveProfile.IsValid())
	{
		DriveProfile = MotionDriveProfile;
	}

	const FGameplayTag RunningProfile = GetPhysicalProfile();

	if (!DriveProfile.IsValid())
	{
		if (IsDriveProfile(RunningProfile))
		{
			ClearPhysicalProfile();
		}
		return;
	}

	// Lowest priority: the layer is only ever taken from nothing, or from the other drive
	if (RunningProfile != DriveProfile)
	{
		if (RunningProfile.IsValid() && !IsDriveProfile(RunningProfile))
		{
			return;
		}

		SetPhysicalProfile(DriveProfile);
		if (GetPhysicalProfile() != DriveProfile)
		{
			return;
		}
	}

	float Strength = 1.f;

	// Negative gives the blend rate back to the profile, which is what a drive with nothing to say wants
	float BlendRate = -1.f;

	FVector Bias = ImpulseState.Bias;
	FVector Torque = ImpulseState.Torque;

	const FRagdollMotionDrive& Motion = GetMotionDriveParams();

	if (bMotionDrive)
	{
		float MotionAlpha, MotionStrength;
		FVector MotionBias, PushBias, TurnBias;
		URagdollStatics::CalculateMotionDriveForCharacter(Motion, MotionDriveState, Character, DeltaTime,
			MotionAlpha, MotionStrength, MotionBias, PushBias, TurnBias, BlendRate);

		// A body with no input behind it is coming to a stop, and wants holding rather than swinging on
		Strength = MotionStrength * (bHasInput ? 1.f : Motion.BrakingStrengthScale);
		BlendRate *= bHasInput ? 1.f : Motion.BrakingRateScale;

		// The alpha scales the lean, not the layer: SetPhysicalAlpha would thin the blend weight while
		// leaving the push at full size, which is a different effect entirely
		Bias += MotionBias * MotionAlpha;
	}

	SetPhysicalStrength(Strength);
	SetPhysicalBlendRate(BlendRate);

	// Unconditional: this is what holds the layer awake, and a layer that sleeps mid-drive stops the
	// per-frame maintenance the force path depends on
	AddPhysicalBias(Bias, Motion.BiasBone, Motion.BiasHeightOffset);
	AddPhysicalTorque(Torque, Motion.BiasBone);

	if (bBaseDrive)
	{
		const FRagdollBaseDrive& Base = GetBaseDriveParams();

		AddPhysicalBias(BaseBias, Base.BiasBone, Base.BiasHeightOffset);
		AddPhysicalTorque(BaseTorque, Base.BiasBone);
		SetPhysicalLean(BaseDriveState.Lean, Base.BiasBone);
	}
}

UPrimitiveComponent* URagdollDriveComponent::GetDriveBase() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character ? Cast<UPrimitiveComponent>(Character->GetMovementBaseObject()) : nullptr;
}

const FRagdollMotionDrive& URagdollDriveComponent::GetMotionDriveParams() const
{
	return MotionDriveSource == ERagdollTuningSource::Asset && MotionDriveAsset
		? MotionDriveAsset->Params
		: MotionDriveParams;
}

const FRagdollBaseMotion& URagdollDriveComponent::GetBaseMotionParams() const
{
	return BaseDriveSource == ERagdollTuningSource::Asset && BaseDriveAsset
		? BaseDriveAsset->Motion
		: BaseMotionParams;
}

const FRagdollBaseDrive& URagdollDriveComponent::GetBaseDriveParams() const
{
	return BaseDriveSource == ERagdollTuningSource::Asset && BaseDriveAsset
		? BaseDriveAsset->Params
		: BaseDriveParams;
}

void URagdollDriveComponent::UpdateBaseMotion(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::UpdateBaseMotion);

	const FRagdollBaseMotion& Motion = GetBaseMotionParams();

	UPrimitiveComponent* Base = GetDriveBase();
	BaseMotionState.ResolvedBase = Base;

	if (DeltaTime <= 0.f)
	{
		return;
	}

	const float BlendAlpha = Motion.InterpRate > 0.f ?
		1.f - FMath::Exp(-Motion.InterpRate * DeltaTime) : 1.f;

	const float AccelerationBlendAlpha = Motion.AccelerationInterpRate > 0.f ?
		1.f - FMath::Exp(-Motion.AccelerationInterpRate * DeltaTime) : 1.f;

	if (!Base)
	{
		// Eased to a stop rather than dropped, so stepping off a moving deck does not pop the layer
		BaseMotionState.LinearVelocity = FMath::Lerp(BaseMotionState.LinearVelocity, FVector::ZeroVector, BlendAlpha);
		BaseMotionState.AngularVelocity = FMath::Lerp(BaseMotionState.AngularVelocity, FVector::ZeroVector, BlendAlpha);
		BaseMotionState.PointAcceleration = FMath::Lerp(BaseMotionState.PointAcceleration, FVector::ZeroVector, AccelerationBlendAlpha);
		BaseMotionState.TiltAcceleration = FMath::Lerp(BaseMotionState.TiltAcceleration, FVector::ZeroVector, BlendAlpha);
		BaseMotionState.TiltAngle = 0.f;
		BaseMotionState.ClearHistory();
		return;
	}

	const float GravityZ = GetWorld() ? GetWorld()->GetGravityZ() : -980.f;
	BaseMotionState.TiltAcceleration = URagdollStatics::CalculateBaseTilt(Base, FVector::DownVector,
		FMath::Abs(GravityZ), GetBaseDriveParams().MaxTiltAngle, BaseMotionState.TiltAngle);

	const FTransform Current = Base->GetComponentTransform();

	if (!BaseMotionState.bHasPreviousTransform)
	{
		BaseMotionState.PreviousTransform = Current;
		BaseMotionState.bHasPreviousTransform = true;
		return;
	}

	const FVector Translation = Current.GetLocation() - BaseMotionState.PreviousTransform.GetLocation();

	// A jump this large is the base being placed somewhere, not travelling there, and differencing it
	// would report a velocity nothing standing on it could survive
	if (Translation.SizeSquared() > FMath::Square(Motion.TeleportDistance))
	{
		BaseMotionState.PreviousTransform = Current;
		return;
	}

	const FQuat DeltaRotation =
		(Current.GetRotation() * BaseMotionState.PreviousTransform.GetRotation().Inverse()).GetNormalized();

	FVector Axis;
	float Angle;
	DeltaRotation.ToAxisAndAngle(Axis, Angle);
	Angle = FMath::UnwindRadians(Angle);

	const FVector TargetLinear = (Translation / DeltaTime).GetClampedToMaxSize(Motion.MaxLinearSpeed);
	const FVector TargetAngular = (Axis * (Angle / DeltaTime)).GetClampedToMaxSize(Motion.MaxAngularSpeed);

	BaseMotionState.LinearVelocity = FMath::Lerp(BaseMotionState.LinearVelocity, TargetLinear, BlendAlpha);
	BaseMotionState.AngularVelocity = FMath::Lerp(BaseMotionState.AngularVelocity, TargetAngular, BlendAlpha);
	BaseMotionState.PreviousTransform = Current;

	// Measured where the character is, not at the base's origin: a deck's far end moves a great deal more
	// than its centre when it rolls
	FVector NewPointVelocity = BaseMotionState.LinearVelocity;
	if (const AActor* Owner = GetOwner())
	{
		const FVector Offset = Owner->GetActorLocation() - Current.GetLocation();
		NewPointVelocity += FVector::CrossProduct(BaseMotionState.AngularVelocity, Offset);
	}

	if (BaseMotionState.bHasPreviousPointVelocity)
	{
		const FVector RawAcceleration = (NewPointVelocity - BaseMotionState.PointVelocity) / DeltaTime;
		BaseMotionState.PointAcceleration = FMath::Lerp(BaseMotionState.PointAcceleration,
			RawAcceleration.GetClampedToMaxSize(Motion.MaxLinearAcceleration), AccelerationBlendAlpha);
	}

	BaseMotionState.PointVelocity = NewPointVelocity;
	BaseMotionState.bHasPreviousPointVelocity = true;
}

void URagdollDriveComponent::UpdateBaseDrive(float DeltaTime, float Alpha, FVector& OutBias, FVector& OutTorque)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::UpdateBaseDrive);

	OutBias = FVector::ZeroVector;
	OutTorque = FVector::ZeroVector;

	const FRagdollBaseDrive& Params = GetBaseDriveParams();

	if (DeltaTime <= 0.f)
	{
		return;
	}

	// A base barely off level should barely reach the body, and this is the whole result rather than a term
	if (Params.TiltRange > 0.f)
	{
		Alpha *= FMath::Clamp(BaseMotionState.TiltAngle / Params.TiltRange, 0.f, 1.f);
	}

	FVector TargetBias = FVector::ZeroVector;
	FVector TargetTorque = FVector::ZeroVector;

	if (BaseMotionState.ResolvedBase.IsValid())
	{
		FVector Acceleration = BaseMotionState.PointAcceleration;
		if (Params.bIgnoreVerticalTranslation)
		{
			Acceleration.Z = 0.f;
		}

		// Negated: the body is left behind by whatever changes under it, and leans up the slope rather than
		// down it, which between them are the whole reaction
		TargetBias = (-Acceleration * Params.TranslationScale
			- BaseMotionState.TiltAcceleration * Params.TiltScale)
			.GetClampedToMaxSize(Params.MaxBias);
		TargetTorque = (-BaseMotionState.AngularVelocity * Params.RotationScale)
			.GetClampedToMaxSize(Params.MaxTorque);

		TargetBias *= Alpha;
		TargetTorque *= Alpha;
	}

	const FRotator TargetLean = URagdollStatics::CalculateBaseLean(Params, BaseDriveState, DeltaTime,
		BaseMotionState.TiltAcceleration, BaseMotionState.TiltAngle,
		BaseMotionState.ResolvedBase.IsValid() ? BaseMotionState.AngularVelocity : FVector::ZeroVector,
		FVector::DownVector) * Alpha;

	if (Params.InterpRate > 0.f)
	{
		const float BlendAlpha = 1.f - FMath::Exp(-Params.InterpRate * DeltaTime);
		BaseDriveState.Bias = FMath::Lerp(BaseDriveState.Bias, TargetBias, BlendAlpha);
		BaseDriveState.Torque = FMath::Lerp(BaseDriveState.Torque, TargetTorque, BlendAlpha);
	}
	else
	{
		BaseDriveState.Bias = TargetBias;
		BaseDriveState.Torque = TargetTorque;
	}

	if (Params.LeanInterpRate > 0.f)
	{
		const float LeanAlpha = 1.f - FMath::Exp(-Params.LeanInterpRate * DeltaTime);
		BaseDriveState.Lean = FQuat::Slerp(BaseDriveState.Lean.Quaternion(), TargetLean.Quaternion(), LeanAlpha)
			.Rotator();
	}
	else
	{
		BaseDriveState.Lean = TargetLean;
	}

	BaseDriveState.ResolvedBase = BaseMotionState.ResolvedBase;
	BaseDriveState.TiltAcceleration = BaseMotionState.TiltAcceleration;
	BaseDriveState.TiltAngle = BaseMotionState.TiltAngle;

	OutBias = BaseDriveState.Bias;
	OutTorque = BaseDriveState.Torque;

#if !UE_BUILD_SHIPPING
	if (PhysicalRagdoll::CVarDebugBaseDrive.GetValueOnGameThread())
	{
		DrawBaseDriveDebug();
	}
#endif
}

void URagdollDriveComponent::DrawBaseDriveDebug() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::DrawBaseDriveDebug);

	if (const AActor* Owner = GetOwner())
	{
		URagdollStatics::DrawBaseDriveDebug(this, Owner->GetActorLocation() + FVector(0.f, 0.f, 100.f),
			BaseDriveState, BaseMotionState.PointAcceleration, BaseMotionState.AngularVelocity);
	}
}

void URagdollDriveComponent::AddDriveImpulse(FVector Bias, FVector Torque, float HalfLife)
{
	ImpulseState.Bias = (ImpulseState.Bias + Bias).GetClampedToMaxSize(MaxImpulseBias);
	ImpulseState.Torque = (ImpulseState.Torque + Torque).GetClampedToMaxSize(MaxImpulseTorque);
	ImpulseState.HalfLife = FMath::Max(ImpulseState.HalfLife, HalfLife);
}

void URagdollDriveComponent::UpdateDriveImpulse(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::UpdateDriveImpulse);

	if (DeltaTime <= 0.f || !ImpulseState.IsActive())
	{
		return;
	}

	const float Decay = ImpulseState.HalfLife > 0.f ? FMath::Pow(0.5f, DeltaTime / ImpulseState.HalfLife) : 0.f;
	ImpulseState.Bias *= Decay;
	ImpulseState.Torque *= Decay;

	if (!ImpulseState.IsActive())
	{
		ImpulseState.Reset();
	}
}

void URagdollDriveComponent::ResetDrives()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::ResetDrives);

	ClearPhysicalLean();

	BaseMotionState = FRagdollBaseMotionState();
	BaseDriveState = FRagdollBaseDriveState();
	MotionDriveState = FRagdollMotionDriveState();
	ImpulseState.Reset();
}

bool URagdollDriveComponent::IsDriveProfile(FGameplayTag ProfileTag) const
{
	return ProfileTag.IsValid() && (ProfileTag == MotionDriveProfile || ProfileTag == BaseDriveProfile);
}

void URagdollDriveComponent::AddDefaultDriveProfile(const FGameplayTag& ProfileTag)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollDriveComponent::AddDefaultDriveProfile);

	if (!ProfileTag.IsValid() || PhysicalProfiles.Contains(ProfileTag))
	{
		return;
	}

	if (const FRagdollPhysicalProfile* Template = PhysicalProfiles.Find(FPhysicalRagdollTags::Ragdoll_Profile))
	{
		// By value: the add below can rehash the map out from under the pointer
		const FRagdollPhysicalProfile Copy = *Template;
		PhysicalProfiles.Add(ProfileTag, Copy);
	}
}
