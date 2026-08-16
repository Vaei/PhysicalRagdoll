// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"

#include "RagdollTypes.generated.h"

class UAnimMontage;
class UCurveFloat;
class UPrimitiveComponent;

UENUM(BlueprintType)
enum class ERagdollState : uint8
{
	None,
	Physical	UMETA(ToolTip="Persistent physics layer blended over regular animation"),
	Ragdoll,
	Recovery
};

UENUM(BlueprintType)
enum class ERagdollDeltaSpace : uint8
{
	/** Relative to the mesh, so only what the animation does to the bone counts */
	Local,

	/** Absolute, so the actor's own movement counts toward the delta as well */
	World
};

/** How hard to take the physical layer away, chosen by how much the gameplay reason can tolerate a blend */
UENUM(BlueprintType)
enum class ERagdollSuspendUrgency : uint8
{
	/** Blend out at the profile's normal rate. The default, and what most gameplay wants. */
	Blend,

	/** Blend out fast enough to be gone within a few frames, without popping */
	Fast,

	/** Off this frame. Pops, so keep it for cases where a visible pop beats a visible blend. */
	Immediate
};

UENUM(BlueprintType)
enum class ERagdollRecoverySide : uint8
{
	Front,		// Face-down (prone) get-up
	Back,		// Face-up (supine) get-up
	Left,		// Left side get-up
	Right		// Right side get-up
};

/**
 * Interpolation parameters for a scalar blend
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollInterp
{
	GENERATED_BODY()

	FRagdollInterp(float InRate = 5.f, bool bInConstantRate = false)
		: Rate(InRate)
		, bConstantRate(bInConstantRate)
	{}

	/** Rate of interpolation. 0 or less is instant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Interp, meta=(UIMin="0", ClampMin="0"))
	float Rate;

	/** Interpolate at a fixed rate per second instead of easing out as it approaches the target */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Interp)
	bool bConstantRate;

	float Interp(float Current, float Target, float DeltaTime) const
	{
		if (Rate <= 0.f)
		{
			return Target;
		}
		return bConstantRate
			? FMath::FInterpConstantTo(Current, Target, DeltaTime, Rate)
			: FMath::FInterpTo(Current, Target, DeltaTime, Rate);
	}
};

/**
 * A set of bodies driven together by a single motor configuration.
 * Contains the root bone and every body below it in the physics asset.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBoneGroup
{
	GENERATED_BODY()

	FRagdollBoneGroup()
	{
		PhysicalAnimData.bIsLocalSimulation = true;
		PhysicalAnimData.OrientationStrength = 400.f;
		PhysicalAnimData.AngularVelocityStrength = 40.f;
	}
	
	FRagdollBoneGroup(FName InRootBone, bool bInIncludeRootBone = true, float InBlendWeight = 1.f, const FPhysicalAnimationData& InPhysicalAnimData = {})
		: RootBone(InRootBone)
		, bIncludeRootBone(bInIncludeRootBone)
		, BlendWeight(InBlendWeight)
		, PhysicalAnimData(InPhysicalAnimData)
	{}

	/** Bone at which this group starts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	FName RootBone = "spine_01";

	/** Whether RootBone itself belongs to the group */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bIncludeRootBone = true;

	/** How much physics overrides animation. 0 = pure animation, 1 = pure physics. Tune feel with the motor strengths, not this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="1", ClampMin="0", ClampMax="1"))
	float BlendWeight = 1.f;

	/** Motor drive settings holding the group toward its animated pose */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	FPhysicalAnimationData PhysicalAnimData;
};

/**
 * Per-bone adjustment applied to a bone and, unless bIncludeSelf is cleared, everything below it.
 * This is how a group drives the torso without taking the arms with it.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBoneOverride
{
	GENERATED_BODY()

	/** Bone this override starts at */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	FName BoneName;

	/** If false, the bone itself is untouched and the override applies only below it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bIncludeSelf = true;

	/** Keep these bones off physics entirely, so they follow animation instead of inheriting the group */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bDisablePhysics = false;

	/** Scales the weight these bones receive from the group */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="1", ClampMin="0", ClampMax="1", EditCondition="!bDisablePhysics", EditConditionHides))
	float BlendWeightScalar = 1.f;
};

