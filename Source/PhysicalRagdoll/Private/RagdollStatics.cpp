// Copyright (c) Jared Taylor

#include "RagdollStatics.h"

#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Components/SkeletalMeshComponent.h"
#include "Curves/CurveFloat.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RagdollStatics)

static TAutoConsoleVariable<int32> CVarRagdollDebugMotionDrive(
	TEXT("p.Ragdoll.DebugMotionDrive"),
	0,
	TEXT("Print the motion drive breakdown: input acceleration, push, brake, and the resulting bias along travel"),
	ECVF_Cheat);

bool URagdollStatics::DoAnyPhysicsBodiesHaveWeight(const USkeletalMeshComponent* Mesh)
{
	for (const FBodyInstance* Body : Mesh->Bodies)
	{
		if (Body && Body->PhysicsBlendWeight > 0.f)
		{
			return true;
		}
	}
	return false;
}

bool URagdollStatics::ShouldBlendPhysicsBones(const USkeletalMeshComponent* Mesh)
{
	return Mesh->Bodies.Num() > 0
		&& CollisionEnabledHasPhysics(Mesh->GetCollisionEnabled())
		&& (Mesh->bBlendPhysics || DoAnyPhysicsBodiesHaveWeight(Mesh));
}

bool URagdollStatics::ShouldRunEndPhysicsTick(const USkeletalMeshComponent* Mesh)
{
	return (Mesh->bEnablePhysicsOnDedicatedServer || !Mesh->IsNetMode(NM_DedicatedServer))
		&& ((Mesh->IsSimulatingPhysics() && Mesh->RigidBodyIsAwake()) || ShouldBlendPhysicsBones(Mesh));
}

bool URagdollStatics::ShouldRunClothTick(const USkeletalMeshComponent* Mesh)
{
	return !Mesh->bDisableClothSimulation && Mesh->CanSimulateClothing();
}

FName URagdollStatics::GetBoneName(const USkeletalMeshComponent* Mesh, const FBodyInstance* BI)
{
	return Mesh->GetBoneName(BI->InstanceBoneIndex);
}

int32 URagdollStatics::ForEach(const USkeletalMeshComponent* Mesh, FName BoneName, bool bIncludeSelf,
	const TFunctionRef<bool(FBodyInstance*)>& Func)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::ForEach);

	if (!Mesh)
	{
		return 0;
	}

	if (BoneName == NAME_None && bIncludeSelf)
	{
		for (FBodyInstance* BI : Mesh->Bodies)
		{
			if (BI && !Func(BI))
			{
				return 1;
			}
		}
		return Mesh->Bodies.Num();
	}

	UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
	if (!PhysicsAsset || !Mesh->GetSkeletalMeshAsset() || !Mesh->IsPhysicsStateCreated() || !Mesh->bHasValidBodies)
	{
		return 0;
	}

	TArray<int32> BodyIndices;
	BodyIndices.Reserve(Mesh->Bodies.Num());
	PhysicsAsset->GetBodyIndicesBelow(BodyIndices, BoneName, Mesh->GetSkeletalMeshAsset(), bIncludeSelf);

	int32 NumBodiesFound = 0;
	for (const int32 BodyIdx : BodyIndices)
	{
		if (!Mesh->Bodies.IsValidIndex(BodyIdx))
		{
			continue;
		}

		++NumBodiesFound;
		if (!Func(Mesh->Bodies[BodyIdx]))
		{
			return NumBodiesFound;
		}
	}

	return NumBodiesFound;
}

bool URagdollStatics::SetBlendWeight(const USkeletalMeshComponent* Mesh, const FName& BoneName, float BlendWeight)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(URagdollStatics::SetBlendWeight);

	FBodyInstance* BI = Mesh ? Mesh->GetBodyInstance(BoneName) : nullptr;
	if (!BI)
	{
		return false;
	}

	BI->PhysicsBlendWeight = FMath::Clamp(BlendWeight, 0.f, 1.f);

	if (FMath::IsNearlyEqual(BI->PhysicsBlendWeight, 1.f))
	{
		BI->PhysicsBlendWeight = 1.f;
	}
	else if (FMath::IsNearlyZero(BI->PhysicsBlendWeight))
	{
		BI->PhysicsBlendWeight = 0.f;
	}

	const bool bWantsSim = BI->PhysicsBlendWeight > 0.f;
	if (bWantsSim != BI->bSimulatePhysics)
	{
		BI->SetInstanceSimulatePhysics(bWantsSim, false, true);
	}

	return true;
}

