// Copyright TimeWalk. Drop-on-actor procedural liveliness for an Inworld MetaHuman.

#include "TimeWalkIdleMotionComponent.h"

// 2026-06-12 Ted #8951: gate the once/sec idle-motion diagnostic UE_LOGs behind a
// compile-time flag (default OFF) so they don't spam the log in normal runs. Flip to
// 1 to bring back the [TWIdle][Master] / [TWIdle][Tick] / [TWIdle LOG] probes when
// debugging idle motion / listening pose.
#ifndef TIMEWALK_IDLE_DIAG
#define TIMEWALK_IDLE_DIAG 0
#endif

#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimInstance.h"
#include "Engine/World.h"
#include "Math/UnrealMathUtility.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"   // UCameraComponent - aim at the LENS, not the CineCameraActor pivot
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if TIMEWALK_WITH_INWORLD
// The voice + character component headers come in via the .h (guarded there too).
#include "Graph/RuntimeData/InworldGraphRuntimeData_EmotionState.h"
#include "InworldCharacterTypes.h"
#endif

namespace
{
	// 2026-06-22: console-noise gate. Idle/listening/nod debug spam is silent by default;
	// relaunch the streamer with -twverbose to bring it back for animation debugging.
	FORCEINLINE bool TWIdle_Verbose()
	{
		static const bool bV = FParse::Param(FCommandLine::Get(), TEXT("twverbose"));
		return bV;
	}

	// Renamed 2026-06-14 from ApproachExp -> ApproachExpComp to avoid an ODR collision with
	// the identically-named anon-namespace helper in TimeWalkIdleMotionAnimInstance.cpp when
	// adaptive-unity grouping changes (adding TimeWalkCameraTunerWidget.cpp shifted the unity
	// blob and exposed C2084 "already has a body").
	FORCEINLINE float ApproachExpComp(float Current, float Target, float Dt, float TimeConstant)
	{
		if (TimeConstant <= KINDA_SMALL_NUMBER) { return Target; }
		const float Alpha = 1.f - FMath::Exp(-Dt / TimeConstant);
		return FMath::Lerp(Current, Target, FMath::Clamp(Alpha, 0.f, 1.f));
	}
}

UTimeWalkIdleMotionComponent::UTimeWalkIdleMotionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// Tick AFTER most gameplay so the speaking envelope + glance targets are current;
	// the actual bone write happens in the post-eval delegate, not here.
	// PrePhysics = BEFORE the skeletal mesh animation eval. Critical: MetaHuman faces run
	// RigLogic in a post-process AnimBP that converts CTRL_expressions_* INPUT curves into
	// the visible morphs/joints. We must set those input curves BEFORE eval so RigLogic
	// reads them. Writing morph OUTPUTS after eval (TG_PostUpdateWork) gets stomped by RigLogic.
	// 2026-06-08 Ted 8427: listening-pose debug uncovered that OnBodyPoseFinalized
	// (the function that APPLIES HeadRot to the bone) was never being invoked. The
	// component ticked in TG_PrePhysics (BEFORE the SkeletalMesh evaluates its
	// AnimBP), so any bone edit we did would be overwritten by AnimBP eval. Move to
	// TG_PostPhysics so we tick AFTER the body AnimBP, and call OnBodyPoseFinalized
	// directly at the end of TickComponent. Net effect: HeadRot is now actually
	// applied to the head bone every frame, including the listening-pose roll.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

// ---------------------------------------------------------------------------
//  Persona layer: map 4 high-level traits onto the ~40 raw physical knobs.
// ---------------------------------------------------------------------------
//
//  Design notes (the mapping is intentionally legible; tune the curves here):
//   Imperiousness (Imp) -- the dominant "stillness" trait. Drives amplitude DOWN,
//      gaze-hold UP, look-away-chance DOWN, glance frequency DOWN, noise speed DOWN.
//      A high-Imp character barely moves and locks eye contact = reads as commanding.
//   Restlessness (Rest) -- the "kinetic energy" trait. Drives noise speed UP, glance
//      frequency UP, glance range UP, blink rate UP, amplitude UP. High-Rest fidgets.
//   Warmth (Warm) -- the "engagement" trait. Drives gaze-hold UP + look-away-chance DOWN
//      (more eye contact on the listener), motion a touch SLOWER/softer, emotion a touch
//      stronger. Distinct from Imp: warmth holds gaze out of interest, imperiousness out
//      of dominance -- so both reduce look-aways but warmth keeps motion soft, imp stiffens.
//   Formality (Form) -- the "composure" trait. Drives spine sway DOWN, overall amplitude
//      DOWN, breathing lean DOWN. High-Form = upright, controlled, period gravitas.
//
//  Imp and Rest are deliberately opposed on amplitude/speed; setting both high produces
//  a tense, coiled stillness (small but fast) which is a legitimate read, not a bug.

void UTimeWalkIdleMotionComponent::RebuildMotionFromPersona()
{
	if (!bUsePersona) { return; }

	const float Imp  = FMath::Clamp(Persona.Imperiousness, 0.f, 1.f);
	const float Rest = FMath::Clamp(Persona.Restlessness, 0.f, 1.f);
	const float Warm = FMath::Clamp(Persona.Warmth, 0.f, 1.f);
	const float Form = FMath::Clamp(Persona.Formality, 0.f, 1.f);

	// ---- Master amplitude -------------------------------------------------
	// Baseline kept low (the native open-mic build wanted subtle, per #8948). Restlessness
	// adds movement; imperiousness + formality remove it. Range ~0.04 .. ~0.35.
	{
		const float Base = 0.10f;
		const float Amp = Base * (1.f + 1.6f * Rest) * (1.f - 0.6f * Imp) * (1.f - 0.4f * Form);
		MasterMovementAmplitude = FMath::Clamp(Amp, 0.03f, 0.5f);
	}

	// ---- Master speed -----------------------------------------------------
	// Restlessness speeds time up; imperiousness + warmth slow it (commanding/calm).
	{
		const float Spd = 1.0f * (1.f + 1.2f * Rest) * (1.f - 0.45f * Imp) * (1.f - 0.2f * Warm);
		MasterMovementSpeed = FMath::Clamp(Spd, 0.4f, 2.5f);
	}

	// ---- Noise speed (fidget rate of the head drift) ----------------------
	// 0.225 was Ted's tuned baseline. Restless = twitchier, imperious = slower/statelier.
	NoiseSpeed = FMath::Clamp(0.225f * (1.f + 1.4f * Rest) * (1.f - 0.5f * Imp), 0.06f, 1.2f);

	// ---- Glance frequency (seconds BETWEEN glances; LOWER = more often) ----
	// Imperious + formal hold their gaze (longer intervals); restless darts around (shorter).
	{
		const float SlowFactor = (1.f + 1.5f * Imp + 0.5f * Form) / FMath::Max(0.2f, 1.f + 1.8f * Rest);
		GlanceIntervalIdle      = FMath::Clamp(4.0f * SlowFactor, 1.5f, 11.0f);
		GlanceIntervalSpeaking  = FMath::Clamp(2.0f * SlowFactor, 0.6f, 7.0f);
	}

	// ---- Glance range (how far a glance swings) ---------------------------
	// Restless = wide, wandering glances; imperious = tight, controlled.
	GlanceYawRangeDeg   = FMath::Clamp(14.0f * (1.f + 0.8f * Rest) * (1.f - 0.5f * Imp), 3.0f, 30.0f);
	GlancePitchRangeDeg = FMath::Clamp(8.0f  * (1.f + 0.8f * Rest) * (1.f - 0.5f * Imp), 2.0f, 18.0f);

	// ---- Gaze hold / look-away (eye contact discipline) -------------------
	// Both imperiousness (dominance stare) and warmth (engaged interest) hold gaze longer
	// and break it less. Restlessness shortens holds + breaks contact more.
	{
		const float HoldFactor = (1.f + 1.3f * Imp + 0.9f * Warm) / FMath::Max(0.2f, 1.f + 1.4f * Rest);
		GazeHoldMin = FMath::Clamp(2.0f * HoldFactor, 1.0f, 9.0f);
		GazeHoldMax = FMath::Clamp(4.0f * HoldFactor, GazeHoldMin + 0.5f, 11.0f);
		GazeAwayMin = FMath::Clamp(0.7f / FMath::Max(0.4f, HoldFactor), 0.3f, 4.0f);
		GazeAwayMax = FMath::Clamp(1.8f / FMath::Max(0.4f, HoldFactor), GazeAwayMin + 0.2f, 6.0f);
		// Chance of breaking eye contact each cycle: low for imperious/warm, high for restless.
		GazeAwayChance = FMath::Clamp(0.45f * (1.f - 0.7f * Imp - 0.5f * Warm + 0.6f * Rest), 0.05f, 0.9f);
	}

	// ---- Blink rate (intervals; LOWER = blinks more often) ----------------
	// Restless blinks faster; imperious/formal blink slower (controlled).
	{
		const float BlinkSlow = (1.f + 0.6f * Imp + 0.4f * Form) / FMath::Max(0.3f, 1.f + 1.0f * Rest);
		BlinkIntervalMin = FMath::Clamp(2.5f * BlinkSlow, 1.2f, 8.0f);
		BlinkIntervalMax = FMath::Clamp(6.0f * BlinkSlow, BlinkIntervalMin + 1.0f, 12.0f);
	}

	// ---- Spine sway + breathing (composure) -------------------------------
	// Formality + imperiousness stiffen the torso; restlessness loosens it.
	SpineAmplitudeDeg = FMath::Clamp(2.0f * (1.f - 0.7f * Form - 0.3f * Imp + 0.5f * Rest), 0.2f, 6.0f);
	BreathsPerMinute  = FMath::Clamp(13.0f * (1.f + 0.4f * Rest - 0.15f * Form), 8.0f, 22.0f);

	// ---- Head amplitude (per-state, on top of the master) -----------------
	// Imperious holds the head still; restless lets it drift more.
	IdleHeadAmplitudeDeg     = FMath::Clamp(1.75f * (1.f - 0.6f * Imp + 0.8f * Rest), 0.4f, 6.0f);
	SpeakingHeadAmplitudeDeg = FMath::Clamp(4.0f  * (1.f - 0.4f * Imp + 0.6f * Rest), 1.0f, 12.0f);

	// ---- Emotion readability ----------------------------------------------
	// Warmth makes the face a touch more expressive; imperiousness/formality restrain it.
	EmotionStrength = FMath::Clamp(0.5f * (1.f + 0.6f * Warm - 0.4f * Imp - 0.3f * Form), 0.15f, 1.2f);
}

