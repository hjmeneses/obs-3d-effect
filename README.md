# 3D effect plugin for OBS Studio

> **About this fork**
>
> This is a fork of [exeldro/obs-3d-effect](https://github.com/exeldro/obs-3d-effect). All credit for the
> plugin goes to Exeldro; the original README follows unchanged below this note.
>
> **Why it exists:** rotating a source with the original filter changed its apparent size with the field of
> view, clipped whatever rotated outside the source bounds, and showed jagged edges at steep angles. The
> [`improvements`](https://github.com/hjmeneses/obs-3d-effect/tree/improvements) branch adds an **Options**
> group to the filter (all off by default, so existing scenes look the same):
>
> | Option | What it does |
> |---|---|
> | Keep size and use real units | The field of view no longer changes the size of the unrotated source (like CSS perspective), and Position X/Y/Z are real source pixels that do not depend on Scale. |
> | Margin | Extra space around the source, as a percentage of its size on each side, so parts that rotate out of the source bounds are not clipped. |
> | Supersampling 2x | Renders at twice the resolution and scales down: smoother edges and less shimmering at steep angles, at the cost of more GPU. |
> | Hide back face | Hides the source when it is turned away from the camera. |
>
> Labels are in English and Spanish. The `master` branch tracks upstream without changes.

Plugin for OBS Studio adding 3D effect filter

# Download

https://obsproject.com/forum/resources/3d-effect.1692/

# Build
1. In-tree build
    - Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
    - Check out this repository to plugins/3D-effect
    - Add `add_subdirectory(3D-effect)` to plugins/CMakeLists.txt
    - Rebuild OBS Studio

1. Stand-alone build (Linux only)
    - Verify that you have package with development files for OBS
    - Check out this repository and run `cmake -S . -B build -DBUILD_OUT_OF_TREE=On && cmake --build build`

# Donations
https://www.paypal.me/exeldro