void URagdollStatics::FinalizeMeshPhysics(USkeletalMeshComponent* Mesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::FinalizeMeshPhysics);

	if (!Mesh)
	{
		return;
	}

	if (Mesh->IsSimulatingPhysics())
	{
		Mesh->SetRootBodyIndex(Mesh->RootBodyData.BodyIndex);
	}

	Mesh->bBlendPhysics = false;

	Mesh->RegisterEndPhysicsTick(Mesh->PrimaryComponentTick.IsTickFunctionRegistered() && ShouldRunEndPhysicsTick(Mesh));
	Mesh->RegisterClothTick(Mesh->PrimaryComponentTick.IsTickFunctionRegistered() && ShouldRunClothTick(Mesh));
}

float URagdollStatics::GetBoneBlendWeight(const USkeletalMeshComponent* Mesh, const FName& BoneName)
{
	const FBodyInstance* BI = Mesh ? Mesh->GetBodyInstance(BoneName) : nullptr;
	return BI ? BI->PhysicsBlendWeight : 0.f;
}

FRagdollMotionInput URagdollStatics::MakeMotionInputFromCharacter(const ACharacter* Character)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::MakeMotionInputFromCharacter);

	FRagdollMotionInput Input;

	if (const UCharacterMovementComponent* CMC = Character ? Character->GetCharacterMovement() : nullptr)
	{
		Input.Velocity = CMC->Velocity;
		Input.InputAcceleration = CMC->GetCurrentAcceleration();
		Input.MaxSpeed = CMC->GetMaxSpeed();
		Input.MaxAcceleration = CMC->GetMaxAcceleration();
		Input.bIsFalling = CMC->IsFalling();

		const FVector VelocityDir = CMC->Velocity.GetSafeNormal2D();
		const AController* Controller = Character->GetController();

		Input.FacingYaw = Character->bUseControllerRotationYaw && Controller
			? Controller->GetControlRotation().Yaw
			: Character->GetActorRotation().Yaw;

		if (Character->bUseControllerRotationYaw || CMC->bUseControllerDesiredRotation)
		{
			// The character faces where the controller looks, so backward is movement against that look
			const FVector Facing = Controller
				? FRotator(0.f, Controller->GetControlRotation().Yaw, 0.f).Vector()
				: Character->GetActorForwardVector();
			Input.Forwardness = VelocityDir.IsNearlyZero() ? 1.f : FVector::DotProduct(VelocityDir, Facing);
		}
		else if (CMC->bOrientRotationToMovement)
		{
			// The character always faces its travel, so it can never be moving backward by facing. What
			// reads as backing off is the request opposing the momentum instead.
			const FVector InputDir = CMC->GetCurrentAcceleration().GetSafeNormal2D();
			Input.Forwardness = InputDir.IsNearlyZero() || VelocityDir.IsNearlyZero()
				? 1.f
				: FVector::DotProduct(InputDir, VelocityDir);
		}
		else
		{
			Input.Forwardness = VelocityDir.IsNearlyZero()
				? 1.f
				: FVector::DotProduct(VelocityDir, Character->GetActorForwardVector());
		}
	}

	return Input;
}

