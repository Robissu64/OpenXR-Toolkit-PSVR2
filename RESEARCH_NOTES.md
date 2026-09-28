# Research notes

## Phase 1 — confirmed Eye Action lifecycle

- `XR_APILAYER_MBUCCHIA_toolkit/framework/dispatch.cpp` discovers and requests `XR_EXT_eye_gaze_interaction`. `layer.cpp::xrCreateInstance` detects the `OpenComposite_` application-name prefix; the tested path reports SteamVR/OpenXR and `playstation_vr2`.
- `layer.cpp::xrCreateSession` calls `OpenXrEyeTracker::beginSession`. The tracker creates an Eye ActionSet, pose action, eye interaction profile binding, and action space. `xrDestroySession` destroys tracker session resources; `xrEndSession` does not destroy the attached ActionSet.
- The original `xrBeginFrame` fallback attached the Eye ActionSet artificially before OpenComposite attached its own ActionSets. Its later `xrAttachSessionActionSets` failed with `XR_ERROR_ACTIONSETS_ALREADY_ATTACHED`.
- V2 (`62125a8`) excluded OpenComposite from that fallback. In the COMPOUND diagnostic log, two `xrBeginFrame` calls occurred before any observed application attach; the Toolkit queried gaze anyway and received `XR_ERROR_ACTIONSET_NOT_ATTACHED`.
- V3 (`7cde664cf201f586ecac1349d69faed9f9000933`) gates the OpenXR eye provider until the intercepted real `xrAttachSessionActionSets` succeeds with the eye set appended and an `xrSyncActions` containing it succeeds. `m_isActionSetUsed` records whether the application's successful attach contained sets; `m_isActionSetAttached` records successful attachment of the eye set (real or artificial); `m_isEyeActionSetSynced` records successful sync. These states reset for a new `XrSession`. The native OpenXR artificial fallback remains available for applications without controller ActionSets.
- The `getProjectedGaze` readiness gate covers both variable-rate shading and the debug overlay. Before readiness, it returns no gaze and emits throttled `gaze_query_skipped_not_ready` diagnostics. Existing `[PSVR2-DIAG]` logs expose attach, sync, `eye_set_ready`, action-state, locate-space, and projected-gaze order.
- In the inspected OpenComposite OpenXR revision `cff07db75c4823afe93ed7027b03d5f7bc86f164`, `DrvOpenXR/XrBackend.cpp::BindInfoSet` attaches an initial info set and anticipates a session restart; `OpenOVR/Reimpl/BaseInput.cpp` attaches legacy/game sets and syncs them in `UpdateActionState` or `InternalUpdate`. The COMPOUND log independently confirms that frames can begin before the later application attach is observed.

The OpenXR [action-set attach rule](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrAttachSessionActionSets.html) permits one attachment per session. The Toolkit therefore appends its eye set to the application's existing attach call and never repairs a session with a second attach. OpenComposite can recreate sessions during startup, so compare logs by `XrSession` handle.

## Validation

COMPOUND Demo through OpenComposite now opens normally and remains stable. The eye set is appended to OpenComposite's real sets, included in sync, and marked ready only after attach and sync. `xrGetActionStatePose` returns `XR_SUCCESS` with `isActive=1`; `xrLocateSpace` returns `XR_SUCCESS`; projected gaze changes with eye movement; Eye-Tracked Foveated Rendering works. Neither attach-already-attached nor action-set-not-attached error recurs.

Gunman Contracts through native OpenXR reports `opencomposite=0`, opens normally, attaches and syncs the eye set, then returns active eye pose and successful space location. Projected gaze varies and ETFR works. No regression or `XR_ERROR` was observed in its log.

The Windows [diagnostic workflow](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/actions/runs/36370147739) passed for V3 and produced `PSVR2-Eye-Diagnostic-x64`. Preserve MSVC v142, recursive submodules, Git LFS, and the Omnicept submodule LFS pull in `.github/workflows/psvr2-diagnostic.yml`. `BUILD_COMMIT.txt` identifies the built commit. `docs/DIAGNOSTIC_INSTALL.md` describes log capture.

## PHASE 2 — CROP RESOLUTION TO FOV (preliminary research only)

`layer.cpp::xrLocateViews` scales `XrFovf` for existing FOV controls; `xrEnumerateViewConfigurationViews` modifies recommended size for existing upscalers; `xrCreateSwapchain` and `xrEndFrame` mediate image size and projection. Examine modified/original tangent spans per eye and the actual application projection and swapchain allocations before designing a crop.

Provisional geometry: per-eye tangent span ratios are `w=(tan(rightNew)-tan(leftNew))/(tan(rightOld)-tan(leftOld))` and `h=(tan(upNew)-tan(downNew))/(tan(upOld)-tan(downOld))`. These describe projection-plane coverage, not guaranteed pixel savings. A design must account for asymmetric/canted eyes, angular pixel density, clipping, texture alignment, swapchain image rectangles, and runtime limits. Changing recommended view sizes only affects applications that honor them; changing swapchain dimensions may break application assumptions. Keep projection matrices coherent with FOV. No Crop Resolution to FOV implementation was made in Phase 1.
