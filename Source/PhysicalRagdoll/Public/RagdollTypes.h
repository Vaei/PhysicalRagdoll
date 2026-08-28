// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "PhysicsControlData.h"

#include "RagdollTypes.generated.h"

class UAnimMontage;
class UCurveFloat;
class UPrimitiveComponent;

/** Where a block of tuning is read from. Asset can be edited while the game is running; inline cannot. */
UENUM(BlueprintType)
enum class ERagdollTuningSource : uint8
{
	Inline	UMETA(ToolTip="Held on the component, and fixed once the game is running"),
	Asset	UMETA(ToolTip="Held in a data asset, so edits apply during play")
};

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

	FRagdollBoneGroup() = default;

	FRagdollBoneGroup(FName InRootBone, bool bInIncludeRootBone = true, float InBlendWeight = 1.f)
		: RootBone(InRootBone)
		, bIncludeRootBone(bInIncludeRootBone)
		, BlendWeight(InBlendWeight)
	{}

	/** Bone the group starts at. Everything below it in the physics asset comes with it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	FName RootBone = "spine_01";

	/** Clear to drive the bones below RootBone while leaving RootBone itself on the animation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bIncludeRootBone = true;

	/**
	 * How much of the final pose is physics rather than animation. Reach for this to dial the whole group
	 * up or down at once; use the spring for how it moves.
	 *
	 * Blended per bone after the simulation, so 1 shows every bit of solver compliance and lower values
	 * hide it behind the clean animated pose.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="1", ClampMin="0", ClampMax="1"))
	float BlendWeight = 1.f;

	/**
	 * What each body is held against. WorldSpace for a body that has to hold its pose, ParentSpace for one
	 * that should follow wherever the chain takes it.
	 *
	 * ParentSpace errors accumulate down a chain, so a long one wanders at the tip however stiff each joint
	 * is. WorldSpace gives every body its own absolute target and does not accumulate.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	EPhysicsControlType ControlType = EPhysicsControlType::WorldSpace;

	/**
	 * How hard the body is pulled back onto its animated pose. Raise it to stop the group sagging or lagging.
	 *
	 * Angular acceleration per radian of error. Its square root is the drive's frequency in rad/s.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Spring, meta=(UIMin="0", UIMax="100000"))
	float AngularStiffness = 5700.f;

	/**
	 * How hard the body resists moving. Raise it to kill wobble and overshoot, lower it to let the group swing.
	 *
	 * Angular acceleration per rad/s. 2*sqrt(AngularStiffness) is critical: below rings, above is sluggish.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Spring, meta=(UIMin="0", UIMax="2000"))
	float AngularDamping = 180.f;

	/**
	 * The most the motor may spend. Set it when something should be able to shove the body around rather
	 * than have the drive quietly cancel it.
	 *
	 * Zero is unlimited, which is a motor that always wins.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Spring, meta=(UIMin="0"))
	float AngularMaxTorque = 0.f;

	/** How hard the body is pulled back to its animated position. Acceleration per cm of error. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Spring, meta=(UIMin="0", UIMax="10000"))
	float LinearStiffness = 5.f;

	/** How hard the body resists being moved. Acceleration per cm/s, with 2*sqrt(LinearStiffness) critical. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Spring, meta=(UIMin="0", UIMax="1000"))
	float LinearDamping = 5.f;

	/** The most the linear motor may spend. Zero is unlimited. @see AngularMaxTorque */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Spring, meta=(UIMin="0"))
	float LinearMaxForce = 0.f;

	/** Drive toward the animation. Clear it and the body holds wherever it started instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bUseSkeletalAnimation = true;

	/**
	 * Makes the spring numbers mean the same thing on a hand as on a torso. Leave it on.
	 *
	 * Normalizes by the body's own inertia, which knows nothing of the chain below it. @see bScaleStrengthByLoad
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bUseAccelerationDriveMode = true;

	/** Stop a driven body colliding with the one it is held against, if the two are interpenetrating */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bDisableCollision = false;

	/**
	 * Stops the top of a chain sagging while the ends stay stiff. Leave it on for any group covering a spine.
	 *
	 * Scales the spring by the inertia the joint actually carries. Parent space only: a world space drive has
	 * no chain hanging off it to correct for.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bScaleStrengthByLoad = true;

	/** Lower it if a joint has gone rigid, so one badly weighted body cannot run away with the scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="1", UIMax="50", ClampMin="1", EditCondition="bScaleStrengthByLoad"))
	float MaxLoadScale = 20.f;

	/**
	 * Stops the load scale making one joint rigid while the rest of the group stays soft. Off by default.
	 *
	 * Caps sqrt(AngularStiffness) after the load scale, in Hz. There is no stability reason to cap it: Chaos
	 * solves drives with XPBD, which saturates into a rigid constraint rather than ringing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="60", ForceUnits="Hz"))
	float MaxDriveFrequency = 0.f;

	/**
	 * Raise it if a long chain still sags at any stiffness. Costs CPU per driven body.
	 *
	 * Chaos solves joints one pair at a time, so a chain's load only reaches its root after enough passes.
	 * Zero leaves the scene default alone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="64"))
	uint8 PositionSolverIterations = 32;

	/** @see PositionSolverIterations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="32"))
	uint8 VelocitySolverIterations = 8;

	/**
	 * Use the physics asset's own joints rather than adding a second constraint per body. Try it if two
	 * constraints on the same pair of bodies are fighting.
	 *
	 * The engine writes their targets from the animated pose. Parent space only, since a joint drive is
	 * parent relative.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(EditCondition="ControlType == EPhysicsControlType::ParentSpace"))
	bool bUseJointDrives = false;

	/**
	 * Take the spring from a constraint profile authored in the physics asset instead of the values above.
	 *
	 * Optional, and forces ParentSpace, since a joint drive is parent relative.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(GetOptions="GetConstraintProfileOptions"))
	FName ControlDataConstraintProfile = NAME_None;
};

/**
 * Per-bone adjustment applied to a bone and, unless bIncludeSelf is cleared, everything below it.
 * This is how a group drives the torso without taking the arms with it.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBoneOverride
{
	GENERATED_BODY()

	/** Bone the override starts at. Everything below it comes with it unless bIncludeSelf is cleared. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	FName BoneName;

	/** Clear to leave this bone alone and apply the override only below it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bIncludeSelf = true;

	/** Take these bones off the layer entirely, so they follow the animation exactly. Use it for an arm holding something. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	bool bDisablePhysics = false;

	/** Thin the layer on these bones without removing it, e.g. so a head does not swim */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", UIMax="1", ClampMin="0", ClampMax="1", EditCondition="!bDisablePhysics", EditConditionHides))
	float BlendWeightScalar = 1.f;

	bool operator==(const FRagdollBoneOverride& Other) const = default;
};

