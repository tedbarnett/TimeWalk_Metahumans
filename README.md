# TimeWalk_Metahumans

A clean, content-only **Unreal Engine 5.8** project containing three TimeWalk MetaHuman characters, prepared for handoff to an external artist for improvement.

## Characters
- **Alexander Hamilton** (`Content/MetaHumans/Alexander`)
- **Aaron Burr** (`Content/MetaHumans/Aaron_Burr`)
- **Peter Stuyvesant** (`Content/MetaHumans/Peter_Stuyvesant`)
- **Common** (`Content/MetaHumans/Common`) — shared MetaHuman body/face skeleton, materials, and rig that all three characters reference.

## What was stripped
All **Inworld** AI integration has been removed. Specifically, the three Inworld character Blueprints were deleted:
- `BP_1776-Hamilton_Inworld` (Hamilton)
- `BP_Aaron_Burr_Inworld` (Aaron Burr)
- `BP_1664-Stuyvesant_Inworld` (Peter Stuyvesant)

This project has **no Inworld plugin and no custom C++ modules** (no `TimeWalkNPC`, no `UnrealMCP`). Only stock UE 5.8 MetaHuman/Groom/LiveLink/RigLogic plugins are enabled, so it opens cleanly without any third-party plugin installs.

## Known residual references (Hamilton only)
Two of Hamilton's animation Blueprints still hard-reference classes from the (removed) `TimeWalkNPC` C++ module and the Inworld runtime, so they will show an **unresolved parent class** if opened in-editor:
- `Content/MetaHumans/Alexander/Face_AnimBP_Hamilton.uasset` — parent `TimeWalkLipsyncAnimInstance` (`/Script/TimeWalkNPC`), plus `AnimNode_InworldViseme` / `InworldVisemeDataAsset` references.
- `Content/MetaHumans/Common/Female/Medium/NormalWeight/Body/ABP_HamiltonBody.uasset` — parent `TimeWalkIdleMotionAnimInstance` (`/Script/TimeWalkNPC`).

These do **not** block project load. The artist can reparent them to a stock `AnimInstance` (or the standard MetaHuman `Face_AnimBP`) if lip-sync/idle logic isn't needed for the art pass. Aaron Burr and Peter Stuyvesant have **no** such residual references — their assets are pure mesh/texture/groom/material.

## Requirements
- **Unreal Engine 5.8**
- **Git LFS is required.** All `*.uasset`, `*.umap`, `*.tga`, `*.png`, `*.exr`, and `*.fbx` files are stored in LFS. Run `git lfs install` before cloning, or run `git lfs pull` after clone to fetch the binary assets.

## How to open
1. `git lfs install` (once, if not already)
2. Clone the repo (or `git lfs pull` inside an existing clone)
3. Double-click `TimeWalk_Metahumans.uproject`, or right-click → *Switch Unreal Engine version* → 5.8, then open.

---
*Created by Helm — July 3, 2026*
