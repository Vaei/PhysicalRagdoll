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

- Physics layer blended over regular animation
- Multiple states, keyed by `FGameplayTag`, cross-fading between them
- Per-bone exclusion from the layer
- Motion drive: shapes the layer from the character's own movement
- Base drive: shapes the layer from the tilt and motion of whatever is carrying the character
- Lean: reacts by moving what the motors hold, so the layer can stay true to the animation and still be pushed
- Bone delta drive: shapes the layer from how fast the animation is moving
- Ragdoll, blendable with animation
- Get-up recovery from ragdoll with montage support
- Suspension by tag, without losing the active profile
- Settings sourced from the physics asset's constraint profiles
- LOD and performance considerations

## Setup

> [!CAUTION]
> OUT OF DATE - NOT READY

### 1. Add the component

Add a `URagdollComponent` to your character. The owner needs a skeletal mesh with a physics asset.

> [!TIP]
> Epic's physics assets are subpar. There is a considerably improved physics asset included in the plugin's content folder titled `BetterPhysicsAsset` - It is shipped on it's own without a skeleton or mesh, press "no" when prompted to remove bones, and select UE5 Manny or Quinn to assign it to them

### 2. Add a profile

`PhysicalProfiles` already ships one entry under the natively declared `Ragdoll.Profile`, set up as a working always-on layer. Assign it to `AutoPhysicalProfile` and you have something on screen.

It is a single `spine_01` group in world space, `StrengthMultiplier` 2, `BlendWeight` 0.6, `AngularStrength` 3.2 with `AngularDampingRatio` 0.25, `LinearStrength` 0.36 with `LinearDampingRatio` 1.1. On top of that, `spine_05` and `neck_01` are scaled down to 0.35 and 0.2 so the head does not swim, and the forearms are disabled outright so the hands hold still.

Add your own tags under `Ragdoll.Profile` and copy that entry as the starting point for each. Switch between them from gameplay:

```cpp
Ragdoll->SetPhysicalProfile(SubtleTag);     // hold a subtle overlap
Ragdoll->SetPhysicalProfile(FlailTag);      // cross-fade to a full body flail
Ragdoll->ClearPhysicalProfile();            // blend the layer out entirely
```

### 3. Drive it from movement

Hold a `FRagdollMotionDrive MotionDriveParams` and a `FRagdollMotionDriveState MotionDriveState`, then call this **every frame** from your character's tick, in C++ or Blueprint:

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

Two things that snippet is doing on purpose:

- The alpha scales the **lean**, not the layer. Handing it to `SetPhysicalAlpha` instead thins the blend weight while leaving the push at full size, which is a different effect and reads as the body being shaken.
- The bias is scoped to a bone, covering everything below it. Passing no bone covers every group the profile drives, arms and legs included, and is refused unless `p.Ragdoll.AllowUnscopedPush` is set.

`AddPhysicalBias` applies for one frame only, so it has to be called every frame to sustain a lean. `AddPhysicalTorque` is the same.

Start from these parameters:

<img width="589" height="640" alt="UnrealEditor-Win64-DebugGame_2026-08-17_10-39-22" src="https://github.com/user-attachments/assets/9cd28a02-f01b-476b-99dc-fb1d67ab7aaf" />

The same call in Blueprint:

<img width="1403" height="717" alt="UnrealEditor-Win64-DebugGame_2026-08-17_10-46-11" src="https://github.com/user-attachments/assets/a7df52e2-c71f-447c-b8ec-1a0b331e4ea1" />

> [!WARNING]
> You will very likely have poor results immediately due to `MaxBias` on the `MotionDriveParams`; this needs to be tuned to suit your project's movement, if the default `4000.f` is too high then try `1200.f` and go from there.

### 4. Optionally scale it from the animation as well

```cpp
URagdollStatics::CalculateBoneDeltaDrive(DeltaParams, DeltaState, Character->GetMesh(), DeltaTime);
Ragdoll->SetPhysicalAlpha(MotionState.Alpha * DeltaState.Alpha);
```

> [!TIP]
> You can normalize the output alpha of `CalculateBoneDeltaDrive` and `CalculateMotionDrive/ForCharacter` to get a combined result

