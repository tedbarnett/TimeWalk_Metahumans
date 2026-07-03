// Copyright TimeWalk. Procedural idle + speaking head/body motion for an Inworld
// MetaHuman character. Reparent the character's BODY Animation Blueprint to this class,
// then drive the neck/head and spine bones from the exposed rotators (via a "Transform
// (Modify) Bone" node per bone in Component space), additively over the base idle pose.
//
// Why this exists: the Inworld viseme path animates the FACE only. Without this the
// head/neck/spine are frozen, which reads as robotic. This adds subtle, continuous,
// Perlin-noise-based motion (breathing + idle sway + head drift) that ramps UP while
// the character is speaking (nods/emphasis) and settles when idle.
//
// Pairs with UTimeWalkLipsyncAnimInstance (face). Both locate the same
// UInworldVoiceAudioComponent to detect speech; this one only needs the on/off + a
// decaying "speaking intensity" envelope, not the phoneme data.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#if TIMEWALK_WITH_INWORLD
// Only pulled in when Inworld is present. The Inworld-typed member + native-delegate
// handler below are plain (NON-reflected) C++ guarded by the same define, so UHT never
// parses them and the module builds clean with NO Inworld installed.
#include "Audio/InworldVoiceAudioComponent.h"
#endif
#include "TimeWalkIdleMotionAnimInstance.generated.h"

/**
 * Body AnimInstance that produces procedural idle + speaking motion.
 *
 * Setup (in the character's BODY Animation Blueprint):
 *   1. Class Settings -> Parent Class -> TimeWalkIdleMotionAnimInstance.
 *   2. In the AnimGraph, after your base/idle pose, add three "Transform (Modify) Bone"
 *      nodes in COMPONENT space, set to ADD to Existing for Rotation:
 *        - head   bone  <- HeadRotation
 *        - neck   bone  <- NeckRotation   (use ~half of head for a natural chain)
 *        - spine_04 (or spine_03) <- SpineRotation
 *      Bind each node's Rotation pin to the matching exposed property below.
 *   3. (Optional) Drive a breathing morph/scale from BreathingAlpha if your rig has one.
 *
 * All motion is small (degrees), smoothed, and noise-driven so it never loops visibly.
 */
UCLASS()
class TIMEWALKIDLE_API UTimeWalkIdleMotionAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// ---- Outputs to bind in the AnimGraph (component-space additive rotations) ----

	/** Additive head rotation (deg) for the head bone. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion")
	FRotator HeadRotation = FRotator::ZeroRotator;

	/** Additive neck rotation (deg) for the neck bone (softer than head). */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion")
	FRotator NeckRotation = FRotator::ZeroRotator;

	/** Additive spine rotation (deg) for an upper-spine bone (subtle sway/breathing lean). */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion")
	FRotator SpineRotation = FRotator::ZeroRotator;

	/** 0..1 breathing phase; bind to a chest morph or scale if available. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion")
	float BreathingAlpha = 0.f;

	/** 0..1 smoothed envelope: 0 = idle, 1 = actively speaking. Drives motion amplitude. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion")
	float SpeakingIntensity = 0.f;

	/** True while a valid voice component is bound. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion")
	bool bVoiceBound = false;

	// ---- Idle variation pool (#23, 2026-06-14) -------------------------------------
	// Real mocap base idle clips (ActorCore, retargeted to MetaHuman) cycled at random so
	// the body pose never loops visibly. The AnimGraph's base Sequence Player binds its
	// Sequence pin to CurrentIdleClip, and (optionally) a second player to NextIdleClip
	// with a Blend node driven by IdleBlendAlpha for a smooth cross-fade between clips.

	/** Pool of base idle/talk clips. Auto-populated from the Retargeted folder if left empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool")
	TArray<TObjectPtr<UAnimSequence>> IdlePool;

	/** Currently-playing base idle clip (bind the base Sequence Player's Sequence pin here). */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|Pool")
	TObjectPtr<UAnimSequence> CurrentIdleClip = nullptr;

	/** Clip we are cross-blending TO (bind a second Sequence Player here). */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|Pool")
	TObjectPtr<UAnimSequence> NextIdleClip = nullptr;

	/** 0 = show CurrentIdleClip, 1 = show NextIdleClip. Bind to a Blend node's Alpha. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|Pool")
	float IdleBlendAlpha = 0.f;

	/** Min/max seconds to hold a clip before cross-blending to the next random one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool", meta = (ClampMin = "2.0", ClampMax = "60.0"))
	float IdleMinHoldSeconds = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool", meta = (ClampMin = "2.0", ClampMax = "60.0"))
	float IdleMaxHoldSeconds = 16.f;

	/** Cross-blend duration between clips (seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool", meta = (ClampMin = "0.1", ClampMax = "4.0"))
	float IdleCrossBlendSeconds = 1.2f;

	/** Master switch for the whole idle-variation pool (#23). DEFAULT OFF on main:
	 *  all 7 retargeted clips reviewed bad (Ted 2026-06-15). When false, the pool is
	 *  neither auto-loaded nor seeded, CurrentIdleClip stays null, and the body falls
	 *  back to the simple AnimGraph idle pose + procedural head motion. The full pool
	 *  pipeline + review tooling lives on branch idle-pool-wip. Flip true to re-enable
	 *  once a better clip set is sourced. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool")
	bool bIdlePoolEnabled = false;

	// ---- Slot-playback mode (#23, zero-AnimGraph-edit path) -------------------------
	// If the AnimGraph already has a Slot node (standard MetaHuman body AnimBP ships a
	// "DefaultSlot"), we can drive the whole pool by playing each clip as a dynamic montage
	// on that slot from C++ -- NO graph node wiring needed. The montage's own blend-in time
	// gives the cross-blend. Set bUseSlotPlayback=true + the slot name and that's it.

	/** When true, play pool clips via PlaySlotAnimationAsDynamicMontage on IdleSlotName
	 *  instead of relying on AnimGraph Sequence-Player pins bound to CurrentIdleClip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool")
	bool bUseSlotPlayback = false;

	/** Slot to play pool clips on (must exist in the AnimGraph). Usually "DefaultSlot". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Pool")
	FName IdleSlotName = TEXT("DefaultSlot");

	// ---- Tunables (safe to tweak live as values; no recompile needed) ----

	/** Max head yaw/pitch in degrees while IDLE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float IdleHeadAmplitudeDeg = 7.0f;

	/** Max head yaw/pitch in degrees while SPEAKING (added on top of idle). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float SpeakingHeadAmplitudeDeg = 6.0f;

	/** Spine sway amplitude (deg). Kept small; spine reads strongly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float SpineAmplitudeDeg = 1.5f;

	/** How fast the idle noise evolves (higher = more fidgety). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.05", ClampMax = "3.0"))
	float NoiseSpeed = 0.9f;

	/** Breaths per minute. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "4.0", ClampMax = "30.0"))
	float BreathsPerMinute = 13.f;

	/** Seconds for SpeakingIntensity to rise to speech / fall back to idle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.02", ClampMax = "2.0"))
	float SpeakingAttackTime = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.05", ClampMax = "3.0"))
	float SpeakingReleaseTime = 0.6f;

protected:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	virtual void NativeUninitializeAnimation() override;

private:
#if TIMEWALK_WITH_INWORLD
	/** Cached voice component on the owning actor. Plain (non-reflected) raw pointer guarded
	 *  by TIMEWALK_WITH_INWORLD; bound via the NATIVE playback delegate (no UFUNCTION). */
	UInworldVoiceAudioComponent* VoiceComponent = nullptr;
	FDelegateHandle VoicePlaybackHandle;
