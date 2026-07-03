// Copyright TimeWalk. Procedural idle + speaking head/body motion for an Inworld MetaHuman.

#include "TimeWalkIdleMotionAnimInstance.h"

// 2026-06-12 Ted #8951: gate the once/sec head-pose diagnostic behind a compile-time
// flag (default OFF). Flip to 1 to bring back the [TWIdleAnim] probe.
#ifndef TIMEWALK_IDLE_DIAG
#define TIMEWALK_IDLE_DIAG 0
#endif

// 2026-06-18 nod-axis diagnostic (NOW OFF). Was used to prove the nod drives ONLY the
// head PITCH channel (TargetHead came back P=-10..-1, Y~=0, R~=0 during a nod) -- the
// C++ math is correct; the earlier "moves left" was the off-axis camera LateralOffset
// (Bug 2), now zeroed. Flip to 1 only to re-run the auto-trigger + channel log.
#ifndef TIMEWALK_NOD_DIAG
#define TIMEWALK_NOD_DIAG 0
#endif

#include "GameFramework/Actor.h"
#include "Engine/World.h"
// (Inworld voice header comes in via the .h, guarded by TIMEWALK_WITH_INWORLD.)
#include "Math/UnrealMathUtility.h"
#include "TimeWalkIdleMotionComponent.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"

// #23 (2026-06-14): folder holding the ActorCore idle clips retargeted to MetaHuman.
static const TCHAR* GTimeWalkIdlePoolPath = TEXT("/Game/TimeWalk/IdleAnims/Retargeted");

namespace
{
	// Smoothly approach Target from Current at a rate defined by TimeConstant (seconds).
	FORCEINLINE float ApproachExp(float Current, float Target, float DeltaSeconds, float TimeConstant)
	{
		if (TimeConstant <= KINDA_SMALL_NUMBER)
		{
			return Target;
		}
		const float Alpha = 1.f - FMath::Exp(-DeltaSeconds / TimeConstant);
		return FMath::Lerp(Current, Target, FMath::Clamp(Alpha, 0.f, 1.f));
	}
}

float UTimeWalkIdleMotionAnimInstance::Noise1D(float T, float Seed)
{
	// NOTE: UE's FMath::PerlinNoise1D does NOT span [-1,1]. In practice it returns roughly
	// [-0.5,0.5], hits exactly 0 at integer inputs, and for slowly-advancing T spends most
	// of its time near the center (~+-0.2). That under-drove the head amplitude ~3-5x
	// (degrees specified as 3.5 read as ~+-1 on the wire). We renormalize to a true [-1,1]
	// so the amplitude tunables mean actual degrees.
	//
	// Empirical full range of PerlinNoise1D is ~+-0.5, so multiply by 2. Clamp guards the
	// occasional overshoot. Offset input by a large per-channel seed so each axis samples an
	// uncorrelated stretch.
	const float Raw = FMath::PerlinNoise1D(T + Seed);
	return FMath::Clamp(Raw * 2.0f, -1.0f, 1.0f);
}

void UTimeWalkIdleMotionAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	TryBindVoiceComponent();

	// Pool disabled by default on main (clips reviewed bad 2026-06-15). Skip load + seed
	// entirely so CurrentIdleClip stays null and the simple idle pose is used.
	if (!bIdlePoolEnabled)
	{
		return;
	}

	EnsureIdlePoolLoaded();

	// Seed the pool: pick a first clip + queue a second so the AnimGraph has both pins valid.
	if (IdlePool.Num() > 0)
	{
		CurrentIdleClip = PickRandomIdle(nullptr);
		NextIdleClip    = PickRandomIdle(CurrentIdleClip);
		IdleBlendAlpha  = 0.f;
		IdleHoldRemaining = FMath::FRandRange(IdleMinHoldSeconds, IdleMaxHoldSeconds);
		bIdleBlending = false;
	}
}

