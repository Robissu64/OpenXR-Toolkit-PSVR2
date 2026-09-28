# Next steps

## PHASE 2 — CROP RESOLUTION TO FOV

Phase 1 Eye Action lifecycle is complete and validated on COMPOUND Demo / OpenComposite and Gunman Contracts / native OpenXR. The stable code baseline is commit `7cde664cf201f586ecac1349d69faed9f9000933` on `fix/opencomposite-eye-actions`. Preserve the validated eye action attach/sync behavior.

The next objective is to design and implement an optional Crop Resolution to FOV feature. It has **not** been implemented yet. Begin by tracing how the Toolkit modifies FOV in `xrLocateViews`, how it reports recommended view sizes, and how application swapchain dimensions and submitted projection views relate to those values. Establish a consistent per-eye resolution calculation for asymmetric FOVs and check texture alignment, swapchain limits, and applications that ignore recommended sizes. Use the preliminary geometry notes in `RESEARCH_NOTES.md` as hypotheses to verify, then test performance and visual correctness on both native OpenXR and OpenComposite.

Preserve the passing diagnostic build workflow, including MSVC v142, recursive submodules, Git LFS, and `git -C external/Omnicept-SDK lfs pull`. Its artifact is `PSVR2-Eye-Diagnostic-x64`, with `BUILD_COMMIT.txt` identifying the source commit.
