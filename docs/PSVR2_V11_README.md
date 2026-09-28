# OpenXR Toolkit PSVR2 v1.1: installation and use

This is an independent community fork of OpenXR Toolkit for PSVR2 on PC. See `RELEASE_NOTES.md` for the v1.1 eye-gaze compatibility fix and PSVR2 hardware validation.

## Requirements

Windows x64, PSVR2 with the Sony PSVR2 PC Adapter, SteamVR as the active OpenXR runtime, and [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) with calibrated, working eye tracking. Close VR games and SteamVR before installation. Uninstall any original OpenXR Toolkit installation that registers a layer with the same internal name.

## Install once

1. Download [`OpenXR-Toolkit-PSVR2-v1.1-x64.zip`](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/releases/download/v1.1/OpenXR-Toolkit-PSVR2-v1.1-x64.zip) from the [v1.1 release](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/releases/tag/v1.1).
2. Extract the entire ZIP to a permanent folder. Keep its DLLs, JSON manifest, scripts, and `shaders` folder together.
3. Double-click `Install.bat` and accept the Windows UAC prompt.
4. Restart SteamVR, then launch a compatible game.

Installation is system-wide and only needed once. Do not move or delete the extracted folder while the layer is installed. To uninstall, close VR games and SteamVR, double-click `Uninstall.bat`, and accept UAC. The PowerShell scripts remain available for manual installation.

## Configure

Press **Ctrl+F2** in a compatible game to open the Toolkit menu. Enable **Eye tracking** and **Foveated rendering** in Performance. Eye tracking must already work in PSVR2Toolkit. OpenVR games require [OpenComposite](https://gitlab.com/znixian/OpenOVR) to route through OpenXR; compatibility varies by game.

With Crop enabled, `Calibration pending` means the Toolkit is recording the original FOV and using a linear estimate. Close the game normally and relaunch; `Exact` means saved calibration is used for tangent-based scaling. `Inactive` means Crop cannot apply under the current settings. Changing FOV or Crop settings that affect resolution requires a game restart. Some games ignore the recommended resolution, so inspect `[FOV-CROP]` and swapchain dimensions in the log to confirm a pixel reduction.

The log is at `%LOCALAPPDATA%\OpenXR-Toolkit\logs\XR_APILAYER_MBUCCHIA_toolkit.log`. Look for `[PSVR2-DIAG]` when troubleshooting eye tracking. Hubris, Gunman Contracts, and COMPOUND Demo passed the v1.1 RC hardware regression tests on PSVR2; BONELAB was tested for the earlier benchmark. These results do not imply universal game compatibility.
