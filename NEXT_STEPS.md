# Next steps

1. From an authenticated checkout of the fork's `main` at `6b9ecb69a4b2dc714b14a86407868af315d02531`, apply the delivered patch (`git apply PSVR2-Eye-Diagnostic.patch`), commit, and push branch `diag/psvr2-eye-actions`. Alternatively `git fetch PSVR2-Eye-Diagnostic.bundle diag/psvr2-eye-actions:diag/psvr2-eye-actions` and push it. The attempted unauthenticated dry-run push in this workspace failed, so the Windows build workflow has not run. On CI failure inspect the first compiler/linker error and patch it.
2. Test native Gunman Contracts first and capture Toolkit log.
3. Test the failing OpenVR game through the same OpenComposite version; capture Toolkit log and OpenComposite log.
4. Compare event order and the first divergence by session. Confirm whether attach, sync, action state, locate space, or DFR is the failure point.
5. Implement the smallest correction consistent with OpenXR action/session rules, then repeat A/B.
6. Only after eye tracking diagnosis, design and test optional Crop Resolution to FOV.

Blocker for an installable build in this environment: Windows MSVC/MSBuild are not installed and Windows GitHub Actions cannot start until changes reach a remote repository with Actions enabled. The configured workflow builds the layer DLL and all dependencies as `PSVR2-Eye-Diagnostic-x64`; it intentionally does not build the signed MSI installer. Physical PSVR2 testing also requires the user's Windows machine and the exact failing OpenVR game. See `docs/DIAGNOSTIC_INSTALL.md`.