/**
 * A named level of physicality, e.g. a subtle overlap on the spine and arms, or a full body flail.
 * Applied on top of regular animation and held indefinitely.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollPhysicalProfile
{
	GENERATED_BODY()

	/** Bodies driven by this profile. Where groups overlap, later entries win. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(TitleProperty="RootBone"))
	TArray<FRagdollBoneGroup> BoneGroups;

	/**
	 * Per-bone adjustments layered on top of the groups, each covering the bone and everything below it.
	 * Order is not significant: where entries overlap, the most restrictive one wins.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(TitleProperty="BoneName"))
	TArray<FRagdollBoneOverride> BoneOverrides;

	/** Scales every motor in the profile */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", ClampMin="0"))
	float StrengthMultiplier = 1.f;

	/**
	 * How fast bones move toward their target weight, per second.
	 * Frame rate independent. Above ~12 the transition reads as a snap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", ClampMin="0", UIMax="12"))
	float BoneBlendRate = 7.f;
};

/**
 * Drives the physical layer from the owner's movement so the physicality reads as a body reacting to
 * its own momentum rather than a constant wobble.
 *
 * The character's acceleration is split relative to its current velocity and each part is scaled on its
 * own, because they read as different things on a body: pushing off, planting to stop, and cutting across
 * your own momentum. The body leans into each of them, the way a person does when they are the one
 * generating the movement. The result is applied as a mass-normalised acceleration bias, so every body
 * leans by the same amount regardless of how heavy it is - the thing that keeps this from looking floppy.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollMotionDrive
{
	GENERATED_BODY()

	/** Maps speed, normalized against the max speed passed in, to a scale on the whole layer. Falls back to SpeedRange/AlphaRange when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Speed")
	TObjectPtr<UCurveFloat> SpeedCurve = nullptr;

	/** Speed range mapped to AlphaRange when no SpeedCurve is set */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Speed", meta=(EditCondition="SpeedCurve == nullptr", ForceUnits="cm/s"))
	FVector2D SpeedRange = FVector2D(0.f, 600.f);

	/** Layer scale at each end of SpeedRange */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Speed", meta=(EditCondition="SpeedCurve == nullptr"))
	FVector2D AlphaRange = FVector2D(0.35f, 1.f);

	/**
	 * Lean while speeding up, along the direction of travel.
	 *
	 * The simulated bodies hang off a kinematic pelvis that is accelerating out from under them, so they
	 * already trail by the full acceleration on their own. 1 cancels that and leaves the body upright.
	 * Only above 1 does it lean into the motion; below 1 it still trails, just less than it would have.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="3"))
	float AccelerationScale = 2.5f;

	/** Lean while slowing down. Usually higher than AccelerationScale, since stopping is the sharper event. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="3"))
	float BrakingScale = 0.5f;

	/** Flips which side of the arc the turn lean falls on */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias")
	bool bInvertTurn = false;

	/**
	 * Lean into a turn, scaling the centripetal acceleration the turn actually implies (speed x yaw rate).
	 * Has to beat the body's own rotational trailing before any lean shows, so it wants to be generous.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="3"))
	float TurnScale = 1.0f;

	/**
	 * Scale applied when moving straight backward, reaching 1 when moving straight ahead.
	 * Backpedalling is a smaller, more careful movement than running, and carrying the same physicality
	 * into it reads as the character being thrown around by a walk.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Direction", meta=(UIMin="0", UIMax="1"))
	float BackwardScale = 0.3f;

	/** Maps forwardness, remapped from -1..1 to 0..1, to the scale. Replaces BackwardScale when set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Direction")
	TObjectPtr<UCurveFloat> DirectionCurve = nullptr;

	/**
	 * How fast bone weights move while nothing is accelerating the character.
	 * Slower than the accelerating rate suits a settled body: it stops the layer twitching at every
	 * small change while the character is just standing or holding a speed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="20"))
	float SteadyBlendRate = 3.f;

	/** How fast bone weights move under full acceleration, where the layer needs to keep up */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="20"))
	float AcceleratingBlendRate = 10.f;

	/**
	 * Motor strength while travelling at a constant speed.
	 * At 1 the body actively holds its pose, which is what stops steady movement reading as a wobble.
	 * The blend weight is untouched, so the layer is still doing its job.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="2"))
	float SteadyStrength = 1.f;

	/**
	 * Motor strength under full acceleration.
	 * Below SteadyStrength the body gives while it is being pushed around, which is what lets the lean
	 * read at all. Too low and it goes floppy the moment you touch the stick.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="2"))
	float AcceleratingStrength = 0.45f;

	/**
	 * Ceiling on the bias, and the thing to check first if the lean never appears.
	 * It has to sit above the character's MaxAcceleration (2048 by default) with the scales applied,
	 * or it clamps away the part that cancels the natural trailing and the body only ever lags.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", ForceUnits="cm/s2"))
	float MaxBias = 4000.f;

	/** How fast the bias follows the movement. Low values trail the motion, high values snap to it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="30"))
	float BiasInterpRate = 12.f;

	/** Whether the layer keeps running while airborne */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling")
	bool bDriveWhileFalling = true;

	/** Maps vertical speed, normalized against ReferenceFallSpeed, to a scale on the layer while airborne */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling", meta=(EditCondition="bDriveWhileFalling"))
	TObjectPtr<UCurveFloat> FallCurve = nullptr;

	/** Vertical speed that maps to 1 on the fall curve's input */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling", meta=(UIMin="1", EditCondition="bDriveWhileFalling", ForceUnits="cm/s"))
	float ReferenceFallSpeed = 1200.f;

	/**
	 * Scales the horizontal bias while airborne.
	 * Off the ground there is nothing to push against, so the same lean that reads as effort on the
	 * ground reads as flailing in the air.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling", meta=(UIMin="0", UIMax="1", EditCondition="bDriveWhileFalling"))
	float FallBiasScale = 0.3f;
};

/**
 * Shapes the physical layer from how hard a reference bone is being moved by the animation itself,
 * rather than from how the capsule is moving. Catches everything capsule velocity misses: an in-place
 * montage, a turn, a lunge, a hard footfall.
 *
 * The exponent is the point of it. Above 1, gentle animation contributes almost nothing and sharp
 * animation ramps hard, so the layer stays out of the way until the animation actually earns it.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBoneDeltaDrive
{
	GENERATED_BODY()

	/**
	 * Bone measured for movement.
	 * Must be one the active profile does not drive, or physics feeds back into its own input.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta)
	FName BoneName = "pelvis";

	/** Space the delta is measured in. Local isolates the animation; World folds in the actor's own movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta)
	ERagdollDeltaSpace Space = ERagdollDeltaSpace::Local;

	/**
	 * Vertical speed range of the bone mapped to 0-1 before the exponent.
	 * On a pelvis this is the walk cycle's bob, so each footfall registers as its own spike. It is
	 * usually the most useful signal here, and the reason vertical is separated from horizontal at all.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(ForceUnits="cm/s"))
	FVector2D VerticalRange = FVector2D(0.f, 60.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(UIMin="0", UIMax="1"))
	float VerticalWeight = 1.f;

	/** Count only downward motion, where the impact of a step lives, ignoring the rise back out of it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta)
	bool bVerticalDownOnly = false;

	/** Horizontal speed range of the bone mapped to 0-1 before the exponent */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(ForceUnits="cm/s"))
	FVector2D HorizontalRange = FVector2D(0.f, 120.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(UIMin="0", UIMax="1"))
	float HorizontalWeight = 0.5f;

	/** Angular speed range of the bone mapped to 0-1 before the exponent */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(ForceUnits="deg/s"))
	FVector2D AngularRange = FVector2D(0.f, 180.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(UIMin="0", UIMax="1"))
	float AngularWeight = 1.f;

	/** Applied to the normalized delta. 1 is linear, above 1 holds the layer back until the animation moves sharply. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(UIMin="0.1", UIMax="8", ClampMin="0.01"))
	float Exponent = 2.f;

	/** Overrides the exponent when set, taking the normalized delta and returning the scale directly */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta)
	TObjectPtr<UCurveFloat> DeltaCurve = nullptr;

	/** Scale at each end of the shaped delta */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta)
	FVector2D AlphaRange = FVector2D(0.f, 1.f);

	/**
	 * How fast the scale follows the delta.
	 * Bone deltas are spiky by nature, so this matters more here than elsewhere.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BoneDelta, meta=(UIMin="0", UIMax="30"))
	float InterpRate = 8.f;
};

/** Carried by the caller across frames. @see URagdollStatics::CalculateBoneDeltaDrive */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBoneDeltaDriveState
{
	GENERATED_BODY()

	/** Scale to feed to SetPhysicalAlpha, or to combine with another driver's alpha */
	UPROPERTY(BlueprintReadWrite, Category=BoneDelta)
	float Alpha = 0.f;

	/** Normalized delta before AlphaRange is applied, for tuning the ranges and exponent */
	UPROPERTY(BlueprintReadWrite, Category=BoneDelta)
	float NormalizedDelta = 0.f;

	/** Measured speeds this frame, for setting the ranges against real numbers rather than guesses */
	UPROPERTY(BlueprintReadOnly, Category="BoneDelta|Debug", meta=(ForceUnits="cm/s"))
	float DebugVerticalSpeed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="BoneDelta|Debug", meta=(ForceUnits="cm/s"))
	float DebugHorizontalSpeed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="BoneDelta|Debug", meta=(ForceUnits="deg/s"))
	float DebugAngularSpeed = 0.f;

	UPROPERTY(BlueprintReadWrite, Category=BoneDelta)
	FTransform PreviousTransform = FTransform::Identity;

	UPROPERTY(BlueprintReadWrite, Category=BoneDelta)
	bool bHasPreviousTransform = false;
};