void UTimeWalkIdleMotionComponent::SetPersonaTrait(const FString& TraitName, float Value)
{
	const float V = FMath::Clamp(Value, 0.f, 1.f);
	const FString T = TraitName.ToLower();
	if      (T == TEXT("imperiousness") || T == TEXT("imperious") || T == TEXT("imp"))  { Persona.Imperiousness = V; }
	else if (T == TEXT("restlessness")  || T == TEXT("restless")  || T == TEXT("rest")) { Persona.Restlessness = V; }
	else if (T == TEXT("warmth")        || T == TEXT("warm"))                            { Persona.Warmth = V; }
	else if (T == TEXT("formality")     || T == TEXT("formal")    || T == TEXT("form")) { Persona.Formality = V; }
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[TWPersona] unknown trait '%s' (use imperiousness|restlessness|warmth|formality)"), *TraitName);
		return;
	}
	bUsePersona = true;
	RebuildMotionFromPersona();
	UE_LOG(LogTemp, Warning, TEXT("[TWPersona] %s=%.2f on %s -> amp=%.2f spd=%.2f noise=%.2f glanceIdle=%.1f gazeHold=%.1f-%.1f awayCh=%.2f blink=%.1f-%.1f spine=%.1f"),
		*T, V, GetOwner() ? *GetOwner()->GetName() : TEXT("?"),
		MasterMovementAmplitude, MasterMovementSpeed, NoiseSpeed, GlanceIntervalIdle,
		GazeHoldMin, GazeHoldMax, GazeAwayChance, BlinkIntervalMin, BlinkIntervalMax, SpineAmplitudeDeg);
}

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::SetListening(bool bListening)
{
	// 2026-06-08 Ted 8387: log SetListening calls so we can tell if mic-click is
	// actually reaching the component.
	// 2026-06-10 Ted: on the RISING edge (just engaged listening), pick a random head
	// direction (left/right) and a random down-tilt so each listen looks a little
	// different and attentive. Locked for the duration so the pose doesn't jitter while
	// the envelope rises/falls. ListeningHeadYawDeg is now a positive MAGNITUDE.
	if (bListening && !bListeningRaw)
	{
		// 2026-06-10 Ted #8738: sample all three from their signed ranges (min/max may be
		// given in either order; FRandRange handles lo>hi fine after a swap). Held for the
		// engagement so the pose doesn't jitter.
		ActiveListeningYawDeg   = Rng.FRandRange(FMath::Min(ListeningHeadYawMinDeg,   ListeningHeadYawMaxDeg),
		                                         FMath::Max(ListeningHeadYawMinDeg,   ListeningHeadYawMaxDeg));
		ActiveListeningPitchDeg = Rng.FRandRange(FMath::Min(ListeningHeadPitchDownMin, ListeningHeadPitchDownMax),
		                                         FMath::Max(ListeningHeadPitchDownMin, ListeningHeadPitchDownMax)); // down magnitude
		ActiveListeningRollDeg  = Rng.FRandRange(FMath::Min(ListeningHeadRollMinDeg,  ListeningHeadRollMaxDeg),
		                                         FMath::Max(ListeningHeadRollMinDeg,  ListeningHeadRollMaxDeg));
	}

	if (TWIdle_Verbose())
	{
		UE_LOG(LogTemp, Display, TEXT("[TimeWalkIdle][SetListening] %d (was %d) activeYaw=%.1f activePitchDown=%.1f activeRoll=%.1f rise=%.2f"),
			bListening ? 1 : 0,
			bListeningRaw ? 1 : 0,
			ActiveListeningYawDeg,
			ActiveListeningPitchDeg,
			ActiveListeningRollDeg,
			ListeningRiseTime);
	}

	// 2026-06-18 Ted: KNOWING NOD on the FALLING edge of listening (was listening, now
	// not == the user just stopped speaking). Catches BOTH the VAD path and the mic-toggle
	// path in one spot. Skip if a reply is already speaking (no nod over speech).
	if (!bListening && bListeningRaw && !bSpeakingRaw)
	{
		MaybeTriggerKnowingNod();
	}

	bListeningRaw = bListening;
}

// ---------------------------------------------------------------------------
//  Knowing nod (2026-06-18 Ted)
// ---------------------------------------------------------------------------
void UTimeWalkIdleMotionComponent::MaybeTriggerKnowingNod()
{
	if (Rng.FRand() < FMath::Clamp(NodChance, 0.f, 1.f))
	{
		TriggerKnowingNod();
	}
}

void UTimeWalkIdleMotionComponent::TriggerKnowingNod()
{
	// Sample amplitude + cycle count once at trigger time; held for the duration so the
	// nod doesn't change shape mid-motion (mirrors the ActiveListening* per-engagement pick).
	const float Lo = FMath::Min(NodDepthMinDeg, NodDepthMaxDeg);
	const float Hi = FMath::Max(NodDepthMinDeg, NodDepthMaxDeg);
	NodActiveAmplitudeDeg = Rng.FRandRange(Lo, Hi);

	const int32 CLo = FMath::Min(NodCountMin, NodCountMax);
	const int32 CHi = FMath::Max(NodCountMin, NodCountMax);
	NodActiveCount = FMath::Max(1, Rng.RandRange(CLo, CHi));

	NodActivePeriod = FMath::Clamp(NodPeriodSeconds, 0.3f, 2.0f);
	NodElapsed = 0.f;
	bNodActive = true;

	if (TWIdle_Verbose())
	{
		UE_LOG(LogTemp, Display, TEXT("[TimeWalkIdle][Nod] trigger amp=%.1f count=%d period=%.2f"),
			NodActiveAmplitudeDeg, NodActiveCount, NodActivePeriod);
	}
}

void UTimeWalkIdleMotionComponent::UpdateKnowingNod(float DeltaTime)
{
	if (!bNodActive)
	{
		ActiveNodPitchDeg = 0.f;
		return;
	}

	NodElapsed += DeltaTime;

	// Total oscillation duration = NodActiveCount full down-up cycles at NodActivePeriod,
	// surrounded by an ease-in and ease-out ramp. The oscillation is a (1-cos) shape so it
	// STARTS and ENDS at 0 (chin dips down then returns), giving NodActiveCount clean dips.
	const float OscDuration = NodActiveCount * NodActivePeriod;
	const float Total = OscDuration + 2.f * NodRampSeconds;

	if (NodElapsed >= Total)
	{
		bNodActive = false;
		ActiveNodPitchDeg = 0.f;
		return;
	}

	// Envelope: ramp in over NodRampSeconds, hold at 1 during the oscillation, ramp out.
	float Env = 1.f;
	if (NodElapsed < NodRampSeconds)
	{
		Env = NodElapsed / NodRampSeconds;
	}
	else if (NodElapsed > NodRampSeconds + OscDuration)
	{
		Env = 1.f - (NodElapsed - NodRampSeconds - OscDuration) / NodRampSeconds;
	}
	Env = FMath::Clamp(Env, 0.f, 1.f);
	// Smoothstep the envelope so the in/out is gentle, not linear.
	Env = Env * Env * (3.f - 2.f * Env);

	// Oscillation phase only advances during the held window. (1-cos)/2 in [0,1]:
	// 0 at start of a cycle, 1 (full chin-down) at mid, back to 0 at cycle end.
	const float OscElapsed = FMath::Clamp(NodElapsed - NodRampSeconds, 0.f, OscDuration);
	const float CycleFrac = (NodActivePeriod > KINDA_SMALL_NUMBER) ? (OscElapsed / NodActivePeriod) : 0.f;
	const float Dip = 0.5f * (1.f - FMath::Cos(CycleFrac * 2.f * PI)); // 0..1 per cycle

	ActiveNodPitchDeg = NodActiveAmplitudeDeg * Dip * Env; // chin-down magnitude, positive
}

#if WITH_EDITOR
void UTimeWalkIdleMotionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// If a Persona trait (or the bUsePersona toggle) changed, re-derive the raw knobs so the
	// editor viewport reflects the new personality immediately. Cheap + idempotent.
	{
		const FName Member = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
		if (Member == GET_MEMBER_NAME_CHECKED(UTimeWalkIdleMotionComponent, Persona) ||
			Member == GET_MEMBER_NAME_CHECKED(UTimeWalkIdleMotionComponent, bUsePersona))
		{
			RebuildMotionFromPersona();
		}
	}

	// When tuning the listening Preview in the editor, make sure the owner's skeletal
	// meshes evaluate animation in-editor and get nudged to repaint so the new slider
	// value shows on the next viewport redraw (editor DeltaSeconds is often 0). #8740.
	if (AActor* Owner = GetOwner())
	{
		TArray<USkeletalMeshComponent*> SkelMeshes;
		Owner->GetComponents<USkeletalMeshComponent>(SkelMeshes);
		for (USkeletalMeshComponent* Mesh : SkelMeshes)
		{
			if (!Mesh) { continue; }
			Mesh->SetUpdateAnimationInEditor(true);
			Mesh->TickAnimation(0.f, false);   // re-evaluate now
			Mesh->RefreshBoneTransforms();
			Mesh->MarkRenderStateDirty();
		}
	}
}
#endif

void UTimeWalkIdleMotionComponent::GetListeningPose(float& OutWeight, float& OutYawDeg, float& OutPitchDeg, float& OutRollDeg) const
{
	// Editor preview: hold the pose at full weight and use the fixed signed Preview*
	// values so dragging a slider is exactly what you see. Only honored in editor worlds
	// (PIE/packaged ignore it). 2026-06-10 Ted #8732.
#if WITH_EDITOR
	if (bEditorPreviewListening)
	{
		const UWorld* W = GetWorld();
		const bool bEditorWorld = W && (W->WorldType == EWorldType::Editor || W->WorldType == EWorldType::EditorPreview);
		if (bEditorWorld)
		{
			OutWeight = 1.f;
			OutYawDeg = PreviewListeningYawDeg;
			OutPitchDeg = PreviewListeningPitchDownDeg;  // + = chin down
			OutRollDeg = PreviewListeningRollDeg;
			return;
		}
	}
#endif
	// Live runtime: smoothed envelope + the per-engagement random-picked signed values.
	OutWeight = ListeningWeight;
	OutYawDeg = ActiveListeningYawDeg;
	OutPitchDeg = ActiveListeningPitchDeg;       // down-tilt magnitude (negated at the bone)
	OutRollDeg = ActiveListeningRollDeg;
}

