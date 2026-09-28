# OpenXR Toolkit PSVR2 v1.1

## Eye Tracking compatibility fix

Fixed Eye-Tracked Foveated Rendering in OpenXR applications that provide their own `XR_EXT_eye_gaze_interaction` bindings. A later `xrSuggestInteractionProfileBindings()` call for the eye-gaze profile could replace the Toolkit's private binding. Its Eye ActionSet would attach and sync, but gaze would remain inactive.

v1.1 preserves both the application's eye-gaze bindings and the Toolkit's private gaze binding before ActionSets are attached. The fix applies to the eye-gaze interaction profile generally; it contains no game-specific logic. The layer also avoids requesting `XR_EXT_eye_gaze_interaction` a second time when the application already enabled it.

## PSVR2 hardware validation

- **Hubris — native OpenXR:** eye tracking, gaze-following foveated rendering, and Exact Crop work. Hubris exposed the binding replacement issue.
- **Gunman Contracts — native OpenXR:** eye tracking, DFR, and Exact Crop work without an observed regression.
- **COMPOUND Demo — OpenVR through OpenComposite:** the Toolkit loads, eye tracking works, and the OpenComposite ActionSet lifecycle fix remains functional.

Existing v1.0 features remain available: PSVR2 eye-tracked foveated rendering, OpenComposite eye-tracking support, Exact Crop Resolution to FOV, persistent FOV calibration, and the installer/uninstaller. Compatibility with other games has not been established.

See the bundled `README.md` for installation and troubleshooting. This is an independent community fork of OpenXR Toolkit.