/**
 * Everything the motion drive reads about the mover this frame. Runtime state, kept out of the tuning
 * parameters, and grouped so the calculation stays usable by anything that moves, not just ACharacter.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollMotionInput
{
	GENERATED_BODY()

	/** Current world velocity */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion)
	FVector Velocity = FVector::ZeroVector;

	/**
	 * Requested acceleration, e.g. UCharacterMovementComponent::GetCurrentAcceleration().
	 *
	 * Acceleration is always read from here and never derived from a change in velocity. Velocity lags
	 * the request, so deriving it leaves the push-off missing for the first several frames, which is
	 * precisely when it should read hardest.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion)
	FVector InputAcceleration = FVector::ZeroVector;

	/** Speed that maps to 1 on the speed curve, e.g. UCharacterMovementComponent::GetMaxSpeed() */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion, meta=(ForceUnits="cm/s"))
	float MaxSpeed = 0.f;

	/** Acceleration that counts as a full push, e.g. UCharacterMovementComponent::GetMaxAcceleration() */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion, meta=(ForceUnits="cm/s2"))
	float MaxAcceleration = 0.f;

	/**
	 * How much the movement is forward rather than backward, from 1 for straight ahead to -1 for straight
	 * back. What "forward" means depends on how the character is rotated, so MakeMotionInputFromCharacter
	 * resolves it from the rotation mode rather than assuming the actor's facing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion, meta=(UIMin="-1", UIMax="1"))
	float Forwardness = 1.f;

	/**
	 * Yaw the character is facing, from whichever rotation drives it.
	 *
	 * The turn lean comes from how fast this is changing, not from the input. When yaw follows the camera
	 * the character rotates under its own simulated bodies and they trail, and that trailing is itself an
	 * outward lean. Input cannot describe it, because once the yaw has caught up the input is pointing
	 * straight ahead again.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion, meta=(ForceUnits="deg"))
	float FacingYaw = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Motion)
	bool bIsFalling = false;
};

/**
 * Carried by whoever drives the motion, since the bias is smoothed across frames.
 * Kept separate from the parameters so the parameters can live in a data asset.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollMotionDriveState
{
	GENERATED_BODY()

	/** Scale to feed to SetPhysicalAlpha */
	UPROPERTY(BlueprintReadWrite, Category=Motion)
	float Alpha = 1.f;

	/** Scale to feed to SetPhysicalStrength */
	UPROPERTY(BlueprintReadWrite, Category=Motion)
	float Strength = 1.f;

	/** Rate to feed to SetPhysicalBlendRate */
	UPROPERTY(BlueprintReadWrite, Category=Motion)
	float BlendRate = 0.f;

	/** World-space acceleration bias to feed to AddPhysicalBias */
	UPROPERTY(BlueprintReadWrite, Category=Motion)
	FVector Bias = FVector::ZeroVector;

	/** The push and turn halves of Bias, kept apart so each can be applied, scaled or inverted on its own */
	UPROPERTY(BlueprintReadWrite, Category=Motion)
	FVector PushBias = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category=Motion)
	FVector TurnBias = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category=Motion)
	float PreviousYaw = 0.f;

	/**
	 * Yaw rate, smoothed. A raw frame-to-frame yaw delta is noisy enough to change sign on its own near
	 * zero, which puts the turn lean on whichever side the noise picked that frame.
	 */
	UPROPERTY(BlueprintReadWrite, Category=Motion, meta=(ForceUnits="deg/s"))
	float SmoothedYawRate = 0.f;

	UPROPERTY(BlueprintReadWrite, Category=Motion)
	bool bHasPreviousYaw = false;

	/** Push contribution along travel, from input intent. Positive leans into the direction of travel. */
	UPROPERTY(BlueprintReadOnly, Category="Motion|Debug")
	float DebugPushAlong = 0.f;

	/** Braking contribution along travel, from measured deceleration. Negative leans back. */
	UPROPERTY(BlueprintReadOnly, Category="Motion|Debug")
	float DebugBrakeAlong = 0.f;

	/** Magnitude of the input acceleration that was read this frame */
	UPROPERTY(BlueprintReadOnly, Category="Motion|Debug")
	float DebugInputAccel = 0.f;

	/** Final bias projected on the travel direction. Negative here is what reads as leaning back. */
	UPROPERTY(BlueprintReadOnly, Category="Motion|Debug")
	float DebugBiasAlong = 0.f;

	/** Centripetal acceleration the turn implied this frame, before TurnScale */
	UPROPERTY(BlueprintReadOnly, Category="Motion|Debug", meta=(ForceUnits="cm/s2"))
	float DebugCentripetal = 0.f;

	/** Scale that BackwardScale or DirectionCurve produced this frame */
	UPROPERTY(BlueprintReadOnly, Category="Motion|Debug")
	float DebugDirectionScale = 1.f;
};

