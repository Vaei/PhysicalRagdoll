# Physical Ragdoll

> [!IMPORTANT]
> **Physically Animated Characters**
> <br>A persistent physics layer blended over regular animation
> <br>Plus ragdoll and get-up recovery
> <br>And its **FREE!**

> [!TIP]
> Supports UE5.8+

> [!CAUTION]
> Physical Ragdoll is a fully experimental prototype that may eventually be deleted entirely, or changed in a significant way

## Features

### Physical Layer

Blends physics into your animations creating a procedural effect.

Supports different states keyed by `FGameplayTag`.

Supports bone exclusion via custom blending, allowing you to remove any procedural motion from extents such as hands while holding a weapon as the rest of the body continues to simulate.

Switching profiles cross-fades every group, including groups that only exist in one of the two profiles. `SetPhysicalAlpha()` scales the whole layer on top of that, for ramping physicality with speed, stamina, or anything else.

### Motion Drive

A constant physics layer reads as a wobble. `URagdollStatics::CalculateMotionDrive` shapes it from the character's own movement so it reads as a body dealing with its momentum.

```cpp
float Alpha, Strength;
FVector Bias;
URagdollStatics::CalculateMotionDriveForCharacter(MotionParams, MotionState, Character, DeltaTime, Alpha, Strength, Bias);
Ragdoll->SetPhysicalAlpha(Alpha);
Ragdoll->SetPhysicalStrength(Strength);
Ragdoll->AddPhysicalBias(Bias);
```

Speed scales the whole layer, through a curve or a plain range - a character standing still barely moves, one at a sprint carries full physicality.

The rest comes from acceleration, split relative to the direction of travel, because the three cases read as different things on a body and want separate scales:

| Component | Case | Property |
| --- | --- | --- |
| Along travel, gaining speed | Pushing off | `AccelerationScale` |
| Along travel, losing speed | Planting to stop | `BrakingScale` |
| Across travel | Input cutting against your own momentum | `TurnScale` |

The body leans **into** each of them, the way a person does when they are the one generating the movement: forward to push off, back to plant when stopping, into the turn when cutting across.

Two things keep this from looking floppy:

- The bias is applied as a **mass-normalised acceleration** (`bAccelChange`), so a hand leans by exactly as much as a torso. Applying it as force instead is what flings light bones around and reads as silly.
- `MaxBias` caps it. Uncapped, a hard direction change produces an acceleration spike that throws the whole upper body.

`AddPhysicalBias` applies for one frame only, so it has to be called every frame to sustain a lean. It takes an optional bone to scope the lean to part of the body; with none it covers the active profile's groups.

### Bone Delta Drive

`URagdollStatics::CalculateBoneDeltaDrive` is a second way to scale the layer, measuring how fast the animation is moving a reference bone instead of how fast the capsule is moving.

```cpp
URagdollStatics::CalculateBoneDeltaDrive(DeltaParams, DeltaState, Character->GetMesh(), DeltaTime);
Ragdoll->SetPhysicalAlpha(MotionState.Alpha * DeltaState.Alpha);
```

> [!TIP]
> You can normalize the output alpha of `CalculateBoneDeltaDrive` and `CalculateMotionDrive/ForCharacter` to get a combined result

`Exponent`: Above 1, gentle animation contributes almost nothing while sharp animation ramps hard, so the layer stays out of the way until the animation earns it.

Linear and angular speed are measured separately against their own ranges and weights, then the larger of the two drives the result - a bone can spin without travelling, or travel without spinning, and either should count.

`Space` picks what the delta means. `Local` measures relative to the mesh so only the animation counts; `World` folds in the actor's own movement as well.

> [!WARNING]
> The reference bone must be one the active profile does not drive, or physics feeds back into its own input and the layer runs away

### Ragdoll

> [!NOTE]
> Unlike a regular ragdoll, this supports blending with animation, so you can influence the ragdoll with your death montage or pose

Physics takes over the character with an impulse, a blend in, and a motor decay so the character sags into the ragdoll rather than dropping instantly. The capsule follows the simulated body, and the mesh's collision profile is swapped for the duration.

