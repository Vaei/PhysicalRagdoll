// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "RagdollTypes.h"

#include "RagdollComponent.generated.h"

class ACharacter;
class UPhysicalAnimationComponent;
class USkeletalMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnRagdollStateChanged, ERagdollState, OldState, ERagdollState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPhysicalProfileChanged, FGameplayTag, OldProfile, FGameplayTag, NewProfile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRagdollBlendComplete);

/**
 * Drives a character's skeletal mesh through UPhysicalAnimationComponent.
 *
 * Physical: a persistent physics layer blended over otherwise regular animation, selected by named
 * profile so a character can run anything from subtle overlap to a full flail, and switch between
 * them at runtime.
 *
 * Ragdoll: physics takes over entirely, with motor decay and an optional impulse.
 *
 * Recovery: get up out of ragdoll, optionally driven by a montage, restoring the physical profile
 * that was active beforehand.
 */
UCLASS(ClassGroup=(Physics), Blueprintable, meta=(BlueprintSpawnableComponent))
class PHYSICALRAGDOLL_API URagdollComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	/** Physical animation component created on the owner if it doesn't already have one */
	UPROPERTY(EditDefaultsOnly, Category="Ragdoll")
	TSubclassOf<UPhysicalAnimationComponent> PhysicalAnimationComponentClass;

	/** Levels of physicality that can be applied over regular animation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical", meta=(Categories="Ragdoll.Profile"))
	TMap<FGameplayTag, FRagdollPhysicalProfile> PhysicalProfiles;

	/** Profile applied on BeginPlay. Empty to start without a physical layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical", meta=(Categories="Ragdoll.Profile"))
	FGameplayTag AutoPhysicalProfile;

	/** Reapply the physical profile that was active before ragdoll once recovery completes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical")
	bool bRestorePhysicalProfileAfterRecovery = true;

	/**
	 * Poll ShouldSuspendPhysicalLayer every tick and suspend or resume from its answer.
	 *
	 * This keeps the component ticking even while the layer is off, since a suspended component that
	 * slept could never notice that it should come back. Leave it off unless the query is in use.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical")
	bool bQueryStateSuspension = false;

	/**
	 * Supply the linear restoring force that local simulation leaves out.
	 *
	 * With bIsLocalSimulation the engine deliberately zeroes the linear drive and keeps only the angular
	 * one, so a body has nothing pulling it back to its target position. Any sustained bias then just
	 * accelerates it until a joint limit catches it, and it stays there - the character straightens up,
	 * further bias has no authority, and stopping never undoes it. This restores that missing half, so a
	 * bias settles at an offset proportional to its strength and springs back when it goes away.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical")
	bool bRestoreLinearDrift = false;

	/** How hard a drifted body is pulled back toward its animated position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical", meta=(UIMin="0", EditCondition="bRestoreLinearDrift"))
	float LinearRestoreStiffness = 15.f;

	/** Damping on the drift, applied only along the drift itself so it cannot fight the animation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical", meta=(UIMin="0", EditCondition="bRestoreLinearDrift"))
	float LinearRestoreDamping = 3.f;

	/** Blend rate used by a Fast suspension, versus the profile's own rate for a normal one */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Physical", meta=(UIMin="1", UIMax="40"))
	float FastSuspendBlendRate = 25.f;

	/** Keeps the layer's cost proportional to how visible it is */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Performance")
	FRagdollLODSettings LOD;

	/** Ragdoll settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FRagdollSettings RagdollSettings;

	/** Recovery settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Recovery")
	FRagdollRecoverySettings RecoverySettings;

	/** Bones that break free and simulate independently during ragdoll (e.g. weapon, hat) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	TArray<FRagdollSeparatedBone> SeparatedBones;

	/**
	 * Upgrade the mesh's collision to include physics while simulating, restoring it afterwards.
	 * The engine skips per-bone physics blending entirely on a mesh without physics collision, and
	 * ACharacter's default CharacterMesh profile is query only, so leaving this off means a physical
	 * profile has no visible effect on a stock character.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Collision")
	bool bAutoEnablePhysicsCollision = true;

	/** Collision profile applied to the mesh during ragdoll. Empty = don't change. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Collision", meta=(GetOptions="Engine.KismetSystemLibrary.GetCollisionProfileNames"))
	FName SimulationCollisionProfile = "Ragdoll";

	/** Collision profile restored when ragdoll ends. Empty = restore whatever was active beforehand. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Collision", meta=(GetOptions="Engine.KismetSystemLibrary.GetCollisionProfileNames"))
	FName DefaultCollisionProfile = NAME_None;

	/** Current recovery blend alpha (0 = ragdoll pose, 1 = default pose) */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Ragdoll|Recovery")
	float RecoveryAlpha = 0.f;

	/** Fired when the state changes */
	UPROPERTY(BlueprintAssignable, Category="Ragdoll")
	FOnRagdollStateChanged OnRagdollStateChanged;

	/** Fired when the active physical profile changes */
	UPROPERTY(BlueprintAssignable, Category="Ragdoll")
	FOnPhysicalProfileChanged OnPhysicalProfileChanged;

	/** Fired when ragdoll blend in and motor decay both complete */
	UPROPERTY(BlueprintAssignable, Category="Ragdoll")
	FOnRagdollBlendComplete OnRagdollBlendComplete;

