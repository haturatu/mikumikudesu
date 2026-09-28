# Preview zero-normal regression

A model vertex with a zero normal must still pass through morphing, skinning,
instance placement, camera transforms, and projection. A degenerate normal uses
`(0, 0, 1)` as a shading fallback. Background draws use an explicit push-constant
flag rather than identifying their vertices by their normals.

## Vivian reproduction

The supplied Vivian.pmx has 31,709 vertices and 117,111 indices. Vertex 9446 is
the only vertex with an exactly zero normal:

- Position: `(0, 16.691345, 0.785809)`
- Normal: `(0, 0, 0)`
- UV: `(0.927500, 0.342600)`
- BDEF2: bones 8 and 9, weights approximately 0.835283 and 0.164717
- Material: `体`
- Ten triangles share the vertex.

Previously, the background shortcut assigned this vertex the clip position
`(0, 16.691345, 0, 1)`, stretching only its adjoining triangles outside the view.
The renderer now selects that shortcut only for the explicit background pass.
The PMX file is read without modification and is not included in the repository.

## Automated coverage

`preview_render` uses a small triangle fan containing the reported vertex data.
It compares GPU readback pixel bytes against an otherwise identical mesh with a
unit +Z normal, and requires visible reference geometry. It covers:

- Already deformed input and preview BDEF1, BDEF2, BDEF4, SDEF, and QDEF
- Zero and near-zero normals
- Perspective and orthographic projection, camera rotation and translation
- Outline rendering and normal-debug shading
- A textured background under model isolation and normal-debug settings

Translations on bones 8 and 9 exercise skinning without rotating the +Z normal.
The synthetic BDEF4 and QDEF cases use two nonzero influences; they exercise
those shader paths rather than reproducing four distinct bone transforms.

To additionally test the ten original Vivian triangles through the PMX loader:

```sh
DAYO_TEST_VIVIAN_PMX="$HOME/Downloads/vivian/Vivian.pmx" \
  ctest --test-dir build/linux-debug -R '^preview_render$' --output-on-failure
```

This optional path validates the model's vertex/index counts, the zero normal,
and the ten shared triangles, then renders those original positions and normals
with controlled materials and bone translations. It checks the renderer's
position handling; it does not validate the model's animation, physics, or
textures. CI runs the portable synthetic fixture without the external asset.