### 5. Ragdoll and recovery

```cpp
Ragdoll->RagdollDeath(Impulse);             // physics takes over
Ragdoll->StartRecovery();                   // get up, then restore the profile
```

#### Anim Graph Setup

Coming soon

<!--TODO ABP setup -->

### Check it is working

| Console | Description |
| --- | --- |
| `p.Ragdoll.TestBias <cm/s2>` | Bias the driven bodies along the actor's forward axis, ignoring movement. Stand still and set it: positive should visibly pitch the torso forward. |
| `p.Ragdoll.DumpBodies` | Log every body's simulation state and blend weight |
| `p.Ragdoll.DebugMotion` | Draw the bias reaching the bodies, and report when nothing is being applied |
| `p.Ragdoll.Death` | Toggle ragdoll and recovery on the player's character |
| `p.Ragdoll.Profile <Tag>` | Apply a physical profile by tag. No argument clears it. |
| `p.Ragdoll.Enable 0` | Take the layer off every character. Suspensions gameplay asked for are untouched. |

## Troubleshooting

**Nothing happens at all.** The mesh needs physics collision. `USkeletalMeshComponent::ShouldBlendPhysicsBones()` returns false without it and the engine skips per-bone blending entirely, and `ACharacter`'s default `CharacterMesh` profile is query only. `bAutoEnablePhysicsCollision` handles this by upgrading the mesh while simulating and restoring it afterwards.

**A group does nothing, or ignores the parent space setting.** Don't drive the physics asset's root body (usually `pelvis`) in parent space. A parent space control needs a parent body to drive against, and the root body has none, so the group silently falls back to world space. Start groups below it, or set `ControlType` to `WorldSpace`. The component logs a warning if a profile does this.

**The whole body shakes instead of leaning.** The push is unscoped. Pass the bone to push from; `spine_03` with a `20.f` height offset is a good starting point.

**It gets violent on a hard direction change.** `MaxBias` is too high for your movement.

**The lean is there but the body looks thin and washed out.** The motion drive alpha has been passed to `SetPhysicalAlpha` rather than multiplied into the bias. See step 3.

**The layer runs away and never settles.** A bone delta drive is reading a bone the active profile drives, so physics is feeding back into its own input.

**A lean lasts one frame.** `AddPhysicalBias` has to be called every frame to sustain it.

**A joint driven with `bUseJointDrives` ignores its motor entirely.** Chaos throws the SLERP drive away on any joint whose twist or either swing is set to Locked, and looks for the twist and swing drives instead. The group turns all of them on and picks the mode to match, so check the physics asset if a joint still hangs loose: a Locked axis with no drive behind it is a free joint inside its limits, and no strength value changes that.

**The body is floppy and no stiffness helps.** The drive is not reaching the bodies. Chaos drives saturate rather than ring, so a stiffness that does nothing at any value means there is no drive at all: check the physics asset for a locked angular axis on a `bUseJointDrives` group, and check `MaxDriveFrequency` is not capping the group down to nothing.

**Making it hold the animation kills the reaction, and making the reaction survive makes it floppy.** They are the same knob pulling opposite ways, because the reaction is a force and the motors exist to cancel forces. Use `SetPhysicalLean` for the reaction instead. See the Lean section.

**Every driven bone goes limp and no strength changes it.** `bOnlyControlChildObject` is on. It reads as though it protects the parent, but the control's constraint is built child-first, and Chaos scales body 0 by `ParentInvMassScale`, so it pins the child instead and the drive can only push a parent that is usually kinematic. Leave it off.

**The base drive barely registers, and raising the strength makes it worse.** The base's tilt is not being read. `TiltScale` is the term that carries a slow roll; velocity and acceleration go to nothing as soon as the base settles at an angle. `URagdollStatics::DrawBaseDriveDebug` prints what each term is worth.

**The base drive jitters, worst in the arms.** The acceleration term is a second difference of the base's transform, so a base moved by replication or a smoothing component arrives noisy. Lower the acceleration smoothing rate, or lean on tilt and rotation instead, neither of which differentiates twice.

