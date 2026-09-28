# PSVR2 / OpenXR Toolkit project context

## Stable baseline

- Fork: [Robissu64/OpenXR-Toolkit-PSVR2](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2).
- Branch: `fix/opencomposite-eye-actions`.
- Functionally validated code commit: `7cde664cf201f586ecac1349d69faed9f9000933`.
- Hardware: Sony PSVR2, RTX 4070 Super, Ryzen 7 5700X. PSVR2Toolkit supplies eye tracking through SteamVR/OpenXR.

Related code: [Toolkit upstream](https://github.com/mbucchia/OpenXR-Toolkit), [OpenComposite OpenXR](https://gitlab.com/znixian/OpenOVR/-/tree/openxr), and [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit). No paid third-party code is used.

## Phase 1 — Eye Action lifecycle (complete)

Eye-Tracked Foveated Rendering (ETFR) works in both tested paths: COMPOUND Demo through OpenVR → OpenComposite → SteamVR OpenXR → PSVR2, and Gunman Contracts through native OpenXR → SteamVR OpenXR → PSVR2. The COMPOUND session stays stable. Both tests show active gaze actions, successful pose location, changing projected gaze, and working ETFR. No `XR_ERROR_ACTIONSETS_ALREADY_ATTACHED` or `XR_ERROR_ACTIONSET_NOT_ATTACHED` remains in COMPOUND; no `XR_ERROR` was observed in the native Gunman Contracts log.

The original Toolkit attached its eye ActionSet artificially in `xrBeginFrame` before OpenComposite attached its own sets. V2 deferred that artificial attach for OpenComposite but still queried gaze before attachment. V3 waits for the real application's successful attach and a successful sync before querying the eye pose. Native OpenXR retains its existing artificial attach/sync fallback for applications without controller ActionSets.

The diagnostic workflow `PSVR2 eye tracking diagnostic` passed for the validated commit and published `PSVR2-Eye-Diagnostic-x64`. Its MSVC v142, recursive submodule, Git LFS, and Omnicept LFS steps are required build setup.

## Next objective

**PHASE 2 — CROP RESOLUTION TO FOV.** This feature has not been implemented. See `NEXT_STEPS.md` and the preliminary research in `RESEARCH_NOTES.md` before changing rendering behavior.
