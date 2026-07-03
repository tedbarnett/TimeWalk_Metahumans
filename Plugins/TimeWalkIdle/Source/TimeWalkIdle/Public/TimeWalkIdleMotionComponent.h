// Copyright TimeWalk. Drop-on-actor procedural liveliness for an Inworld MetaHuman.
//
// WHY THIS EXISTS (read this before touching AnimGraphs again):
//   The Inworld plugin gives you the AI brain + mouth visemes only. It ships NO gaze,
//   NO blink, NO idle body motion. Those have to be built. The previous approach drove
//   head/neck/spine through hand-wired "Transform (Modify) Bone" nodes inside a body
//   AnimBP -- which (a) is tedious node-wiring and (b) silently failed because the
//   MetaHuman body actually runs RTG_metahuman_base_skel_AnimBP (a retarget AnimBP),
//   not the AnimBP we were editing.
//
//   THIS component sidesteps all of that. You ADD ONE COMPONENT to the Hamilton actor
//   (or any Inworld MetaHuman) and press play. Zero AnimGraph nodes. It:
//     * finds the body skeletal mesh + the Inworld voice + character components itself,
//     * applies additive head/neck/spine rotation AFTER the retarget pose each frame
//       (so retargeting never overwrites it),
//     * produces breathing, idle drift, and purposeful "look-around" glances,
//     * ramps motion UP while speaking (from the Inworld voice start/complete events),
//     * optionally biases motion by the Inworld EMOTION label (stressed = more fidget),
//     * drives procedural blinks on the FACE mesh via morph/curve (composes with visemes;
//       different channels -- eyes vs mouth -- so they never fight).
//
// HOW TO USE:
//   1. Open BP_1776-Hamilton_TimeWalk (or your character).
//   2. Add Component -> "TimeWalk Idle Motion".
//   3. (Usually nothing else.) If auto-detect misses a mesh, set the override names below.
//   4. Press Play. He breathes, glances around, blinks, and gets livelier while talking.
//
// Tunables are EditAnywhere so you can tweak live in the component's Details panel.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#if TIMEWALK_WITH_INWORLD
// Only pulled in when Inworld is present. The Inworld-typed members + native-delegate
// handlers below are plain (NON-reflected) C++ guarded by the same define, so UHT never
// parses them and the module builds clean with NO Inworld installed.
#include "Audio/InworldVoiceAudioComponent.h"
#include "Components/InworldCharacterComponent.h"
#endif
#include "TimeWalkIdleMotionComponent.generated.h"

class USkeletalMeshComponent;

/**
 * High-level, human-meaningful personality traits for a character's idle motion.
 * Each is 0..1. These do NOT drive the body directly -- RebuildMotionFromPersona()
 * maps them onto the ~40 low-level physical knobs (head amplitude, glance frequency,
 * gaze hold, noise speed, blink rate, spine sway, etc). Author these once per character
 * in the BP Details panel; the raw knobs are derived. Set bUsePersona=false on the
 * component to ignore this and hand-tune the raw knobs instead.
 *
 * Think of it as: "who is this person" -> a posture/motion signature.
 *   Imperiousness -- still, fixed, dominant. Locks eye contact, rarely glances away,
 *                    upright, small slow motion. (Stuyvesant, a domineering statesman.)
 *   Restlessness  -- nervous kinetic energy. Fast fidgety noise, frequent wide glances,
 *                    more blinks. (A sharp, animated, quick-witted figure.)
 *   Warmth        -- engaged and approachable. Holds the listener's gaze, softer/slower
 *                    motion, a touch more emotional readability.
 *   Formality     -- stiff, composed, controlled. Low spine sway, low overall amplitude,
 *                    minimal breathing lean. (Period-appropriate gravitas.)
 */
USTRUCT(BlueprintType)
struct FTimeWalkPersona
{
	GENERATED_BODY()

	/** Still, fixed, dominant presence. Up = less movement, locked gaze, rare look-aways, slow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Imperiousness = 0.4f;

	/** Nervous kinetic energy. Up = faster/twitchier noise, more frequent + wider glances, more blinks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Restlessness = 0.4f;

	/** Engaged + approachable. Up = more eye contact on the listener, softer/slower motion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Warmth = 0.5f;

	/** Stiff, composed, controlled. Up = low spine sway, low overall amplitude, minimal breathing lean. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Formality = 0.5f;
};

UCLASS(ClassGroup = (TimeWalk), meta = (BlueprintSpawnableComponent, DisplayName = "TimeWalk Idle Motion"))
class TIMEWALKIDLE_API UTimeWalkIdleMotionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTimeWalkIdleMotionComponent();

	// ---------------------------------------------------------------------
	//  Inworld runtime toggle (Ted). When Inworld IS compiled in
	//  (TIMEWALK_WITH_INWORLD==1) and this is false, the component SKIPS the
	//  voice/character binding + emotion bias and behaves as the pure-idle path
	//  (breathing/gaze/blink/camera-attach still run). When Inworld is compiled
	//  OUT (TIMEWALK_WITH_INWORLD==0) this toggle is inert but still visible.
	// ---------------------------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|Inworld")
	bool bEnableInworld = true;

	// ---------------------------------------------------------------------
	//  Persona layer (2026-06-13 Ted) -- high-level trait sliders that DERIVE
	//  the ~40 raw physical knobs below. Author once per character; live-tunable.
	// ---------------------------------------------------------------------

	/** When true, the four Persona traits drive the raw motion knobs (head amplitude,
	 *  glance frequency, gaze hold, noise speed, blink rate, spine sway...) via
	 *  RebuildMotionFromPersona(). When false, the raw knobs are used as-authored and
	 *  Persona is ignored -- so existing hand-tuned characters are unaffected. Default true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Persona")
	bool bUsePersona = true;

	/** The character's personality signature. Edit these (not the raw knobs) when bUsePersona=true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Persona")
	FTimeWalkPersona Persona;

	/** Recompute every raw motion knob from the current Persona traits. Called on BeginPlay,
	 *  on OnRegister (editor), after any Persona PostEditChangeProperty, and by the console
	 *  commands. Idempotent. No-op (leaves raw knobs untouched) when bUsePersona=false. */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Persona")
	void RebuildMotionFromPersona();

