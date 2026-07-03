# TimeWalkIdle

Standalone, drop-in procedural **liveliness** for a MetaHuman character — breathing,
subtle body sway, eye gaze, and blinking — with **no Inworld dependency required**.

Drop `UTimeWalkIdleMotionComponent` on any MetaHuman actor (or use a character BP that
already has it) and press Play. It finds the body + face skeletal meshes itself and drives
a calm, natural idle. Zero AnimGraph wiring needed.

## Inworld is optional

This plugin builds and runs **with or without** the Inworld plugins installed.

- **No Inworld installed** (this handoff project): the plugin compiles the Inworld code
  paths OUT entirely (`TIMEWALK_WITH_INWORLD=0`, auto-detected in `TimeWalkIdle.Build.cs`).
  The character breathes, sways, gazes, and blinks. It just doesn't talk — which is exactly
  what a character/likeness artist needs to evaluate the model.
- **Inworld installed** (the full talking-character project): the plugin auto-detects the
  `InworldCharacter` plugin and compiles the speech paths IN. It then also **ramps motion
  while the character speaks** and **biases fidget by the Inworld emotion label** (stressed
  = more motion). No code change — same plugin, richer behavior when Inworld is present.

## The toggle

Even when Inworld IS compiled in, the component exposes a Details-panel checkbox:

- **`bEnableInworld`** (category *TimeWalk | Inworld*, default **on**) — turn it **off** to
  force pure-idle behavior (skip the voice/emotion binding) even in an Inworld-enabled
  project. When Inworld is compiled out, the toggle is inert but still visible/harmless.

## What this plugin does NOT include

Just the idle/gaze/camera *motion*. The full talking-character system (AI brain, mouth
lipsync/visemes, microphone) lives in the separate `TimeWalkCharacters` plugin in the
main TimeWalk project. TimeWalkIdle is the portable, Inworld-free motion layer.

## Build notes

- Module `TimeWalkIdle` (Runtime). Deps: Core/CoreUObject/Engine/AnimGraphRuntime +
  HairStrandsCore/AssetRegistry. Inworld deps added conditionally only when detected.
- Inworld speech events bind via the SDK's **native** delegates
  (`OnVoiceAudioStartNative`/`CompleteNative`/`PlaybackNative`, `AddUObject`) rather than
  dynamic `UFUNCTION` delegates — so the handlers sit in `#if TIMEWALK_WITH_INWORLD`
  blocks without tripping UHT (which forbids reflected `UFUNCTION`s inside custom `#if`).

*Created by Helm — July 3, 2026*
