# TimeWalk MetaHumans — Character Blueprint Notes

*Last updated by Helm — July 3, 2026*

Three parallel, drag-and-drop character Blueprints. Drag any one into a level and it
renders the correct MetaHuman (face + body + grooms), is **fully clothed**, and plays the
**real retargeted TimeWalk idle** looping. No Inworld / TimeWalkNPC plugin content required.

## The three top-level character BPs (consistent `<era>-<Name>-` naming)

| Character | Blueprint | Face mesh | Body mesh |
|---|---|---|---|
| Alexander Hamilton | `Content/MetaHumans/Alexander/1776-Alexander_Hamilton-BP` | `Alexander/Face/1776-Alexander_Hamilton-SM_Face` | `Alexander/.../m_med_unw_body` (stock archetype) |
| Aaron Burr | `Content/MetaHumans/Aaron_Burr/1776-Aaron_Burr-BP` | `Aaron_Burr/Face/1776-Aaron_Burr-SM_Face` | `Aaron_Burr/.../m_med_nrw_body` (his own) |
| Peter Stuyvesant | `Content/MetaHumans/Peter_Stuyvesant/1664-Peter_Stuyvesant-BP` | `Peter_Stuyvesant/Face/1664-Peter_Stuyvesant-SM_Face` | `Peter_Stuyvesant/.../1664-Peter_Stuyvesant-SM_Body` |

### Body-mesh notes
- **Hamilton** uses the stock MetaHuman male medium/underweight archetype body
  (`m_med_unw_body`) — a shared archetype name, *not* character-specific, so it was **not**
  renamed to a `-SM_Body` pattern.
- **Aaron Burr** now uses **his own** body mesh `m_med_nrw_body` (medium/normal-weight),
  which lives under `Aaron_Burr/Male/Medium/NormalWeight/Body/` and matches what his source
  livelink BP referenced. (Earlier handoff notes said Burr reused Hamilton's `m_med_unw_body`;
  that was the pre-fix state — his Body component now points at his own `m_med_nrw_body`.)
  It is also a stock archetype name, so it was **not** renamed to a `-SM_Body` pattern.
- **Stuyvesant** has a genuinely character-specific body (`Peter_Body_UE`), so it **was**
  renamed to `1664-Peter_Stuyvesant-SM_Body`. (The unused stock `m_med_nrw_body` in his
  folder was left untouched.)

## Consistent naming applied (2026-07-03)

Character-unique assets renamed in place (kept in their folders) via
`EditorAssetLibrary.rename_asset`. **Nothing under `Content/MetaHumans/Common` was renamed
or modified** (only the retired placeholder idle was deleted from `Common/Anims`).

Per character (`<prefix>` = `1776-Alexander_Hamilton`, `1776-Aaron_Burr`,
`1664-Peter_Stuyvesant`):