	/** Live-set one trait by name ("imperiousness"|"restlessness"|"warmth"|"formality") and
	 *  immediately rebuild. Used by the TW.Persona.* console commands. */
	void SetPersonaTrait(const FString& TraitName, float Value);

	// ---------------------------------------------------------------------
	//  Editor-mode debugging
	// ---------------------------------------------------------------------

	/** Auto-flip Update Animation In Editor on the body+face SkeletalMeshComponents
	 *  when this component is registered in the editor (not PIE), so AnimBP-driven
	 *  motion (gaze, blinks, head turn) is visible in the editor viewport for slider
	 *  debugging without entering PIE. Default true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Editor")
	bool bAutoEnableInEditorTick = true;

	virtual void OnRegister() override;

	// ---------------------------------------------------------------------
	//  Listening mode (driven by chat widget when mic is toggled on)
	// ---------------------------------------------------------------------

	/** Toggle 'listening' attentive posture. When true, the character locks gaze
	 *  on the camera, cocks the head slightly to one side, suppresses random
	 *  glance-away behavior, and slows blink rate. Driven by
	 *  UHamiltonChatWidget::HandleMicClicked when the microphone goes on/off. */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Listening")
	void SetListening(bool bListening);

	// ---------------------------------------------------------------------
	//  Knowing nod (2026-06-18 Ted) -- a slow, dignified head dip just after the
	//  user stops speaking (falling edge of listening). Rolls ~NodChance probability.
	//  Additive HEAD-PITCH oscillation read by the AnimInstance each tick, easing in
	//  and out. SAME sign convention as the listening pitch: GetKnowingNodPitchDeg()
	//  returns a "chin-down magnitude" (positive = chin down); the AnimInstance negates
	//  it at the bone (positive bone pitch tilts chin UP on this rig).
	// ---------------------------------------------------------------------

	/** Start a knowing nod NOW (used for the Settings "Test nod" button + the trigger). */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Nod")
	void TriggerKnowingNod();

	// --- Nod AXIS selector (2026-06-18 Ted, TEMPORARY tuning aid) --------------------
	//  EMPIRICAL FINDING / WHY THIS EXISTS: a prior diagnostic proved the nod C++ emits a
	//  pure, correctly-signed PITCH on the head bone. But with the camera on-axis (gaze
	//  bias 0) the dip still reads as a LATERAL / sideways head move, NOT a forward/back
	//  chin dip. That is the classic MetaHuman "head bone local axes are rotated" gotcha:
	//  the Transform(Modify)Bone applies HeadRotation about the head bone's OWN axes, and
	//  on this rig those axes are swapped relative to component/world. The head bone basis
	//  (measured in UpdateGaze, ~line 1171) is: bone +Y = world FORWARD (out of face),
	//  bone +Z = world LEFT/RIGHT (ear-to-ear), bone +X = world UP-the-neck. An FRotator
	//  Pitch rotates about the bone's Y (= world forward) => that's a world ROLL/lateral
	//  tilt (matches what Ted saw). The TRUE world-vertical nod is rotation about the
	//  ear-to-ear axis = bone +Z = the FRotator YAW channel.
	//
	//  2026-06-18 Ted CONFIRMED LIVE + BAKED: the correct combo is NodAxis = 2 (Roll),
	//  NodAxisSign = +1. The nod is now HARDCODED to FinalRoll += GetKnowingNodPitchDeg()
	//  in the AnimInstance and no longer depends on these properties. The two diagnostic
	//  Settings-panel selector rows have been REMOVED. These UPROPERTYs are kept only so
	//  any stale Blueprint/CDO references don't break; they default to the winning combo
	//  (Roll / +1) and are otherwise inert.

	/** [BAKED / INERT] Which head-rotation channel the nod magnitude drives: 0 = Pitch,
	 *  1 = Yaw, 2 = Roll. Default 2 (Roll) = Ted's confirmed true world-vertical nod for
	 *  this rig's rotated head bone. NOTE: the AnimInstance now HARDCODES Roll/+1 and does
	 *  NOT read this property; kept only to avoid breaking stale references. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "0", ClampMax = "2"))
	int32 NodAxis = 2;

	/** [BAKED / INERT] Sign multiplier formerly applied to the nod magnitude. Ted confirmed
	 *  +1. The AnimInstance now hardcodes sign +1 and does NOT read this property. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float NodAxisSign = 1.f;

	/** Live setters (bypass the Details-panel CDO-vs-instance propagation bug). */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Nod")
	void SetNodAxis(int32 Axis) { NodAxis = FMath::Clamp(Axis, 0, 2); }

	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Nod")
	void SetNodAxisSign(float S) { NodAxisSign = (S < 0.f) ? -1.f : 1.f; }

