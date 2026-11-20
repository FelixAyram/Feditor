# Terrain Align v4: matching spline end caps

## Fixes

- Fixed the left contour side so it explicitly includes the far-left end-cap corner. The previous loop started one sample too late, creating a diagonal shortcut from the last interior end-profile vertex to a point partway down the side.
- Replaced the fixed 0.75 m contour sampling with the longitudinal section count from `spline::omis_teile`, including the same patchwork section-count adjustment used by `build_spline_meshes`. For ordinary splines, side-ring vertices now use the same longitudinal stations as the rendered spline mesh.
- Increased the adaptive polycut fallback depth from 6 to 8. On the standard 5 m terrain cell this reduces the terminal classification scale from 7.8125 cm to about 1.953 cm; only ambiguous cells take this fallback path.

## Validation

- Compiled `terrain_polycut.cpp` in a C++20 integration harness with dependency shims.
- Verified the end-offset and skew test contour includes the exact far-left cap vertex and carries contour Z values into newly generated seam vertices.
- Ran the polycut integration path: 126,401 vertices, 50,883 triangles; contour heights were preserved.
- The full Windows/DirectX target was not built in this environment. Rebuild the project locally and inspect the same spline near both endpoints.
