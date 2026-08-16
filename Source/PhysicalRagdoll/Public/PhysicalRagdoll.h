// Copyright (c) Jared Taylor

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

PHYSICALRAGDOLL_API DECLARE_LOG_CATEGORY_EXTERN(LogPhysicalRagdoll, Log, All);

class FPhysicalRagdollModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