public:
	URagdollComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// --- Physical API ---

	/** Blend to a profile from PhysicalProfiles. An empty tag clears the physical layer. */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical", meta=(Categories="Ragdoll.Profile"))
	void SetPhysicalProfile(FGameplayTag ProfileTag);

	/** Blend to a profile supplied directly, ignoring PhysicalProfiles */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical", meta=(Categories="Ragdoll.Profile"))
	void SetPhysicalProfileWithSettings(FGameplayTag ProfileTag, const FRagdollPhysicalProfile& Profile);

	/** Blend the physical layer out entirely */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical")
	void ClearPhysicalProfile();

	/** Scales the whole physical layer, for ramping physicality with speed, stamina, etc. */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical")
	void SetPhysicalAlpha(float NewAlpha);

	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	float GetPhysicalAlpha() const { return PhysicalAlpha; }

	/**
	 * Scales the profile's motor strength without touching how much physics shows through.
	 * High holds the pose, low lets the body give. Separate from the alpha because raising one and
	 * lowering the other are opposite intentions that would otherwise cancel.
	 */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical")
	void SetPhysicalStrength(float NewStrength);

	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	float GetPhysicalStrength() const { return PhysicalStrength; }

	/**
	 * Override how fast bone weights move, replacing the profile's BoneBlendRate.
	 * Negative returns control to the profile.
	 */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical")
	void SetPhysicalBlendRate(float NewBlendRate);

	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	float GetPhysicalBlendRate() const;

	/**
	 * Lean the driven bodies by a world-space acceleration, for this frame only.
	 * Call each frame to sustain it. See URagdollStatics::CalculateMotionDrive.
	 *
	 * Applied at a point above each body's centre of mass. A force through the centre of mass produces no
	 * torque and only shoves the chain along, so the height is what turns the push into a lean.
	 *
	 * @param Bias			World-space acceleration. Mass normalized, so weight does not change the lean.
	 * @param BoneName		Bone to lean, covering everything below it. None uses the profile's groups.
	 * @param HeightOffset	Lever arm above the centre of mass. Larger pitches harder; negative inverts it.
	 */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical", meta=(AdvancedDisplay="HeightOffset"))
	void AddPhysicalBias(FVector Bias, FName BoneName = NAME_None, float HeightOffset = 50.f);

	/**
	 * Roll the driven bodies about a world axis, for this frame only.
	 *
	 * Where AddPhysicalBias takes a direction to lean toward and works out the axis, this takes the axis
	 * directly, for cases that are already rotational - a rocking deck's angular velocity, for instance,
	 * which has no meaningful direction to lean toward.
	 *
	 * @param Torque		World-space angular acceleration, in radians. Inertia normalized.
	 * @param BoneName		Bone to roll around, covering everything below it. None uses the profile's groups.
	 */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical")
	void AddPhysicalTorque(FVector Torque, FName BoneName = NAME_None);

	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	FGameplayTag GetPhysicalProfile() const { return ActiveProfileTag; }

	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	bool IsPhysicalLayerActive() const { return CurrentState == ERagdollState::Physical; }

	// --- Suspend API ---

	/**
	 * Take the physical layer away while something more important needs the character, without losing
	 * the active profile. Reasons are tracked by tag, so two systems suspending at once cannot resume
	 * each other's suspension, and the strongest urgency currently asked for is the one that applies.
	 */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical", meta=(Categories="Ragdoll.Suspend"))
	void SuspendPhysicalLayer(FGameplayTag Reason, ERagdollSuspendUrgency Urgency = ERagdollSuspendUrgency::Blend);

	/** Lift one suspension. The layer returns once every reason has been lifted. */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical", meta=(Categories="Ragdoll.Suspend"))
	void ResumePhysicalLayer(FGameplayTag Reason);

	/** Lift every suspension at once */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll|Physical")
	void ResumePhysicalLayerAll();

	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	bool IsPhysicalSuspended() const { return SuspendReasons.Num() > 0; }

	/**
	 * Decide from the owner's current state whether the layer should be off, and how urgently.
	 *
	 * Override in a derived component (or Blueprint) to drive suspension from gameplay state rather than
	 * from explicit Suspend and Resume calls. Requires bQueryStateSuspension.
	 *
	 * @param OutUrgency	How hard to take the layer away when returning true
	 * @return				Whether the layer should currently be suspended
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCosmetic, Category="Ragdoll|Physical")
	bool ShouldSuspendPhysicalLayer(ERagdollSuspendUrgency& OutUrgency) const;
	virtual bool ShouldSuspendPhysicalLayer_Implementation(ERagdollSuspendUrgency& OutUrgency) const;

	/** Strongest urgency currently asked for across all reasons */
	UFUNCTION(BlueprintPure, Category="Ragdoll|Physical")
	ERagdollSuspendUrgency GetSuspendUrgency() const;

	/** Current LOD scale on the layer, 0 while culled. @see FRagdollLODSettings */
	UFUNCTION(BlueprintPure, Category="Ragdoll|Performance")
	float GetLODScale() const { return LODScale; }

	/** False on a dedicated server, where none of this is worth simulating */
	UFUNCTION(BlueprintPure, Category="Ragdoll")
	bool IsRagdollRunnable() const;

	// --- Ragdoll API ---

	/** Full ragdoll death: disables movement and capsule collision, then starts ragdoll. Call StartRecovery() to get up. */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll")
	void RagdollDeath(FVector Impulse = FVector::ZeroVector);

	/** Start ragdoll with the component's settings */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll")
	void StartRagdoll(FVector Impulse = FVector::ZeroVector);

	/** Start ragdoll with custom settings */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll")
	void StartRagdollWithSettings(const FRagdollSettings& Settings, FVector Impulse = FVector::ZeroVector);

	/** Stop ragdoll immediately, without a get-up */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll")
	void StopRagdoll();

	// --- Recovery API ---

	/** Start getting up from ragdoll. Determines the side from the simulation root and plays the matching montage. */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll")
	void StartRecovery();

	/** Stop recovery, snapping to the recovered pose */
	UFUNCTION(BlueprintCallable, BlueprintCosmetic, Category="Ragdoll")
	void StopRecovery();

	// --- Query API ---

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	ERagdollState GetCurrentState() const { return CurrentState; }

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	bool IsRagdoll() const { return CurrentState == ERagdollState::Ragdoll; }

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	bool IsRecovering() const { return CurrentState == ERagdollState::Recovery; }

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	bool IsSimulating() const { return CurrentState != ERagdollState::None; }

	/** Highest physics blend weight currently applied (0 = animation, 1 = physics) */
	UFUNCTION(BlueprintPure, Category="Ragdoll")
	float GetBlendAlpha() const;

	// --- Debug ---

	/** Toggle ragdoll on the owning character. Console command: p.Ragdoll.Death */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|Debug", meta=(DevelopmentOnly))
	void DebugRagdollDeath(float ImpulseScaleXY = 1.f, float ImpulseScaleZ = 1.f);

	/** Log every body's simulation state and blend weight. Console command: p.Ragdoll.DumpBodies */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|Debug", meta=(DevelopmentOnly))
	void DebugDumpBodies() const;

	/** Draw the bias reaching AddPhysicalBias. Console command: p.Ragdoll.DebugMotion */
	void DrawMotionDebug() const;

