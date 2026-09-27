# PSVR2 / OpenXR Toolkit 1.3.2 project context

Goal: independently diagnose and correct eye tracked dynamic foveated rendering (DFR) with OpenComposite. Later, assess resolution cropping tied to FOV. No third party paid code is used.

Hardware: Sony PSVR2, RTX 4070 Super, Ryzen 7 5700X. PSVR2Toolkit is installed and eye tracking confirmed. Gunman Contracts is the positive control: native OpenXR, Toolkit injected, developer gaze/eyeL/eyeR move, visual DFR follows gaze. An OpenVR game through OpenComposite is the negative control; exact game/version and logs await physical testing.

Repositories: [fork](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2), [Toolkit upstream](https://github.com/mbucchia/OpenXR-Toolkit), [OpenComposite openxr](https://gitlab.com/znixian/OpenOVR/-/tree/openxr), [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit). Public conceptual reference: https://patreon.fastfox.racing/posts/3x-fps-in-vr-eye-166260249 .

Scope of this iteration: instrument action lifecycle and gaze, perform A/B comparison. Crop Resolution to FOV is research only.