/**
 * Drives the physical layer from whatever the character is standing on, so a body riding a moving
 * surface reacts to it. A rocking ship deck is the case this exists for: the deck rolls, and the body
 * on it should sway against that roll rather than stand welded to the boards.
 *
 * Both velocities are read straight off the base, never derived from a change in position.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBaseDrive
{
	GENERATED_BODY()

	/** How much the base's motion under the character leans the body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="3"))
	float TranslationScale = 1.f;

	/** How much the base's rotation rolls the body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="3"))
	float RotationScale = 1.f;

	/**
	 * Whether the base's vertical motion counts.
	 * A ship's heave is often better ignored, since a body riding it up and down reads as bobbing rather
	 * than as bracing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base)
	bool bIgnoreVerticalTranslation = true;

	/** Ceiling on the translation bias */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", ForceUnits="cm/s2"))
	float MaxBias = 2000.f;

	/** Ceiling on the rotation torque */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0"))
	float MaxTorque = 30.f;

	/** How fast the response follows the base. Low trails it, which is usually what a heavy deck wants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="30"))
	float InterpRate = 6.f;
};

/** Carried by the caller across frames. @see URagdollStatics::CalculateBaseDrive */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBaseDriveState
{
	GENERATED_BODY()

	/** Feed to URagdollComponent::AddPhysicalBias */
	UPROPERTY(BlueprintReadWrite, Category=Base)
	FVector Bias = FVector::ZeroVector;

	/** Feed to URagdollComponent::AddPhysicalTorque */
	UPROPERTY(BlueprintReadWrite, Category=Base)
	FVector Torque = FVector::ZeroVector;

	/** Base the drive resolved to this frame, for confirming the fallback found what you expected */
	UPROPERTY(BlueprintReadOnly, Category="Base|Debug")
	TWeakObjectPtr<UPrimitiveComponent> ResolvedBase = nullptr;
};

