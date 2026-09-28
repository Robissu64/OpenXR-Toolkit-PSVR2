# OpenXR Toolkit PSVR2 v1.1 release candidate

This is a PSVR2 hardware test package for the eye-gaze binding fix. It is not the public v1.1 release. See `RELEASE_NOTES.md` for the fix and regression status.

## Requirements

Windows x64, PSVR2 with the Sony PSVR2 PC Adapter, SteamVR as the active OpenXR runtime, and [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) with calibrated, working eye tracking. Close VR games and SteamVR before installation. Remove any original OpenXR Toolkit installation that registers a layer with the same internal name.

## Install

1. Download the `OpenXR-Toolkit-PSVR2-v1.1-RC-x64` artifact from the successful GitHub Actions run for this branch.
2. Extract every file to a permanent folder. Keep the DLLs, JSON manifest, scripts, and `shaders` folder together.
3. Double-click `Install.bat` and accept the Windows UAC prompt.
4. Restart SteamVR, then launch the game.

Installation is system-wide and only needed once. Do not move or delete the extracted folder while the layer is installed. To uninstall, close VR games and SteamVR, double-click `Uninstall.bat`, and accept UAC. `Install-Layer.ps1` and `Uninstall-Layer.ps1` remain available for manual installation.

## Testing

Enable **Eye tracking** and **Foveated rendering** in the Toolkit menu (`Ctrl+F2`). For the RC regression checks, use Hubris through native OpenXR, Gunman Contracts through native OpenXR, and COMPOUND Demo through OpenComposite. Confirm that gaze tracks eye movement and that each game remains stable. In the log at `%LOCALAPPDATA%\OpenXR-Toolkit\logs\XR_APILAYER_MBUCCHIA_toolkit.log`, look for `[PSVR2-DIAG]` entries for the eye binding merge, successful ActionSet attach and first sync, first active gaze, and the first change in projected gaze.

Exact Crop still requires the game to accept the recommended resolution. Its calibration and settings are unchanged in this RC. See the [v1.0 user guide](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/blob/v1.0/docs/PSVR2_V1_README.md) for controls, Crop calibration, and troubleshooting.