/**
 * A bone override gameplay adds while the game is running, on top of whatever the profile authored.
 * An arm holding something wants taking off the layer for as long as it is holding it.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollRuntimeBoneOverride
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical)
	FRagdollBoneOverride Override;

	/** Profile it applies to. Empty applies to whichever profile is running. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(Categories="Ragdoll.Profile"))
	FGameplayTag ProfileTag;

	bool operator==(const FRagdollRuntimeBoneOverride& Other) const = default;
};

/**
 * A named level of physicality, e.g. a subtle overlap on the spine and arms, or a full body flail.
 * Applied on top of regular animation and held indefinitely.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollPhysicalProfile
{
	GENERATED_BODY()

	/** Bodies this profile drives, one group per set of springs. Where groups overlap, later entries win. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(TitleProperty="RootBone"))
	TArray<FRagdollBoneGroup> BoneGroups;

	/**
	 * Carve bones out of the groups above: thin the layer on a head, take it off an arm entirely.
	 *
	 * Each covers the bone and everything below it. Order is not significant; where they overlap the most
	 * restrictive wins.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(TitleProperty="BoneName"))
	TArray<FRagdollBoneOverride> BoneOverrides;

	/** Scales every spring in the profile at once, for dialling the whole thing up or down */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", ClampMin="0"))
	float StrengthMultiplier = 1.f;

	/**
	 * How fast the profile fades in and out. Above about 12 the transition reads as a snap.
	 *
	 * Bone weight change per second, frame rate independent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(UIMin="0", ClampMin="0", UIMax="12"))
	float BoneBlendRate = 7.f;

	/**
	 * Constraint profile from the physics asset, applied to every joint while this profile runs. Use it to
	 * widen limits or change drives that only suit this level of physicality.
	 *
	 * Optional. Reverts to URagdollComponent::DefaultConstraintProfile when the profile ends.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Physical, meta=(GetOptions="GetConstraintProfileOptions"))
	FName ConstraintProfile = NAME_None;
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

	/**
	 * Shape how much physicality the character gets at each speed. Replaces SpeedRange and AlphaRange.
	 *
	 * Input is speed normalized against the mover's max speed, output scales the whole layer.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Speed")
	TObjectPtr<UCurveFloat> SpeedCurve = nullptr;

	/** Speed range the layer fades in over, when no SpeedCurve is set */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Speed", meta=(EditCondition="SpeedCurve == nullptr", ForceUnits="cm/s"))
	FVector2D SpeedRange = FVector2D(0.f, 600.f);

	/** How much layer the character gets at each end of SpeedRange. X is standing still. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Speed", meta=(EditCondition="SpeedCurve == nullptr"))
	FVector2D AlphaRange = FVector2D(0.35f, 1.f);

	/**
	 * How much the body leans into a push-off. Go above 1 to lean forward, below to trail behind.
	 *
	 * The bodies hang off an accelerating kinematic pelvis and already trail on their own, so 1 only
	 * cancels that and leaves the body upright.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="3"))
	float AccelerationScale = 2.5f;

	/** How much the body pitches when braking. Usually higher than AccelerationScale, stopping being sharper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="3"))
	float BrakingScale = 0.5f;

	/** Flips which side of the arc the turn lean falls on, if it is leaning the wrong way */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias")
	bool bInvertTurn = false;

	/**
	 * How much the body banks into a turn. Wants to be generous before any lean shows at all.
	 *
	 * Scales the centripetal acceleration the turn implies (speed x yaw rate), and has to beat the body's
	 * own rotational trailing first.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="3"))
	float TurnScale = 1.0f;

	/**
	 * How much layer survives when backpedalling. Lower it if walking backwards throws the body around.
	 *
	 * Reaches 1 moving straight ahead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Direction", meta=(UIMin="0", UIMax="1"))
	float BackwardScale = 0.3f;

	/** Shape the direction scale yourself. Input is forwardness remapped from -1..1 to 0..1. Replaces BackwardScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Direction")
	TObjectPtr<UCurveFloat> DirectionCurve = nullptr;

	/**
	 * How fast the layer changes while the character is settled. Lower it if the layer twitches when idle.
	 *
	 * Bone weight blend rate while nothing is accelerating.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="20"))
	float SteadyBlendRate = 3.f;

	/** How fast the layer changes under full acceleration, where it has to keep up with the movement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="20"))
	float AcceleratingBlendRate = 10.f;

	/**
	 * How firmly the body holds its pose at a constant speed. 1 is the group's own spring, and stops
	 * steady movement reading as a wobble.
	 *
	 * Scales the motor only. The blend weight is untouched, so the layer is still doing its job.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="2"))
	float SteadyStrength = 1.f;

	/**
	 * How much the body gives while accelerating. Below SteadyStrength is what lets the lean read at all;
	 * too low and it goes floppy the moment you touch the stick.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="0", UIMax="2"))
	float AcceleratingStrength = 0.45f;

	/**
	 * Ceiling on the push. Check this first if the lean never appears at any scale.
	 *
	 * Has to sit above the mover's MaxAcceleration with the scales applied, or it clamps away the part that
	 * cancels the natural trailing and the body only ever lags.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", ForceUnits="cm/s2"))
	float MaxBias = 4000.f;

	/** How fast the push follows the movement. Low trails the motion, high snaps to it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(UIMin="0", UIMax="30"))
	float BiasInterpRate = 12.f;

	/** Keep the layer running while airborne. Clear it to drop the physicality off the ground entirely. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling")
	bool bDriveWhileFalling = true;

	/** Shape how much layer a fall gets. Input is vertical speed normalized against ReferenceFallSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling", meta=(EditCondition="bDriveWhileFalling"))
	TObjectPtr<UCurveFloat> FallCurve = nullptr;

	/** Fall speed that counts as a full fall on the curve's input */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling", meta=(UIMin="1", EditCondition="bDriveWhileFalling", ForceUnits="cm/s"))
	float ReferenceFallSpeed = 1200.f;

	/**
	 * How much of the push survives in the air. Lower it if the character flails while airborne.
	 *
	 * Off the ground there is nothing to push against, so the lean that reads as effort reads as flailing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Falling", meta=(UIMin="0", UIMax="1", EditCondition="bDriveWhileFalling"))
	float FallBiasScale = 0.3f;

	/** Bone the push is scoped to. Lower down the spine puts more of the body into it; None reads as a shake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias")
	FName BiasBone = "spine_03";

	/** Lever arm above the centre of mass. Higher tips the body further for the same push. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Bias", meta=(ForceUnits="cm"))
	float BiasHeightOffset = 20.f;

	/**
	 * How much firmer the body gets the moment input stops. Raise it if he swings on after you let go.
	 *
	 * Multiplies the motor strength while there is no input.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="1", UIMax="8"))
	float BrakingStrengthScale = 3.f;

	/** How much faster the layer settles once input stops. @see BrakingStrengthScale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion|Strength", meta=(UIMin="1", UIMax="8"))
	float BrakingRateScale = 5.f;
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
 * How the motion of whatever the character is riding is measured. Derived from the base's transform, since
 * a base moved by replication, smoothing or a spline leaves no velocity to read.
 */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBaseMotion
{
	GENERATED_BODY()

	/** Smoothing on the base's measured velocity. Raise it if the drive jitters on a replicated base. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BaseMotion, meta=(UIMin="0", UIMax="60"))
	float InterpRate = 20.f;

	/** Smoothing on the base's measured acceleration. A second difference, so it needs far more than velocity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BaseMotion, meta=(UIMin="0", UIMax="60"))
	float AccelerationInterpRate = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BaseMotion, meta=(UIMin="0", ForceUnits="cm/s"))
	float MaxLinearSpeed = 4000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BaseMotion, meta=(UIMin="0", ForceUnits="cm/s2"))
	float MaxLinearAcceleration = 4000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BaseMotion, meta=(UIMin="0", ForceUnits="rad/s"))
	float MaxAngularSpeed = 8.f;

	/** A frame that moves the base further than this is a teleport, not travel, and is discarded */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=BaseMotion, meta=(UIMin="0", ForceUnits="cm"))
	float TeleportDistance = 500.f;
};

