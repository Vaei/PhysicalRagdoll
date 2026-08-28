// Copyright (c) Jared Taylor

#include "RagdollAssets.h"

#include "PhysicsEngine/PhysicsAsset.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RagdollAssets)

TArray<FString> URagdollProfileAsset::GetConstraintProfileOptions() const
{
	TArray<FString> Options { TEXT("None") };

	if (const UPhysicsAsset* PhysicsAsset = ConstraintProfileSource.LoadSynchronous())
	{
		for (const FName& ProfileName : PhysicsAsset->GetConstraintProfileNames())
		{
			Options.Add(ProfileName.ToString());
		}
	}

	return Options;
}
