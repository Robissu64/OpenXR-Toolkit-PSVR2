# Status

## Phase 1 — Eye Action lifecycle: validated

- Stable branch: `fix/opencomposite-eye-actions`.
- Functionally validated code commit: `7cde664cf201f586ecac1349d69faed9f9000933` (`fix: gate OpenComposite eye gaze on attach and sync`).
- Windows CI: [`PSVR2 eye tracking diagnostic` run 36370147739](https://github.com/Robissu64/OpenXR-Toolkit-PSVR2/actions/runs/36370147739) completed successfully for that commit; artifact: `PSVR2-Eye-Diagnostic-x64`. The workflow writes `BUILD_COMMIT.txt` into the artifact.
- COMPOUND Demo / OpenComposite: game opens and session remains stable; eye set attaches with OpenComposite sets, sync includes it, `eye_set_ready` follows attach and sync, action pose is active, space location succeeds, projected gaze changes with eye movement, and ETFR works. Neither `XR_ERROR_ACTIONSETS_ALREADY_ATTACHED` nor `XR_ERROR_ACTIONSET_NOT_ATTACHED` occurs.
- Gunman Contracts / native OpenXR (`opencomposite=0`): game opens, eye set attaches and syncs, action pose becomes active after the first sync, space location succeeds, projected gaze changes, and ETFR works. No regression or `XR_ERROR` was observed in the log.
- The V1 attach timing bug and V2 early gaze query are resolved. See `RESEARCH_NOTES.md` for the lifecycle explanation and `TEST_MATRIX.md` for the validated cases.

## Phase 2 — CROP RESOLUTION TO FOV: next objective

Research and design only so far. No Crop Resolution to FOV implementation exists in this phase. See `NEXT_STEPS.md`.