void URagdollStatics::CalculateMotionDrive(const FRagdollMotionDrive& Params, FRagdollMotionDriveState& State,
	const FRagdollMotionInput& Input, float DeltaTime, float& OutAlpha, float& OutStrength, FVector& OutBias,
	FVector& OutPushBias, FVector& OutTurnBias, float& OutBlendRate,
	float AccelerationMultiplier, float BrakingMultiplier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::CalculateMotionDrive);

	ON_SCOPE_EXIT
	{
		OutAlpha = State.Alpha;
		OutStrength = State.Strength;
		OutBias = State.Bias;
		OutPushBias = State.PushBias;
		OutTurnBias = State.TurnBias;
		OutBlendRate = State.BlendRate;
	};

	if (DeltaTime <= 0.f)
	{
		return;
	}

	if (Input.bIsFalling && !Params.bDriveWhileFalling)
	{
		State.Alpha = 0.f;
		State.Bias = FVector::ZeroVector;
		return;
	}

	const float Speed = Input.Velocity.Size2D();

	if (Params.SpeedCurve)
	{
		State.Alpha = Params.SpeedCurve->GetFloatValue(Input.MaxSpeed > 0.f ? Speed / Input.MaxSpeed : 0.f);
	}
	else
	{
		const float SpeedAlpha = FMath::Clamp(FMath::GetRangePct(Params.SpeedRange, Speed), 0.f, 1.f);
		State.Alpha = FMath::Lerp(Params.AlphaRange.X, Params.AlphaRange.Y, SpeedAlpha);
	}

	if (Input.bIsFalling && Params.FallCurve)
	{
		State.Alpha *= Params.FallCurve->GetFloatValue(FMath::Abs(Input.Velocity.Z) / FMath::Max(Params.ReferenceFallSpeed, 1.f));
	}

	// Backpedalling carries less physicality than running, whatever the speed says
	const float DirectionAlpha = FMath::Clamp((Input.Forwardness + 1.f) * 0.5f, 0.f, 1.f);
	State.DebugDirectionScale = Params.DirectionCurve
		? Params.DirectionCurve->GetFloatValue(DirectionAlpha)
		: FMath::Lerp(Params.BackwardScale, 1.f, DirectionAlpha);

	State.Alpha = FMath::Clamp(State.Alpha * State.DebugDirectionScale, 0.f, 1.f);

	// Vertical is left out throughout: gravity is a constant that would otherwise sit in the bias
	// permanently, and airborne shaping is the fall curve's job.
	const FVector InputHorizontal(Input.InputAcceleration.X, Input.InputAcceleration.Y, 0.f);
	const FVector VelocityDir = Input.Velocity.GetSafeNormal2D();

	FVector TargetBias = FVector::ZeroVector;

	State.DebugInputAccel = InputHorizontal.Size();
	State.DebugPushAlong = 0.f;
	State.DebugBrakeAlong = 0.f;

	if (!InputHorizontal.IsNearlyZero())
	{
		// Direction and magnitude both come from the requested acceleration, never from a change in
		// velocity. Velocity only says whether that request is driving the body onward or against itself.
		const float AlongTravel = VelocityDir.IsNearlyZero() ? 1.f : FVector::DotProduct(InputHorizontal.GetSafeNormal(), VelocityDir);
		const bool bOpposingTravel = AlongTravel < 0.f;

		const float AccelScale = Params.AccelerationScale * AccelerationMultiplier;
		const float BrakeScale = Params.BrakingScale * BrakingMultiplier;

		TargetBias = InputHorizontal * (bOpposingTravel ? BrakeScale : AccelScale);

		if (bOpposingTravel)
		{
			State.DebugBrakeAlong = InputHorizontal.Size() * BrakeScale;
		}
		else
		{
			State.DebugPushAlong = InputHorizontal.Size() * AccelScale;
		}

	}

	const FVector PushTarget = TargetBias;
	FVector TurnTarget = FVector::ZeroVector;

	// The turn lean is the centripetal acceleration the turn implies, toward the inside of the arc.
	// Derived from how fast the facing is rotating, since that is what the bodies are trailing behind.
	State.DebugCentripetal = 0.f;
	if (State.bHasPreviousYaw && !VelocityDir.IsNearlyZero())
	{
		const float YawDelta = FMath::FindDeltaAngleDegrees(State.PreviousYaw, Input.FacingYaw);
		const float RawYawRate = YawDelta / DeltaTime;

		// Smoothed before use: a raw yaw delta changes sign on its own near zero, which is what puts the
		// lean on the correct side one frame and the wrong side the next
		State.SmoothedYawRate = Params.BiasInterpRate > 0.f
			? FMath::Lerp(State.SmoothedYawRate, RawYawRate, 1.f - FMath::Exp(-Params.BiasInterpRate * DeltaTime))
			: RawYawRate;

		// Right of travel. A positive yaw rate turns right, so this points into the turn.
		const FVector TurnInward = FVector::CrossProduct(FVector::UpVector, VelocityDir);

		State.DebugCentripetal = Speed * FMath::DegreesToRadians(State.SmoothedYawRate);
		TurnTarget = TurnInward * State.DebugCentripetal * Params.TurnScale * (Params.bInvertTurn ? -1.f : 1.f);
		TargetBias += TurnTarget;
	}
	State.PreviousYaw = Input.FacingYaw;
	State.bHasPreviousYaw = true;

	if (Input.bIsFalling)
	{
		TargetBias *= Params.FallBiasScale;
	}

	// Clamped apart, then summed. Clamping the sum lets the push saturate the ceiling and squeeze the
	// turn down to a rounding error, which leaves the body's own outward trailing as the only thing
	// visible in a corner - the lean looks inverted and gets worse the faster you go.
	const FVector ScaledPush = (PushTarget * State.DebugDirectionScale).GetClampedToMaxSize(Params.MaxBias);
	const FVector ScaledTurn = (TurnTarget * State.DebugDirectionScale).GetClampedToMaxSize(Params.MaxBias);

	TargetBias = ScaledPush + ScaledTurn;
	State.DebugBiasAlong = VelocityDir.IsNearlyZero()
		? TargetBias.Size()
		: FVector::DotProduct(TargetBias, VelocityDir);

