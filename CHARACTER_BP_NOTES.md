# TimeWalk MetaHumans — Character Blueprint Notes

*Last updated by Helm — July 3, 2026*

Three parallel, drag-and-drop character Blueprints. Drag any one into a level and it
renders the correct MetaHuman (face + body + grooms) and plays a subtle looping idle.

## The three top-level character BPs

| Character | Blueprint | Face mesh | Body mesh |
|---|---|---|---|
| Alexander Hamilton | `Content/MetaHumans/Alexander/BP_1776-Alexander_Hamilton_livelink` | `Alexander/Face/Alexande_FaceMesh` | `Alexander/.../m_med_unw_body` |
| Aaron Burr | `Content/MetaHumans/Aaron_Burr/BP_1776-Aaron_Burr_livelink` | `Aaron_Burr/Face/Aaron_Burr_FaceMesh` | shared `Alexander/.../m_med_unw_body` * |
| Peter Stuyvesant | `Content/MetaHumans/Peter_Stuyvesant/BP_1664-Peter_Stuyvesant_livelink` | `Peter_Stuyvesant/Face/Peter_Stuyvesant_FaceMesh` | `Peter_Stuyvesant/.../Peter_Body_UE` |

\* Aaron Burr shipped with **no body skeletal mesh of his own** (only a `BodyBaseColor`
texture). His BP reuses the shared male medium/underweight body (`m_med_unw_body`, same
`metahuman_base_skel` skeleton Hamilton uses). If a Burr-specific body mesh is later
imported, just swap the Body component's Skeletal Mesh Asset.

## Component structure (all three, mirrored from Hamilton's original)

- `Root` (Scene) → `Body` (SkeletalMesh) → `Face` (SkeletalMesh, `Face_AnimBP`) + groom
  components (Hair, Eyebrows, Eyelashes, Fuzz, Beard, Mustache) + empty clothing slots
  (Legs, Feet, Torso, Hat/cap/cape) + `LODSync`.
- Face is driven by `Common/Face/Face_AnimBP`.
- LiveLink wiring inherited from the original livelink BPs is intact.

## Idle animation

- **Asset:** `Content/MetaHumans/Common/Anims/A_MH_SubtleIdle` (AnimSequence on
  `metahuman_base_skel`).
- **What it is:** a 4-second / 120-frame (30 fps) **seamless loop** authored procedurally
  (one full sine cycle, returns to start → loops with no pop). Subtle breathing: gentle
  vertical bob on `pelvis`, chest-rise pitch on `spine_02` + `spine_04`, small
  clavicle lift. Amplitudes are deliberately small (~0.6 cm / <1°) — a calm standing idle,
  not a big animation.
- **Why authored, not stock:** this project (and the installed UE 5.8 engine) ship **no
  idle/breathing AnimSequence** compatible with the MetaHuman body skeleton — no Manny/Quinn
  content is installed, and the only engine "idle" (`Tutorial_Idle`) is on the legacy UE4
  mannequin skeleton (incompatible). So a real looping idle was built from scratch on the
  correct skeleton.
- **How it's wired:** each character's `Body` component is set to
  `AnimationMode = Use Animation Asset`, `Anim To Play = A_MH_SubtleIdle`, `Looping = true`.
  All three are identical/consistent. The body's RigLogic post-process ABP still runs on top.

## Known in-editor follow-up (one small manual step — headless couldn't do it)

**Aaron Burr's groom BINDING assets** (`binding_asset` on the Hair/Eyebrows/Eyelashes/
Fuzz/Mustache groom components) could not be set headlessly — UE 5.8's headless Python
refuses to persist `binding_asset` overrides on inherited SCS GroomComponents (confirmed
across 3 API paths; only a freshly-populated slot like Beard accepted it). 

- **Impact:** LOW. The groom **assets themselves are correctly assigned and render** (0 load
  errors). Without an explicit binding, UE auto-computes a runtime binding to the attached
  face mesh, so the hair still shows up. It's a quality/consistency refinement, not a break.
- **To finish it (2 min in-editor):** open `BP_1776-Aaron_Burr_livelink`, select each groom
  component, and set its **Binding Asset** to the matching one under
  `Aaron_Burr/MaleHair/GroomBinding/` (or `FemaleHair/GroomBinding/` for Fuzz):
  - Hair → `Hair_S_HairLoss_m_head_Archetype_Binding`
  - Eyebrows → `Eyebrows_M_Dense_..._Face_Archetype_Binding`
  - Eyelashes → `Eyelashes_S_Sparse_..._Face_Archetype_Binding`
  - Fuzz → `Peachfuzz_M_Thin_f_head_Archetype_Binding`
  - Mustache → `Mustache_S_Stubble_m_head_Archetype_Binding`
  - (Beard is already set.)
  Hamilton's and Stuyvesant's bindings were authored in-editor originally and are all set.

## Verification (headless, -NullRHI)

All three BPs load, resolve their generated class + Body/Face/Groom components with no
missing classes / unresolved refs, and report `IDLE_OK` (mode=SingleNode, anim=A_MH_SubtleIdle,
looping=true). `0 error(s)` on load. Burr confirmed as a top-level asset in
`Content/MetaHumans/Aaron_Burr/`.