void UTimeWalkIdleMotionAnimInstance::EnsureIdlePoolLoaded()
{
	if (IdlePool.Num() > 0)
	{
		return; // explicitly configured in the AnimBP -- respect it.
	}
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(GTimeWalkIdlePoolPath));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	TArray<FAssetData> Found;
	ARM.Get().GetAssets(Filter, Found);
	for (const FAssetData& AD : Found)
	{
		if (UAnimSequence* Seq = Cast<UAnimSequence>(AD.GetAsset()))
		{
			IdlePool.Add(Seq);
		}
	}
	UE_LOG(LogTemp, Log, TEXT("[TWIdlePool] Loaded %d idle clips from %s"), IdlePool.Num(), GTimeWalkIdlePoolPath);
}

UAnimSequence* UTimeWalkIdleMotionAnimInstance::PickRandomIdle(UAnimSequence* AvoidClip) const
{
	if (IdlePool.Num() == 0)
	{
		return nullptr;
	}
	if (IdlePool.Num() == 1)
	{
		return IdlePool[0];
	}
	UAnimSequence* Pick = nullptr;
	for (int32 Tries = 0; Tries < 8; ++Tries)
	{
		Pick = IdlePool[FMath::RandRange(0, IdlePool.Num() - 1)];
		if (Pick != AvoidClip)
		{
			break;
		}
	}
	return Pick;
}

void UTimeWalkIdleMotionAnimInstance::UpdateIdleSlotPlayback(float DeltaSeconds)
{
	// Zero-AnimGraph-edit path: play each pool clip as a dynamic montage on an existing Slot.
	if (IdlePool.Num() == 0)
	{
		return;
	}

	auto PlayNext = [this](UAnimSequence* Clip)
	{
		if (!Clip)
		{
			return;
		}
		// Loop the single clip for its full length; we re-trigger before it ends to swap.
		const float BlendIn  = IdleCrossBlendSeconds;
		const float BlendOut = IdleCrossBlendSeconds;
		// InPlayRate=1, LoopCount=1 (we manage looping/swap ourselves), InTimeToStartAt=0.
		PlaySlotAnimationAsDynamicMontage(Clip, IdleSlotName, BlendIn, BlendOut, 1.f, 1, -1.f, 0.f);
		CurrentIdleClip   = Clip;
		// Re-trigger slightly before the clip ends so the blend-in overlaps the previous clip's
		// blend-out (seamless cross-fade). Guard tiny clips.
		const float Len = Clip->GetPlayLength();
		SlotClipRemaining = FMath::Max(0.5f, Len - IdleCrossBlendSeconds);
	};

	if (!bSlotPlaybackStarted)
	{
		bSlotPlaybackStarted = true;
		PlayNext(PickRandomIdle(nullptr));
		return;
	}

	SlotClipRemaining -= DeltaSeconds;
	if (SlotClipRemaining <= 0.f)
	{
		PlayNext(PickRandomIdle(CurrentIdleClip));
	}
}