void UTimeWalkIdleMotionComponent::OnRegister()
{
	Super::OnRegister();

	if (!bAutoEnableInEditorTick)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	UWorld* World = Owner->GetWorld();
	if (!World)
	{
		return;
	}

	// Only act in the editor world (not PIE, not packaged game). PIE will use the
	// normal BeginPlay path and tick naturally.
	const bool bIsEditorWorld = (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::EditorPreview);
	if (!bIsEditorWorld)
	{
		return;
	}

	// Derive raw knobs from persona so editor-mode preview uses the authored personality.
	RebuildMotionFromPersona();

	// Make THIS component tick while the editor is open (without PIE) so the idle
	// motion math runs and writes head/neck rotations + face curves every editor tick.
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;

	// Flip Update Animation In Editor on every SkeletalMeshComponent on the owner.
	// This applies to MetaHuman body + face + any clothing/LOD meshes. Without this,
	// the AnimBP graph (including our FAnimNode_TimeWalkFace) doesn't evaluate in the
	// editor viewport, so dragging sliders has no visible effect until PIE.
	TArray<USkeletalMeshComponent*> SkelMeshes;
	Owner->GetComponents<USkeletalMeshComponent>(SkelMeshes);
	for (USkeletalMeshComponent* Mesh : SkelMeshes)
	{
		if (!Mesh)
		{
			continue;
		}
		Mesh->SetUpdateAnimationInEditor(true);
		// NOTE: the multi-threaded-animation flag lives on UAnimInstance, not the mesh,
		// and the AnimInstance doesn't exist yet at OnRegister time (created when the
		// mesh starts ticking). If editor-mode writes look raced (slider drags get lost),
		// flag down -- we can flip it on the instance after the mesh has init'd.
	}
}

void UTimeWalkIdleMotionComponent::BeginPlay()
{
	Super::BeginPlay();

	// Derive the raw motion knobs from the character's persona traits (no-op if bUsePersona=false).
	RebuildMotionFromPersona();

	Rng.Initialize(FMath::Rand());
	BlinkTimer = Rng.FRandRange(BlinkIntervalMin, BlinkIntervalMax);

	ResolveTargets();
	TryBindVoice();

	// Head/neck/spine bone motion is now owned by the AnimGraph (ABP_HamiltonBody reads
	// HeadRotation/NeckRotation/SpineRotation from TimeWalkIdleMotionAnimInstance and applies
	// them via Transform(Modify)Bone nodes -- the supported path). The old C++ bone-poke via
	// OnBoneTransformsFinalized was abandoned: it raced the retarget buffer flip (no visible
	// motion) and re-entered the finalize flow (stack overflow). This component now owns only
	// blink + the speaking/emotion envelope. DO NOT re-register the bone-finalized delegate.

	// Blink IS still C++-driven, but on the FACE mesh and applied in a post-eval hook so the
	// face AnimBP can't wipe it. Morphs (unlike the bone path) don't recurse the finalize flow.
	if (bEnableBlink && FaceMesh)
	{
		FOnBoneTransformsFinalizedMultiCast::FDelegate Del =
			FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &UTimeWalkIdleMotionComponent::OnFacePoseFinalized);
		FaceFinalizeHandle = FaceMesh->RegisterOnBoneTransformsFinalizedDelegate(Del);
	}
}

void UTimeWalkIdleMotionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (BodyMesh && BoneFinalizeHandle.IsValid())
	{
		BodyMesh->UnregisterOnBoneTransformsFinalizedDelegate(BoneFinalizeHandle);
		BoneFinalizeHandle.Reset();
	}

	if (FaceMesh && FaceFinalizeHandle.IsValid())
	{
		FaceMesh->UnregisterOnBoneTransformsFinalizedDelegate(FaceFinalizeHandle);
		FaceFinalizeHandle.Reset();
	}

#if TIMEWALK_WITH_INWORLD
	if (VoiceComponent)
	{
		if (VoiceStartHandle.IsValid())    { VoiceComponent->OnVoiceAudioStartNative.Remove(VoiceStartHandle); VoiceStartHandle.Reset(); }
		if (VoiceCompleteHandle.IsValid()) { VoiceComponent->OnVoiceAudioCompleteNative.Remove(VoiceCompleteHandle); VoiceCompleteHandle.Reset(); }
		VoiceComponent = nullptr;
	}
	CharacterComponent = nullptr;
#endif
	bVoiceBound = false;

	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
//  2026-06-09 Ted #8649: "what moves the head" experiment helpers
// ---------------------------------------------------------------------------
// Console commands (work in -game stream instances via -AllowPixelStreamingCommands
// or any console): TW.Idle.PauseBody 1|0, TW.Idle.PauseFace 1|0.
// Pausing uses USkeletalMeshComponent::bPauseAnims: freezes that mesh's ENTIRE anim
// evaluation (sequence players, procedural nodes, post-process AnimBP) at its current
// pose. Body frozen + head still moving => head motion comes from the FACE mesh path
// (ARKit/LiveLink neck rotation or RigLogic), not the body idle.

