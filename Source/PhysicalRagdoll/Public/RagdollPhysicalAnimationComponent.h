// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"

#include "RagdollPhysicalAnimationComponent.generated.h"

/**
 * Safety wrapper around UPhysicalAnimationComponent.
 * Guards against engine crash when bone transforms are empty during tick.
 */
UCLASS(ClassGroup=(Physics), Blueprintable, meta=(BlueprintSpawnableComponent))
class PHYSICALRAGDOLL_API URagdollPhysicalAnimationComponent : public UPhysicalAnimationComponent
{
	GENERATED_BODY()

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
};