void UTimeWalkIdleMotionAnimInstance::UpdateIdlePool(float DeltaSeconds)
{
	// Pool disabled (default on main): nothing to update; simple idle is in effect.
	if (!bIdlePoolEnabled)
	{
		return;
	}

	// Route to slot-playback if enabled (no AnimGraph variable binding required).
	if (bUseSlotPlayback)
	{
		UpdateIdleSlotPlayback(DeltaSeconds);
		return;
	}

	if (IdlePool.Num() <= 1 || !CurrentIdleClip)
	{
		return; // nothing to cycle
	}

	if (bIdleBlending)
	{
		// Advance the cross-blend.
		IdleBlendRemaining -= DeltaSeconds;
		const float T = (IdleCrossBlendSeconds > KINDA_SMALL_NUMBER)
			? 1.f - FMath::Clamp(IdleBlendRemaining / IdleCrossBlendSeconds, 0.f, 1.f)
			: 1.f;
		// Smoothstep for an ease-in/out feel.
		IdleBlendAlpha = T * T * (3.f - 2.f * T);
		if (IdleBlendRemaining <= 0.f)
		{
			// Blend finished: Next becomes Current, queue a new Next, reset alpha.
			CurrentIdleClip   = NextIdleClip;
			NextIdleClip      = PickRandomIdle(CurrentIdleClip);
			IdleBlendAlpha    = 0.f;
			bIdleBlending     = false;
			IdleHoldRemaining = FMath::FRandRange(IdleMinHoldSeconds, IdleMaxHoldSeconds);
		}
	}
	else
	{
		// Holding the current clip; count down then start a blend.
		IdleHoldRemaining -= DeltaSeconds;
		if (IdleHoldRemaining <= 0.f)
		{
			if (!NextIdleClip || NextIdleClip == CurrentIdleClip)
			{
				NextIdleClip = PickRandomIdle(CurrentIdleClip);
			}
			bIdleBlending      = true;
			IdleBlendRemaining = IdleCrossBlendSeconds;
			IdleBlendAlpha     = 0.f;
		}
	}
}

void UTimeWalkIdleMotionAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

#if WITH_EDITOR
	// 2026-06-10 Ted #8740: editor live-tuning. In a non-PIE editor viewport DeltaSeconds
	// is frequently 0, so the normal update below (and its RInterpTo smoothing) never
	// advances -- dragging the Preview sliders appeared to do nothing. When the component
	// is in preview mode, SNAP the head/neck pose to the configured values directly,
	// independent of DeltaSeconds, so every editor redraw reflects the current sliders.
	{
		if (!IdleMotionComponent)
		{
			if (const AActor* OwnerActor = GetOwningActor())
			{
				IdleMotionComponent = OwnerActor->FindComponentByClass<UTimeWalkIdleMotionComponent>();
			}
		}
		const UWorld* W = GetWorld();
		const bool bEditorWorld = W && (W->WorldType == EWorldType::Editor || W->WorldType == EWorldType::EditorPreview);
		if (IdleMotionComponent && bEditorWorld && IdleMotionComponent->bEditorPreviewListening)
		{
			float PW = 0.f, PYaw = 0.f, PPitchDownMag = 0.f, PRoll = 0.f;
			IdleMotionComponent->GetListeningPose(PW, PYaw, PPitchDownMag, PRoll);
			const FRotator SnapHead(-PPitchDownMag, PYaw, PRoll); // +down-mag negated => chin down
			HeadRotation  = SnapHead;
			NeckRotation  = SnapHead * 0.5f;
			SpineRotation = FRotator::ZeroRotator;
			return;
		}
	}