protected:
	void SetState(ERagdollState NewState);
	void CacheReferences();
	bool HasValidPhysics() const;

	// Physical layer
	void SetupPhysical(FGameplayTag ProfileTag, const FRagdollPhysicalProfile& Profile);
	void TeardownPhysical();
	void TickPhysical(float DeltaTime);
	void ResolveBoneOverrides();
	float CalculateLODScale() const;
	void SuspendImmediately();
	void ApplyEnabledCVar();
	void TickStateSuspension();
	void GatherTargetWeights(TMap<FName, float>& OutTargets) const;
	void ApplyPendingPhysicalProfile();

	// Ragdoll
	void SetupRagdoll(const FVector& Impulse);
	void TeardownRagdoll();
	void TickRagdoll(float DeltaTime);

	// Recovery
	void SetupRecovery();
	void TeardownRecovery();
	void TickRecovery(float DeltaTime);
	void OnRecoveryMontageEnded(UAnimMontage* Montage, bool bInterrupted);
	void OnRecoveryTimerExpired();
	void FinishRecovery();
	void FinalizeRecoveryTransform() const;

	/** Override to select the get-up side from something other than the simulation root orientation */
	virtual ERagdollRecoverySide DetermineRecoverySide() const;

	/** Override to select the get-up montage procedurally */
	virtual UAnimMontage* GetRecoveryMontage(ERagdollRecoverySide Side) const;

	/** Override to filter separated bones based on gameplay state */
	virtual void GetActiveSeparatedBones(TArray<FRagdollSeparatedBone>& OutBones) const;

	// Capsule tracking
	void WakeBodiesBelow(FName BoneName, bool bIncludeSelf) const;

	void UpdateCapsuleToFollowMesh() const;
	void SnapMeshToCapsule() const;

	// Shared helpers
	void ApplySimulationCollisionProfile();
	void RestoreCollisionProfile();
	void EnsurePhysicsCollision();
	void RestoreCollisionEnabled();
	void WarnIfGroupUnanchored(const FRagdollBoneGroup& Group) const;
	void Wake();
	void Sleep();