- BP → `<prefix>-BP`
- Face mesh → `<prefix>-SM_Face` (also fixed Hamilton's `Alexande_FaceMesh` typo)
- Grooms → `<prefix>-Groom_Hair`, `-Groom_Beard`, `-Groom_Mustache`, `-Groom_Eyebrows`,
  `-Groom_Eyelashes`, `-Groom_Fuzz` (a groom only exists if the character actually has it —
  Hamilton has no Beard/Mustache groom; he's clean-shaven by design.)
- Character garments → wired into the existing Torso / Legs / Feet / Hat / cap / CAPE
  clothing SkeletalMeshComponents (see below).
- Stuyvesant's own body → `1664-Peter_Stuyvesant-SM_Body`.

Shared idle clips under `/Game/TimeWalk/IdleAnims/` are a **shared library** and were
deliberately **not** character-prefixed.

## Clothing (fully dressed — wired 2026-07-03)

Characters previously imported in underwear (empty clothing slots). Garment meshes were
copied from the source project (`TimeWalk_AI`) preserving `/Game` paths, then each garment
`SkeletalMeshComponent` was wired to the matching mesh (slot assignments mirrored from each
character's source livelink BP):

| Slot | Hamilton | Burr | Stuyvesant |
|---|---|---|---|
| Torso | `Latest/Clothes/New_Torso/Torso_New_Test_1` | `Aaron_Burr_Cloths/Aaron_Burr_Torso/Torso` | `Cloths/Torso/Torso` |
| Legs  | `Latest/Clothes/New_Pant/Pant_New_2` | `Aaron_Burr_Cloths/Aaron_Burr_Pant/Pant` | `Cloths/Pant/Pant` |
| Feet  | `Latest/Clothes/Shoes/Shoes` | `Aaron_Burr_Cloths/Aaron_Burr_Shoes/Shoes` | *(none — source has no shoe; barefoot/covered by design)* |
| Hat   | `Latest/Clothes/Hat/Hat` | `Aaron_Burr_Cloths/Aaron_Burr_Hat/Hat` | `hat`→`Cloths/Dutch_Cap/hat` |
| cap   | — | — | `Cloths/Skull_Cap/Skull_Cap` |
| CAPE  | — | — | `Cloths/Cape/Cape_Latest` |

Verified headless: every wired slot resolves to a real mesh (no `None`). Stuyvesant's `Feet`
slot is intentionally empty — his source BP assigned no shoe mesh.

## Idle animation — REAL retargeted TimeWalk idle (placeholder removed)

The earlier procedural placeholder `A_MH_SubtleIdle` has been **deleted** and replaced with
the **real retargeted TimeWalk idle AnimSequences** (clean — no Inworld/TimeWalkNPC deps,
skeleton = `metahuman_base_skel`):

| Character | Idle clip (looping, `Use Animation Asset`) |
|---|---|
| Hamilton | `TimeWalk/IdleAnims/Retargeted/A_AC_m_standby_idle_m-standby-idle_MH2` |
| Burr | `TimeWalk/IdleAnims/Retargeted/A_AC_m_standby_idle_m-standby-idle_MH2` |
| Stuyvesant | `TimeWalk/IdleAnims/Retargeted/A_AC_male_idle_cautious_male-idle-cautious_MH2` |

A third retargeted clip (`A_AC_male_idle_279398_male-idle_279398_MH2`) is also copied into
the shared idle library for variety/future use. Each character's `Body` component is set to
`AnimationMode = Use Animation Asset`, `Anim To Play = <clip>`, `saved_looping = true`. The
huge raw source mocap (`IdleAnims/Source/`) and the TimeWalkNPC-referencing
`A_Metahuman_Idle` were **not** copied (size + clean-dependency reasons).

## Face blink — MANUAL one-toggle step (headless could not author it)

**Status: BLINK_MANUAL_STEP for all three.** A periodic eye-blink was **not** shippable
headlessly: the Face is driven by `Common/Face/Face_AnimBP` + `Face_PostProcess_AnimBP`
(both under `Common`, which we must not modify), that Face_AnimBP exposes **no** blink
parameter, and UE 5.8 headless Python cannot reliably author a new AnimGraph (blend + curve
driver nodes) for a layered blink ABP. Rather than ship a broken stub, blink is left as a
small in-editor step:

**To add a periodic blink (per character, ~2–3 min):**
1. In the Content Browser, **duplicate** `Content/MetaHumans/Common/Face/Face_PostProcess_AnimBP`
   into the character's folder as `<prefix>-ABP_FaceBlink` (project-local copy — do **not**
   edit the Common original).
2. Open it. In the AnimGraph, after the existing post-process pose, add a **Modify Curve**
   node (Blend mode = Add) that drives the `CTRL_expressions_eyeBlinkL` and
   `CTRL_expressions_eyeBlinkR` curves.
3. Feed those curve values from a small **randomized timer** in the Event Graph: on a
   3–6 s random interval, play a quick 0→1→0 blink ramp (~0.12 s) into two named-curve
   variables read by the Modify Curve node.
4. On the character BP's **Face** component, set **Post Process Anim Blueprint** (or the
   `Anim Class`) to `<prefix>-ABP_FaceBlink`. Compile + save.

Until this is done, faces are static-eyed but otherwise fully correct. This is the *only*
blink work outstanding.

## Known in-editor follow-up — Aaron Burr's groom BINDING assets (one small step, ~2 min)

Burr's groom **binding** assets could not be set headlessly (UE 5.8 refuses to persist
`binding_asset` overrides on inherited SCS GroomComponents; only a freshly-populated slot
like Beard accepted it). **Impact: LOW** — the groom assets themselves are correctly
assigned and render (UE auto-computes a runtime binding to the face mesh), so hair shows up.
This is a quality/consistency refinement, not a break.

**To finish it:** open `1776-Aaron_Burr-BP`, select each groom component, set its
**Binding Asset** to the matching one under `Aaron_Burr/MaleHair/GroomBinding/` (or
`FemaleHair/GroomBinding/` for Fuzz). Note the **groom assets were renamed** to the
`-Groom_*` pattern, but the **binding assets were not** — match by type:

| Groom component (now `-Groom_*`) | Binding asset (unchanged name) |
|---|---|
| `-Groom_Hair` | `Hair_S_HairLoss_m_head_Archetype_Binding` |
| `-Groom_Eyebrows` | `Eyebrows_M_Dense_..._Face_Archetype_Binding` |
| `-Groom_Eyelashes` | `Eyelashes_S_Sparse_..._Face_Archetype_Binding` |
| `-Groom_Fuzz` | `Peachfuzz_M_Thin_f_head_Archetype_Binding` |
| `-Groom_Mustache` | `Mustache_S_Stubble_m_head_Archetype_Binding` |
| `-Groom_Beard` | already set |

Hamilton's and Stuyvesant's bindings were authored in-editor originally and are all set.

## Verification (headless, -NullRHI)

All three BPs load by their **new** paths and resolve Face + Body + all grooms + **all wired
garment slots** + the **real looping idle** with zero missing references
(`ALL_FINAL_OK=True`). Hamilton's empty Beard/Mustache groom slots are expected
(clean-shaven). Stuyvesant's empty Feet slot is expected (no source shoe). Nothing under
`Content/MetaHumans/Common` was renamed or modified (only the placeholder idle deleted).
