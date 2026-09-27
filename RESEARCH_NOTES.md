# Research notes

## Code paths confirmed

- `XR_APILAYER_MBUCCHIA_toolkit/framework/dispatch.cpp` enumerates runtime extensions using a bootstrap instance and requests `XR_EXT_eye_gaze_interaction`; report both discovery and actual request.
- `layer.cpp::xrCreateInstance` detects OpenComposite via application name prefix `OpenComposite_` and logs runtime; constructs an OpenXR eye tracker when configured unless FB/other provider is selected.
- `layer.cpp::xrCreateSession` calls eye tracker `beginSession`; `eyetracker.cpp::OpenXrEyeTracker::beginSession` creates pose action set, gaze action, suggests eye interaction profile binding, and creates action space. Cleanup happens at `xrDestroySession`, not `xrEndSession`.
- `layer.cpp::xrAttachSessionActionSets` appends eye action set to an application's attach; `xrSyncActions` similarly appends eye action set. `xrBeginFrame` attaches and syncs artificially only if `m_isActionSetUsed` is false. That boolean is set by whether the original application's attach contains sets, not by whether subsequent syncs occur. `m_isActionSetAttached` records only artificial attach; the application attach path does not set it.
- OpenComposite openxr revision `cff07db75c4823afe93ed7027b03d5f7bc86f164`: `DrvOpenXR/XrBackend.cpp::BindInfoSet` attaches its own info action set to an initial session; `OpenOVR/Reimpl/BaseInput.cpp` later attaches legacy/game action sets and syncs actions in both `UpdateActionState` and `InternalUpdate`. A session restart is explicitly anticipated by the comment in `BindInfoSet`. Native OpenXR applications can follow a different ordering. Whether either path is failing on the user's exact game is UNCONFIRMED.

## Architecture and constraints

The layer sits between OpenComposite (the OpenXR application as seen by the loader) and SteamVR's OpenXR runtime. OpenComposite translates OpenVR input and frames into OpenXR calls; the Toolkit augments attach/sync in its OpenXR API layer. The PSVR2Toolkit may expose eye gaze through SteamVR/OpenXR upstream, independently of game input. Diagnostic logs must distinguish original versus forwarded action sets and group events by XrSession.

OpenXR specification: [xrAttachSessionActionSets](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrAttachSessionActionSets.html) must be called only once per session; [xrSyncActions](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrSyncActions.html) requires attached sets. Creating an action or changing suggested bindings after attaching its parent set fails. Thus attempting to repair a session through a second attach is invalid. A possible later fix is to sync the already attached gaze set at an appropriate frame point if the application never syncs; first verify game and runtime behavior, focus, and per-session order. The diagnostic build changes no action attachment or rendering policy.

Diagnostic source changes: `log.{h,cpp}` adds ordered monotonic log records; `framework/dispatch.cpp` reports eye extension discovery and request; `eyetracker.cpp` reports gaze action, binding, space, sampled state and pose; `layer.cpp` reports instance/runtime, session, original/forwarded attach and sync, and artificial frame syncing. `docs/DIAGNOSTIC_INSTALL.md` describes capture. `.github/workflows/psvr2-diagnostic.yml` builds a zip artifact without the original release signing requirement.

Potential risks: loader layer registration may conflict with the official Toolkit; unsigned debug build should be isolated and removed after tests; log contains app names and positional gaze data; hot path file I/O can affect frame time (first 180 frames sampled, then every 600th). An OpenComposite session restart can produce multiple distinct session handles. `m_isActionSetUsed` is not reset on session creation in the base code; this warrants testing but is not itself proof of the reported eye tracking failure.

## Working hypotheses, not yet verified on hardware

The problematic game's attach may succeed without ever calling sync in the relevant session, so Toolkit sees `m_isActionSetUsed=true` and never performs its own sync. Another possibility is failure or early creation of an action or session; compare session handles, attach/sync result and gaze `isActive` between controls. A second attach to the same session cannot safely repair an already attached set.

## Crop Resolution to FOV (preliminary only)

`layer.cpp::xrLocateViews` scales XrFovf for simple/advanced controls; `xrEnumerateViewConfigurationViews` modifies recommended size for existing upscalers; `xrCreateSwapchain` and `xrEndFrame` mediate image size and projection. Examine tangent span ratios of modified/original angles per eye and the application/graphics API projection and swapchain sizes before design. Do not implement in this iteration.

Provisional design only: compute per-eye tangent span ratios `w=(tan(rightNew)-tan(leftNew))/(tan(rightOld)-tan(leftOld))` and `h=(tan(upNew)-tan(downNew))/(tan(upOld)-tan(downOld))`; these represent projection-plane coverage, not a guarantee of pixel savings. Derive requested width/height while preserving intended angular pixel density, minding asymmetric/canted eyes, clipping, texture alignment, swapchain image rect and runtime limits. Compare actual app swapchain allocations: changing `recommendedImageRectWidth/Height` affects only apps that honor recommendations; intercepting swapchain dimensions can break app assumptions. Projection matrices must remain coherent with FOV. Defer implementation and deeper study.