## Tuning

### Physical profiles

`AngularStrength` is a frequency, not a torque: `Stiffness = (Strength * 2pi)^2`. Get it right before anything else, and expect the number to be far larger than it looks like it should be.

### Tuning during play

Settings held on a component are copied into the running actor, so changing one means leaving play. Put them in an asset instead and every edit lands immediately:

| Asset | Holds | Assign to |
| --- | --- | --- |
| `URagdollProfileAsset` | One `FRagdollPhysicalProfile` | `PhysicalProfileAssets`, keyed by the same tag as `PhysicalProfiles` |
| `URagdollMotionDriveAsset` | `FRagdollMotionDrive` | the motion drive's `MotionDriveAsset` |
| `URagdollBaseDriveAsset` | `FRagdollBaseDrive` | the base drive's `BaseDriveAsset` |

Each block has its own source: `PhysicalProfileSource`, `MotionDriveSource` and `BaseDriveSource` switch between the inline settings and the asset and hide whichever is not in use, so a component can be moved over one block at a time. The drive parameters are read every frame and need nothing further. A profile's springs are built once, so the component watches its asset and rebuilds them when it is edited.

### The spring

A group's angular drive is three independent terms:

| Term | Is | Units |
| --- | --- | --- |
| `AngularStiffness` | How hard it pulls back per radian of error | rad/s2 per rad |
| `AngularDamping` | How hard it resists moving | rad/s2 per rad/s |
| `AngularMaxTorque` | The ceiling on what the motor may spend. Zero is unlimited. | rad/s2 |

They are independent, which is the point: `2 * sqrt(AngularStiffness)` is the damping that settles without overshoot, above that is sluggish and below that rings. `AngularMaxTorque` is what lets an outside force win an argument with the motor rather than be silently cancelled, so a base that should be able to stagger the body wants a ceiling here rather than a weaker spring.

`MaxDriveFrequency` caps `sqrt(AngularStiffness)` after the load scale, in Hz, and is off by default. Chaos solves drives with XPBD, whose correction saturates rather than overshoots, so a very high stiffness settles into a rigid constraint rather than ringing. The cap is only for keeping one heavily loaded joint from going rigid while the rest of its group stays soft.

`bUseAccelerationDriveMode` is on by default, which normalizes each motor by **its own body's inertia** and ignores everything hanging off it. `bScaleStrengthByLoad` is what corrects for that, so `AngularStrength` means the same thing at a wrist and at a spine root and **single digits is the working range everywhere**. It is a frequency in Hz: 3 to 6 is a body holding its pose, and anything approaching a tenth of the physics rate rings instead of holding. `MaxDriveFrequency` caps what the load scale can push it to.

`AngularDampingRatio` 1 is critically damped and settles; below about 0.5 the body rings for several cycles after every disturbance, which is most of what reads as floppiness at any strength. `bScaleStrengthByLoad` carries the ratio as well as the strength, so it means the same thing at a wrist and at a spine root.

Leave `BlendWeight` at 1 and tune the feel with the strengths. `BlendWeight` is for fading the layer in and out.

`SetPhysicalAlpha` scales the whole layer, `SetPhysicalStrength` the motors, `SetPhysicalBlendRate` the fade.

### Motion drive

Speed scales the layer, through a curve or a range. Acceleration leans it, split by direction of travel:

| Component | Case | Property |
| --- | --- | --- |
| Along travel, gaining speed | Starting | `AccelerationScale` |
| Along travel, losing speed | Stopping | `BrakingScale` |
| Across travel | Input cutting against your own momentum | `TurnScale` |

`MaxBias` caps the result. The bias is a mass-normalised acceleration, so a hand leans as much as a torso. `PushBias` and `TurnBias` come back separately, to scale or invert either half.

### Lean

Everything else here pushes the bodies with a force, and a force and the motors are working against each other. Stiffen the layer enough to stay true to the animation and the push stops reading; raise the push until it survives and the pose is gone. There is no setting of the two that gives both.

`SetPhysicalLean` rotates what the motors are driving towards instead, so the two stop competing: a stiffer layer tracks the lean *more* exactly. Use it whenever the character has to look animated and still react.