protected:
	UPROPERTY(Transient)
	ERagdollState CurrentState = ERagdollState::None;

	/** Physical animation component on the same actor */
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UPhysicalAnimationComponent> PhysicalAnimation;

	/** Cached skeletal mesh */
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<USkeletalMeshComponent> Mesh;

	/** Profile currently driving the physical layer */
	UPROPERTY(Transient)
	FGameplayTag ActiveProfileTag;

	/** Working copy of the active profile, so runtime settings survive a profile map edit */
	UPROPERTY(Transient)
	FRagdollPhysicalProfile ActiveProfile;

	/** Profile to reapply once recovery completes, or once every suspension is lifted */
	UPROPERTY(Transient)
	FGameplayTag PendingProfileTag;

	/** Active suspensions and the urgency each asked for */
	UPROPERTY(Transient)
	TMap<FGameplayTag, ERagdollSuspendUrgency> SuspendReasons;

	/** Overrides expanded across each covered subtree, so lookup during tick is a single map hit per bone */
	UPROPERTY(Transient)
	TMap<FName, FRagdollBoneOverride> ResolvedBoneOverrides;

	/** Smoothed physics blend weight currently applied per bone */
	UPROPERTY(Transient)
	TMap<FName, float> BoneWeights;

	/** Global scale on the physical layer */
	UPROPERTY(Transient)
	float PhysicalAlpha = 1.f;

	/** Global scale on the profile's motor strength */
	UPROPERTY(Transient)
	float PhysicalStrength = 1.f;

	/** Overrides the profile's BoneBlendRate while not negative */
	UPROPERTY(Transient)
	float PhysicalBlendRateOverride = -1.f;

	/** Scale from the LOD settings, multiplied into the layer alongside PhysicalAlpha */
	UPROPERTY(Transient)
	float LODScale = 1.f;

	/** Interpolated strength multiplier fed to the physical animation component */
	UPROPERTY(Transient)
	float CurrentStrength = 0.f;

	/** Physics blend weight during ragdoll */
	UPROPERTY(Transient)
	float RagdollWeight = 0.f;

	/** Motor strength during ragdoll, decaying to 0 */
	UPROPERTY(Transient)
	float RagdollMotorStrength = 1.f;

	bool bRagdollBlendComplete = false;

	bool bLoggedConvergedBodies = false;

	/**
	 * Frame a bias or torque last arrived.
	 *
	 * The layer must not sleep while anything is driving it. Sleeping stops the per-frame maintenance the
	 * force path depends on - reapplying blend weights, finalizing the mesh physics, and refreshing the
	 * motor strength, which is also what keeps the bodies awake - so a bias applied to a sleeping layer
	 * quietly does nothing.
	 */
	uint64 LastBiasFrame = 0;

	/** Last bias handed to AddPhysicalBias, for the motion debug */
	FVector LastAppliedBias = FVector::ZeroVector;
	TArray<FVector> LastBiasOrigins;
	FDelegateHandle DebugCVarChangedHandle;

	/** Whether we changed the collision profile on the mesh */
	bool bCollisionProfileChanged = false;

	UPROPERTY(Transient)
	FName OriginalCollisionProfileName = NAME_None;

	/** Whether we upgraded the mesh's collision enabled state */
	bool bCollisionEnabledChanged = false;

	TEnumAsByte<ECollisionEnabled::Type> OriginalCollisionEnabled = ECollisionEnabled::QueryOnly;

	/** Whether to restore movement and capsule collision when returning to None */
	bool bRestoreMovementOnEnd = false;

	UPROPERTY(Transient)
	TObjectPtr<UAnimMontage> ActiveRecoveryMontage;

	FTimerHandle RecoveryTimerHandle;

	FVector RecoveryStartLocation = FVector::ZeroVector;
	FQuat RecoveryStartRotation = FQuat::Identity;
	FVector RecoveryTargetLocation = FVector::ZeroVector;
	FQuat RecoveryTargetRotation = FQuat::Identity;

	float RecoveryCapsuleStartZ = 0.f;
	float RecoveryCapsuleTargetZ = 0.f;
	float RecoveryElapsedTime = 0.f;
	float RecoveryTotalDuration = 0.f;

	/** Mesh relative transform captured before physics first took over */
	FTransform CachedMeshRelativeTransform = FTransform::Identity;
	bool bHasCachedMeshTransform = false;
};
