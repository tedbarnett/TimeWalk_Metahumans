# TimeWalk_Metahumans

A clean, content-only **Unreal Engine 5.8** project containing three TimeWalk MetaHuman characters, prepared for handoff to an external artist for improvement.

## Characters
- **Alexander Hamilton** (`Content/MetaHumans/Alexander`) — BP `1776-Alexander_Hamilton-BP`
- **Aaron Burr** (`Content/MetaHumans/Aaron_Burr`) — BP `1776-Aaron_Burr-BP`
- **Peter Stuyvesant** (`Content/MetaHumans/Peter_Stuyvesant`) — BP `1664-Peter_Stuyvesant-BP`
- **Common** (`Content/MetaHumans/Common`) - shared MetaHuman body/face skeleton, materials, and rig that all three characters reference. Untouched — never renamed or modified.

Drag any one character BP into a level: it renders the correct MetaHuman (face + body +
grooms), is **fully clothed**, and plays the **real retargeted TimeWalk idle** looping. No
Inworld / TimeWalkNPC plugin content required.

### Current state (updated 2026-07-03)
- **Fully dressed.** Every character's Torso / Legs / Feet / Hat / cap / CAPE slots are
  wired to real garment meshes (previously they imported in underwear). Stuyvesant has no
  shoe by design; Hamilton is clean-shaven by design.
- **Real idle, looping.** The earlier procedural placeholder (`A_MH_SubtleIdle`) was
  removed and replaced with the real retargeted TimeWalk idle clips
  (`/Game/TimeWalk/IdleAnims/Retargeted/`): Hamilton + Burr use the standby idle, Stuyvesant
  uses the cautious idle. The huge raw source mocap was intentionally skipped.
- **Consistent naming.** Character-unique assets follow `<era>-<Name>-` (`1776-...`,
  `1664-...`): `-BP`, `-SM_Face`, `-SM_Body` (Stuyvesant only), `-Groom_*`. Shared idle
  clips are a shared library and are **not** prefixed. Nothing under `Common` was renamed.
- **Blink** is a documented one-toggle in-editor step (see `CHARACTER_BP_NOTES.md`); it
  could not be authored headlessly.
- **Aaron Burr** has his own top-level BP and uses **his own** body mesh (`m_med_nrw_body`).
- **Remaining in-editor steps** (both small, both in `CHARACTER_BP_NOTES.md`): (1) Burr's
  groom binding assets (~2 min); (2) the optional blink toggle. Everything else is done.

See **[CHARACTER_BP_NOTES.md](CHARACTER_BP_NOTES.md)** for the full component map, garment
slot assignments, idle wiring, the groom-binding step, and the blink step.

## What was stripped
All **Inworld** AI integration has been removed. Specifically, the three Inworld character Blueprints were deleted:
- `BP_1776-Hamilton_Inworld` (Hamilton)
- `BP_Aaron_Burr_Inworld` (Aaron Burr)
- `BP_1664-Stuyvesant_Inworld` (Peter Stuyvesant)

This project has **no Inworld plugin and no custom C++ modules** (no `TimeWalkNPC`, no `UnrealMCP`). Only stock UE 5.8 MetaHuman/Groom/LiveLink/RigLogic plugins are enabled, so it opens cleanly without any third-party plugin installs.

All three characters' Face and Body skeletal meshes use the **stock MetaHuman post-process AnimBPs** (`Face_PostProcess_AnimBP`, `f_med_nrw_animbp` / `m_med_nrw_animbp`). There are **no** residual references to Inworld or the `TimeWalkNPC` module anywhere in the project - verified by a full project scan (zero hits). All assets are pure mesh/texture/groom/material with stock rigs.

## Licensing
These characters were created with Epic's **MetaHuman** framework and are subject to Epic Games' MetaHuman / Unreal Engine EULA - they may be used in Unreal Engine projects but not sold as standalone assets or used outside the UE ecosystem. No third-party paid (Fab/Quixel/Megascans) assets are included.

## Requirements
- **Unreal Engine 5.8**
- **Git LFS is required.** All `*.uasset`, `*.umap`, `*.tga`, `*.png`, `*.exr`, and `*.fbx` files are stored in LFS. Run `git lfs install` before cloning, or run `git lfs pull` after clone to fetch the binary assets.

## How to open
1. `git lfs install` (once, if not already)
2. Clone the repo (or `git lfs pull` inside an existing clone)
3. Double-click `TimeWalk_Metahumans.uproject`, or right-click → *Switch Unreal Engine version* → 5.8, then open.

## Historical reference
A sourced likeness brief for the artist — birthdates, ages, life summary, and documented physical appearance of each character frozen at their target date (Hamilton &amp; Burr: Aug 28, 1776; Stuyvesant: the Sept 8, 1664 surrender of New Amsterdam). See [Historical Background & Likeness Reference](Historical_Background.html).

---
*Created by Helm - July 3, 2026*
*Last updated by Helm — July 3, 2026 (fully clothed + real TimeWalk retargeted idle + consistent 1776-/1664- naming)*