/** Carried across frames by URagdollStatics::CalculateBaseMotion */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollBaseMotionState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category=BaseMotion, meta=(ForceUnits="cm/s"))
	FVector LinearVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category=BaseMotion, meta=(ForceUnits="rad/s"))
	FVector AngularVelocity = FVector::ZeroVector;

	/** Velocity of the base at the character. Only its rate of change reaches the body. */
	UPROPERTY(BlueprintReadOnly, Category=BaseMotion, meta=(ForceUnits="cm/s"))
	FVector PointVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category=BaseMotion, meta=(ForceUnits="cm/s2"))
	FVector PointAcceleration = FVector::ZeroVector;

	/** Slope under the character, pointing downhill. @see URagdollStatics::CalculateBaseTilt */
	UPROPERTY(BlueprintReadOnly, Category=BaseMotion, meta=(ForceUnits="cm/s2"))
	FVector TiltAcceleration = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category=BaseMotion, meta=(ForceUnits="deg"))
	float TiltAngle = 0.f;

	bool bHasPreviousPointVelocity = false;

	/** What the motion was measured from, for confirming the drive found what you expected */
	UPROPERTY(BlueprintReadOnly, Category=BaseMotion)
	TWeakObjectPtr<UPrimitiveComponent> ResolvedBase = nullptr;

	FTransform PreviousTransform = FTransform::Identity;

	bool bHasPreviousTransform = false;

	void ClearHistory()
	{
		PreviousTransform = FTransform::Identity;
		bHasPreviousTransform = false;
		bHasPreviousPointVelocity = false;
	}
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

	/**
	 * How much the base moving under the character shoves the body. Raise it for a deck that lurches.
	 *
	 * Scales the base's acceleration at the character, applied as a bias.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="3"))
	float TranslationScale = 1.f;

	/**
	 * How much the base turning under the character twists the body.
	 *
	 * rad/s of base roll to rad/s2 of torque. A deck peaks near 0.15 rad/s, so useful values are tens.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="50"))
	float RotationScale = 1.f;

	/**
	 * How much a sloped base pushes the body downhill. This is the term that carries a steady list.
	 *
	 * Scales the slope, already in cm/s2: a 10 degree list is about 170.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="3"))
	float TiltScale = 1.f;

	/**
	 * Tilt at which the whole drive reaches full effect. Raise it to keep a nearly level base off the body.
	 *
	 * Everything below it scales down with the slope. Zero gives full effect at any tilt.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="45", ForceUnits="deg"))
	float TiltRange = 12.f;

	/** Tilt past this stops counting, so a base that rolls right over cannot throw the body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="90", ForceUnits="deg"))
	float MaxTiltAngle = 30.f;

	/**
	 * How far the body leans uphill on a sloped base, keeping its weight over its feet.
	 *
	 * Degrees of body per degree of base. This is a static posture, unlike SwayScale.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Base|Lean", meta=(UIMin="0", UIMax="2"))
	float TiltLeanScale = 0.f;

	/**
	 * How much a rocking base throws the body off balance. This is the main knob for feeling the deck move.
	 *
	 * The fraction of the base's rotation the body fails to follow: 0 rides it welded, 1 is left standing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Base|Lean", meta=(UIMin="0", UIMax="1"))
	float SwayScale = 0.f;

	/**
	 * How steady the character looks on his feet. Lower it to make him struggle with the deck.
	 *
	 * The recovery spring's frequency in Hz: low is still catching up when the deck rolls back the other way.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Base|Lean", meta=(UIMin="0.1", UIMax="4", ClampMin="0.01", ForceUnits="Hz"))
	float BalanceFrequency = 1.f;

	/** Below 1 he overcorrects and settles rather than gliding to the deck's angle. 1 is critical damping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Base|Lean", meta=(UIMin="0", UIMax="2"))
	float BalanceDampingRatio = 0.6f;

	/** Ceiling on the combined tilt lean and sway, so nothing can bend the body double */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Base|Lean", meta=(UIMin="0", UIMax="45", ForceUnits="deg"))
	float MaxLeanAngle = 20.f;

	/**
	 * Extra smoothing on the lean. Lower it if the lean jitters, though the sway spring already smooths.
	 *
	 * Separate from InterpRate, which trails the forces. Zero applies the lean with no smoothing at all.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Base|Lean", meta=(UIMin="0", UIMax="30"))
	float LeanInterpRate = 6.f;

	/** Clear it to let a base heaving up and down push the body. Usually reads as bobbing rather than bracing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base)
	bool bIgnoreVerticalTranslation = true;

	/** Ceiling on the translation and tilt bias together, so one violent frame cannot throw the body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", ForceUnits="cm/s2"))
	float MaxBias = 2000.f;

	/** Ceiling on the rotation torque. Raise it if a stiff drive is cancelling the base's twist entirely. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0"))
	float MaxTorque = 30.f;

	/** How fast the forces follow the base. Low trails it, which is usually what a heavy deck wants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="30"))
	float InterpRate = 6.f;

	/** Bone the response is scoped to. Lower down the spine puts more of the body into it; None reads as a shake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base)
	FName BiasBone = "spine_03";

	/** Lever arm above the centre of mass. @see URagdollComponent::AddPhysicalBias */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(ForceUnits="cm"))
	float BiasHeightOffset = 20.f;

	/**
	 * How quickly the base takes the body back once the player stops moving. Lower means a longer handover.
	 *
	 * Input takes the layer instantly; only the release is interpolated.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Base, meta=(UIMin="0", UIMax="10"))
	float InputReleaseRate = 1.5f;
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

	/** Feed to URagdollComponent::SetPhysicalLean */
	UPROPERTY(BlueprintReadWrite, Category=Base)
	FRotator Lean = FRotator::ZeroRotator;

	/** World-space rotation vector the body is currently off balance by, and how fast it is recovering */
	UPROPERTY(BlueprintReadWrite, Category=Base)
	FVector Sway = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category=Base)
	FVector SwayVelocity = FVector::ZeroVector;

	/** Slope of the base under the character in cm/s2, pointing downhill, before TiltScale */
	UPROPERTY(BlueprintReadOnly, Category="Base|Debug", meta=(ForceUnits="cm/s2"))
	FVector TiltAcceleration = FVector::ZeroVector;

	/** How far the base is tilted from level, for reading off what the tilt term is working with */
	UPROPERTY(BlueprintReadOnly, Category="Base|Debug", meta=(ForceUnits="deg"))
	float TiltAngle = 0.f;

	/** Base the drive resolved to this frame, for confirming the fallback found what you expected */
	UPROPERTY(BlueprintReadOnly, Category="Base|Debug")
	TWeakObjectPtr<UPrimitiveComponent> ResolvedBase = nullptr;
};

