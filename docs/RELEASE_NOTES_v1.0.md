# OpenXR Toolkit PSVR2 v1.0 release notes

This independent community fork adds PSVR2 eye tracking through OpenXR, an Eye ActionSet lifecycle fix for OpenComposite, Eye-Tracked Dynamic Foveated Rendering, and Crop Resolution to FOV with persistent calibration and exact tangent-based scaling.

## Requirements and quick install

Windows x64, PSVR2 with the Sony PSVR2 PC Adapter, SteamVR as the active OpenXR runtime, and [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) with calibrated, working eye tracking. Extract the entire release ZIP to a permanent folder and run `Install-Layer.ps1` as Administrator. Keep that folder in place. Installation is system-wide and only needed once. Run `Uninstall-Layer.ps1` as Administrator to uninstall. See the [installation guide](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/blob/release/v1.0/docs/PSVR2_V1_README.md) or the `README.md` included in the ZIP.

## Crop calibration

The first launch with Crop enabled may show `Calibration pending` and use a linear estimate. Close the game normally and relaunch; `Exact` indicates that saved FOV calibration and tangent-based scaling are active. `Inactive` indicates Crop is unavailable under current settings; check `[FOV-CROP]` in the log. Restart the game after changing FOV or Crop settings that affect resolution. A game must accept the recommended resolution for Crop to reduce rendered pixels.

## Tested games and performance

**Only BONELAB, Gunman Contracts, and COMPOUND Demo have been successfully tested.** Gunman Contracts was validated through native OpenXR with eye tracking, DFR, and Exact Crop. At FOV 90%, its recommended per-eye resolution changed from 3400 × 3468 to 2756 × 2872, about 32.9% fewer recommended pixels. COMPOUND Demo was validated through OpenComposite with eye tracking and DFR; Exact Crop was not measured there.

The BONELAB benchmark used a Ryzen 7 5700X, RTX 4070 Super, 32 GB RAM, High preset, SteamVR 120 Hz and 100% render resolution, and the same scene. XR Telemetry measured average GPU frametime of **12.55 ms** at baseline (about 60 FPS), **11.27 ms** with DFR Performance + Narrow at FOV 100% and Crop Off (about 60 FPS), and **5.54 ms** with DFR Performance + Narrow at FOV 85% and Exact Crop On (about 120 FPS). The optimized run reduced average GPU frametime by about **55.9%** versus baseline; about **99.95%** of GPU frames met the roughly **8.33 ms** budget. These results are specific to this hardware, game, and scene, not a universal performance guarantee.

## Known limits

Some games ignore the recommended render resolution. OpenComposite compatibility varies by game. Vertigo 2 stayed flat through OpenComposite even without the Toolkit layer, so this is treated as an external game/OpenComposite compatibility issue rather than a confirmed Toolkit regression. Other games have not been validated.

For setup, controls, credits, and contribution information, see the [project README](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/blob/release/v1.0/README.md).
