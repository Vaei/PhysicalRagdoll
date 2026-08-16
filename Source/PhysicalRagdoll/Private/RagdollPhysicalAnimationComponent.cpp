// Copyright (c) Jared Taylor

#include "RagdollPhysicalAnimationComponent.h"

#include "Components/SkeletalMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RagdollPhysicalAnimationComponent)

void URagdollPhysicalAnimationComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	// Guard against engine crash in UpdatePhysicsEngineImp when bone transforms are empty.
	// This can happen when physics blending is enabled mid-tick before animation evaluation completes.
	if (USkeletalMeshComponent* SkelMesh = GetSkeletalMesh())
	{
		if (SkelMesh->GetBoneSpaceTransformsView().IsEmpty())
		{
			return;
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}