```cpp
Ragdoll->SetPhysicalLean(WorldLean, TEXT("spine_03"));   // every frame, like the bias
```

It is scoped to a bone, and everything below rides along holding its animated shape, so leaning at `spine_03` bends the torso from there up and leaves the legs planted. A frame without a call puts that bone back on the animation.

### Drives

`URagdollDriveComponent` is `URagdollComponent` plus the two always-on drives and a one-off impulse channel. Call `TickDrives` once a frame from the owner and it does the rest: picks which drive owns the layer, applies its result, and hands the layer back when neither wants it.

Override `IsMotionDriveEnabled` and `IsBaseDriveEnabled` to gate them on gameplay, and `GetDriveBase` to say what the base drive rides (it defaults to the character's movement base).

### Base drive

Readings of the surface the character is riding. The first two produce a lean, the rest produce forces:

| Term | Reads | Property |
| --- | --- | --- |
| Tilt lean | The base's angle away from gravity | `TiltLeanScale`, capped by `MaxLeanAngle` |
| Sway | The base's own rotation, which the body is left behind by | `SwayScale`, `BalanceFrequency`, `BalanceDampingRatio`, capped by `MaxLeanAngle` |
| Tilt | The base's angle away from gravity | `TiltScale`, capped by `MaxTiltAngle` |
| Translation | Acceleration of the base at the character | `TranslationScale`, capped by `MaxBias` |
| Rotation | The base's angular velocity | `RotationScale`, capped by `MaxTorque` |

Start with the lean terms and leave the force terms at zero. `TiltLeanScale` is degrees of body per degree of base, so 0.7 leans a body 7 degrees on a deck heeled 10.

Sway is a balance model rather than a reading. `SwayScale` is the fraction of the base's rotation the body fails to follow, and the spring underneath it is what brings the body back upright: `BalanceFrequency` is how fast it recovers and `BalanceDampingRatio` below 1 lets it overshoot and settle. This is the knob for how able the character looks on his feet. A steady list settles out to nothing, so only a base that keeps rotating produces sway.

Tilt is the term that carries a slow roll either way. Velocity and acceleration go to nothing the moment the base settles at an angle, and a body standing on a heeled deck is still on a slope.

> [!WARNING]
> `SetPhysicalStrength` will not make a base drive's **forces** stronger. Strength is the motors holding the pose, so raising it makes the body resist them harder, not less. It does the opposite to the lean terms, which is the reason to prefer them.

### Bone delta drive

`Exponent` above 1 keeps gentle animation out of the way. Linear and angular speed are measured separately and the larger wins. `Space`: `Local` counts animation only, `World` folds in the actor's movement.

> [!WARNING]
> The reference bone must be one the active profile does not drive, or physics feeds back into its own input and the layer runs away

### Ragdoll and recovery

`SeparatedBones` break their constraint on ragdoll start, so a weapon or hat comes free. Override `GetActiveSeparatedBones` to filter on gameplay state. `bRestorePhysicalProfileAfterRecovery` reapplies the profile that was active beforehand.

### Suspension

`SuspendPhysicalLayer` and `ResumePhysicalLayer` take reasons as tags; the strongest urgency asked for wins. `bQueryStateSuspension` plus `ShouldSuspendPhysicalLayer` drives it from gameplay state instead of explicit calls.

### Physics asset constraint profiles

| Setting | Where | Effect |
| --- | --- | --- |
| `ControlDataConstraintProfile` | Per bone group, and on `RagdollSettings` | Initializes those bodies' strengths from the named constraint profile's joint drives instead of `ControlData` |
| `ConstraintProfile` | Per physical profile, and on `RagdollSettings` | Applied to every joint while that state is active |
| `DefaultConstraintProfile` | Component | What constraints revert to once a state that applied one ends |

All optional. `ProfileSourcePhysicsAsset` feeds the dropdowns when there is no owner to resolve from.

### LOD

`LOD` scales the layer down with distance and visibility. It never applies to a locally controlled pawn.

## Changelog

### 1.0.0
* Initial Release