	/** Read accessors for the AnimInstance (which owns the head bone application). */
	int32 GetNodAxis() const { return NodAxis; }
	float GetNodAxisSign() const { return NodAxisSign; }

	/** Roll the probability; if it passes (and not already speaking), kicks off TriggerKnowingNod(). */
	void MaybeTriggerKnowingNod();

	/** Current additive nod pitch in degrees, chin-down POSITIVE (matches the listening-pitch
	 *  down-magnitude convention; the AnimInstance negates it at the head bone). 0 when no nod. */
	float GetKnowingNodPitchDeg() const { return ActiveNodPitchDeg; }

	// --- Nod tunables (EditAnywhere AND live-settable via the setters below) ----------
	/** Probability per trigger (0..1) that a nod fires on the listening falling edge.
	 *  2026-06-18 Ted: baked default 0.30 -> 0.21. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float NodChance = 0.21f;

	/** Min nod amplitude (chin-down degrees). 2026-06-18 Ted: baked default 2 -> 4.
	 *  2026-06-19 Ted: final demo baked 4 -> 2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "25.0"))
	float NodDepthMinDeg = 2.f;

	/** Max nod amplitude (chin-down degrees). 2026-06-18 Ted: baked default 8 -> 11. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "25.0"))
	float NodDepthMaxDeg = 11.f;

	/** Tips forward-and-back this many times (min), sampled once per nod. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "1", ClampMax = "6"))
	int32 NodCountMin = 2;

	/** Tips forward-and-back this many times (max), sampled once per nod. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "1", ClampMax = "6"))
	int32 NodCountMax = 3;

	/** Seconds per single down-up cycle (SLOW = dignified, not a bobble).
	 *  2026-06-18 Ted: baked default 0.9 -> 0.93. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Nod", meta = (ClampMin = "0.3", ClampMax = "2.0", UIMin = "0.3", UIMax = "2.0"))
	float NodPeriodSeconds = 0.93f;

	/** Live setters (bypass the Details-panel CDO-vs-instance propagation bug). */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Nod")
	void SetNodChance(float V) { NodChance = FMath::Clamp(V, 0.f, 1.f); }

	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Nod")
	void SetNodDepthRange(float Min, float Max) { NodDepthMinDeg = FMath::Max(0.f, Min); NodDepthMaxDeg = FMath::Max(NodDepthMinDeg, Max); }

	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Nod")
	void SetNodPeriod(float V) { NodPeriodSeconds = FMath::Clamp(V, 0.3f, 2.0f); }

	// ---------------------------------------------------------------------
	//  Emotion-driven facial expression (#10)
	// ---------------------------------------------------------------------

	/** Set the current emotion as an EInworldEmotionLabel value (passed as int32 to keep
	 *  the Inworld enum header out of this component). The chat widget calls this per
	 *  reply utterance with CharacterComponent->EmotionState->GetEmotionLabel(). The face
	 *  expression smoothly interpolates toward the target preset and decays to neutral. */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Emotion")
	void SetEmotion(int32 EmotionLabel);

	/** Current emotion label (EInworldEmotionLabel as int; -1 = none yet). */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Emotion")
	int32 GetCurrentEmotionLabel() const { return CurrentEmotionLabel; }

	/** Master strength for emotion expressions. 0 = off (flat face), 1 = full preset.
	 *  Hamilton is a dignified statesman -- default is restrained-but-readable. Live-tunable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Emotion",
		meta = (ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.0", UIMax = "1.5"))
	float EmotionStrength = 0.5f;  // = keyboard "3" (3/9 * 1.5). Ted-chosen default 2026-06-12.

	/** Seconds to blend from the current expression to a new emotion target (and to decay). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Emotion",
		meta = (ClampMin = "0.05", ClampMax = "3.0"))
	float EmotionBlendSeconds = 0.6f;

	/** Smoothed 0..1 listening envelope. 2026-06-09 Ted #8684: read by
	 *  UTimeWalkIdleMotionAnimInstance, which owns the head bones now -- the
	 *  component's own tick math no longer drives bones, so listening must be
	 *  applied in the AnimInstance. */
	float GetListeningWeight() const { return ListeningWeight; }