#if !UE_BUILD_SHIPPING
	if (CVarRagdollDebugMotionDrive.GetValueOnGameThread() > 0 && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x5241474D, 0.f, FColor::Yellow, FString::Printf(
			TEXT("MotionDrive  input %.0f  push %.0f  brake %.0f  turn %.0f  bias %.0f  speed %.0f  strength %.2f  dir %.2f"),
			State.DebugInputAccel, State.DebugPushAlong, State.DebugBrakeAlong, State.DebugCentripetal,
			State.DebugBiasAlong, Speed, State.Strength, State.DebugDirectionScale));
	}
#endif

	// How hard the body is being pushed around right now, which is what the motors give against.
	// At a constant speed this is zero, so the body holds its pose instead of wobbling along.
	const float Reference = Input.MaxAcceleration > 0.f ? Input.MaxAcceleration : Params.MaxBias;
	const float AccelAlpha = Reference > 0.f ? FMath::Clamp(TargetBias.Size() / Reference, 0.f, 1.f) : 0.f;
	const float TargetStrength = FMath::Lerp(Params.SteadyStrength, Params.AcceleratingStrength, AccelAlpha);
	const float TargetBlendRate = FMath::Lerp(Params.SteadyBlendRate, Params.AcceleratingBlendRate, AccelAlpha);

	if (Params.BiasInterpRate > 0.f)
	{
		const float BlendAlpha = 1.f - FMath::Exp(-Params.BiasInterpRate * DeltaTime);
		State.Bias = FMath::Lerp(State.Bias, TargetBias, BlendAlpha);
		State.PushBias = FMath::Lerp(State.PushBias, ScaledPush, BlendAlpha);
		State.TurnBias = FMath::Lerp(State.TurnBias, ScaledTurn, BlendAlpha);
		State.Strength = FMath::Lerp(State.Strength, TargetStrength, BlendAlpha);
		State.BlendRate = FMath::Lerp(State.BlendRate, TargetBlendRate, BlendAlpha);
	}
	else
	{
		State.Bias = TargetBias;
		State.PushBias = ScaledPush;
		State.TurnBias = ScaledTurn;
		State.Strength = TargetStrength;
		State.BlendRate = TargetBlendRate;
	}
}

void URagdollStatics::CalculateMotionDriveForCharacter(const FRagdollMotionDrive& Params, FRagdollMotionDriveState& State,
	const ACharacter* Character, float DeltaTime, float& OutAlpha, float& OutStrength, FVector& OutBias,
	FVector& OutPushBias, FVector& OutTurnBias, float& OutBlendRate,
	float AccelerationMultiplier, float BrakingMultiplier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::CalculateMotionDriveForCharacter);

	CalculateMotionDrive(Params, State, MakeMotionInputFromCharacter(Character), DeltaTime,
		OutAlpha, OutStrength, OutBias, OutPushBias, OutTurnBias, OutBlendRate,
		AccelerationMultiplier, BrakingMultiplier);
}

