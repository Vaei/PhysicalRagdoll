// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RagdollTypes.h"

#include "RagdollStatics.generated.h"

class ACharacter;
class USkeletalMeshComponent;
struct FBodyInstance;

/**
 * Per-bone physics blending helpers.
 *
 * USkeletalMeshComponent only exposes whole-subtree operations, which cannot express a group that
 * drives the torso but leaves the arms out. These resolve a weight per body instead, and derive the
 * simulation state from that weight so the two can't disagree.
 *
 * Writing FBodyInstance::PhysicsBlendWeight directly skips the bookkeeping the engine's setters do,
 * so FinalizeMeshPhysics must be called once after any batch of writes. UPhysicsControlComponent
 * writes the weight the same way and skips the same bookkeeping, so this is still needed alongside it.
 */
UCLASS()
class PHYSICALRAGDOLL_API URagdollStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

protected:
	static bool DoAnyPhysicsBodiesHaveWeight(const USkeletalMeshComponent* Mesh);
	static bool ShouldBlendPhysicsBones(const USkeletalMeshComponent* Mesh);
	static bool ShouldRunEndPhysicsTick(const USkeletalMeshComponent* Mesh);
	static bool ShouldRunClothTick(const USkeletalMeshComponent* Mesh);

public:
	/** Bone name for a body, via FBodyInstance::InstanceBoneIndex */
	static FName GetBoneName(const USkeletalMeshComponent* Mesh, const FBodyInstance* BI);

	/** Iterate the bodies at and below a bone. Return false from the lambda to stop early. */
	static int32 ForEach(const USkeletalMeshComponent* Mesh, FName BoneName, bool bIncludeSelf, const TFunctionRef<bool(FBodyInstance*)>& Func);

	/** Set one bone's blend weight, enabling simulation on that body only while the weight is above zero */
	static bool SetBlendWeight(const USkeletalMeshComponent* Mesh, const FName& BoneName, float BlendWeight);

	/** Must be called after a batch of SetBlendWeight calls */
	static void FinalizeMeshPhysics(USkeletalMeshComponent* Mesh);

	/** @return FBodyInstance::PhysicsBlendWeight for the bone */
	UFUNCTION(BlueprintPure, Category=Ragdoll)
	static float GetBoneBlendWeight(const USkeletalMeshComponent* Mesh, const FName& BoneName);

	/** Upgrade collision to include physics, returning the previous state. Physics blending is skipped entirely without it. */
	static ECollisionEnabled::Type EnablePhysicsCollision(USkeletalMeshComponent* Mesh);

	/**
	 * Shape a physical layer from movement, so it reads as a body dealing with its momentum rather than a
	 * constant wobble.
	 *
	 * The push comes from Input.InputAcceleration rather than from the change in velocity, because that
	 * is the mover's intent and it is already at full magnitude on the frame the stick moves. Braking
	 * comes from measured deceleration instead, since releasing the stick zeroes the intent while the
	 * body is still very much slowing down.
	 *
	 * @param Params		Tuning, safe to keep in a data asset or profile
	 * @param State			Carried by the caller across frames, and where the results land
	 * @param Input			What the mover is doing this frame
	 * @param DeltaTime		Seconds since the last call
	 * @param OutAlpha		Feed to URagdollComponent::SetPhysicalAlpha
	 * @param OutStrength	Feed to URagdollComponent::SetPhysicalStrength
	 * @param OutBlendRate	Feed to URagdollComponent::SetPhysicalBlendRate
	 * @param OutBias		Feed to URagdollComponent::AddPhysicalBias
	 * @param OutPushBias	The push half of OutBias on its own
	 * @param OutTurnBias	The turn half of OutBias on its own, for applying or inverting separately
	 * @param AccelerationMultiplier	Multiplied with the parameters' AccelerationScale, for pushing the
	 *									lean harder in a particular situation without editing the asset
	 * @param BrakingMultiplier			Multiplied with the parameters' BrakingScale, likewise
	 */
	UFUNCTION(BlueprintCallable, Category=Ragdoll, meta=(AdvancedDisplay="AccelerationMultiplier,BrakingMultiplier"))
	static void CalculateMotionDrive(const FRagdollMotionDrive& Params, UPARAM(ref) FRagdollMotionDriveState& State,
		const FRagdollMotionInput& Input, float DeltaTime, float& OutAlpha, float& OutStrength, FVector& OutBias,
		FVector& OutPushBias, FVector& OutTurnBias, float& OutBlendRate,
		float AccelerationMultiplier = 1.f, float BrakingMultiplier = 1.f);

	/** CalculateMotionDrive, filling the input off the character's movement component */
	UFUNCTION(BlueprintCallable, Category=Ragdoll, meta=(AdvancedDisplay="AccelerationMultiplier,BrakingMultiplier"))
	static void CalculateMotionDriveForCharacter(const FRagdollMotionDrive& Params, UPARAM(ref) FRagdollMotionDriveState& State,
		const ACharacter* Character, float DeltaTime, float& OutAlpha, float& OutStrength, FVector& OutBias,
		FVector& OutPushBias, FVector& OutTurnBias, float& OutBlendRate,
		float AccelerationMultiplier = 1.f, float BrakingMultiplier = 1.f);

	/** Fill a motion input from a character's movement component */
	UFUNCTION(BlueprintPure, Category=Ragdoll)
	static FRagdollMotionInput MakeMotionInputFromCharacter(const ACharacter* Character);

	/**
	 * Draw what the motion drive is doing. Call after CalculateMotionDrive with the same input and state.
	 *
	 * White is the direction of travel, cyan the raw input acceleration, green the push along travel,
	 * red the braking along travel, and thick yellow the final bias that reaches the bodies. When the
	 * yellow points behind the white, that is the backward lean, and whichever of green or red is
	 * winning tells you which term produced it.
	 */
	UFUNCTION(BlueprintCallable, Category=Ragdoll, meta=(WorldContext="WorldContextObject", AdvancedDisplay="DrawScale"))
	static void DrawMotionDriveDebug(const UObject* WorldContextObject, const FVector& Origin,
		const FRagdollMotionInput& Input, const FRagdollMotionDriveState& State, float DrawScale = 0.05f);

	/**
	 * Shape a physical layer from the surface the character is riding, so a body on a moving base reacts
	 * to it. Feed State.Bias to AddPhysicalBias and State.Torque to AddPhysicalTorque.
	 *
	 * BaseComponent is passed rather than looked up, so this works for anything that carries a character:
	 * a lift, a vehicle, a rocking ship deck the character is not technically based on. Leaving it null
	 * falls back to Character's movement base, and Character is separate for the same reason - the thing
	 * being ridden and the thing riding it are not always related the way the movement code assumes.
	 *
	 * @param Params			Tuning, safe to keep in a data asset or profile
	 * @param State				Carried by the caller across frames, and where the results land
	 * @param BaseComponent		Surface being ridden. Null falls back to Character's movement base.
	 * @param Character			Used for the fallback base and for where on the base to measure
	 * @param DeltaTime			Seconds since the last call
	 * @param OutBias			Feed to URagdollComponent::AddPhysicalBias
	 * @param OutTorque			Feed to URagdollComponent::AddPhysicalTorque
	 */
	UFUNCTION(BlueprintCallable, Category=Ragdoll)
	static void CalculateBaseDrive(const FRagdollBaseDrive& Params, UPARAM(ref) FRagdollBaseDriveState& State,
		UPrimitiveComponent* BaseComponent, const ACharacter* Character, float DeltaTime,
		FVector& OutBias, FVector& OutTorque);

	UFUNCTION(BlueprintCallable, Category=Ragdoll)
	static void CalculateBoneDeltaDrive(const FRagdollBoneDeltaDrive& Params, UPARAM(ref) FRagdollBoneDeltaDriveState& State,
		const USkeletalMeshComponent* Mesh, float DeltaTime, float& OutAlpha);
};