#endif

	/** 2026-06-09 Ted #8684: cached idle-motion component; supplies the listening
	 *  envelope + pose targets (head turn on mic press). The graph's bones are
	 *  driven HERE, so listening must blend here too. */
	UPROPERTY(Transient)
	TObjectPtr<class UTimeWalkIdleMotionComponent> IdleMotionComponent = nullptr;

	/** Locate + bind the voice component. Safe to call repeatedly. */
	void TryBindVoiceComponent();

#if TIMEWALK_WITH_INWORLD
	/** Bound to the NATIVE OnVoiceAudioPlaybackNative delegate: marks that speech audio
	 *  arrived this frame. Plain C++ (no UFUNCTION), guarded so UHT never parses it. */
	void HandleVoiceAudioPlayback(
		UInworldVoiceAudioComponent* InVoiceComponent,
		const FInworldVoiceAudioPlaybackInfo& PlaybackInfo,
		const FInworldData_TTSOutput& TTSOutput,
		const TArray<FInworldPhoneSpan>& PhoneSpans);
#endif

	/** Independent noise time accumulators per channel so they never sync up. */
	float NoiseTime = 0.f;

	/** Continuous breathing phase accumulator (radians). */
	float BreathPhase = 0.f;

	/** Wall-clock-ish accumulator for speaking-decay timing. */
	double LastPlaybackSeconds = -1000.0;

	/** Pseudo-Perlin sample in [-1,1] from FMath::PerlinNoise1D with a per-channel seed. */
	static float Noise1D(float T, float Seed);

	// ---- Idle variation pool runtime ------------------------------------------------
	/** Load the Retargeted/ folder into IdlePool if it's empty (called once on init). */
	void EnsureIdlePoolLoaded();

	/** Advance the clip selector / cross-blend each frame. */
	void UpdateIdlePool(float DeltaSeconds);

	/** Pick a random clip from the pool that isn't AvoidClip (when possible). */
	UAnimSequence* PickRandomIdle(UAnimSequence* AvoidClip) const;

	/** Seconds remaining to hold the current clip before starting a cross-blend. */
	float IdleHoldRemaining = 0.f;

	/** True while a cross-blend (Current->Next) is in progress. */
	bool bIdleBlending = false;

	/** Seconds remaining in the active cross-blend. */
	float IdleBlendRemaining = 0.f;

	/** Slot-playback runtime: advance the slot-montage clip cycler each frame. */
	void UpdateIdleSlotPlayback(float DeltaSeconds);

	/** Seconds left before the current slot clip should be re-triggered / swapped. */
	float SlotClipRemaining = 0.f;

	/** True once we've kicked off the first slot montage. */
	bool bSlotPlaybackStarted = false;
};
