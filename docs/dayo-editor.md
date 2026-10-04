# Dayo editor workspaces

The native editor builds `dayo_editor` and uses an `EditorSession` for motion
operations and stable key selection. Application owns renderer/media integration;
pose binding, interpolation, model commands, and viewport editing live in
`src/editor/`. This feature depends on the libmmd editor-pose API.

## Bone and morph editing

Select a model and use Inspector's Bone / Morph section. Bone values are sampled
VMD/VPD local inputs, rather than solved skinning translations. The Physics
checkbox loads the animator's step-sampled `inputPhysics`; position-only edits
and Revert preserve that flag. Changing model,
frame, or motion revision reloads scratch values; an ordinary UI frame preserves
unregistered input. Position/quaternion edits preview immediately through
transient overrides. Register replaces a matching name/frame key; Revert reloads
the base value and Init restores the neutral pose. Existing interpolation is
retained when a bone key is replaced. Registering a VPD-derived edit removes
only that bone's persistent VPD override, with Undo restoring it.

Viewport markers use CPU projection shared by Preview and native renderers.
Click selects the closest marker. Shift adds and Ctrl toggles. Drag an empty
area for a rectangle; Shift adds its contents and Ctrl inverts them. Translation
and rotation gizmos support Local / World and apply a delta to every selected
bone. Numeric fields remain VMD-local authoring inputs. IK/inherited bones use
numeric editing; their world gizmo is disabled. Physics-driven bones require
physics to be disabled for the selection before gizmo editing. Right drag
orbits, middle drag pans, and the wheel changes distance.

Morph values are sampled from their current track and use absolute live
weights. Revert and Init change preview scratch; Register writes a key.
Unregistered preview edits remain separate from project motion. The quit dialog
warns before discarding them; saving a project does not implicitly register keys.

## Timeline and interpolation

Timeline retains transport, waveform, zoom, scrolling, and clipboard operations.
Diamonds are selectable and draggable. Shift-click in Key List extends a range;
Ctrl-click toggles a key. Canvas rectangles replace selection, Shift adds, and
Ctrl inverts. Key movement uses the original motion as its baseline and produces
one Undo entry on release. Esc cancels a drag. Stable IDs survive frame sorting,
collisions, Undo/Redo, and target switches; vector indices are resolved only for
an operation. Visibility, IK, and external-parent keys share one Timeline row.
A coincident diamond selects all underlying IDs; external-parent keys support
selection, copy/cut/paste, deletion, dragging, and Undo/Redo.

Animation workspace includes Interpolation. Select Bone or Camera keys and
choose an axis (Bone: XYZ/rotation; Camera: XYZ/rotation/distance/FoV). Edit the
Bezier points or choose Catmull-Rom, then Register. All axes applies to every
channel of compatible selected keys. Mixed selections are reported. Copy,
Paste, and Init affect curve scratch. Untouched keys retain their previous
linear/Catmull-Rom behavior when per-key interpolation is enabled.

## Models, view, and camera

View → Models / External parents opens visibility-key registration, reversible
model deletion, Motion/Deform/Postprocess/Raster drag ordering, and safe model /
bone pickers for external-parent links. Attachments replace the original PMX
parent pose, preserving the solved child-relative pose. Animation evaluation
converts parent PMX positions through the common preview space into the child
model space before passing them to libmmd, so different model normalizations
preserve attachment alignment. New visibility keys inherit the effective IK
states; same-frame edits retain them. External-parent children must be movable
and cannot be driven by a rigid body. Editor visibility in Inspector remains separate. External-parent
registration/removal updates runtime links and VMdayo keys in one history
command. Authored links use motion keys without a static fallback, so cutting
or deleting the last key removes the attachment. Effective links are sampled by frame and parent models are evaluated
before their children. Attachment transforms run after each model's local
IK/physics solve and before skinning; cross-model physics/IK feedback is outside
this attachment contract.

Renderer selects Preview/Subayai/BDPT through the existing capability fallback.
View exposes info, rigid-body wireframes, model tracking, denoising, free camera,
1x/half/quarter preview resolution, runtime mode, physics, material settings,
and manipulation history. Native OIDN dispatch is a beauty passthrough when
Denoiser is disabled, including structured-buffer inputs.

Camera workspace loads current camera/light/shadow values and provides
Register/Revert/Init. Camera tracking uses model/bone pickers. Record camera
range confirms replacement of existing camera keys, captures the viewport
camera, and creates one Undo entry for the recording. During playback, Space
registers a key at the current frame; advancing frames alone creates no keys.
Stop or reaching the range end commits it and selects the recorded keys.
Animation menu controls playback start/end; Media can unload audio without unloading the video background.

Edit → Preferences persists language/theme under the platform configuration
folder (`$XDG_CONFIG_HOME/mikumikudesu` or the home `.config` fallback on Unix).
Core editing controls have English/Japanese labels; diagnostics and labels
without a translation retain English. Reset defaults resets preferences/layout,
not project assets. Help includes shortcut and borrowed-asset windows. Save As
requires confirmation before replacing another project, and quitting a modified
project offers Save / Discard / Cancel. Successful project loads establish a
clean savepoint and reset unregistered scratch. Undo/Redo restores history state
identities, so Undo back to the saved state is clean even after branching. Dirty
checks compare the effective settings used by serialization, including playback,
audio and output controls; legacy material and numeric order edits use history.

## Regression coverage

`dayo_editor_tests` covers stable selection under collisions and Undo/Redo,
original-baseline dragging, cancellation, per-key curves, persistent scratch,
frame-isolated overrides, VPD registration, savepoints/branching, sparse
Space-triggered camera recording and recorded selection, real material edits, external-parent
cycle rollback, IK-preserving visibility registration, cross-model normalized
attachments, external-parent clipboard/drag/Undo and child eligibility, deletion/order undo, quaternion/projection math, and headless
ImGui marker/rectangle input. Existing Preview regression and synchronization
validation tests remain enabled.