void URagdollStatics::DrawMotionDriveDebug(const UObject* WorldContextObject, const FVector& Origin,
	const FRagdollMotionInput& Input, const FRagdollMotionDriveState& State, float DrawScale)
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return;
	}

	const FVector TravelDir = Input.Velocity.GetSafeNormal2D();
	const FVector InputDir = FVector(Input.InputAcceleration.X, Input.InputAcceleration.Y, 0.f);
	const FVector Reference = TravelDir.IsNearlyZero() ? InputDir.GetSafeNormal() : TravelDir;

	auto Arrow = [World, &Origin](const FVector& Vector, const FColor& Color, float Thickness)
	{
		if (!Vector.IsNearlyZero())
		{
			DrawDebugDirectionalArrow(World, Origin, Origin + Vector, 24.f, Color, false, -1.f, 0, Thickness);
		}
	};

	// Stacked so overlapping arrows stay readable
	Arrow(Reference * 60.f, FColor::White, 1.5f);
	Arrow(InputDir * DrawScale, FColor::Cyan, 2.f);
	Arrow(Reference * State.DebugPushAlong * DrawScale, FColor::Green, 3.f);
	Arrow(Reference * State.DebugBrakeAlong * DrawScale, FColor::Red, 3.f);
	Arrow(State.Bias * DrawScale, FColor::Yellow, 5.f);

	DrawDebugString(World, Origin + FVector(0.f, 0.f, 25.f), FString::Printf(
		TEXT("input %.0f  push %.0f  brake %.0f  bias %.0f  speed %.0f"),
		State.DebugInputAccel, State.DebugPushAlong, State.DebugBrakeAlong, State.DebugBiasAlong, Input.Velocity.Size2D()),
		nullptr, FColor::White, 0.f, true, 1.2f);
#endif
}

void URagdollStatics::CalculateBaseDrive(const FRagdollBaseDrive& Params, FRagdollBaseDriveState& State,
	UPrimitiveComponent* BaseComponent, const ACharacter* Character, float DeltaTime,
	FVector& OutBias, FVector& OutTorque)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::CalculateBaseDrive);

	ON_SCOPE_EXIT
	{
		OutBias = State.Bias;
		OutTorque = State.Torque;
	};

	if (DeltaTime <= 0.f)
	{
		return;
	}

	UPrimitiveComponent* Base = BaseComponent ? BaseComponent :
		(Character ? Cast<UPrimitiveComponent>(Character->GetMovementBaseObject()) : nullptr);
	State.ResolvedBase = Base;

	FVector TargetBias = FVector::ZeroVector;
	FVector TargetTorque = FVector::ZeroVector;

	if (Base)
	{
		const FVector AngularVelocity = Base->GetPhysicsAngularVelocityInRadians();

		// Velocity of the base at the character's feet rather than at its origin, since a deck's far end
		// moves a great deal more than its centre when it rolls
		FVector PointVelocity = Base->GetComponentVelocity();
		if (Character)
		{
			const FVector Offset = Character->GetActorLocation() - Base->GetComponentLocation();
			PointVelocity += FVector::CrossProduct(AngularVelocity, Offset);
		}

		if (Params.bIgnoreVerticalTranslation)
		{
			PointVelocity.Z = 0.f;
		}

		// Negated: the body is left behind by whatever moves under it, which is the whole reaction
		TargetBias = (-PointVelocity * Params.TranslationScale).GetClampedToMaxSize(Params.MaxBias);
		TargetTorque = (-AngularVelocity * Params.RotationScale).GetClampedToMaxSize(Params.MaxTorque);
	}

	if (Params.InterpRate > 0.f)
	{
		const float BlendAlpha = 1.f - FMath::Exp(-Params.InterpRate * DeltaTime);
		State.Bias = FMath::Lerp(State.Bias, TargetBias, BlendAlpha);
		State.Torque = FMath::Lerp(State.Torque, TargetTorque, BlendAlpha);
	}
	else
	{
		State.Bias = TargetBias;
		State.Torque = TargetTorque;
	}
}

