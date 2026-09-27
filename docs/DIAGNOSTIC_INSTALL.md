# PSVR2 eye diagnostic: install and A/B capture

This is an **unsigned test build**. Keep the installed official version's installer available for restoration. Do not register two OpenXR Toolkit layers with the same layer name at once. Extract the entire Actions artifact to a persistent directory (for example `C:\OpenXR-Toolkit-PSVR2-Diagnostic`) and retain its `shaders` folder and all bundled DLLs next to the layer DLL.

1. Stop all VR games and SteamVR. In Windows Settings > Apps, uninstall the official OpenXR Toolkit if installed. Keep the original configuration but back it up first if desired.
2. Open PowerShell as Administrator in the extracted directory and run `powershell -ExecutionPolicy Bypass -File .\Install-Layer.ps1`. This registers the extracted manifest with the OpenXR loader. Keep the directory in place.
3. Restart SteamVR. Run Gunman Contracts (native OpenXR) with the same Toolkit eye tracking and DFR settings as the previously successful control. Move eyes around for at least 15 seconds; close game.
4. Copy `%LOCALAPPDATA%\OpenXR-Toolkit\logs\XR_APILAYER_MBUCCHIA_toolkit.log` to `Gunman-OpenXR.log` **before starting the second game**. Find `[PSVR2-DIAG]` and record whether gaze/eyeL/eyeR move in the developer overlay.
5. Run the previously failing OpenVR game with OpenComposite openxr, same runtime and PSVR2Toolkit, for at least 15 seconds. Copy the same log to `Game-OpenComposite.log` and copy `opencomposite.log` if available. Record game name and OpenComposite version/commit.
6. Return both complete logs (remove any personal information if needed), `BUILD_COMMIT.txt`, the game name, graphics API, runtime, and whether the DFR sharp region tracked eyes in each.

Look for `[PSVR2-DIAG]` followed by `extension`, `session created`, `xrAttachSessionActionSets`, `xrSyncActions`, `eye xrGetActionStatePose`, and `eye xrLocateSpace`. Lines have `seq` and `tick_ms` for event ordering. Gaze and sync sampling covers first 180 calls and every 600th thereafter. Failed or inactive gaze pose calls are always reported.

To remove the diagnostic layer, stop SteamVR and games, run `powershell -ExecutionPolicy Bypass -File .\Uninstall-Layer.ps1` as Administrator from the same directory, then reinstall the official version if desired.
