# Keyframe editor usability study

## Before implementation

Tested the native application with Tda Miku and the combined telephone dance/facial motion (119,365 bone keys and 2,815 morph keys). Captured the default layout, resized the timeline, scrolled its controls, switched to the key list, selected a list row, and attempted to drag a diamond.

- At 1280×720, playback and always-expanded media controls push the canvas and editing actions below the visible panel. Scrolling to the actions hides playback and the canvas.
- Canvas diamonds cannot be selected or dragged: a click only seeks. Selection requires a separate, unfiltered flat list containing over 122,000 keys.
- The list has no search, track filter, frame navigation, or useful connection to the Inspector. The Inspector still displays an unrelated bone and zero-valued registration controls after selecting a key.
- The workspace selector uses unconstrained `Selectable` widths. The first item occupies the remaining menu bar and overlaps the later workspace targets.
- Canvas labels, selected keys, multi-selection, navigation gestures and disabled actions do not explain what can be edited.

![Default layout before changes](before-layout.png)
![Expanded timeline before changes](before-timeline.png)
![Key list before changes](before-key-list.png)

## Acceptance goals

At 1280×720, keep playback, frame navigation and editing actions visible alongside a usable timeline. Support searchable tracks, visible key selection, modifier multi-selection, direct dragging, precise frame moves and selected-key value editing. A drag or an Apply action must create one undo step; Escape must cancel a drag. Keep model and global motion targets independent, preserve key interpolation data and avoid processing every key during ordinary repainting. Verify real gestures and project persistence with the telephone motion, record after screenshots, and run compiler/test/static-analysis coverage primarily in GitHub Actions.

## Implemented workflow

Playback, the editable frame number, the motion target, search, track filtering and edit actions stay above the canvas. Playback/media settings and export/interpolation options are in their own popups. Animation uses a taller timeline and collapses model/material properties. Workspace buttons have individual hit regions.

The canvas selects actual keys, shows selected keys in gold, supports captured dragging, Ctrl toggling, Shift ranges, rectangular selection and filtered Ctrl+A. Dragging shows a ghost and commits on release; Escape leaves the document untouched. Single-key edits follow the key's new frame. Group moves preserve relative spacing and the current playhead, including when a bound clamps the offset. Search matches Japanese names and any English names supplied by the model. The ruler scrubs, zooms around the pointer and pans; Fit and previous/next-key actions remain visible.

The Inspector displays only the selected key's existing fields while editing. Bone rotations are in degrees; interpolation controls and physics flags are retained. Apply creates one history entry. Multiple selection provides a numeric frame offset. Clear key selection returns to model properties. Non-finite values cannot be applied.

![Selected bone frame and position after editing](after-bone-edit.png)
![Filtered facial track and its selected morph weight](after-morph-edit.png)
![610 facial keys selected for a precise group move](after-group-selection.png)

## Real application verification

Used the native Vulkan application in a 1280×720 X11 window, with screenshots captured from its window. The model is Tda式初音ミク デフォ服ver (Tda / 金子卵黄). Input went through the window system, including keyboard shortcuts and clipboard paste of Japanese track names.

| Operation | Observed result |
| --- | --- |
| Switch Layout → Animation | The distinct workspace button responds; playback, filtering, actions and the canvas remain visible. |
| Select and drag a center key | Frame 665 → 673; one Undo restores it, and Redo reapplies it. |
| Edit the selected key in Inspector | Set frame 675 and position X = 1.234, then Apply. The playhead follows the key. |
| Search `あ` and edit a facial key | At frame 674, change weight 0.800 → 0.250. Undo and Redo restore the two values. |
| Filtered Ctrl+A and numeric group move | Select all 610 `あ` keys, move +1 frame, then Undo. Relative timing and the final stored document are restored. |
| Save and reopen the project | The application loads the saved edits. The parser finds 119,365 bone keys, 2,815 morph keys and a 4,668-frame duration. |
| Compare saved data to the original | Exactly one bone key and one morph key changed as above. All other values, rotations and interpolation control points are retained. |
| Add camera VMD, select global motion, drag then Escape | Camera fields appear in Inspector. After saving, all 1,158 camera keys match the input, independently of model edits. |

## Automated coverage

The new `keyframe_editor` CTest target drives real ImGui mouse/key events without a GPU. It verifies diamond selection and seeking, captured drag with a single history entry, cancellation, Undo/Redo, Ctrl selection, filtered select-all, stale-selection invalidation and rendering the full VMD frame range. Additional checks cover mixed-track movement, zero/upper frame bounds, duplicate references, unrelated tracks and interpolation preservation.

Compiler, complete test, sanitizer, formatting and clang-tidy coverage runs in the [PR's GitHub Actions checks](https://github.com/haturatu/mikumikudesu/pull/267/checks). Local compilation is for the native interaction review; local test execution was limited to debugging the new interaction harness. The review also includes CodeQL.
