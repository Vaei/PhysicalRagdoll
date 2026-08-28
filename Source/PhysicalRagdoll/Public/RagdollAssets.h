// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RagdollTypes.h"

class UPhysicsAsset;

#include "RagdollAssets.generated.h"

/**
 * Tuning held in an asset rather than on the component, so it can be edited while the game is running.
 *
 * A component reading its settings from one of these picks up every change immediately, which is the
 * difference between tuning a spring in one session and restarting for each value.
 */
UCLASS(Abstract)
class PHYSICALRAGDOLL_API URagdollTuningAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Bumped on every edit, so a component holding cached setup knows to rebuild it */
	uint32 GetRevision() const { return Revision; }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		Super::PostEditChangeProperty(PropertyChangedEvent);
		++Revision;
	}
#endif

private:
	uint32 Revision = 1;
};

/** One entry of URagdollComponent::PhysicalProfiles. @see URagdollComponent::PhysicalProfileAssets */
UCLASS(BlueprintType)
class PHYSICALRAGDOLL_API URagdollProfileAsset : public URagdollTuningAsset
{
	GENERATED_BODY()

public:
	/**
	 * Physics asset the constraint profile names are read from.
	 * Editor only: an asset has no mesh to ask, so without it the profile pickers below have nothing to list.
	 */
	UPROPERTY(EditAnywhere, Category=Physical)
	TSoftObjectPtr<UPhysicsAsset> ConstraintProfileSource;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Physical, meta=(ShowOnlyInnerProperties))
	FRagdollPhysicalProfile Profile;

	UFUNCTION()
	TArray<FString> GetConstraintProfileOptions() const;
};

/** @see URagdollStatics::CalculateMotionDrive */
UCLASS(BlueprintType)
class PHYSICALRAGDOLL_API URagdollMotionDriveAsset : public URagdollTuningAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Motion, meta=(ShowOnlyInnerProperties))
	FRagdollMotionDrive Params;
};

/** @see URagdollStatics::CalculateBaseDrive */
UCLASS(BlueprintType)
class PHYSICALRAGDOLL_API URagdollBaseDriveAsset : public URagdollTuningAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Base)
	FRagdollBaseDrive Params;

	/** How the base's own motion is measured before any of the above reads it */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Base)
	FRagdollBaseMotion Motion;
};
