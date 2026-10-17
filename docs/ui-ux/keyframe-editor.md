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