#endif

	if (DeltaSeconds <= 0.f)
	{
		return;
	}

	// #23: cycle the base idle-clip variation pool (random clip + timed cross-blend).
	UpdateIdlePool(DeltaSeconds);

	// Voice component may initialize after the anim instance; keep trying until bound.
	if (!bVoiceBound)
	{
		TryBindVoiceComponent();
	}

	// --- Speaking envelope -------------------------------------------------
	// HandleVoiceAudioPlayback stamps LastPlaybackSeconds whenever audio frames arrive.
	// Treat "spoke within the last ~0.25s" as actively speaking, then attack/release the
	// SpeakingIntensity envelope so motion ramps and settles smoothly.
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const bool bSpeakingNow = (Now - LastPlaybackSeconds) < 0.25;
	const float TargetIntensity = bSpeakingNow ? 1.f : 0.f;
	const float TC = bSpeakingNow ? SpeakingAttackTime : SpeakingReleaseTime;
	SpeakingIntensity = ApproachExp(SpeakingIntensity, TargetIntensity, DeltaSeconds, TC);

	// --- Advance noise + breath clocks ------------------------------------
	NoiseTime += DeltaSeconds * NoiseSpeed;
	BreathPhase += DeltaSeconds * (BreathsPerMinute / 60.f) * 2.f * PI;
	if (BreathPhase > 2.f * PI * 1024.f)
	{
		BreathPhase = FMath::Fmod(BreathPhase, 2.f * PI);
	}

	// --- Amplitudes (idle baseline + speaking add) ------------------------
	// 2026-06-09 Ted #8685: scale by the component's MasterMovementAmplitude so the
	// web-page slider (TW.Idle.Amplitude console cmd -> component) reaches the bones
	// driven here. Without this the component's master amp only fed its own dead
	// (non-bone-driving) tick math -- same trap as the listening-pose bug (#8684).
	float MasterAmp = 1.f;
	if (!IdleMotionComponent)
	{
		if (const AActor* OwnerActor = GetOwningActor())
		{
			IdleMotionComponent = OwnerActor->FindComponentByClass<UTimeWalkIdleMotionComponent>();
		}
	}
	if (IdleMotionComponent)
	{
		MasterAmp = FMath::Max(0.f, IdleMotionComponent->MasterMovementAmplitude);
	}
	const float HeadAmp = (IdleHeadAmplitudeDeg + SpeakingHeadAmplitudeDeg * SpeakingIntensity) * MasterAmp;
	const float SpineAmp = SpineAmplitudeDeg * (1.f + 0.5f * SpeakingIntensity) * MasterAmp;

	// --- Head: uncorrelated noise per axis --------------------------------
	// Distinct seeds keep yaw/pitch/roll independent so it reads organic, not pendular.
	const float HeadYaw   = Noise1D(NoiseTime, 13.1f)  * HeadAmp;
	const float HeadPitch = Noise1D(NoiseTime, 71.7f)  * HeadAmp * 0.7f;   // less nod than turn
	const float HeadRoll  = Noise1D(NoiseTime, 137.3f) * HeadAmp * 0.35f;  // subtle tilt

	// Gentle breathing nod superimposed (chin dips slightly on exhale).
	const float BreathNod = FMath::Sin(BreathPhase) * 0.6f;

	// --- Listening pose (2026-06-09 Ted #8684) -----------------------------
	// On mic press the component raises its listening envelope; blend the noisy
	// idle target toward a sustained head turn + cock so the character visibly
	// attends to the user. The component can't drive bones itself (graph owns
	// them), so the blend lives here.
	// 2026-06-10 Ted: listening pose now = random L/R yaw (~40 deg total) + a down-tilt
	// (~10-20 deg) + the existing slight roll. Yaw/pitch are chosen per-engagement in the
	// component (ActiveListening*Deg, signed) so each listen looks a little different.
	// 2026-06-10 Ted #8732: one accessor returns weight + signed yaw/pitch/roll for BOTH
	// live runtime and editor preview (bEditorPreviewListening), so they can't drift.
	// The pitch coming back is a "down-tilt magnitude" with POSITIVE = chin down (the
	// intuitive slider semantics). 2026-06-10 #8734: the screenshot proved that on THIS
	// head bone a positive FRotator pitch tilts the chin UP, so we NEGATE the listening
	// pitch here at the single point of application -> positive API value = chin actually
	// goes down. (Supersedes the earlier "positive = down" note, which was backwards.)
	float ListenW = 0.f, ListenYaw = 0.f, ListenPitchDownMag = 0.f, ListenRoll = 0.f;
	if (IdleMotionComponent)
	{
		IdleMotionComponent->GetListeningPose(ListenW, ListenYaw, ListenPitchDownMag, ListenRoll);
	}
	const float ListenPitch = -ListenPitchDownMag; // chin down = negative bone pitch on this rig

	// NOTE: all three are mutable -- the knowing-nod axis selector below adds its
	// (signed) magnitude to whichever of Pitch/Yaw/Roll the component selects.
	float FinalYaw   = FMath::Lerp(HeadYaw, ListenYaw, ListenW);
	float FinalPitch = FMath::Lerp(HeadPitch + BreathNod, ListenPitch, ListenW);
	float FinalRoll  = FMath::Lerp(HeadRoll, ListenRoll, ListenW);

	// --- Knowing nod (2026-06-18 Ted) --------------------------------------
	// Additive head dip layered ON TOP of idle + listening (they sum, not overwrite).
	// The component returns a chin-down MAGNITUDE (positive) from GetKnowingNodPitchDeg().
	//
	// AXIS: BAKED 2026-06-18. Ted tested the live Nod-axis selector and CONFIRMED the
	// correct combo is NodAxis = 2 (Roll), NodAxisSign = +1. This rig's head bone local
	// axes are rotated, so the true world-vertical chin dip lands on the ROLL channel.
	// Reproduces exactly what the old switch produced for axis=2/sign=+1:
	//     NodSigned = (+1) * GetKnowingNodPitchDeg();  FinalRoll += NodSigned;
	// The temporary axis/sign selector + its Settings-panel rows have been removed.
	float NodContribDeg = 0.f;
	if (IdleMotionComponent)
	{
		NodContribDeg = IdleMotionComponent->GetKnowingNodPitchDeg();
		FinalRoll += NodContribDeg; // axis = Roll, sign = +1 (hardcoded)
	}

	const FRotator TargetHead(FinalPitch, FinalYaw, FinalRoll);