/**
 * Keeps the cost of the physical layer proportional to how much anyone can actually see of it.
 *
 * Physical animation is per-body constraint solving, so a crowd of characters running it at full rate
 * off screen is the obvious way for this to become the most expensive thing in a frame.
 *
 * None of it applies to a locally controlled pawn. That one is always on screen, always being studied,
 * and is never the reason a frame is slow.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollLODSettings
{
	GENERATED_BODY()

	/** Highest mesh LOD that still runs the layer. Beyond this it blends out. Negative disables the check. Does not affect local pawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD, meta=(DisplayName="LOD Threshold"))
	int32 LODThreshold = -1;

	/**
	 * Stop while the mesh has not been rendered recently.
	 * Off by default: the check is a frame behind, so it can hold the layer off during the frames that
	 * matter, and it never applies to the locally controlled pawn regardless.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD)
	bool bDisableWhenNotRendered = false;

	/** How long the mesh can go unrendered before the layer blends out */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD, meta=(EditCondition="bDisableWhenNotRendered", ForceUnits="s"))
	float NotRenderedThreshold = 0.3f;

	/**
	 * Distance past which a pawn this client does not control stops running the layer.
	 * Locally controlled pawns ignore every distance check, since that is the one the player studies.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD, meta=(UIMin="0", ForceUnits="cm"))
	float RemoteCullDistance = 4000.f;

	/** Distance at which a remote pawn's layer starts fading out, reaching zero at RemoteCullDistance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD, meta=(UIMin="0", ForceUnits="cm"))
	float RemoteFadeDistance = 2500.f;

	/** Scales the layer on pawns this client does not control, before distance fading */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LOD, meta=(UIMin="0", UIMax="1"))
	float RemoteScale = 1.f;
};

