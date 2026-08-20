// Copyright (c) Jared Taylor

using UnrealBuildTool;

public class PhysicalRagdoll : ModuleRules
{
	public PhysicalRagdoll(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"GameplayTags",
				"PhysicsCore",
				"PhysicsControl",
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
			}
			);
	}
}