void UTimeWalkIdleMotionComponent::DebugPauseBodyAnim(bool bPause)
{
	if (BodyMesh)
	{
		BodyMesh->bPauseAnims = bPause;
		UE_LOG(LogTemp, Warning, TEXT("[TWIdle][Debug] body anim %s on %s"),
			bPause ? TEXT("PAUSED") : TEXT("RESUMED"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("?"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[TWIdle][Debug] PauseBody: no BodyMesh resolved"));
	}
}

void UTimeWalkIdleMotionComponent::DebugPauseFaceAnim(bool bPause)
{
	if (FaceMesh)
	{
		FaceMesh->bPauseAnims = bPause;
		UE_LOG(LogTemp, Warning, TEXT("[TWIdle][Debug] face anim %s on %s"),
			bPause ? TEXT("PAUSED") : TEXT("RESUMED"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("?"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[TWIdle][Debug] PauseFace: no FaceMesh resolved"));
	}
}

static void TWIdle_ForEachComponent(UWorld* World, TFunctionRef<void(UTimeWalkIdleMotionComponent*)> Fn)
{
	int32 Visited = 0;
	for (TObjectIterator<UTimeWalkIdleMotionComponent> It; It; ++It)
	{
		// Don't filter by the dispatching world -- PS data-channel commands can arrive
		// with a different/null world context than the game world. Game-world instances
		// are the registered, non-template ones.
		if (!It->IsTemplate() && It->IsRegistered())
		{
			Fn(*It);
			++Visited;
		}
	}
	if (TWIdle_Verbose())
	{
		UE_LOG(LogTemp, Display, TEXT("[TWIdle][Debug] console cmd visited %d component(s) (dispatch world=%s)"),
			Visited, World ? *World->GetName() : TEXT("null"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GTWIdlePauseBodyCmd(
	TEXT("TW.Idle.PauseBody"),
	TEXT("Pause (1) / resume (0) BODY skeletal-mesh animation on all TimeWalk idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			const bool bPause = Args.Num() == 0 || Args[0] != TEXT("0");
			TWIdle_ForEachComponent(World, [bPause](UTimeWalkIdleMotionComponent* C) { C->DebugPauseBodyAnim(bPause); });
		}));

static FAutoConsoleCommandWithWorldAndArgs GTWIdlePauseFaceCmd(
	TEXT("TW.Idle.PauseFace"),
	TEXT("Pause (1) / resume (0) FACE skeletal-mesh animation on all TimeWalk idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			const bool bPause = Args.Num() == 0 || Args[0] != TEXT("0");
			TWIdle_ForEachComponent(World, [bPause](UTimeWalkIdleMotionComponent* C) { C->DebugPauseFaceAnim(bPause); });
		}));

// 2026-06-09 Ted #8684: drive the listening pose from the console/data channel so
// it can be tested without clicking the in-stream mic button. TW.Idle.Listen 1|0.
static FAutoConsoleCommandWithWorldAndArgs GTWIdleListenCmd(
	TEXT("TW.Idle.Listen"),
	TEXT("Engage (1) / release (0) the listening pose (head turn + gaze lock) on all TimeWalk idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			const bool bOn = Args.Num() == 0 || Args[0] != TEXT("0");
			TWIdle_ForEachComponent(World, [bOn](UTimeWalkIdleMotionComponent* C) { C->SetListening(bOn); });
		}));

// 2026-06-09 Ted #8685: live head-motion amplitude from the web page slider.
// TW.Idle.Amplitude <0..1>. Sets MasterMovementAmplitude on all idle-motion
// characters at runtime (not persisted -- page slider is a live tuning control).
static FAutoConsoleCommandWithWorldAndArgs GTWIdleAmplitudeCmd(
	TEXT("TW.Idle.Amplitude"),
	TEXT("Set head/body procedural motion amplitude 0..1 on all TimeWalk idle-motion characters (web slider)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() == 0) { return; }
			const float Amp = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 1.f);
			TWIdle_ForEachComponent(World, [Amp](UTimeWalkIdleMotionComponent* C)
			{
				C->MasterMovementAmplitude = Amp;
				UE_LOG(LogTemp, Warning, TEXT("[TWIdle][Amplitude] set %.2f on %s"),
					Amp, C->GetOwner() ? *C->GetOwner()->GetName() : TEXT("?"));
			});
		}));

// 2026-06-13 Ted: live persona tuning from the console / PS data channel.
//   TW.Persona.Set <trait> <0..1>   -- e.g. TW.Persona.Set imperiousness 0.8
//   TW.Persona.Imperious <0..1>     -- shorthands per trait
//   TW.Persona.Restless  <0..1>
//   TW.Persona.Warm      <0..1>
//   TW.Persona.Formal    <0..1>
// Applies to ALL idle-motion characters in the world (typically just the active one).
// Live-only -- to PERSIST a value, set it on the character's BP component Details panel.
static FAutoConsoleCommandWithWorldAndArgs GTWPersonaSetCmd(
	TEXT("TW.Persona.Set"),
	TEXT("Set a persona trait live: TW.Persona.Set <imperiousness|restlessness|warmth|formality> <0..1>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 2) { UE_LOG(LogTemp, Warning, TEXT("[TWPersona] usage: TW.Persona.Set <trait> <0..1>")); return; }
			const FString Trait = Args[0];
			const float Val = FCString::Atof(*Args[1]);
			TWIdle_ForEachComponent(World, [&Trait, Val](UTimeWalkIdleMotionComponent* C) { C->SetPersonaTrait(Trait, Val); });
		}));

static void TWPersona_SetOne(const TArray<FString>& Args, UWorld* World, const TCHAR* Trait)
{
	if (Args.Num() == 0) { UE_LOG(LogTemp, Warning, TEXT("[TWPersona] usage: <cmd> <0..1>")); return; }
	const float Val = FCString::Atof(*Args[0]);
	const FString T = Trait;
	TWIdle_ForEachComponent(World, [&T, Val](UTimeWalkIdleMotionComponent* C) { C->SetPersonaTrait(T, Val); });
}

static FAutoConsoleCommandWithWorldAndArgs GTWPersonaImpCmd(
	TEXT("TW.Persona.Imperious"), TEXT("Set imperiousness 0..1 live on all idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World) { TWPersona_SetOne(Args, World, TEXT("imperiousness")); }));

static FAutoConsoleCommandWithWorldAndArgs GTWPersonaRestCmd(
	TEXT("TW.Persona.Restless"), TEXT("Set restlessness 0..1 live on all idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World) { TWPersona_SetOne(Args, World, TEXT("restlessness")); }));

static FAutoConsoleCommandWithWorldAndArgs GTWPersonaWarmCmd(
	TEXT("TW.Persona.Warm"), TEXT("Set warmth 0..1 live on all idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World) { TWPersona_SetOne(Args, World, TEXT("warmth")); }));

static FAutoConsoleCommandWithWorldAndArgs GTWPersonaFormCmd(
	TEXT("TW.Persona.Formal"), TEXT("Set formality 0..1 live on all idle-motion characters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World) { TWPersona_SetOne(Args, World, TEXT("formality")); }));

// ---------------------------------------------------------------------------
//  Target resolution (auto-detect so the user just drops the component on)
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::ResolveTargets()
{
	AActor* Owner = GetOwner();
	if (!Owner) { return; }

	TArray<USkeletalMeshComponent*> Meshes;
	Owner->GetComponents<USkeletalMeshComponent>(Meshes);

	// Explicit overrides first.
	for (USkeletalMeshComponent* M : Meshes)
	{
		if (!M) { continue; }
		if (BodyMeshComponentName != NAME_None && M->GetFName() == BodyMeshComponentName) { BodyMesh = M; }
		if (FaceMeshComponentName != NAME_None && M->GetFName() == FaceMeshComponentName) { FaceMesh = M; }
	}

	// Auto-detect: the BODY mesh contains our HeadBone; the FACE mesh contains the blink morph
	// or is the one WITHOUT the body skeleton. MetaHuman names the face mesh "Face".
	for (USkeletalMeshComponent* M : Meshes)
	{
		if (!M || !M->GetSkeletalMeshAsset()) { continue; }

		const bool bHasHead = (M->GetBoneIndex(HeadBone) != INDEX_NONE);
		const FString Name = M->GetName();

		if (!BodyMesh && bHasHead && !Name.Contains(TEXT("Face")))
		{
			BodyMesh = M;
		}
		if (!FaceMesh && (Name.Contains(TEXT("Face")) || Name.Contains(TEXT("Head"))))
		{
			FaceMesh = M;
		}
	}

	// Fallbacks: if only one mesh, it's the body. If body still missing, take the first with head.
	if (!BodyMesh)
	{
		for (USkeletalMeshComponent* M : Meshes)
		{
			if (M && M->GetBoneIndex(HeadBone) != INDEX_NONE) { BodyMesh = M; break; }
		}
	}
	if (!BodyMesh && Meshes.Num() > 0) { BodyMesh = Meshes[0]; }

	bBodyMeshFound = (BodyMesh != nullptr);
	bFaceMeshFound = (FaceMesh != nullptr);

	// --- Gaze: resolve eye bones on the face mesh (gaze no-ops if absent) --
	LeftEyeBoneIndex = INDEX_NONE;
	RightEyeBoneIndex = INDEX_NONE;
	if (FaceMesh)
	{
		LeftEyeBoneIndex = FaceMesh->GetBoneIndex(LeftEyeBone);
		RightEyeBoneIndex = FaceMesh->GetBoneIndex(RightEyeBone);
	}
	// Gaze is now MORPH-driven (eye_look{Left,Right,Up,Down}_{L,R}), not bone-driven.
	// It only needs a Face mesh, not resolved eye bones -- so enable whenever the face exists.
	bGazeBonesFound = (FaceMesh != nullptr);

	// --- Diagnostics: confirm what resolved at runtime ---------------------
	{
		FString MeshList;
		for (USkeletalMeshComponent* M : Meshes)
		{
			if (!M) { continue; }
			MeshList += FString::Printf(TEXT("[%s head=%d] "),
				*M->GetName(),
				M->GetBoneIndex(HeadBone));
		}
		UE_LOG(LogTemp, Warning,
			TEXT("[TWIdle] ResolveTargets owner=%s Body=%s Face=%s headBoneIdx=%d | meshes: %s"),
			*GetNameSafe(Owner),
			BodyMesh ? *BodyMesh->GetName() : TEXT("NULL"),
			FaceMesh ? *FaceMesh->GetName() : TEXT("NULL"),
			BodyMesh ? BodyMesh->GetBoneIndex(HeadBone) : -2,
			*MeshList);
		if (GEngine && bShowDebugOverlay)
		{
			GEngine->AddOnScreenDebugMessage(7001, 30.f,
				BodyMesh ? FColor::Green : FColor::Red,
				FString::Printf(TEXT("[TWIdle] Body=%s Face=%s headIdx=%d"),
					BodyMesh ? *BodyMesh->GetName() : TEXT("NULL"),
					FaceMesh ? *FaceMesh->GetName() : TEXT("NULL"),
					BodyMesh ? BodyMesh->GetBoneIndex(HeadBone) : -2));
		}
	}
}

void UTimeWalkIdleMotionComponent::TryBindVoice()
{
#if TIMEWALK_WITH_INWORLD
	// Runtime toggle: when Inworld is compiled in but disabled on this component,
	// skip the voice/character binding entirely -> pure-idle behavior.
	if (!bEnableInworld) { return; }

	AActor* Owner = GetOwner();
	if (!Owner) { return; }

	if (!VoiceComponent)
	{
		VoiceComponent = Owner->FindComponentByClass<UInworldVoiceAudioComponent>();
	}
	if (VoiceComponent && !bVoiceBound)
	{
		// Bind the NATIVE (non-dynamic) delegates via AddUObject -> no UFUNCTION needed, so
		// the handlers stay plain C++ guarded by TIMEWALK_WITH_INWORLD (no reflection). Clear
		// any stale handle first so a PIE-restart re-bind can't double-add.
		if (VoiceStartHandle.IsValid())    { VoiceComponent->OnVoiceAudioStartNative.Remove(VoiceStartHandle); }
		if (VoiceCompleteHandle.IsValid()) { VoiceComponent->OnVoiceAudioCompleteNative.Remove(VoiceCompleteHandle); }
		VoiceStartHandle    = VoiceComponent->OnVoiceAudioStartNative.AddUObject(this, &UTimeWalkIdleMotionComponent::HandleVoiceStart);
		VoiceCompleteHandle = VoiceComponent->OnVoiceAudioCompleteNative.AddUObject(this, &UTimeWalkIdleMotionComponent::HandleVoiceComplete);
		bVoiceBound = true;
	}

	if (!CharacterComponent)
	{
		CharacterComponent = Owner->FindComponentByClass<UInworldCharacterComponent>();
	}
#endif // TIMEWALK_WITH_INWORLD
}

#if TIMEWALK_WITH_INWORLD
void UTimeWalkIdleMotionComponent::HandleVoiceStart(UInworldVoiceAudioComponent*, const FInworldData_TTSOutput&, bool)
{
	bSpeakingRaw = true;
}

void UTimeWalkIdleMotionComponent::HandleVoiceComplete(UInworldVoiceAudioComponent*, const FInworldData_TTSOutput&, bool)
{
	bSpeakingRaw = false;
}
#endif // TIMEWALK_WITH_INWORLD

// ---------------------------------------------------------------------------
//  Noise
// ---------------------------------------------------------------------------

float UTimeWalkIdleMotionComponent::Noise1D(float T, float Seed)
{
	// UE's PerlinNoise1D spans only ~[-0.5,0.5]; renormalize to ~[-1,1] so degree tunables
	// mean actual degrees. (Same fix as the retired anim-instance version.)
	const float Raw = FMath::PerlinNoise1D(T + Seed);
	return FMath::Clamp(Raw * 2.0f, -1.0f, 1.0f);
}

float UTimeWalkIdleMotionComponent::CurrentEmotionBias() const
{
#if TIMEWALK_WITH_INWORLD
	// Runtime toggle: emotion bias only when Inworld is enabled on this component.
	if (!bEnableInworld) { return 1.0f; }
	if (!bUseEmotionBias || !CharacterComponent) { return 1.0f; }

	const UInworldGraphRuntimeData_EmotionState* Emotion = CharacterComponent->EmotionState;
	if (!Emotion) { return 1.0f; }

	switch (Emotion->GetEmotionLabel())
	{
	case EInworldEmotionLabel::BELLIGERENCE:
	case EInworldEmotionLabel::DOMINEERING:
	case EInworldEmotionLabel::CONTEMPT:
		return 1.5f;  // agitated -> more head movement / faster glances
	case EInworldEmotionLabel::CRITICISM:
		return 1.3f;
	case EInworldEmotionLabel::TENSION:
	case EInworldEmotionLabel::TENSE_HUMOR:
		return 1.25f;
	case EInworldEmotionLabel::STONEWALLING:
		return 0.7f;  // withdrawn -> stiller
	default:
		return 1.0f;  // NEUTRAL etc.
	}
#else
	return 1.0f;  // Inworld compiled out -> neutral bias
#endif
}

// ---------------------------------------------------------------------------
//  Tick: compute envelope + glance + smoothed rotations (NO bone write here)
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (DeltaTime <= 0.f) { return; }

	if (!bVoiceBound) { TryBindVoice(); }

	// #10 smoothly drive the emotion expression toward its current target each frame.
	UpdateEmotionFace(DeltaTime);

	// --- Speaking envelope -------------------------------------------------
	const float TargetIntensity = bSpeakingRaw ? 1.f : 0.f;
	const float TC = bSpeakingRaw ? SpeakingAttackTime : SpeakingReleaseTime;
	SpeakingIntensity = ApproachExpComp(SpeakingIntensity, TargetIntensity, DeltaTime, TC);

	const float EmotionBias = CurrentEmotionBias();

	// 2026-06-08 Ted #8507: master gain knobs. Speed multiplies ALL clocks; Amplitude multiplies ALL
	// rotation magnitudes. Setting Speed=0 freezes time-evolving motion (breathing/glance still fade
	// to their current values then hold). Setting Amplitude=0 produces a perfectly-still head.
	const float MasterSpeed = FMath::Max(0.f, MasterMovementSpeed);
	const float MasterAmp   = FMath::Max(0.f, MasterMovementAmplitude);

	// 2026-06-09 Ted #8649: diagnostic for "sliders have no effect". Logs the LIVE
	// instance's master values once per second. If you drag the slider in the editor
	// and this logged value does NOT change -> editor-side propagation bug (edits
	// landing on the BP CDO / wrong instance, not the running component). If the
	// value DOES change but motion doesn't -> downstream math bug. One test decides.
#if TIMEWALK_IDLE_DIAG
	{
		static float MasterDiagAccum = 0.f;
		MasterDiagAccum += DeltaTime;
		if (MasterDiagAccum > 1.f)
		{
			MasterDiagAccum = 0.f;
			UE_LOG(LogTemp, Warning, TEXT("[TWIdle][Master] this=%p amp=%.2f speed=%.2f headMotion=%d owner=%s"),
				this, MasterAmp, MasterSpeed, bEnableHeadMotion ? 1 : 0,
				GetOwner() ? *GetOwner()->GetName() : TEXT("null"));
		}
	}
#endif

	// --- Clocks ------------------------------------------------------------
	// Idle is calm; speeds up ~2x while speaking so talking reads lively but idle reads still.
	const float SpeechSpeedMul = FMath::Lerp(1.0f, 2.0f, SpeakingIntensity);
	NoiseTime += DeltaTime * NoiseSpeed * SpeechSpeedMul * FMath::Lerp(1.0f, EmotionBias, 0.5f) * MasterSpeed;
	BreathPhase += DeltaTime * (BreathsPerMinute / 60.f) * 2.f * PI * MasterSpeed;
	if (BreathPhase > 2.f * PI * 1024.f) { BreathPhase = FMath::Fmod(BreathPhase, 2.f * PI); }

	// --- Listening envelope ------------------------------------------------
	// Smooth 0..1 weight that the rest of the math blends against. When mic is on,
	// rises toward 1 (with ListeningRiseTime time-constant); when off, falls back
	// to 0 at the same rate. Driven by SetListening() called from chat widget.
	ListeningWeight = ApproachExpComp(ListeningWeight, bListeningRaw ? 1.f : 0.f, DeltaTime, ListeningRiseTime);

	// 2026-06-18 Ted: advance the knowing-nod state machine. Updates ActiveNodPitchDeg,
	// which the AnimInstance reads via GetKnowingNodPitchDeg() and adds to the head pitch.
	UpdateKnowingNod(DeltaTime);

	// 2026-06-08 Ted 8387: log envelope state every ~1s while listening so we can
	// tell if the math is running. Tossed when listening pose is confirmed working.
#if TIMEWALK_IDLE_DIAG
	static float DiagLogAccum = 0.f;
	DiagLogAccum += DeltaTime;
	if (bListeningRaw && DiagLogAccum > 1.f)
	{
		DiagLogAccum = 0.f;
		UE_LOG(LogTemp, Warning, TEXT("[TimeWalkIdle][Tick] listening=1 weight=%.2f bEnableHeadMotion=%d targetRollDeg=%.1f"),
			ListeningWeight,
			bEnableHeadMotion ? 1 : 0,
			ActiveListeningRollDeg);
	}
#endif

	// --- Look-around: pick a new glance target on a timer ------------------
	if (bEnableLookAround)
	{
		GlanceTimer -= DeltaTime;
		if (GlanceTimer <= 0.f)
		{
			const float BaseInterval = FMath::Lerp(GlanceIntervalIdle, GlanceIntervalSpeaking, SpeakingIntensity);
			// 2026-06-08 Ted #8507: master speed shortens the gap between glances (higher speed = more frequent).
			const float SpeedDenom = FMath::Max(0.05f, EmotionBias * FMath::Max(0.05f, MasterSpeed));
			GlanceTimer = FMath::Max(0.2f, (BaseInterval / SpeedDenom) * Rng.FRandRange(0.6f, 1.4f));
			GlanceTarget.X = Rng.FRandRange(-GlanceYawRangeDeg, GlanceYawRangeDeg) * MasterAmp;
			GlanceTarget.Y = Rng.FRandRange(-GlancePitchRangeDeg, GlancePitchRangeDeg) * MasterAmp;
		}
		// Saccade-like ease toward the target.
		GlanceCurrent.X = FMath::FInterpTo(GlanceCurrent.X, GlanceTarget.X, DeltaTime, 6.f);
		GlanceCurrent.Y = FMath::FInterpTo(GlanceCurrent.Y, GlanceTarget.Y, DeltaTime, 6.f);
	}
	else
	{
		GlanceCurrent = FVector2D::ZeroVector;
	}

	// Listening: suppress random glance-aways so head/gaze lock on the user.
	GlanceCurrent = FMath::Lerp(GlanceCurrent, FVector2D::ZeroVector, ListeningWeight);

	// --- Amplitudes --------------------------------------------------------
	// 2026-06-08 Ted #8507: master amplitude applies once here -- everything downstream
	// (head, neck, spine, glance, gaze) scales together. Set to 0 for a perfectly still head.
	const float HeadAmp = (IdleHeadAmplitudeDeg + SpeakingHeadAmplitudeDeg * SpeakingIntensity) * EmotionBias * MasterAmp;
	const float SpineAmp = SpineAmplitudeDeg * (1.f + 0.5f * SpeakingIntensity) * EmotionBias * MasterAmp;

	// --- Head: noise drift + glance + breathing nod ------------------------
	const float NoisyHeadYaw = Noise1D(NoiseTime, 13.1f) * HeadAmp + GlanceCurrent.X;
	// 2026-06-09 Ted #8684: listening also turns the head (yaw), not just roll --
	// roll-only was invisible. Blend noisy yaw toward the sustained listening turn.
	const float HeadYaw = FMath::Lerp(NoisyHeadYaw, ActiveListeningYawDeg, ListeningWeight);
	const float HeadPitch = Noise1D(NoiseTime, 71.7f) * HeadAmp * 0.7f + GlanceCurrent.Y
		+ (bEnableBreathing ? FMath::Sin(BreathPhase) * 0.6f : 0.f);
	const float NoisyHeadRoll = Noise1D(NoiseTime, 137.3f) * HeadAmp * 0.35f;
	// Listening: blend the noisy roll toward a sustained cocked-head pose.
	const float HeadRoll = FMath::Lerp(NoisyHeadRoll, ActiveListeningRollDeg, ListeningWeight);

	const FRotator TargetHead = bEnableHeadMotion ? FRotator(HeadPitch, HeadYaw, HeadRoll) : FRotator::ZeroRotator;
	const FRotator TargetNeck = TargetHead * 0.5f; // cervical chain follows at half
	const float SpineYaw = Noise1D(NoiseTime * 0.5f, 211.9f) * SpineAmp;
	const float SpinePitch = (bEnableBreathing ? FMath::Sin(BreathPhase) * SpineAmp * 0.6f : 0.f);
	const FRotator TargetSpine = bEnableHeadMotion ? FRotator(SpinePitch, SpineYaw, 0.f) : FRotator::ZeroRotator;

	// --- Smooth toward targets --------------------------------------------
	const float Smooth = 0.10f;
	HeadRot = FMath::RInterpTo(HeadRot, TargetHead, DeltaTime, 1.f / Smooth);
	NeckRot = FMath::RInterpTo(NeckRot, TargetNeck, DeltaTime, 1.f / Smooth);
	SpineRot = FMath::RInterpTo(SpineRot, TargetSpine, DeltaTime, 1.f / Smooth);

	// --- Blink (face) ------------------------------------------------------
	if (bEnableBlink) { ApplyBlink(DeltaTime); }

	// --- Gaze (eyes) -------------------------------------------------------
	// Compute target eye angles here; the actual additive eye-bone write happens in
	// OnFacePoseFinalized (post-AnimBP) alongside blink.
	if (bEnableGaze && bGazeBonesFound) { UpdateGaze(DeltaTime); }

	// --- Apply RigLogic INPUT curves (BEFORE eval this frame) --------------
	// We tick at TG_PrePhysics, so SetMorphTarget here puts the CTRL_expressions_* curves in
	// the input pool that the Face post-process AnimBP (RigLogic) reads to drive lids/eyes.
	// On MetaHuman, SetMorphTarget(name, w) sets the named anim curve as well as any morph.
	if (FaceMesh)
	{
		// SetMorphTarget only drives MORPH assets, not RigLogic CONTROL curves (CTRL_expressions_*)
		// -> it silently no-ops on control-curve names. The correct injection is the Face
		// AnimInstance's AddCurveValue, which puts the value in the curve pool the post-process
		// AnimBP (RigLogic) reads. Apply every frame; RigLogic converts it to lid/eye pose.
		if (UAnimInstance* FaceAnim = FaceMesh->GetAnimInstance())
		{
			// DRIVER CURVES: the Face AnimBP re-evaluates its pose, so curves we AddCurveValue
			// here do NOT survive into the final pose unless a graph node reads them. We write
			// 3 simple driver curves; a Modify Curve node in Face_AnimBP_Hamilton reads these
			// and fans them out into the real CTRL_expressions_* curves before Output Pose.
			// (This mirrors how FAnimNode_InworldViseme injects visemes -- the proven pattern.)
			const float BlinkVal = bForceBlinkTest ? 1.0f : (bEnableBlink ? CurrentBlinkValue : 0.f);
			const float YawN   = (bEnableGaze && bGazeBonesFound) ? FMath::Clamp(GazeCurrentDeg.X / FMath::Max(1.f, MaxEyeYawDeg),   -1.f, 1.f) : 0.f;
			const float PitchN = (bEnableGaze && bGazeBonesFound) ? FMath::Clamp(GazeCurrentDeg.Y / FMath::Max(1.f, MaxEyePitchDeg), -1.f, 1.f) : 0.f;

			// Per-direction 0..1 drivers so the graph Modify Curve node is a trivial 1:1 copy
			// (no math in the graph): CTRL_..._eyeBlinkL = TW_Blink, ..._eyeLookRightL = TW_LookRight, etc.
			FaceAnim->AddCurveValue(DriveBlink,     BlinkVal);
			FaceAnim->AddCurveValue(DriveLookRight, FMath::Max(0.f,  YawN));
			FaceAnim->AddCurveValue(DriveLookLeft,  FMath::Max(0.f, -YawN));
			FaceAnim->AddCurveValue(DriveLookUp,    FMath::Max(0.f,  PitchN));
			FaceAnim->AddCurveValue(DriveLookDown,  FMath::Max(0.f, -PitchN));

			// Also write the real control curves directly, in case a future setup reads them
			// pre-graph; harmless if the graph overwrites. Belt and suspenders.
			if (bEnableBlink)
			{
				FaceAnim->AddCurveValue(BlinkMorphLeft,  BlinkVal);
				FaceAnim->AddCurveValue(BlinkMorphRight, BlinkVal);
			}
			if (bEnableGaze && bGazeBonesFound)
			{
				ApplyGazeCurves(FaceAnim);
			}
			bFaceAnimResolved = true;
		}
		else
		{
			bFaceAnimResolved = false;
		}
	}

	// Log heartbeat (every ~1s) so I can verify from the log file, not just on-screen.
#if TIMEWALK_IDLE_DIAG
	TickLogAccum += DeltaTime;
	if (TickLogAccum >= 1.0f)
	{
		TickLogAccum = 0.f;
		UE_LOG(LogTemp, Warning, TEXT("[TWIdle LOG] blink=%.2f phase=%.2f timer=%.2f emoBias=%.2f speak=%.2f faceAnim=%d forceBlink=%d"),
			CurrentBlinkValue, BlinkPhase, BlinkTimer, CurrentEmotionBias(), SpeakingIntensity,
			bFaceAnimResolved ? 1 : 0, bForceBlinkTest ? 1 : 0);
	}
#endif

	// Always-on diagnostic so we can confirm on the Pixel Streaming view whether values flow.
	if (GEngine && bShowDebugOverlay)
	{
		GEngine->AddOnScreenDebugMessage(7004, 0.f, FColor::Orange,
			FString::Printf(TEXT("[TWIdle TICK] blink=%.2f(en%d phase%.2f t%.2f) eyeTgt(%.1f,%.1f) eyeCur(%.1f,%.1f) gz%d"),
				CurrentBlinkValue, bEnableBlink ? 1 : 0, BlinkPhase, BlinkTimer,
				GazeTargetDeg.X, GazeTargetDeg.Y,
				GazeCurrentDeg.X, GazeCurrentDeg.Y,
				(bEnableGaze && bGazeBonesFound) ? 1 : 0));
		GEngine->AddOnScreenDebugMessage(7005, 0.f, FColor::Cyan,
			FString::Printf(TEXT("[TWIdle DOT] fwd/right/up dots logged; headIdx=%d bGaze=%d bBlink=%d"),
				BodyMesh ? BodyMesh->GetBoneIndex(HeadBone) : -1,
				bEnableGaze ? 1 : 0, bEnableBlink ? 1 : 0));
	}

	// 2026-06-08 Ted 8427: finally invoke the post-pose hook. We're now in
	// TG_PostPhysics so the body AnimBP has already evaluated; this is the right
	// time to layer our additive HeadRot/NeckRot/SpineRot on top of the AnimBP
	// pose. Without this call, all our head-rotation math was computed and thrown
	// away every frame (listening-pose bug).
	OnBodyPoseFinalized();
}

// ---------------------------------------------------------------------------
//  Blink: drive face morph targets directly (composes with visemes -- eyes vs mouth)
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::ApplyBlink(float DeltaTime)
{
	// State machine ONLY: compute CurrentBlinkValue (0=open, 1=closed). The actual morph
	// write happens in OnFacePoseFinalized so it survives the face AnimBP.
	if (BlinkPhase < 0.f)
	{
		CurrentBlinkValue = 0.f; // eyes open between blinks
		// Not blinking; count down. Blink a bit faster while speaking/agitated.
		// Listening: slower blinks (focused attention) -- divide rate by ListeningBlinkSlowdown,
		// weighted by ListeningWeight so it eases in/out smoothly.
		const float Speed = FMath::Lerp(1.0f, 1.4f, SpeakingIntensity) * CurrentEmotionBias();
		const float ListeningRateMul = FMath::Lerp(1.0f, 1.0f / FMath::Max(0.25f, ListeningBlinkSlowdown), ListeningWeight);
		const float Rate = Speed * ListeningRateMul;
		BlinkTimer -= DeltaTime * Rate;
		if (BlinkTimer <= 0.f)
		{
			BlinkPhase = 0.f; // start a blink
			BlinkTimer = Rng.FRandRange(BlinkIntervalMin, BlinkIntervalMax);
		}
		else
		{
			return;
		}
	}

	// Advance the blink. 0..0.5 close, 0.5..1 open. Triangle profile.
	BlinkPhase += DeltaTime / FMath::Max(0.02f, BlinkDuration);
	const float Closed = (BlinkPhase < 0.5f)
		? (BlinkPhase / 0.5f)
		: FMath::Max(0.f, 1.f - (BlinkPhase - 0.5f) / 0.5f);

	CurrentBlinkValue = FMath::Clamp(Closed, 0.f, 1.f);

	if (BlinkPhase >= 1.f)
	{
		BlinkPhase = -1.f;       // done
		CurrentBlinkValue = 0.f; // eyes open
	}
}

// ---------------------------------------------------------------------------
//  Face post-eval hook: write eye-blink morphs AFTER Face_AnimBP_Hamilton runs,
//  so the face AnimBP can't overwrite them this frame.
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::OnFacePoseFinalized()
{
	if (!FaceMesh) { return; }

	// ONE-TIME: dump the real ANIM CURVE names (RigLogic CTRL_expressions_* inputs) on the
	// Face skeleton so we confirm the exact control names instead of guessing convention.
	if (!bMorphNamesDumped)
	{
		bMorphNamesDumped = true;
		if (USkeletalMesh* SkM = FaceMesh->GetSkeletalMeshAsset())
		{
			if (USkeleton* Skel = SkM->GetSkeleton())
			{
				int32 total = 0;
				Skel->ForEachCurveMetaData([&total](FName CurveName, const FCurveMetaData&)
				{
					++total;
					const FString N = CurveName.ToString();
					if (N.Contains(TEXT("eyeBlink")) || N.Contains(TEXT("eyeLook")) || N.Contains(TEXT("EyeBlink")) || N.Contains(TEXT("EyeLook")))
					{
						UE_LOG(LogTemp, Warning, TEXT("[TWIdle CURVES]   %s"), *N);
					}
				});
				UE_LOG(LogTemp, Warning, TEXT("[TWIdle CURVES] Face skeleton has %d anim curves (eyeBlink/eyeLook listed above)"), total);
				if (GEngine && bShowDebugOverlay)
				{
					GEngine->AddOnScreenDebugMessage(7007, 30.f, FColor::Magenta,
						FString::Printf(TEXT("[TWIdle CURVES] Face animCurves=%d (see Output Log for eyeBlink/eyeLook names)"), total));
				}
			}
		}
	}

	// NOTE: blink + gaze curve writes moved to TickComponent (PrePhysics, BEFORE eval) so
	// RigLogic reads them as CTRL_expressions_* INPUT curves. Writing here (post-eval) is too
	// late -- RigLogic already ran and overwrites raw morph outputs. This hook is now diag-only.

	if (GEngine && bShowDebugOverlay)
	{
		GEngine->AddOnScreenDebugMessage(7003, 0.5f, FColor::Yellow,
			FString::Printf(TEXT("[TWIdle] blink=%.2f gaze=%s eye(%.0f,%.0f)"),
				CurrentBlinkValue,
				bGazeLookingAtCamera ? TEXT("CAM") : TEXT("away"),
				GazeCurrentDeg.X, GazeCurrentDeg.Y));
	}
}

// ---------------------------------------------------------------------------
//  Gaze state machine: hold eye contact on the player camera, glance away on a
//  randomized timer, return. Computes GazeCurrentDeg (smoothed eye yaw/pitch).
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::UpdateGaze(float DeltaTime)
{
	// Advance the hold/away state machine.
	GazeStateTimer -= DeltaTime;
	if (GazeStateTimer <= 0.f)
	{
		// While speaking, look away less (more engaged eye contact).
		const float AwayChance = GazeAwayChance * FMath::Lerp(1.0f, 0.4f, SpeakingIntensity);
		const bool bWasLooking = bGazeLookingAtCamera;
		if (bWasLooking)
		{
			// Decide whether to break contact.
			bGazeLookingAtCamera = (Rng.FRand() >= AwayChance);
		}
		else
		{
			// Always return to the camera after a look-away.
			bGazeLookingAtCamera = true;
		}

		if (bGazeLookingAtCamera)
		{
			GazeStateTimer = Rng.FRandRange(GazeHoldMin, GazeHoldMax);
		}
		else
		{
			GazeStateTimer = Rng.FRandRange(GazeAwayMin, GazeAwayMax);
			// Pick a look-away offset (eyes drift to a plausible nearby point).
			GazeTargetDeg.X = Rng.FRandRange(-MaxEyeYawDeg, MaxEyeYawDeg);
			GazeTargetDeg.Y = Rng.FRandRange(-MaxEyePitchDeg * 0.6f, MaxEyePitchDeg * 0.6f);
		}
	}

	// When looking at the camera, compute the yaw/pitch from head to camera.
	if (bGazeLookingAtCamera)
	{
		FVector CamLoc;
		if (GetCameraWorldLocation(CamLoc) && FaceMesh)
		{
			// Measure yaw/pitch relative to where the HEAD bone already points, not the mesh
			// component (the mesh carries the MetaHuman -90deg import rotation which pinned the
			// angle to the clamp rail = eye(28,20)). Camera dead-ahead -> ~0,0; small offsets ->
			// small eye angles.
			const int32 HeadIdx = BodyMesh ? BodyMesh->GetBoneIndex(HeadBone) : INDEX_NONE;
			FVector RefFwd, RefRight, RefUp, EyePos;
			if (BodyMesh && HeadIdx != INDEX_NONE)
			{
				const FTransform HeadW = BodyMesh->GetBoneTransform(HeadIdx);
				// MEASURED on this rig (GEOM dots fwd=-0.01 right=1.00 with camera in front):
				// the head bone's +Y axis points OUT of the face (forward), +X is up the neck,
				// and -Z is to the character's right. So:
				RefFwd   = HeadW.GetUnitAxis(EAxis::Y);   // forward (out of face)
				RefRight = HeadW.GetUnitAxis(EAxis::Z);   // character's left/right
				RefUp    = HeadW.GetUnitAxis(EAxis::X);   // up the neck
				EyePos   = HeadW.GetLocation();
			}
			else
			{
				const FTransform MeshXform = FaceMesh->GetComponentTransform();
				RefFwd   = MeshXform.GetUnitAxis(EAxis::X);
				RefRight = MeshXform.GetUnitAxis(EAxis::Y);
				RefUp    = MeshXform.GetUnitAxis(EAxis::Z);
				EyePos   = FaceMesh->GetComponentLocation();
			}

			const FVector ToCam = (CamLoc - EyePos).GetSafeNormal();
			const float FwdDot   = FVector::DotProduct(ToCam, RefFwd);
			const float RightDot = FVector::DotProduct(ToCam, RefRight);
			const float UpDot    = FVector::DotProduct(ToCam, RefUp);
			const float Yaw   = FMath::RadiansToDegrees(FMath::Atan2(RightDot, FwdDot));
			const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(UpDot, FwdDot));
			GazeTargetDeg.X = FMath::Clamp(Yaw,   -MaxEyeYawDeg,   MaxEyeYawDeg);
			GazeTargetDeg.Y = FMath::Clamp(Pitch, -MaxEyePitchDeg, MaxEyePitchDeg);
			if (GEngine && bShowDebugOverlay)
			{
				GEngine->AddOnScreenDebugMessage(7006, 0.f, FColor::Green,
					FString::Printf(TEXT("[TWIdle GEOM] dots fwd=%.2f right=%.2f up=%.2f | rawYaw=%.0f rawPitch=%.0f | headValid=%d"),
						FwdDot, RightDot, UpDot, Yaw, Pitch, (BodyMesh && HeadIdx != INDEX_NONE) ? 1 : 0));
			}
		}
		else
		{
			GazeTargetDeg = FVector2D::ZeroVector; // no camera -> look straight ahead
		}
	}

	// 2026-06-18 Ted: apply the Settings "Eye gaze L/R" bias. Added to the gaze YAW target
	// (not the camera) so the pupils sit slightly off-center while still tracking the camera.
	// Re-clamp to the eye yaw rail so a maxed bias can't exceed the natural eye range.
	const float BiasedTargetYaw = FMath::Clamp(GazeTargetDeg.X + GazeYawBiasDeg, -MaxEyeYawDeg, MaxEyeYawDeg);

	// 2026-06-18 Ted: apply the Settings "Eye gaze Up/Dn" bias to the gaze PITCH target.
	// SIGN: +pitch -> eyeLookUp* (see GetFaceCurveWeights: PitchN>0 -> LookUp), so POSITIVE
	// bias raises the gaze (pupils UP). Re-clamp to the eye pitch rail so a maxed bias can't
	// exceed the natural eye range -- mirrors the yaw-bias handling exactly.
	const float BiasedTargetPitch = FMath::Clamp(GazeTargetDeg.Y + GazePitchBiasDeg, -MaxEyePitchDeg, MaxEyePitchDeg);

	// Smooth (saccade-like) toward the target.
	GazeCurrentDeg.X = FMath::FInterpTo(GazeCurrentDeg.X, BiasedTargetYaw, DeltaTime, GazeSlewSpeed);
	GazeCurrentDeg.Y = FMath::FInterpTo(GazeCurrentDeg.Y, BiasedTargetPitch, DeltaTime, GazeSlewSpeed);
}

// Apply the smoothed gaze angles additively to both eye bones in component space.
UTimeWalkIdleMotionComponent::FFaceCurveWeights UTimeWalkIdleMotionComponent::GetFaceCurveWeights() const
{
	FFaceCurveWeights W;
	W.Blink = bForceBlinkTest ? 1.0f : (bEnableBlink ? FMath::Clamp(CurrentBlinkValue, 0.f, 1.f) : 0.f);
	if (bEnableGaze && bGazeBonesFound)
	{
		const float YawN   = FMath::Clamp(GazeCurrentDeg.X / FMath::Max(1.f, MaxEyeYawDeg),   -1.f, 1.f);
		const float PitchN = FMath::Clamp(GazeCurrentDeg.Y / FMath::Max(1.f, MaxEyePitchDeg), -1.f, 1.f);
		W.LookRight = FMath::Max(0.f,  YawN);
		W.LookLeft  = FMath::Max(0.f, -YawN);
		W.LookUp    = FMath::Max(0.f,  PitchN);
		W.LookDown  = FMath::Max(0.f, -PitchN);
	}

	// #10 mix the smoothed emotion expression in (already strength-scaled + interpolated).
	// BROWS + EYES are always applied. The MOUTH curves (smile/frown/press/nose) are gated
	// OFF while he's SPEAKING so the lip-sync owns the jaw/mouth cleanly -- writing emotion
	// mouth-corner/press curves on top of visemes made the mouth look "weirdly constrained"
	// during speech (Ted 2026-06-12). MouthGate: 1 when silent, ->0 as SpeakingIntensity rises.
	const float MouthGate = 1.f - FMath::Clamp(SpeakingIntensity, 0.f, 1.f);
	W.BrowDownL = CurrentEmotionFace.BrowDownL;  W.BrowDownR = CurrentEmotionFace.BrowDownR;
	W.BrowLateralL = CurrentEmotionFace.BrowLateralL;  W.BrowLateralR = CurrentEmotionFace.BrowLateralR;
	W.BrowRaiseInL = CurrentEmotionFace.BrowRaiseInL;  W.BrowRaiseInR = CurrentEmotionFace.BrowRaiseInR;
	W.BrowRaiseOuterL = CurrentEmotionFace.BrowRaiseOuterL;  W.BrowRaiseOuterR = CurrentEmotionFace.BrowRaiseOuterR;
	W.EyeWideL = CurrentEmotionFace.EyeWideL;  W.EyeWideR = CurrentEmotionFace.EyeWideR;
	W.EyeSquintL = CurrentEmotionFace.EyeSquintL;  W.EyeSquintR = CurrentEmotionFace.EyeSquintR;
	// nose wrinkle reads partly as a mid-face/mouth thing -> gate it with the mouth too.
	W.MouthSmileL = CurrentEmotionFace.MouthSmileL * MouthGate;  W.MouthSmileR = CurrentEmotionFace.MouthSmileR * MouthGate;
	W.MouthFrownL = CurrentEmotionFace.MouthFrownL * MouthGate;  W.MouthFrownR = CurrentEmotionFace.MouthFrownR * MouthGate;
	W.MouthPressL = CurrentEmotionFace.MouthPressL * MouthGate;  W.MouthPressR = CurrentEmotionFace.MouthPressR * MouthGate;
	W.NoseWrinkleL = CurrentEmotionFace.NoseWrinkleL * MouthGate;  W.NoseWrinkleR = CurrentEmotionFace.NoseWrinkleR * MouthGate;
	return W;
}

// #10 emotion -> facial expression preset. RAW weights (0..1), pre-strength. Subtle but
// readable for a dignified statesman. Many of the 19 Gottman labels share a face.
// Label indices match EInworldEmotionLabel: 0 Neutral,1 Contempt,2 Belligerence,3 Domineering,
// 4 Criticism,5 Anger,6 Tension,7 TenseHumor,8 Defensiveness,9 Whining,10 Sadness,11 Stonewalling,
// 12 Interest,13 Validation,14 Humor,15 Affection,16 Surprise,17 Joy,18 Disgust.
UTimeWalkIdleMotionComponent::FFaceCurveWeights UTimeWalkIdleMotionComponent::EmotionPreset(int32 Label)
{
	FFaceCurveWeights P;
	auto Brows = [&P](float down, float furrow, float innerUp, float outerUp)
	{
		P.BrowDownL = P.BrowDownR = down;
		P.BrowLateralL = P.BrowLateralR = furrow;
		P.BrowRaiseInL = P.BrowRaiseInR = innerUp;
		P.BrowRaiseOuterL = P.BrowRaiseOuterR = outerUp;
	};
	auto Mouth = [&P](float smile, float frown, float press)
	{
		P.MouthSmileL = P.MouthSmileR = smile;
		P.MouthFrownL = P.MouthFrownR = frown;
		P.MouthPressL = P.MouthPressR = press;
	};
	auto Eyes = [&P](float wide, float squint) { P.EyeWideL = P.EyeWideR = wide; P.EyeSquintL = P.EyeSquintR = squint; };
	auto Nose = [&P](float wrinkle) { P.NoseWrinkleL = P.NoseWrinkleR = wrinkle; };

	switch (Label)
	{
	case 17: case 14:                 // Joy, Humor
		Brows(0,0,0,0.25f); Mouth(0.9f,0,0); Eyes(0,0.5f); break;
	case 15: case 13:                 // Affection, Validation
		Brows(0,0,0.15f,0.1f); Mouth(0.55f,0,0); Eyes(0,0.25f); break;
	case 12:                          // Interest
		Brows(0,0,0,0.35f); Mouth(0.2f,0,0); Eyes(0.2f,0); break;
	case 5: case 2: case 3:           // Anger, Belligerence, Domineering
		Brows(0.7f,0.8f,0,0); Mouth(0,0,0.7f); Eyes(0,0.5f); break;
	case 1: case 4:                   // Contempt, Criticism (one-sided sneer-ish, keep symmetric-subtle)
		Brows(0.4f,0.4f,0,0); Mouth(0,0,0.4f); Nose(0.25f); break;
	case 18:                          // Disgust
		Brows(0.5f,0.3f,0,0); Mouth(0,0.2f,0.2f); Nose(0.85f); Eyes(0,0.4f); break;
	case 10: case 9:                  // Sadness, Whining
		Brows(0,0.2f,0.8f,0); Mouth(0,0.7f,0); break;
	case 6: case 7: case 8:           // Tension, Tense Humor, Defensiveness
		Brows(0.3f,0.4f,0,0); Mouth(0,0,0.5f); break;
	case 16:                          // Surprise
		Brows(0,0,0,0.9f); Mouth(0,0,0); Eyes(0.85f,0); break;
	case 11:                          // Stonewalling -> near-neutral, slight press
		Mouth(0,0,0.25f); break;
	case 0: default:                  // Neutral
		break;
	}
	return P;
}

void UTimeWalkIdleMotionComponent::SetEmotion(int32 EmotionLabel)
{
	// Always rebuild the target (label OR strength may have changed). Only the smoothing
	// toward TargetEmotionFace is gradual; recomputing the target is cheap + idempotent.
	CurrentEmotionLabel = EmotionLabel;
	const FFaceCurveWeights Raw = EmotionPreset(EmotionLabel);
	const float S = FMath::Max(0.f, EmotionStrength);
	// scale the whole preset by master strength into the target the tick blends toward.
	FFaceCurveWeights& T = TargetEmotionFace;
	T.BrowDownL = Raw.BrowDownL*S; T.BrowDownR = Raw.BrowDownR*S;
	T.BrowLateralL = Raw.BrowLateralL*S; T.BrowLateralR = Raw.BrowLateralR*S;
	T.BrowRaiseInL = Raw.BrowRaiseInL*S; T.BrowRaiseInR = Raw.BrowRaiseInR*S;
	T.BrowRaiseOuterL = Raw.BrowRaiseOuterL*S; T.BrowRaiseOuterR = Raw.BrowRaiseOuterR*S;
	T.MouthSmileL = Raw.MouthSmileL*S; T.MouthSmileR = Raw.MouthSmileR*S;
	T.MouthFrownL = Raw.MouthFrownL*S; T.MouthFrownR = Raw.MouthFrownR*S;
	T.MouthPressL = Raw.MouthPressL*S; T.MouthPressR = Raw.MouthPressR*S;
	T.NoseWrinkleL = Raw.NoseWrinkleL*S; T.NoseWrinkleR = Raw.NoseWrinkleR*S;
	T.EyeWideL = Raw.EyeWideL*S; T.EyeWideR = Raw.EyeWideR*S;
	T.EyeSquintL = Raw.EyeSquintL*S; T.EyeSquintR = Raw.EyeSquintR*S;
	UE_LOG(LogTemp, Verbose, TEXT("[TWEmotion] SetEmotion label=%d strength=%.2f"), EmotionLabel, S);
}

void UTimeWalkIdleMotionComponent::UpdateEmotionFace(float DeltaTime)
{
	// Exponential-ish smoothing toward TargetEmotionFace over ~EmotionBlendSeconds.
	const float Rate = (EmotionBlendSeconds > KINDA_SMALL_NUMBER)
		? FMath::Clamp(DeltaTime / EmotionBlendSeconds, 0.f, 1.f) : 1.f;
	auto L = [Rate](float& cur, float tgt) { cur = FMath::Lerp(cur, tgt, Rate); };
	FFaceCurveWeights& C = CurrentEmotionFace; const FFaceCurveWeights& T = TargetEmotionFace;
	L(C.BrowDownL,T.BrowDownL); L(C.BrowDownR,T.BrowDownR);
	L(C.BrowLateralL,T.BrowLateralL); L(C.BrowLateralR,T.BrowLateralR);
	L(C.BrowRaiseInL,T.BrowRaiseInL); L(C.BrowRaiseInR,T.BrowRaiseInR);
	L(C.BrowRaiseOuterL,T.BrowRaiseOuterL); L(C.BrowRaiseOuterR,T.BrowRaiseOuterR);
	L(C.MouthSmileL,T.MouthSmileL); L(C.MouthSmileR,T.MouthSmileR);
	L(C.MouthFrownL,T.MouthFrownL); L(C.MouthFrownR,T.MouthFrownR);
	L(C.MouthPressL,T.MouthPressL); L(C.MouthPressR,T.MouthPressR);
	L(C.NoseWrinkleL,T.NoseWrinkleL); L(C.NoseWrinkleR,T.NoseWrinkleR);
	L(C.EyeWideL,T.EyeWideL); L(C.EyeWideR,T.EyeWideR);
	L(C.EyeSquintL,T.EyeSquintL); L(C.EyeSquintR,T.EyeSquintR);
}

void UTimeWalkIdleMotionComponent::ApplyGazeCurves(UAnimInstance* FaceAnim)
{
	if (!FaceAnim) { return; }

	// Convert smoothed yaw/pitch (deg) into 0..1 directional weights, inject as RigLogic
	// CONTROL curves (CTRL_expressions_eyeLook*) via AddCurveValue. RigLogic reads these and
	// rotates the eyeballs. No bone CS poking, no axis guessing.
	const float YawN   = FMath::Clamp(GazeCurrentDeg.X / FMath::Max(1.f, MaxEyeYawDeg),   -1.f, 1.f);
	const float PitchN = FMath::Clamp(GazeCurrentDeg.Y / FMath::Max(1.f, MaxEyePitchDeg), -1.f, 1.f);

	const float LookRight = FMath::Max(0.f,  YawN);
	const float LookLeft  = FMath::Max(0.f, -YawN);
	const float LookUp    = FMath::Max(0.f,  PitchN);
	const float LookDown  = FMath::Max(0.f, -PitchN);

	FaceAnim->AddCurveValue(GazeLookRightL, LookRight);
	FaceAnim->AddCurveValue(GazeLookRightR, LookRight);
	FaceAnim->AddCurveValue(GazeLookLeftL,  LookLeft);
	FaceAnim->AddCurveValue(GazeLookLeftR,  LookLeft);
	FaceAnim->AddCurveValue(GazeLookUpL,    LookUp);
	FaceAnim->AddCurveValue(GazeLookUpR,    LookUp);
	FaceAnim->AddCurveValue(GazeLookDownL,  LookDown);
	FaceAnim->AddCurveValue(GazeLookDownR,  LookDown);
}

// World location the eyes aim at. Prefers an explicit gaze-target actor (e.g. the
// "Character Chat Camera") when set - this bypasses PlayerCameraManager entirely so the
// gaze is correct even when a pawn possession steals the view target or the manager POV
// lags the view-target blend. Falls back to the player-0 PlayerCameraManager POV.
bool UTimeWalkIdleMotionComponent::GetCameraWorldLocation(FVector& OutLoc) const
{
	// 1) Explicit override actor wins (fixed conversation shot / chat camera).
	if (AActor* TargetActor = GazeTargetActorOverride.Get())
	{
		// A CineCameraActor's ROOT/pivot is NOT the lens - the lens is on the
		// UCameraComponent, which is usually offset/rotated from the actor origin.
		// Aiming at GetActorLocation() would send the eyes to the pivot (off to the
		// side), which is exactly the bug we saw. So prefer the camera component's
		// world location; fall back to the actor location for a non-camera target.
		if (const UCameraComponent* CamComp = TargetActor->FindComponentByClass<UCameraComponent>())
		{
			OutLoc = CamComp->GetComponentLocation();
		}
		else
		{
			OutLoc = TargetActor->GetActorLocation();
		}
		return true;
	}

	// 2) Default: track the active player camera (PIE: the player's view).
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
		{
			if (APlayerCameraManager* CamMgr = PC->PlayerCameraManager)
			{
				OutLoc = CamMgr->GetCameraLocation();
				return true;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
//  Post-pose hook: apply additive component-space rotation AFTER retarget
// ---------------------------------------------------------------------------

void UTimeWalkIdleMotionComponent::OnBodyPoseFinalized()
{
	if (!BodyMesh || !bEnableHeadMotion) { return; }

	AddBoneRotationCS(BodyMesh, SpineBone, SpineRot);
	AddBoneRotationCS(BodyMesh, NeckBone, NeckRot);
	AddBoneRotationCS(BodyMesh, HeadBone, HeadRot);

	// NOTE: do NOT call FinalizeBoneTransform() here -- this callback fires from
	// within the finalize flow, so re-entering it recurses infinitely (stack
	// overflow). The dual-buffer write in AddBoneRotationCS already lands the edit
	// in the render-read buffer; just flag the render data dirty.
	BodyMesh->MarkRenderDynamicDataDirty();

	// Diagnostics: prove the post-pose hook is firing and what we're applying.
	if (GEngine && bShowDebugOverlay)
	{
		GEngine->AddOnScreenDebugMessage(7002, 0.5f, FColor::Cyan,
			FString::Printf(TEXT("[TWIdle] poseFinalized HeadRot=%s speak=%.2f"),
				*HeadRot.ToCompactString(), SpeakingIntensity));
	}
}

void UTimeWalkIdleMotionComponent::AddBoneRotationCS(USkeletalMeshComponent* Mesh, FName BoneName, const FRotator& AdditiveRot)
{
	if (!Mesh || AdditiveRot.IsNearlyZero()) { return; }

	const int32 BoneIndex = Mesh->GetBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE) { return; }

	TArray<FTransform>& CS = Mesh->GetEditableComponentSpaceTransforms();
	if (!CS.IsValidIndex(BoneIndex)) { return; }

	// Apply the additive rotation in component space about the bone's current orientation.
	// Write BOTH the editable buffer and the current read buffer: inside the
	// OnBoneTransformsFinalized callback the double-buffer may already have flipped, so the
	// render thread reads from CurrentReadComponentTransforms. Updating both guarantees the
	// edit is what gets skinned this frame regardless of flip timing.
	const FQuat Add(AdditiveRot);

	FTransform& EditXform = CS[BoneIndex];
	EditXform.SetRotation((Add * EditXform.GetRotation()).GetNormalized());

	TArray<FTransform>& ReadCS =
		const_cast<TArray<FTransform>&>(Mesh->GetComponentSpaceTransforms());
	if (ReadCS.IsValidIndex(BoneIndex))
	{
		FTransform& ReadXform = ReadCS[BoneIndex];
		ReadXform.SetRotation((Add * ReadXform.GetRotation()).GetNormalized());
	}
}