#if TIMEWALK_NOD_DIAG
	{
		// Auto-fire a nod every ~4s so the log captures an isolated nod cycle.
		static double NodDiagLastTrig = 0.0;
		const double NodDiagNow = GetWorld() ? GetWorld()->GetRealTimeSeconds() : 0.0;
		if (IdleMotionComponent && (NodDiagNow - NodDiagLastTrig) > 4.0)
		{
			NodDiagLastTrig = NodDiagNow;
			IdleMotionComponent->TriggerKnowingNod();
		}
		// While a nod is contributing, log the channel split + the head bone's
		// COMPONENT-space basis (the Modify Bone nodes apply HeadRotation in component
		// space, so this is the frame that matters -- NOT the bone-local frame).
		if (NodContribDeg > 0.05f)
		{
			static double NodDiagLastLog = 0.0;
			if (NodDiagNow - NodDiagLastLog > 0.12)
			{
				NodDiagLastLog = NodDiagNow;
				UE_LOG(LogTemp, Warning,
					TEXT("[NodDiag] nod=%.2f -> FinalPitch=%.2f FinalYaw=%.2f FinalRoll=%.2f | TargetHead(P=%.2f Y=%.2f R=%.2f)"),
					NodContribDeg, FinalPitch, FinalYaw, FinalRoll,
					TargetHead.Pitch, TargetHead.Yaw, TargetHead.Roll);
			}
		}
	}
#endif

	// Neck follows the head at roughly half, for a natural cervical chain.
	const FRotator TargetNeck = TargetHead * 0.5f;

	// --- Spine: slow sway + breathing lean --------------------------------
	const float SpineYaw   = Noise1D(NoiseTime * 0.5f, 211.9f) * SpineAmp;
	const float SpinePitch = FMath::Sin(BreathPhase) * SpineAmp * 0.6f; // chest rise
	const FRotator TargetSpine(SpinePitch, SpineYaw, 0.f);

	// 2026-06-12 Ted #8859 DIAG: prove what reaches the bone at runtime. Logs once/sec.