	//  2026-06-10 Ted #8738: listening pose = three RANDOM SIGNED RANGES, sampled once per
	//  engagement (rising edge of SetListening) and held for the duration. Values below are
	//  HEAD-bone degrees; the neck follows at half so total visible ≈ head*1.5.
	//
	//  Yaw   (signed: + = his left, - = his right). Tuned range Ted picked: -15 .. 10.
	//  Pitch (DOWN-tilt MAGNITUDE, positive; negated at the bone => chin down). Ted gave
	//        bone-sign -10..-3 which == down magnitude 3..10.
	//  Roll  (signed). Ted picked 3 .. 10.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "-90.0", ClampMax = "90.0", UIMin = "-60.0", UIMax = "60.0"))
	float ListeningHeadYawMinDeg = -15.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "-90.0", ClampMax = "90.0", UIMin = "-60.0", UIMax = "60.0"))
	float ListeningHeadYawMaxDeg = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "25.0"))
	float ListeningHeadPitchDownMin = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "25.0"))
	float ListeningHeadPitchDownMax = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "-30.0", ClampMax = "30.0", UIMin = "-20.0", UIMax = "20.0"))
	float ListeningHeadRollMinDeg = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "-30.0", ClampMax = "30.0", UIMin = "-20.0", UIMax = "20.0"))
	float ListeningHeadRollMaxDeg = 10.f;

	/** Signed yaw/pitch actually applied this listening engagement. Chosen in SetListening()
	 *  (random L/R direction + random pitch pick) and read by the AnimInstance each tick.
	 *  Persist across the envelope rise/fall so the pose doesn't jitter mid-listen. */
	float ActiveListeningYawDeg = -2.f;
	float ActiveListeningPitchDeg = 6.f;   // down-tilt magnitude
	float ActiveListeningRollDeg = 6.f;

	float GetActiveListeningYawDeg() const { return ActiveListeningYawDeg; }
	float GetActiveListeningPitchDeg() const { return ActiveListeningPitchDeg; }

	// --- Editor live-tuning of the listening pose (2026-06-10 Ted #8732) -------------
	//  Turn ON bEditorPreviewListening in the Details panel to HOLD the listening pose
	//  in the editor viewport (no PIE, no mic, no random pick) so you can drag the
	//  sliders below and watch the head move in real time. What you set is exactly what
	//  you see -- the runtime random L/R + random down-tilt is bypassed in preview.
	//  Leave it OFF for shipping; live behaviour is unchanged. Editor-only field.
	UPROPERTY(EditAnywhere, Category = "TimeWalk|IdleMotion|Listening|Editor Preview")
	bool bEditorPreviewListening = false;

	/** Preview head yaw (deg, signed: + = his left / - = his right). Drag with preview ON. */
	UPROPERTY(EditAnywhere, Category = "TimeWalk|IdleMotion|Listening|Editor Preview", meta = (EditCondition = "bEditorPreviewListening", ClampMin = "-90.0", ClampMax = "90.0", UIMin = "-60.0", UIMax = "60.0"))
	float PreviewListeningYawDeg = -2.f;

	/** Preview head down-tilt (deg, + = chin down). Drag with preview ON. */
	UPROPERTY(EditAnywhere, Category = "TimeWalk|IdleMotion|Listening|Editor Preview", meta = (EditCondition = "bEditorPreviewListening", ClampMin = "-30.0", ClampMax = "30.0", UIMin = "-20.0", UIMax = "20.0"))
	float PreviewListeningPitchDownDeg = 7.f;

	/** Preview head roll (deg). Drag with preview ON. */
	UPROPERTY(EditAnywhere, Category = "TimeWalk|IdleMotion|Listening|Editor Preview", meta = (EditCondition = "bEditorPreviewListening", ClampMin = "-30.0", ClampMax = "30.0", UIMin = "-20.0", UIMax = "20.0"))
	float PreviewListeningRollDeg = 6.f;

	/** Unified listening pose for the AnimInstance: returns the effective listening
	 *  weight (0..1) and signed head yaw/pitch/roll (deg). In editor preview it returns
	 *  the fixed Preview* values at weight 1; otherwise the live envelope + random-picked
	 *  ActiveListening* values. One place so runtime and preview never drift. */
	void GetListeningPose(float& OutWeight, float& OutYawDeg, float& OutPitchDeg, float& OutRollDeg) const;

