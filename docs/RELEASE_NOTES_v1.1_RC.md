# OpenXR Toolkit PSVR2 v1.1 release candidate

This is a **test build**, not the final v1.1 release. Do not distribute it as a replacement for the public v1.0 package until the regression checks below pass on PSVR2 hardware.

## Eye-tracked foveated rendering fix

Some native OpenXR applications request `XR_EXT_eye_gaze_interaction` and suggest their own bindings for `/interaction_profiles/ext/eye_gaze_interaction`. A later successful suggestion for the same profile replaces earlier bindings. This could leave the Toolkit's gaze action unbound even though its ActionSet was attached and synchronized.

The layer now forwards the application's latest eye-gaze bindings together with the Toolkit's private gaze binding before ActionSets are attached. Identical action and binding pairs are included once. Bindings for other interaction profiles are unchanged. The layer also avoids adding `XR_EXT_eye_gaze_interaction` a second time when the application already requested it.

Hubris on PSVR2 exposed this issue and was validated with the diagnostic build: the merged suggestion returned `XR_SUCCESS`, gaze became active, projected gaze changed with eye movement, and the foveated region followed the eyes. This result does not establish compatibility with every OpenXR game.

## Regression status before final v1.1

- **Hubris, native OpenXR:** validated on PSVR2 with the merged binding fix. Recheck using this RC artifact.
- **Gunman Contracts, native OpenXR:** eye tracking, DFR, and Exact Crop passed on an earlier build. Recheck using this RC artifact.
- **COMPOUND Demo, OpenComposite:** eye tracking and the ActionSet lifecycle passed on an earlier build. Recheck using this RC artifact.

Keep the public v1.0 release available while these checks are pending. No v1.1 GitHub Release has been published.