/** A one-off kick decaying over a half life: a wave slamming the deck, a shove, a blast */
USTRUCT(BlueprintType)
struct PHYSICALRAGDOLL_API FRagdollImpulseState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category=Impulse, meta=(ForceUnits="cm/s2"))
	FVector Bias = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category=Impulse)
	FVector Torque = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category=Impulse, meta=(ForceUnits="s"))
	float HalfLife = 0.f;

	bool IsActive() const
	{
		return Bias.SizeSquared() > 1.f || Torque.SizeSquared() > UE_KINDA_SMALL_NUMBER;
	}

	void Reset()
	{
		Bias = FVector::ZeroVector;
		Torque = FVector::ZeroVector;
		HalfLife = 0.f;
	}
};

/**
 * Keeps the cost of the physical layer proportional to how much anyone can actually see of it.
 *
 * The physical layer is per-body constraint solving, so a crowd of characters running it at full rate
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
		ControlData.AngularStrength = 1.6f;
		ControlData.AngularDampingRatio = 0.5f;
		ControlData.LinearStrength = 1.6f;
		ControlData.LinearDampingRatio = 0.5f;
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

	/** What each simulated body is held relative to while the motors are still decaying */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	EPhysicsControlType ControlType = EPhysicsControlType::WorldSpace;

	/** Spring and damper settings for simulated bodies */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll)
	FPhysicsControlData ControlData;

	/**
	 * Constraint profile authored in the physics asset, whose joint drives initialize the strengths in place
	 * of ControlData. Optional, and forces ParentSpace. The strengths still decay to zero, so this shapes
	 * how the body gives up its pose.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll, meta=(GetOptions="GetConstraintProfileOptions"))
	FName ControlDataConstraintProfile = NAME_None;

	/**
	 * Constraint profile authored in the physics asset, applied to every joint while ragdolling.
	 * Optional, and the usual place to widen joint limits or drop drives that only suit a live character.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Ragdoll, meta=(GetOptions="GetConstraintProfileOptions"))
	FName ConstraintProfile = NAME_None;

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
	float FallbackDuration = 1.f;

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