#if WITH_EDITOR
	//  Force the owner's skeletal meshes to update-in-editor + refresh when a listening
	//  Preview property changes, so dragging a slider repaints the viewport immediately.
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** How fast the listening posture rises (seconds to ~63% of final). Lower = snappier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "0.05", ClampMax = "2.0", UIMin = "0.1", UIMax = "1.5"))
	float ListeningRiseTime = 0.35f;

	/** Slower blinks while listening (multiplier on min/max blink interval). >1 = blinks less often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Listening", meta = (ClampMin = "0.5", ClampMax = "4.0", UIMin = "1.0", UIMax = "3.0"))
	float ListeningBlinkSlowdown = 1.8f;

	/** Final eye-control weights for the AnimGraph node (FAnimNode_TimeWalkFace). All 0..1.
	 *  Computed each tick; the node writes them into the Face pose's CTRL_expressions_* curves. */
	struct FFaceCurveWeights
	{
		float Blink = 0.f;     // 0 open, 1 closed (both eyes)
		float LookRight = 0.f;
		float LookLeft = 0.f;
		float LookUp = 0.f;
		float LookDown = 0.f;

		// #10 emotion expression curves (MetaHuman CTRL_expressions_*), all 0..1, smoothed.
		// Driven by the current Inworld emotion via SetEmotion(); blended onto the face node
		// alongside blink/gaze. Lip-sync owns the jaw/mouth separately.
		float BrowDownL = 0.f;        // anger/disgust: brows pulled down
		float BrowDownR = 0.f;
		float BrowLateralL = 0.f;     // brows pulled together (furrow)
		float BrowLateralR = 0.f;
		float BrowRaiseInL = 0.f;     // sadness: inner brow up
		float BrowRaiseInR = 0.f;
		float BrowRaiseOuterL = 0.f;  // surprise/interest: whole brow up
		float BrowRaiseOuterR = 0.f;
		float MouthSmileL = 0.f;      // joy/affection/validation/humor
		float MouthSmileR = 0.f;
		float MouthFrownL = 0.f;      // sadness
		float MouthFrownR = 0.f;
		float MouthPressL = 0.f;      // anger/tension: lips press
		float MouthPressR = 0.f;
		float NoseWrinkleL = 0.f;     // disgust
		float NoseWrinkleR = 0.f;
		float EyeWideL = 0.f;         // surprise
		float EyeWideR = 0.f;
		float EyeSquintL = 0.f;       // anger/joy (cheek raise feel)
		float EyeSquintR = 0.f;
	};
	FFaceCurveWeights GetFaceCurveWeights() const;

	// ---------------------------------------------------------------------
	//  Target wiring -- left blank, the component auto-detects. Only set these
	//  if the actor has multiple skeletal meshes and auto-detect picks wrong.
	// ---------------------------------------------------------------------

	/** Name of the BODY skeletal mesh component. Blank = auto-detect (first body-skeleton mesh). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName BodyMeshComponentName = NAME_None;

	/** Name of the FACE skeletal mesh component (for blinks). Blank = auto-detect MetaHuman face. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName FaceMeshComponentName = NAME_None;

	/** Body bone names. Defaults match MetaHuman skeleton. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName HeadBone = TEXT("head");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName NeckBone = TEXT("neck_02");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName SpineBone = TEXT("spine_04");

	/** Eye-blink morph target / curve name on the FACE mesh. MetaHuman uses CTRL_expressions; the
	 *  common morph is "eyeBlinkLeft"/"eyeBlinkRight" or the pose "EyeBlink". We drive both L/R. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName BlinkMorphLeft = TEXT("CTRL_expressions_eyeBlinkL");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName BlinkMorphRight = TEXT("CTRL_expressions_eyeBlinkR");

	/** Eye bones to aim for gaze. MetaHuman face skeleton standard is FACIAL_L_Eye / FACIAL_R_Eye.
	 *  If these don't exist on the resolved face mesh, gaze auto-disables (logged) and the rest
	 *  of the system is unaffected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName LeftEyeBone = TEXT("FACIAL_L_Eye");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName RightEyeBone = TEXT("FACIAL_R_Eye");

	// Morph-driven gaze targets (this MetaHuman rig). Drive both L/R eyes per direction.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookRightL = TEXT("CTRL_expressions_eyeLookRightL");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookRightR = TEXT("CTRL_expressions_eyeLookRightR");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookLeftL = TEXT("CTRL_expressions_eyeLookLeftL");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookLeftR = TEXT("CTRL_expressions_eyeLookLeftR");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookUpL = TEXT("CTRL_expressions_eyeLookUpL");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookUpR = TEXT("CTRL_expressions_eyeLookUpR");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookDownL = TEXT("CTRL_expressions_eyeLookDownL");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName GazeLookDownR = TEXT("CTRL_expressions_eyeLookDownR");

	// DRIVER curves read by the Modify Curve node in Face_AnimBP_Hamilton. The component
	// writes these every frame; the graph node fans them out to the real CTRL_expressions_*.
	// Blink: 0=open 1=closed. GazeYaw/Pitch: -1..1 (yaw +=right, pitch +=up).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName DriveBlink = TEXT("TW_Blink");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName DriveLookRight = TEXT("TW_LookRight");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName DriveLookLeft = TEXT("TW_LookLeft");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName DriveLookUp = TEXT("TW_LookUp");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Targets")
	FName DriveLookDown = TEXT("TW_LookDown");

	// ---------------------------------------------------------------------
	//  Feature toggles
	// ---------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bEnableHeadMotion = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bEnableLookAround = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bEnableBreathing = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bEnableBlink = true;

	/** DIAGNOSTIC: force eye-blink morph to a constant 1.0 so we can see if any morph write lands. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bForceBlinkTest = false;

	/** Runtime: have we logged the Face mesh's actual morph target names yet? */
	bool bMorphNamesDumped = false;

	/** Runtime: did the Face AnimInstance resolve this frame (needed for curve injection)? */
	bool bFaceAnimResolved = false;

	/** Accumulator for ~1s log heartbeat. */
	float TickLogAccum = 0.f;

	/** Eye gaze: pupils aim at the active player camera, with periodic natural look-aways.
	 *  Fully self-contained -- toggle off to disable without affecting head/idle/blink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bEnableGaze = true;

	/** Bias amplitude/frequency by the Inworld emotion label (stressed/angry = more motion). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bUseEmotionBias = true;

	/** Draw the on-screen blink/gaze/head diagnostic readouts. OFF by default -- clean screen for demos. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Features")
	bool bShowDebugOverlay = false;

	// ---------------------------------------------------------------------
	//  Master gain knobs (2026-06-08 Ted #8507) -- ONE-STOP "turn it all down"
	//  controls. These multiply the individual sliders below. Setting Speed=0
	//  freezes him completely. Setting Amplitude=0 makes him perfectly still.
	// ---------------------------------------------------------------------

	/** Master multiplier on ALL motion AMPLITUDES (head, neck, spine, glance offsets).
	 *  0.0 = perfectly still regardless of individual sliders. 1.0 = full. 2.0 = double everything.
	 *  2026-06-12 Ted #8940: default lowered 1.0 -> 0.3; full-amplitude head motion read as
	 *  too much in the native open-mic build. 2026-06-12 Ted #8948: 0.3 still too much ->
	 *  0.1 (~0.3x the prior 0.3). Keeps him subtly alive without the bobbing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Master", meta = (ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0"))
	float MasterMovementAmplitude = 0.1f;

	/** Master multiplier on ALL motion SPEEDS (noise evolution, glance frequency, gaze slew).
	 *  0.0 = frozen in time. 1.0 = current. 2.0 = double speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Master", meta = (ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0"))
	float MasterMovementSpeed = 1.0f;

	// ---------------------------------------------------------------------
	//  Tuning -- all degrees / seconds, live-editable.
	//  These individual sliders are multiplied by the Master knobs above.
	// ---------------------------------------------------------------------

	/** Head yaw/pitch amplitude (deg) while IDLE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "25.0", UIMin = "0.0", UIMax = "10.0"))
	float IdleHeadAmplitudeDeg = 1.75f;

	/** Extra head amplitude (deg) added while SPEAKING. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "15.0"))
	float SpeakingHeadAmplitudeDeg = 4.0f;

	/** Spine sway amplitude (deg). Small -- spine reads strongly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "6.0"))
	float SpineAmplitudeDeg = 2.0f;

	/** Idle noise evolution rate (higher = more fidgety). Halved 2026-06-07: head read 2x too fast/twitchy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.05", ClampMax = "3.0", UIMin = "0.05", UIMax = "1.0"))
	float NoiseSpeed = 0.225f;

	/** Breaths per minute. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "4.0", ClampMax = "30.0", UIMin = "6.0", UIMax = "20.0"))
	float BreathsPerMinute = 13.0f;

	// ---- Look-around (purposeful glances) ----

	/** Average seconds between picking a new glance target while IDLE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|LookAround", meta = (ClampMin = "0.5", ClampMax = "12.0", UIMin = "1.0", UIMax = "10.0"))
	float GlanceIntervalIdle = 4.0f;

	/** Average seconds between glances while SPEAKING (more frequent = engaged). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|LookAround", meta = (ClampMin = "0.3", ClampMax = "8.0", UIMin = "0.5", UIMax = "6.0"))
	float GlanceIntervalSpeaking = 2.0f;

	/** Max yaw of a glance offset (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|LookAround", meta = (ClampMin = "0.0", ClampMax = "35.0", UIMin = "0.0", UIMax = "25.0"))
	float GlanceYawRangeDeg = 14.0f;

	/** Max pitch of a glance offset (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|LookAround", meta = (ClampMin = "0.0", ClampMax = "25.0", UIMin = "0.0", UIMax = "15.0"))
	float GlancePitchRangeDeg = 8.0f;

	// ---- Blink ----

	/** Min/Max seconds between blinks (randomized). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Blink", meta = (ClampMin = "0.5", ClampMax = "12.0", UIMin = "1.0", UIMax = "8.0"))
	float BlinkIntervalMin = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Blink", meta = (ClampMin = "0.5", ClampMax = "15.0", UIMin = "2.0", UIMax = "10.0"))
	float BlinkIntervalMax = 6.0f;

	/** Duration of a single blink (close+open), seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Blink", meta = (ClampMin = "0.05", ClampMax = "0.6", UIMin = "0.08", UIMax = "0.3"))
	float BlinkDuration = 0.14f;

	// ---- Gaze (eye tracking) ----

	/** Max eye deflection (deg) from straight-ahead. MetaHuman eyes look unnatural past ~30. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "5.0", ClampMax = "40.0", UIMin = "10.0", UIMax = "35.0"))
	float MaxEyeYawDeg = 28.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "5.0", ClampMax = "30.0", UIMin = "5.0", UIMax = "25.0"))
	float MaxEyePitchDeg = 20.0f;

	/** 2026-06-18 Ted: small constant EYE-GAZE yaw bias (deg) added to the computed gaze
	 *  yaw each frame so the pupils sit slightly left/right of dead-center while still
	 *  tracking the camera. Driven LIVE by the Settings "Eye gaze L/R" slider. SIGN: this
	 *  is added to GazeCurrentDeg.X, where +yaw -> eyeLookRight* (character's own right =
	 *  VIEWER'S LEFT). The widget negates so the slider reads intuitively (right end =
	 *  pupils toward viewer's right). 0 = centered (camera-tracked, unbiased). */
	//  2026-06-18 Ted: range widened +-8 -> +-20 deg (clamp + slider both). Baked default
	//  0 -> -8 (his eyes sit slightly toward HIS right / a touch left of dead-center; Ted's
	//  chosen resting gaze). Slider still seeds from this via the widget sign-flip.
	//  2026-06-18 Ted: baked default flipped -8.2 -> +8.2 (component-level GazeYawBiasDeg).
	//  2026-06-19 Ted: final demo baked +8.2 -> +9.8 (panel reads -9.8).
	//  NOTE the widget sign-flip: slider value = -GazeYawBiasDeg, so the panel will read -9.8.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "-20.0", ClampMax = "20.0", UIMin = "-20.0", UIMax = "20.0"))
	float GazeYawBiasDeg = 0.0f;   // 2026-07-01 Helm: was 9.8 (old Hamilton demo tuning). That baked ~10deg pupil offset made him read as looking off-center even with the camera dead-ahead. Zeroed for true eye contact in NY1776.

	/** Live setter for the eye-gaze yaw bias (Settings slider). Clamped to +-20 deg. */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Gaze")
	void SetGazeYawBias(float Deg) { GazeYawBiasDeg = FMath::Clamp(Deg, -20.f, 20.f); }

	/** 2026-06-18 Ted: small constant EYE-GAZE PITCH bias (deg) added to the computed gaze
	 *  pitch each frame so the pupils sit slightly up/down of dead-center while still
	 *  tracking the camera. Driven LIVE by the Settings "Eye gaze Up/Dn" slider. SIGN:
	 *  added to GazeTargetDeg.Y, where +pitch -> eyeLookUp* (POSITIVE = pupils UP). The
	 *  widget maps slider value directly (no sign flip; slider-up = look up). 0 = centered.
	 *  2026-06-19 Ted: baked default 0 -> -2.0 (Up/Dn gaze). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "-20.0", ClampMax = "20.0", UIMin = "-20.0", UIMax = "20.0"))
	float GazePitchBiasDeg = 0.0f;   // 2026-07-01 Helm: was -2.0 (old demo tuning). Zeroed with the yaw bias for centered eye contact.

	/** Live setter for the eye-gaze pitch bias (Settings slider). Clamped to +-20 deg. */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Gaze")
	void SetGazePitchBias(float Deg) { GazePitchBiasDeg = FMath::Clamp(Deg, -20.f, 20.f); }

	/** Explicit gaze target actor (e.g. the "Character Chat Camera" CineCameraActor). When set
	 *  and valid, the eyes aim at THIS actor's world location instead of the player-0
	 *  PlayerCameraManager POV. Fixes the case where the rendered view target is the chat cam
	 *  but PlayerCameraManager->GetCameraLocation() still reports the pawn/spawn POV (possession
	 *  steals the view target, or the manager POV lags the blend). Leave null for default
	 *  camera-tracking behavior. Set it from the Level Blueprint after Set View Target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze")
	TWeakObjectPtr<AActor> GazeTargetActorOverride;

	/** Live setter for the explicit gaze target actor. Pass the Character Chat Camera; pass
	 *  nullptr to revert to PlayerCameraManager tracking. */
	UFUNCTION(BlueprintCallable, Category = "TimeWalk|IdleMotion|Gaze")
	void SetGazeTargetActor(AActor* InActor) { GazeTargetActorOverride = InActor; }

	/** How fast the eyes slew toward a new target (higher = snappier saccade). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "2.0", ClampMax = "30.0", UIMin = "4.0", UIMax = "24.0"))
	float GazeSlewSpeed = 12.0f;

	/** Min/Max seconds to hold eye contact on the camera before glancing away. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "0.5", ClampMax = "10.0", UIMin = "1.0", UIMax = "8.0"))
	float GazeHoldMin = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "0.5", ClampMax = "12.0", UIMin = "2.0", UIMax = "10.0"))
	float GazeHoldMax = 4.0f;

	/** Min/Max seconds spent looking away before returning to the camera. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "0.2", ClampMax = "6.0", UIMin = "0.3", UIMax = "4.0"))
	float GazeAwayMin = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "0.2", ClampMax = "8.0", UIMin = "0.5", UIMax = "6.0"))
	float GazeAwayMax = 1.8f;

	/** Probability (0..1) that on each cycle the character looks AWAY rather than holding contact.
	 *  Lowered while speaking (more engaged eye contact). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Gaze", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float GazeAwayChance = 0.45f;

	// ---- Smoothing ----

	/** Attack/Release seconds for the speaking envelope. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.05", UIMax = "1.0"))
	float SpeakingAttackTime = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TimeWalk|IdleMotion|Tuning", meta = (ClampMin = "0.05", ClampMax = "3.0", UIMin = "0.1", UIMax = "2.0"))
	float SpeakingReleaseTime = 0.6f;

	// ---------------------------------------------------------------------
	//  Read-only runtime state (handy for debugging in the Details panel).
	// ---------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|State")
	float SpeakingIntensity = 0.0f;

	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|State")
	bool bBodyMeshFound = false;

	/** 2026-06-09 Ted #8649: debug helpers for the "what moves the head" experiment.
	    Pause/resume animation evaluation on the body or face skeletal mesh at runtime
	    (console: TW.Idle.PauseBody 1 / TW.Idle.PauseFace 1). Pausing the BODY freezes
	    the idle sequence + any procedural bone nodes; if the head STILL moves, the
	    motion source is the FACE AnimBP (ARKit/LiveLink neck path) or RigLogic. */
	void DebugPauseBodyAnim(bool bPause);
	void DebugPauseFaceAnim(bool bPause);

	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|State")
	bool bFaceMeshFound = false;

	UPROPERTY(BlueprintReadOnly, Transient, Category = "TimeWalk|IdleMotion|State")
	bool bVoiceBound = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// ---- Resolved targets ----
	UPROPERTY(Transient) TObjectPtr<USkeletalMeshComponent> BodyMesh = nullptr;
	UPROPERTY(Transient) TObjectPtr<USkeletalMeshComponent> FaceMesh = nullptr;

