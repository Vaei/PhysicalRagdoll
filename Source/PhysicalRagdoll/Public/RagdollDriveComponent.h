// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "RagdollComponent.h"

#include "RagdollDriveComponent.generated.h"

class URagdollBaseDriveAsset;
class URagdollMotionDriveAsset;

/**
 * Ragdoll component with the two always-on drives, plus a one-off impulse channel.
 * Motion drive reads the owner's own movement, base drive reads whatever is carrying it.
 *
 * The drives sit below gameplay: they take the layer only when nothing else holds it, and the base drive
 * outranks the motion drive while something is actually carrying the owner and it is not being moved.
 */
UCLASS(ClassGroup=(Physics), Blueprintable, meta=(BlueprintSpawnableComponent))
class PHYSICALRAGDOLL_API URagdollDriveComponent : public URagdollComponent
{
	GENERATED_BODY()

public:
	URagdollDriveComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// ---- Motion drive ----

	/** Profile the layer runs while the motion drive owns it. Invalid leaves the motion drive nothing to run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Motion Drive", meta=(Categories="Ragdoll.Profile"))
	FGameplayTag MotionDriveProfile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Motion Drive")
	ERagdollTuningSource MotionDriveSource = ERagdollTuningSource::Inline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Motion Drive", meta=(EditCondition="MotionDriveSource == ERagdollTuningSource::Inline", EditConditionHides))
	FRagdollMotionDrive MotionDriveParams;

	/** Edits to it apply during play, which the inline settings cannot do */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Motion Drive", meta=(EditCondition="MotionDriveSource == ERagdollTuningSource::Asset", EditConditionHides))
	TObjectPtr<URagdollMotionDriveAsset> MotionDriveAsset;

	/** @return the motion drive tuning in use, from the asset or inline per MotionDriveSource */
	const FRagdollMotionDrive& GetMotionDriveParams() const;

	UPROPERTY(Transient, BlueprintReadWrite, Category="Ragdoll|Motion Drive")
	FRagdollMotionDriveState MotionDriveState;

	// ---- Base drive ----

	/** Profile the layer runs while the base drive owns it. Invalid leaves the base drive with nothing to run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Base Drive", meta=(Categories="Ragdoll.Profile"))
	FGameplayTag BaseDriveProfile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Base Drive")
	ERagdollTuningSource BaseDriveSource = ERagdollTuningSource::Inline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Base Drive", meta=(EditCondition="BaseDriveSource == ERagdollTuningSource::Inline", EditConditionHides))
	FRagdollBaseDrive BaseDriveParams;

	/** Edits to it apply during play, which the inline settings cannot do */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Base Drive", meta=(EditCondition="BaseDriveSource == ERagdollTuningSource::Asset", EditConditionHides))
	TObjectPtr<URagdollBaseDriveAsset> BaseDriveAsset;

	/** @return the base drive tuning in use, from the asset or inline per BaseDriveSource */
	const FRagdollBaseDrive& GetBaseDriveParams() const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Base Drive", meta=(EditCondition="BaseDriveSource == ERagdollTuningSource::Inline", EditConditionHides))
	FRagdollBaseMotion BaseMotionParams;

	/** @return the base motion tuning in use, from the asset or inline per BaseDriveSource */
	const FRagdollBaseMotion& GetBaseMotionParams() const;

	/** 1 while the mover has input, interpolating back to 0 once it stops */
	UPROPERTY(Transient, BlueprintReadOnly, Category="Ragdoll|Base Drive")
	float BaseDriveInputAlpha = 0.f;

	UPROPERTY(Transient, BlueprintReadWrite, Category="Ragdoll|Base Drive")
	FRagdollBaseDriveState BaseDriveState;

	UPROPERTY(Transient, BlueprintReadWrite, Category="Ragdoll|Base Drive")
	FRagdollBaseMotionState BaseMotionState;

	// ---- Impulse ----

	/** Ceiling on stacked impulses, so a pile of hits landing in one frame cannot throw the body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Impulse", meta=(UIMin="0", ForceUnits="cm/s2"))
	float MaxImpulseBias = 6000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll|Impulse", meta=(UIMin="0"))
	float MaxImpulseTorque = 60.f;

	UPROPERTY(Transient, BlueprintReadWrite, Category="Ragdoll|Impulse")
	FRagdollImpulseState ImpulseState;

public:
	/**
	 * Run both drives and the impulse channel, and combine what they produce into the physical layer.
	 * Call every frame from the owner's tick.
	 */
	UFUNCTION(BlueprintCallable, Category=Ragdoll)
	void TickDrives(float DeltaTime);

	/** Whether the body leans into its own momentum. Override to gate the drive on gameplay state. */
	UFUNCTION(BlueprintPure, Category="Ragdoll|Motion Drive")
	virtual bool IsMotionDriveEnabled() const { return true; }

	/** Whether the body reacts to whatever is carrying it, a rocking deck above all. */
	UFUNCTION(BlueprintPure, Category="Ragdoll|Base Drive")
	virtual bool IsBaseDriveEnabled() const { return true; }

	/** What the base drive rides. Null means nothing is carrying the owner. */
	UFUNCTION(BlueprintPure, Category="Ragdoll|Base Drive")
	virtual UPrimitiveComponent* GetDriveBase() const;

	/** Call every frame, enabled or not, so the frame it turns on is not measured against a stale transform */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|Base Drive")
	void UpdateBaseMotion(float DeltaTime);

	/** Lean and roll the base asks for this frame */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|Base Drive")
	void UpdateBaseDrive(float DeltaTime, float Alpha, FVector& OutBias, FVector& OutTorque);

	/** Kick the body once. HalfLife long reads as a stumble, short as a jolt. */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|Impulse")
	void AddDriveImpulse(FVector Bias, FVector Torque = FVector::ZeroVector, float HalfLife = 0.2f);

	/** Decay the live impulse. Call every frame. */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|Impulse")
	void UpdateDriveImpulse(float DeltaTime);

	UFUNCTION(BlueprintPure, Category="Ragdoll|Impulse")
	bool HasActiveDriveImpulse() const { return ImpulseState.IsActive(); }

	/** Drop every drive's carried state, so the next frame one runs measures against a fresh sample */
	UFUNCTION(BlueprintCallable, Category=Ragdoll)
	void ResetDrives();

	/** True for either drive's profile, so an owner can tell one it may replace from one gameplay asked for */
	UFUNCTION(BlueprintPure, Category=Ragdoll)
	bool IsDriveProfile(FGameplayTag ProfileTag) const;

protected:
	/** Seed a profile entry from the plugin's ready made one, so a drive tag resolves without authoring */
	void AddDefaultDriveProfile(const FGameplayTag& ProfileTag);

	/** Whether any drive ran last frame, so the carried state is dropped once on the falling edge */
	bool bDrivesActive = false;

#if !UE_BUILD_SHIPPING
	/** What the base drive is reading and what it made of it. Console command: p.Ragdoll.DebugBase */
	void DrawBaseDriveDebug() const;
#endif
};
