# Phase 1 A/B test matrix

Validated on PSVR2 using code commit `7cde664cf201f586ecac1349d69faed9f9000933` from `fix/opencomposite-eye-actions`.

| Case | OpenXR path | Eye ActionSet lifecycle | Gaze and ETFR | Errors / stability | Result |
| --- | --- | --- | --- | --- | --- |
| COMPOUND Demo | OpenVR → OpenComposite (`OpenComposite_compound`) → SteamVR OpenXR → PSVR2 | Attached with OpenComposite's real ActionSets; included in `xrSyncActions`; `eye_set_ready` after successful attach and sync | `xrGetActionStatePose`: `XR_SUCCESS`, `isActive=1`; `xrLocateSpace`: `XR_SUCCESS`; projected gaze varies with eye movement; ETFR works | Opens normally; session stable; no `XR_ERROR_ACTIONSETS_ALREADY_ATTACHED` or `XR_ERROR_ACTIONSET_NOT_ATTACHED` | Pass |
| Gunman Contracts | Native OpenXR (`opencomposite=0`) → SteamVR OpenXR → PSVR2 | Eye ActionSet attached and synced; first successful sync precedes active pose | `xrGetActionStatePose`: `XR_SUCCESS`, `isActive=1`; `xrLocateSpace`: `XR_SUCCESS`; projected gaze varies; ETFR works | Opens normally; no regression observed; no `XR_ERROR` in log | Pass |

The diagnostic logs establish attach, sync, readiness, action-state, space-location, and projected-gaze order per `XrSession`. OpenComposite may create more than one session during startup; compare events by session handle. Crop Resolution to FOV was outside Phase 1 and remains untested.
