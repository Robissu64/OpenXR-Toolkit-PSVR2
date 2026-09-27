# A/B test matrix

| Case | Runtime | Engine path | Eye tracking | DFR | FOV crop | Result |
| --- | --- | --- | --- | --- | --- | --- |
| Gunman Contracts | Record from `PSVR2-DIAG` log | Native OpenXR | Confirmed before diagnostic build | Confirmed before diagnostic build | Out of scope | Awaiting diagnostic log |
| Same machine, OpenVR game | Record from log | OpenComposite openxr | Problem reported, stage unknown | Problem reported | Out of scope | Awaiting diagnostic log and game/version |

Keep runtime, PSVR2Toolkit, settings, graphics API, and Toolkit build identical when possible. Compare session, attach, sync, gaze action state and locate space results by sequence/tick. Treat each newly created session separately.