/**
 * A bone whose physics constraint should be broken during ragdoll, allowing it to separate and simulate independently.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollSeparatedBone
{
	GENERATED_BODY()

	/** Bone name to separate during ragdoll */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	FName BoneName;

	/** Impulse scale applied to this bone when it separates (multiplied by the ragdoll impulse) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll, meta=(UIMin="0"))
	float ImpulseScale = 1.f;
};

/**
 * Settings for the ragdoll blend
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollSettings
{
	GENERATED_BODY()

	FRagdollSettings()
		: BlendIn(4.f, true)
		, MotorDecay(2.f, true)
	{
		PhysicalAnimData.OrientationStrength = 100.f;
		PhysicalAnimData.AngularVelocityStrength = 10.f;
		PhysicalAnimData.PositionStrength = 100.f;
		PhysicalAnimData.VelocityStrength = 10.f;
	}

	/** Bone at which to start simulation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	FName SimulationRootBone = "pelvis";

	/** Whether to include the simulation root bone itself */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	bool bIncludeSimulationRoot = true;

	/** Controls how fast physics blend weight ramps from its current value to 1 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	FRagdollInterp BlendIn;

	/** Controls motor strength decay from 1 to 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	FRagdollInterp MotorDecay;

	/** Motor drive settings for simulated bodies */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	FPhysicalAnimationData PhysicalAnimData;

	/** Impulse strength applied on ragdoll start */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll, meta=(UIMin="0"))
	float ImpulseStrength = 500.f;
};

/**
 * Settings for getting up out of ragdoll
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollRecoverySettings
{
	GENERATED_BODY()

	/** Montage to play per get-up side. Sides without an entry fall back to the timer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Recovery)
	TMap<ERagdollRecoverySide, TObjectPtr<UAnimMontage>> Montages;

	/** Duration of recovery when no montage is available */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Recovery, meta=(UIMin="0", ForceUnits="s"))
	float FallbackDuration = 2.f;

	/** Optional curve controlling the recovery blend alpha, evaluated at normalized time. Takes priority over MontageCurveName. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Recovery)
	TObjectPtr<UCurveFloat> BlendCurve = nullptr;

	/** Curve name read from the recovery montage for the blend alpha */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Recovery)
	FName MontageCurveName = NAME_None;

	/** Scales how fast rotation completes relative to location. 0.5 = rotation finishes in half the time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Recovery, meta=(UIMin="0.01", UIMax="10", ClampMin="0.01"))
	float RotationTimeScale = 1.f;
};