#if TIMEWALK_WITH_INWORLD
	// Inworld caches + native-delegate handlers. All plain C++ (NOT reflected) and guarded
	// so UHT never sees them -> the module builds with zero Inworld headers when absent. We
	// bind the NATIVE (non-dynamic) Inworld delegates, which need no UFUNCTION. Raw pointers
	// are safe here: the owning actor keeps these components alive for the component's life,
	// and they are re-resolved each bind attempt.
	UInworldVoiceAudioComponent* VoiceComponent = nullptr;
	UInworldCharacterComponent* CharacterComponent = nullptr;

	// Speaking on/off, bound to the Inworld voice start/complete NATIVE delegates.
	void HandleVoiceStart(UInworldVoiceAudioComponent* InVoice, const FInworldData_TTSOutput& Voice, bool bInteractionStart);
	void HandleVoiceComplete(UInworldVoiceAudioComponent* InVoice, const FInworldData_TTSOutput& Voice, bool bInteractionEnd);
	FDelegateHandle VoiceStartHandle;
	FDelegateHandle VoiceCompleteHandle;
#endif // TIMEWALK_WITH_INWORLD

	void ResolveTargets();
	void TryBindVoice();
	bool bSpeakingRaw = false;
	// Listening envelope: bListeningRaw is the on/off signal from SetListening();
	// ListeningWeight is the smoothed 0..1 driver used in tick math.
	bool bListeningRaw = false;
	float ListeningWeight = 0.f;

	// ---- Knowing nod state (2026-06-18) ----
	// State machine driven in TickComponent. When bNodActive, an additive head-pitch
	// oscillation ramps in, oscillates NodActiveCount times at NodActivePeriod, ramps
	// out to 0. Amplitude + count are sampled once at TriggerKnowingNod() time and held
	// for the duration (mirrors how ActiveListening* are sampled once per engagement).
	// ActiveNodPitchDeg is the current chin-down-magnitude (positive) read by the
	// AnimInstance via GetKnowingNodPitchDeg(); the AnimInstance negates it at the bone.
	bool  bNodActive = false;
	float NodElapsed = 0.f;          // seconds since this nod began
	float NodActiveAmplitudeDeg = 0.f; // sampled chin-down amplitude for this nod
	float NodActivePeriod = 0.9f;    // sampled seconds per down-up cycle
	int32 NodActiveCount = 2;        // sampled number of down-up cycles
	float NodRampSeconds = 0.25f;    // ease-in / ease-out window at each end
	float ActiveNodPitchDeg = 0.f;   // current additive pitch (chin-down positive)
	void UpdateKnowingNod(float DeltaTime);

	// #10 emotion: target label (EInworldEmotionLabel as int; -1 = none set yet -> neutral).
	// CurrentEmotionFace is the smoothed expression actually rendered; it interpolates
	// toward TargetEmotionFace each tick (EmotionBlendSeconds) and is mixed into
	// GetFaceCurveWeights(). TargetEmotionFace = preset for the current label * strength.
	int32 CurrentEmotionLabel = -1;
	FFaceCurveWeights TargetEmotionFace;
	FFaceCurveWeights CurrentEmotionFace;
	void UpdateEmotionFace(float DeltaTime);
	static FFaceCurveWeights EmotionPreset(int32 Label);

	// Apply additive bone rotation AFTER the mesh's own anim eval (post-retarget).
	void ApplyBodyMotion(float DeltaTime);
	void ApplyBlink(float DeltaTime);

	// Hook that runs after the body mesh finalizes its pose each frame (parameterless
	// per FOnBoneTransformsFinalizedMultiCast). Reads the smoothed Head/Neck/SpineRot
	// computed in TickComponent and applies them additively to the component-space pose.
	void OnBodyPoseFinalized();
	FDelegateHandle BoneFinalizeHandle;

	// Blink must be applied AFTER Face_AnimBP_Hamilton evaluates, or the face AnimBP wipes
	// our SetMorphTarget every frame. We compute the blink value in ApplyBlink (state machine)
	// and write the eye morphs in this post-eval hook on the FACE mesh. No recursion risk
	// (morphs don't re-enter the finalize flow the way FinalizeBoneTransform did for bones).
	void OnFacePoseFinalized();
	FDelegateHandle FaceFinalizeHandle;
	float CurrentBlinkValue = 0.0f; // 0 = eyes open, 1 = fully closed

	// ---- Gaze (eye tracking) ----
	// Self-contained: resolves eye bones on the face mesh, runs a hold/look-away state machine,
	// and writes additive eye-bone rotation in OnFacePoseFinalized (same post-eval hook as blink).
	// If eye bones don't resolve or bEnableGaze is false, the whole feature no-ops cleanly.
	void UpdateGaze(float DeltaTime);
	void ApplyGazeCurves(class UAnimInstance* FaceAnim);
	bool GetCameraWorldLocation(FVector& OutLoc) const;
	bool bGazeBonesFound = false;
	int32 LeftEyeBoneIndex = INDEX_NONE;
	int32 RightEyeBoneIndex = INDEX_NONE;
	bool bGazeLookingAtCamera = true;
	float GazeStateTimer = 0.0f;
	FVector2D GazeTargetDeg = FVector2D::ZeroVector;  // desired eye yaw/pitch (deg)
	FVector2D GazeCurrentDeg = FVector2D::ZeroVector; // smoothed eye yaw/pitch (deg)

	// Apply an additive component-space rotation to one bone in the editable CS transform array.
	void AddBoneRotationCS(USkeletalMeshComponent* Mesh, FName BoneName, const FRotator& AdditiveRot);

	// Emotion -> motion multiplier (1.0 neutral, higher for agitated states).
	float CurrentEmotionBias() const;

	// Noise + clocks.
	static float Noise1D(float T, float Seed);
	float NoiseTime = 0.0f;
	float BreathPhase = 0.0f;

	// Look-around state.
	float GlanceTimer = 0.0f;
	FVector2D GlanceCurrent = FVector2D::ZeroVector; // yaw, pitch (deg)
	FVector2D GlanceTarget = FVector2D::ZeroVector;

	// Smoothed output rotations (component space, deg).
	FRotator HeadRot = FRotator::ZeroRotator;
	FRotator NeckRot = FRotator::ZeroRotator;
	FRotator SpineRot = FRotator::ZeroRotator;

	// Blink state.
	float BlinkTimer = 0.0f;
	float BlinkPhase = -1.0f; // <0 = not blinking; 0..1 during a blink

	FRandomStream Rng;
};