#if TIMEWALK_IDLE_DIAG
	{
		static double TWDiagLast = 0.0;
		const double TWDiagNow = GetWorld() ? GetWorld()->GetRealTimeSeconds() : 0.0;
		if (TWDiagNow - TWDiagLast > 1.0)
		{
			TWDiagLast = TWDiagNow;
			UE_LOG(LogTemp, Warning,
				TEXT("[TWIdleAnim] comp=%s ListenW=%.2f ListenYaw=%.1f FinalYaw=%.1f HeadRot=(P=%.1f Y=%.1f R=%.1f) dt=%.3f"),
				IdleMotionComponent ? TEXT("YES") : TEXT("NO"),
				ListenW, ListenYaw, FinalYaw,
				HeadRotation.Pitch, HeadRotation.Yaw, HeadRotation.Roll, DeltaSeconds);
		}
	}
#endif

	// --- Smooth toward targets (per-frame critically-damped feel) ---------
	const float Smooth = 0.10f; // seconds; keeps motion fluid, not jittery
	HeadRotation  = FMath::RInterpTo(HeadRotation,  TargetHead,  DeltaSeconds, 1.f / Smooth);
	NeckRotation  = FMath::RInterpTo(NeckRotation,  TargetNeck,  DeltaSeconds, 1.f / Smooth);
	SpineRotation = FMath::RInterpTo(SpineRotation, TargetSpine, DeltaSeconds, 1.f / Smooth);

	// --- Breathing alpha (0..1) for an optional chest morph ---------------
	BreathingAlpha = 0.5f * (FMath::Sin(BreathPhase) + 1.f);
}

void UTimeWalkIdleMotionAnimInstance::NativeUninitializeAnimation()
{
#if TIMEWALK_WITH_INWORLD
	if (VoiceComponent)
	{
		if (VoicePlaybackHandle.IsValid())
		{
			VoiceComponent->OnVoiceAudioPlaybackNative.Remove(VoicePlaybackHandle);
			VoicePlaybackHandle.Reset();
		}
		VoiceComponent = nullptr;
	}
#endif
	bVoiceBound = false;

	Super::NativeUninitializeAnimation();
}

void UTimeWalkIdleMotionAnimInstance::TryBindVoiceComponent()
{
#if TIMEWALK_WITH_INWORLD
	if (bVoiceBound)
	{
		return;
	}

	AActor* OwnerActor = GetOwningActor();
	if (!OwnerActor)
	{
		return;
	}

	UInworldVoiceAudioComponent* Found = OwnerActor->FindComponentByClass<UInworldVoiceAudioComponent>();
	if (!Found)
	{
		return;
	}

	VoiceComponent = Found;
	// Bind the NATIVE (non-dynamic) playback delegate via AddUObject -> no UFUNCTION
	// needed, so the handler stays plain C++ guarded by TIMEWALK_WITH_INWORLD. Clear any
	// stale handle first so a PIE-restart re-bind can't double-add.
	if (VoicePlaybackHandle.IsValid())
	{
		VoiceComponent->OnVoiceAudioPlaybackNative.Remove(VoicePlaybackHandle);
	}
	VoicePlaybackHandle = VoiceComponent->OnVoiceAudioPlaybackNative.AddUObject(this, &UTimeWalkIdleMotionAnimInstance::HandleVoiceAudioPlayback);
	bVoiceBound = true;
#endif // TIMEWALK_WITH_INWORLD
}

#if TIMEWALK_WITH_INWORLD
void UTimeWalkIdleMotionAnimInstance::HandleVoiceAudioPlayback(
	UInworldVoiceAudioComponent* /*InVoiceComponent*/,
	const FInworldVoiceAudioPlaybackInfo& /*PlaybackInfo*/,
	const FInworldData_TTSOutput& /*TTSOutput*/,
	const TArray<FInworldPhoneSpan>& /*PhoneSpans*/)
{
	// Stamp the time of the most recent speech-audio frame. NativeUpdateAnimation reads
	// this to drive the SpeakingIntensity envelope (attack while frames arrive, release
	// when they stop).
	if (UWorld* World = GetWorld())
	{
		LastPlaybackSeconds = World->GetTimeSeconds();
	}
}
#endif // TIMEWALK_WITH_INWORLD
