# Physical Ragdoll <img align="right" width=128, height=128 src="https://github.com/Vaei/PhysicalRagdoll/blob/main/Resources/Icon128.png">

> [!IMPORTANT]
> **Physically Animated Characters**
> <br>A persistent physics layer blended over regular animation
> <br>Plus ragdoll and get-up recovery
> <br>And its **FREE!**

> [!TIP]
> Supports UE5.8+

> [!CAUTION]
> Physical Ragdoll is a fully experimental prototype that may eventually be deleted entirely, or changed in a significant way. It is not fully released so expect bugs and undocumented changes.

## Features

### Physical Layer

Blends physics into your animations creating a procedural effect.

Supports different states keyed by `FGameplayTag`.

Supports bone exclusion via custom blending, allowing you to remove any procedural motion from extents such as hands while holding a weapon as the rest of the body continues to simulate.

Switching profiles cross-fades every group, including groups that only exist in one of the two profiles. `SetPhysicalAlpha()` scales the whole layer on top of that, for ramping physicality with speed, stamina, or anything else.

### Motion Drive

A constant physics layer reads as a wobble. `URagdollStatics::CalculateMotionDrive` shapes it from the character's own movement so it reads as a body dealing with its momentum.

```cpp
float Alpha, Strength, BlendRate;
FVector Bias, PushBias, TurnBias;
URagdollStatics::CalculateMotionDriveForCharacter(MotionDriveParams, MotionDriveState, this, DeltaTime,
	Alpha, Strength, Bias, PushBias, TurnBias, BlendRate);
Ragdoll->SetPhysicalAlpha(Alpha);
Ragdoll->SetPhysicalStrength(Strength);
Ragdoll->AddPhysicalBias(SpecificBiasBone, Bias * Alpha, HeightOffset);  // spine_03, 20.f height offset - starting values for you
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

#### Example Motion Drive

This is what my example project did:
```cpp
static constexpr float BrakingStrengthScale = 3.f;
static constexpr float BrakingRateScale = 5.f;
static constexpr float BiasHeightOffset = 20.f;
static const FName BiasBoneName(TEXT("spine_03"));

const bool bHasInput = GetCharacterMovement() && GetCharacterMovement()->GetCurrentAcceleration().Size2D() > 1.f;
const float StrengthScale = bHasInput ? 1.f : BrakingStrengthScale;
const float BlendRateScale = bHasInput ? 1.f : BrakingRateScale;

float MotionAlpha, MotionStrength, BlendRate;
FVector MotionBias, PushBias, TurnBias;
URagdollStatics::CalculateMotionDriveForCharacter(Ragdoll->MotionDriveParams, Ragdoll->MotionDriveState,
	this, DeltaTime, MotionAlpha, MotionStrength, MotionBias, PushBias, TurnBias, BlendRate);

Ragdoll->SetPhysicalStrength(MotionStrength * StrengthScale);
Ragdoll->SetPhysicalBlendRate(BlendRate * BlendRateScale);
Ragdoll->AddPhysicalBias(MotionBias * MotionAlpha, BiasBoneName, BiasHeightOffset);
```

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

### Physics Asset Profiles

Every state can optionally take its settings from the constraint profiles authored in the physics asset, picked from a dropdown listing that asset's profile names.

| Setting | Where | Effect |
| --- | --- | --- |
| `ControlDataConstraintProfile` | Per bone group, and on `RagdollSettings` | Initializes those bodies' strengths from the named constraint profile's joint drives instead of `ControlData` |
| `ConstraintProfile` | Per physical profile, and on `RagdollSettings` | Applied to every joint while that state is active |
| `DefaultConstraintProfile` | Component | What constraints revert to once a state that applied one ends |

All optional: leave any of them as `None` to keep the inline settings and leave constraints untouched.

The dropdowns list the names from the owner's physics asset, resolved from its skeletal mesh. Set `ProfileSourcePhysicsAsset` when there is no owner to resolve from - editing a Blueprint of the component itself, or a mesh only assigned at runtime.

### Physics Control

The drive is `UPhysicsControlComponent` from the engine's PhysicsControl plugin. `URagdollComponent` creates one on the owner if there isn't one already, and makes it tick after itself.

Each driven bone gets a control (the spring/damper toward the animated pose) and a body modifier (simulation state and blend weight), both named `Ragdoll_<Bone>` and in the set `Ragdoll`, so anything else on the same control component is untouched.

Strengths are in `FPhysicsControlData` terms rather than raw stiffness: `Stiffness = (Strength * 2pi)^2`, and damping comes from `DampingRatio` where 1 is critically damped. `AngularStrength` 3.2 is the old `OrientationStrength` 400.

## How to Use

Add a `URagdollComponent` to your character. The owner needs a skeletal mesh with a physics asset.

> [!TIP]
> Epic's physics assets are subpar. There is a considerably improved physics asset included in the plugin's content folder titled `BetterPhysicsAsset` - It is shipped on it's own without a skeleton or mesh, press "no" when prompted to remove bones, and select UE5 Manny or Quinn to assign it to them

`Ragdoll.Profile` is declared natively as the parent tag. Add your own profile tags under it, then add matching entries to `PhysicalProfiles`, e.g.:

| Profile | Bone Groups | Overrides | AngularStrength |
| --- | --- | --- | --- |
| `Ragdoll.Profile.Subtle` | `spine_01` | `clavicle_l`/`clavicle_r` disabled | 3.2 |
| `Ragdoll.Profile.Flail` | `spine_01` | none | 0.9 |

Leave `BlendWeight` at 1 and tune the feel with `AngularStrength` (with `AngularDampingRatio` near 1 for a settled body, lower for a looser one). `BlendWeight` is for fading the layer in and out.

Two things will silently produce no visible effect:

- **The mesh needs physics collision.** `USkeletalMeshComponent::ShouldBlendPhysicsBones()` returns false without it and the engine skips per-bone blending entirely, and `ACharacter`'s default `CharacterMesh` profile is query only. `bAutoEnablePhysicsCollision` handles this by upgrading the mesh while simulating and restoring it afterwards.
- **Don't drive the physics asset's root body** (usually `pelvis`) in parent space. A parent space control needs a parent body to drive against, and the root body has none, so the group silently falls back to world space. Start groups below it, or set `ControlType` to `WorldSpace`. The component logs a warning if a profile does this.

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

<img width="589" height="640" alt="UnrealEditor-Win64-DebugGame_2026-08-17_10-39-22" src="https://github.com/user-attachments/assets/9cd28a02-f01b-476b-99dc-fb1d67ab7aaf" />

Then in your character's tick from either C++ or BP:

<img width="1403" height="717" alt="UnrealEditor-Win64-DebugGame_2026-08-17_10-46-11" src="https://github.com/user-attachments/assets/a7df52e2-c71f-447c-b8ec-1a0b331e4ea1" />

Test, then tune from there.

### Console Commands

| Command | Description |
| --- | --- |
| `p.Ragdoll.Death` | Toggle ragdoll and recovery on the player's character |
| `p.Ragdoll.Profile <Tag>` | Apply a physical profile by tag. No argument clears it. |
| `p.Ragdoll.DumpBodies` | Log every body's simulation state and blend weight |

## Changelog

### 1.0.0
* Initial Release