Bones listed in `SeparatedBones` have their constraint broken on ragdoll start, letting a weapon or a hat come free and simulate independently.

Entering ragdoll from a physical profile starts at the blend weight the layer had already reached, so there is no pop.

### Recovery

Get up out of ragdoll, driven by a per-side montage or by a timer when no montage is available. The get-up side is chosen from the orientation of the simulation root, and the mesh transform and capsule height blend from the ragdoll pose back to standing across the montage.

The physical profile that was active before the ragdoll is reapplied once recovery completes.

#### Anim Graph Setup

Coming soon

<!--TODO ABP setup -->

### Engine Crash Fix

`URagdollPhysicalAnimationComponent` extends `UPhysicalAnimationComponent` and guards against the engine crash in `UpdatePhysicsEngineImp` when bone transforms are empty during tick. `URagdollComponent` creates one automatically if the owner doesn't already have a physical animation component, and warns if it finds an unguarded one.

## How to Use

Add a `URagdollComponent` to your character. The owner needs a skeletal mesh with a physics asset.

> [!TIP]
> Epic's physics assets are subpar. There is a considerably improved physics asset included in the plugin's content folder titled `BetterPhysicsAsset` - It is shipped on it's own without a skeleton or mesh, press "no" when prompted to remove bones, and select UE5 Manny or Quinn to assign it to them

`Ragdoll.Profile` is declared natively as the parent tag. Add your own profile tags under it, then add matching entries to `PhysicalProfiles`, e.g.:

| Profile | Bone Groups | Overrides | OrientationStrength |
| --- | --- | --- | --- |
| `Ragdoll.Profile.Subtle` | `spine_01` | `clavicle_l`/`clavicle_r` disabled | 400 |
| `Ragdoll.Profile.Flail` | `spine_01` | none | 30 |

Leave `BlendWeight` at 1 and tune the feel with `OrientationStrength` (with `AngularVelocityStrength` around a tenth of it, as damping). `BlendWeight` is for fading the layer in and out.

Two things will silently produce no visible effect:

- **The mesh needs physics collision.** `USkeletalMeshComponent::ShouldBlendPhysicsBones()` returns false without it and the engine skips per-bone blending entirely, and `ACharacter`'s default `CharacterMesh` profile is query only. `bAutoEnablePhysicsCollision` handles this by upgrading the mesh while simulating and restoring it afterwards.
- **Don't drive the physics asset's root body** (usually `pelvis`) in local simulation. Local simulation zeroes the linear drive, so the root body has nothing holding it in place and falls through the world. Start groups below it, or clear `bIsLocalSimulation` and set `PositionStrength`. The component logs a warning if a profile does this.

Then drive it from gameplay:

```cpp
Ragdoll->SetPhysicalProfile(SubtleTag);     // hold a subtle overlap
Ragdoll->SetPhysicalAlpha(SpeedAlpha);      // scale it with movement speed
Ragdoll->SetPhysicalProfile(FlailTag);      // cross-fade to a full body flail
Ragdoll->ClearPhysicalProfile();            // blend the layer out entirely

Ragdoll->RagdollDeath(Impulse);             // physics takes over
Ragdoll->StartRecovery();                   // get up, then restore the profile
```

Set `AutoPhysicalProfile` to apply a profile on BeginPlay.

### Demo Setup

_To get an always-on procedural layer setup as a starting point_

**Assuming the properties on your ragdoll component are all at default**

Assign `AutoPhysicalProfile` as `Profile.Ragdoll`

In your character C++ or BP:

Add `FRagdollMotionDriveState MotionDriveState;` and `FRagdollMotionDrive MotionDriveParams;`

Use these params as a starting point:

<!-- TODO image -->

Then in your character's tick from either C++ or BP:

<!-- TODO image -->

### Console Commands

| Command | Description |
| --- | --- |
| `p.Ragdoll.Death` | Toggle ragdoll and recovery on the player's character |
| `p.Ragdoll.Profile <Tag>` | Apply a physical profile by tag. No argument clears it. |
| `p.Ragdoll.DumpBodies` | Log every body's simulation state and blend weight |

## Changelog

### 1.0.0
* Initial Release
