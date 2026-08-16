// Copyright (c) Jared Taylor

#pragma once

#include "NativeGameplayTags.h"

namespace FPhysicalRagdollTags
{
	PHYSICALRAGDOLL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ragdoll_Profile);
	PHYSICALRAGDOLL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ragdoll_Suspend);

	/** Reserved for the component's own state query, so it cannot collide with a caller's reason */
	PHYSICALRAGDOLL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ragdoll_Suspend_State);

	/** Reserved for p.Ragdoll.Enable, so toggling it cannot resume a suspension gameplay asked for */
	PHYSICALRAGDOLL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ragdoll_Suspend_Disabled);
}
