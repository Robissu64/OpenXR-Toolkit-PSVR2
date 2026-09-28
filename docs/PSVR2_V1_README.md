# OpenXR Toolkit PSVR2 v1.0: installation and use

This is an independent community fork of OpenXR Toolkit for PSVR2 on PC. Read the [project README](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/blob/release/v1.0/README.md) for features, tested games, benchmark results, credits, and contribution details.

## Before installing

You need Windows x64, a PSVR2 headset with the Sony PSVR2 PC Adapter, SteamVR set as the active OpenXR runtime, and [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit). Calibrate eye tracking and confirm that it works through PSVR2Toolkit before using Eye-Tracked Foveated Rendering. Close VR games and SteamVR during installation. If the original OpenXR Toolkit is installed, uninstall it first so two layers with the same internal name are not registered.

## Install once

1. Download [`OpenXR-Toolkit-PSVR2-v1.0-x64.zip`](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/releases/download/v1.0/OpenXR-Toolkit-PSVR2-v1.0-x64.zip) from [Releases](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/releases/tag/v1.0).
2. Extract **all** files to a permanent folder. Do not run the installer from inside the ZIP. Keep the DLLs, JSON manifest, scripts, and `shaders` folder together.
3. Run `Install-Layer.ps1` as Administrator from that folder. Restart SteamVR and launch a compatible game.

The installation is system-wide. You do **not** reinstall for each game or reboot. Do **not** move or delete the extracted folder after installation, because the layer registration points to files there.

To uninstall, close VR games and SteamVR, run `Uninstall-Layer.ps1` as Administrator from that same folder, then remove the folder if desired.

## Configure the Toolkit

In a compatible OpenXR game, press **Ctrl+F2** to open the menu. Enable **Eye tracking** and **Foveated rendering** in Performance. For the tested BONELAB benchmark example, select **Performance** and **Narrow** for foveated rendering, then in Appearance select **Field of view: Simple**, **Adjustment: 85%**, and **Crop Resolution to FOV: On**. This is the benchmark configuration, not a recommendation for every game. Reducing FOV reduces the visible field of view.

When Crop is first enabled, `Calibration pending` means the Toolkit is observing the original FOV and using a linear estimate. Close the game normally, then start it again; `Exact` means the saved calibration is being used for tangent-based scaling. `Inactive` means Crop cannot apply under current conditions; inspect `[FOV-CROP]` in the log. A changed FOV or Crop setting requires a **game restart** to affect resolution. Swapchains already created by the game are not resized during the session.

Native OpenXR games can load the installed layer automatically. OpenVR games require [OpenComposite](https://gitlab.com/znixian/OpenOVR); its compatibility varies by game. Avoid replacing game DLLs without checking that project's game-specific instructions.

## Troubleshooting

- **Menu missing:** confirm SteamVR is the active OpenXR runtime, the installation completed, and the extracted folder has not moved.
- **Eye tracking or DFR inactive:** confirm eye tracking works in PSVR2Toolkit; then check the Toolkit's Eye tracking and Foveated rendering settings. Look for `[PSVR2-DIAG]` messages.
- **`Calibration pending` persists:** look for `calibration stored - restart for exact crop`, close the game normally, then relaunch. Calibration files are under `%LOCALAPPDATA%\OpenXR-Toolkit\configs\fov_crop_calibration_*.txt`.
- **`Inactive`:** check for FSR/NIS/CAS, a manual resolution override, Advanced FOV, or FOV 100%; the reason appears in `[FOV-CROP]`.
- **No pixel reduction:** some games ignore the recommended resolution. Compare `crop recommendation accepted/ignored` with the requested dimensions logged for `xrCreateSwapchain`.

Log file: `%LOCALAPPDATA%\OpenXR-Toolkit\logs\XR_APILAYER_MBUCCHIA_toolkit.log`.

The only successfully tested games so far are **BONELAB, Gunman Contracts, and COMPOUND Demo**. Vertigo 2 stayed flat through OpenComposite even without this Toolkit layer; other games have not been validated. See the [v1.0 release notes](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/blob/release/v1.0/docs/RELEASE_NOTES_v1.0.md) for scope and limitations.