void URagdollStatics::CalculateBoneDeltaDrive(const FRagdollBoneDeltaDrive& Params, FRagdollBoneDeltaDriveState& State,
	const USkeletalMeshComponent* Mesh, float DeltaTime, float& OutAlpha)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::CalculateBoneDeltaDrive);

	ON_SCOPE_EXIT
	{
		OutAlpha = State.Alpha;
	};

	if (!Mesh || DeltaTime <= 0.f || Params.BoneName == NAME_None)
	{
		return;
	}

	const ERelativeTransformSpace Space = Params.Space == ERagdollDeltaSpace::World ? RTS_World : RTS_Component;
	const FTransform Current = Mesh->GetBoneTransform(Params.BoneName, Space);

	// The first frame has nothing to compare against, and would otherwise read as an enormous delta
	if (!State.bHasPreviousTransform)
	{
		State.PreviousTransform = Current;
		State.bHasPreviousTransform = true;
		return;
	}

	const FVector Delta = Current.GetLocation() - State.PreviousTransform.GetLocation();
	const float AngularSpeed = FMath::RadiansToDegrees(Current.GetRotation().AngularDistance(State.PreviousTransform.GetRotation())) / DeltaTime;
	State.PreviousTransform = Current;

	// Downward is negative, so clamping before taking magnitude is what isolates the impact half of a step
	const float VerticalDelta = Params.bVerticalDownOnly ? FMath::Min(Delta.Z, 0.f) : Delta.Z;
	const float VerticalSpeed = FMath::Abs(VerticalDelta) / DeltaTime;
	const float HorizontalSpeed = Delta.Size2D() / DeltaTime;

	State.DebugVerticalSpeed = VerticalSpeed;
	State.DebugHorizontalSpeed = HorizontalSpeed;
	State.DebugAngularSpeed = AngularSpeed;

	const float VerticalAlpha = FMath::Clamp(FMath::GetRangePct(Params.VerticalRange, VerticalSpeed), 0.f, 1.f) * Params.VerticalWeight;
	const float HorizontalAlpha = FMath::Clamp(FMath::GetRangePct(Params.HorizontalRange, HorizontalSpeed), 0.f, 1.f) * Params.HorizontalWeight;
	const float AngularAlpha = FMath::Clamp(FMath::GetRangePct(Params.AngularRange, AngularSpeed), 0.f, 1.f) * Params.AngularWeight;

	// Whichever axis is working hardest drives it, so a bone can bob without travelling, travel without
	// bobbing, or spin without either, and each still registers
	State.NormalizedDelta = FMath::Clamp(FMath::Max3(VerticalAlpha, HorizontalAlpha, AngularAlpha), 0.f, 1.f);

	const float Shaped = Params.DeltaCurve
		? Params.DeltaCurve->GetFloatValue(State.NormalizedDelta)
		: FMath::Pow(State.NormalizedDelta, FMath::Max(Params.Exponent, 0.01f));

	const float TargetAlpha = FMath::Clamp(FMath::Lerp(Params.AlphaRange.X, Params.AlphaRange.Y, Shaped), 0.f, 1.f);

	State.Alpha = Params.InterpRate > 0.f
		? FMath::Lerp(State.Alpha, TargetAlpha, 1.f - FMath::Exp(-Params.InterpRate * DeltaTime))
		: TargetAlpha;
}

ECollisionEnabled::Type URagdollStatics::EnablePhysicsCollision(USkeletalMeshComponent* Mesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RagdollStatics::EnablePhysicsCollision);

	const ECollisionEnabled::Type Previous = Mesh->GetCollisionEnabled();

	switch (Previous)
	{
	case ECollisionEnabled::NoCollision:
	case ECollisionEnabled::ProbeOnly:
		Mesh->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
		break;
	case ECollisionEnabled::QueryOnly:
	case ECollisionEnabled::QueryAndProbe:
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		break;
	default:
		break;
	}

	return Previous;
}
