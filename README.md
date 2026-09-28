# OpenXR Toolkit PSVR2

An open-source community fork of [OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit), focused on PlayStation VR2 on PC. This is an independent, unofficial project; it is not affiliated with Sony or the original OpenXR Toolkit maintainers.

## Main features

- PSVR2 eye tracking through OpenXR, including an Eye ActionSet lifecycle fix for OpenVR games using OpenComposite.
- Eye-Tracked Dynamic Foveated Rendering (DFR).
- Crop Resolution to FOV, with exact tangent-based scaling after automatic, persistent FOV calibration. Games must use the recommended render resolution for the crop to save pixels.

## Requirements

- Windows x64, a PSVR2 headset, and the Sony PSVR2 PC Adapter.
- SteamVR installed and **set as the active OpenXR runtime**.
- [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) installed, with PSVR2 eye tracking calibrated and confirmed working before enabling DFR here. Follow that project's installation guide for its own setup.
- A compatible native OpenXR game, or an OpenVR game that works through [OpenComposite](https://gitlab.com/znixian/OpenOVR).

## Installation

1. Download [`OpenXR-Toolkit-PSVR2-v1.0-x64.zip`](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/releases/download/v1.0/OpenXR-Toolkit-PSVR2-v1.0-x64.zip) from the [v1.0 release](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/releases/tag/v1.0).
2. Extract the **entire ZIP** to a permanent folder. Do not run the installer from inside the ZIP; keep the extracted DLL, JSON manifest, scripts, and `shaders` folder together.
3. If the original OpenXR Toolkit is installed, uninstall it first to avoid registering two layers with the same internal name.
4. Run `Install-Layer.ps1` from the extracted folder as Administrator. Restart SteamVR and the game.

Installation is **system-wide and one-time**. You do not need to reinstall it for each game or after each reboot. **Do not move or delete the extracted folder after installation**: the registered layer points to its files.

To uninstall, close VR games and SteamVR, then run `Uninstall-Layer.ps1` as Administrator from that same folder. You can remove the folder after uninstalling.

## First run and Toolkit menu

Start a compatible OpenXR game and press **Ctrl+F2** to open the Toolkit menu. For Eye-Tracked Foveated Rendering, enable **Eye tracking** and **Foveated rendering** in the Performance settings. Eye tracking must already be working through PSVR2Toolkit.

The configuration used for the **BONELAB benchmark** was:

| Setting | Example value |
| --- | --- |
| Foveated rendering | Performance, Narrow |
| Field of view | Simple, Adjustment 85% |
| Crop Resolution to FOV | On |

**This is the configuration used for the BONELAB benchmark, not necessarily the best setting for every game.** Reducing FOV also reduces the visible field of view.

### Exact Crop calibration

On the **first launch** with Crop enabled, the menu may show `Calibration pending`. The Toolkit observes the headset's original FOV and saves a calibration; this run uses a linear estimate. **Close the game normally**, then launch it again. The menu should show `Exact`, meaning exact tangent-based resolution scaling is in use.

| Status | Meaning |
| --- | --- |
| `Inactive` | Crop is unavailable for the current settings or game; check `[FOV-CROP]` in the log. |
| `Calibration pending` | The original FOV is being recorded; restart the game after closing it normally. |
| `Exact` | A valid calibration was loaded and exact scaling is active. |

Changing FOV or Crop settings that affect resolution **requires restarting the game**. Existing swapchains are not resized while a game is running. Some games ignore the recommended resolution, so `Exact` alone does not prove that fewer pixels were rendered; check the swapchain dimensions in the log.

## OpenXR and OpenVR games

For native OpenXR games, the installed API layer can load automatically. OpenVR games require OpenComposite to route them through OpenXR. OpenComposite compatibility varies by game; this release does not claim universal OpenVR support. Consult the [OpenComposite project](https://gitlab.com/znixian/OpenOVR) for game-specific setup rather than replacing DLLs indiscriminately.

## Tested games

**Only these games have been successfully tested so far:** BONELAB, Gunman Contracts, and COMPOUND Demo. Gunman Contracts was tested through native OpenXR; COMPOUND Demo was tested through OpenComposite. BONELAB was used for the benchmark below.

Vertigo 2 was also tested, but its OpenComposite path stayed flat even after the Toolkit layer was removed. This is treated as an external OpenComposite/game compatibility issue, not a confirmed Toolkit regression. Other games have not been validated.

## BONELAB benchmark

Measured with XR Telemetry on a Ryzen 7 5700X, RTX 4070 Super, and 32 GB RAM. BONELAB used the High preset, SteamVR at 120 Hz and 100% render resolution, in the same scene for each run.

| Configuration | Average GPU frametime | Observed frame rate |
| --- | ---: | ---: |
| Baseline | 12.55 ms | Approximately 60 FPS |
| DFR only: Performance + Narrow, FOV 100%, Crop Off | 11.27 ms | Approximately 60 FPS |
| DFR + Exact Crop: Performance + Narrow, FOV 85%, Crop On | 5.54 ms | Approximately 120 FPS |

The optimized run had **about 55.9% lower average GPU frametime** than baseline; approximately **99.95% of GPU frames** were within the roughly **8.33 ms budget** for 120 Hz. These results apply to this hardware, game, and scene, and are **not a universal performance guarantee**.

## Troubleshooting

If the menu does not appear, check the active OpenXR runtime and that the extracted installation folder has not moved. If DFR is inactive, first confirm eye tracking in PSVR2Toolkit, then check the Toolkit's Eye tracking and Foveated rendering settings. If Crop is `Inactive`, check for incompatible settings such as resolution overrides, FSR/NIS/CAS, Advanced FOV, or FOV 100%. Diagnostic messages are in `%LOCALAPPDATA%\OpenXR-Toolkit\logs\XR_APILAYER_MBUCCHIA_toolkit.log` under `[PSVR2-DIAG]` and `[FOV-CROP]`. See the [installation and troubleshooting guide](docs/PSVR2_V1_README.md).

## Credits

Robissu64 led the project, hardware testing, PSVR2 validation, gameplay testing, and release. ChatGPT by OpenAI provided substantial assistance with investigation, debugging, OpenXR/OpenComposite analysis, implementation planning, log and benchmark analysis, and documentation.

The original [OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit) was created by Matthieu Bucchianeri and Jean-Luc Dupiot; its copyright and license are retained in [LICENSE](LICENSE). Thanks also to the independent [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) and [OpenComposite](https://gitlab.com/znixian/OpenOVR) projects. This fork does not claim ownership of those projects.

## Contributions welcome

Game compatibility reports, code contributions, bug reports, benchmark data, documentation, and OpenComposite testing are welcome through [Issues](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/issues) and [Pull Requests](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/pulls).

For performance reports, please include the **game, GPU, CPU, SteamVR refresh rate, SteamVR render resolution, Toolkit settings, and before/after GPU frametime** when possible.
