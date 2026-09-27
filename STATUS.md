# Status

- Base branch: `main` at `6b9ecb69a4b2dc714b14a86407868af315d02531`.
- Working branch: `diag/psvr2-eye-actions`; current revision is `git rev-parse HEAD` in the delivered checkout.
- Build: diagnostic sources and Windows GitHub Actions workflow prepared; this Linux environment lacks MSVC/MSBuild. Windows CI execution requires pushing this branch or a Windows machine. No DLL or installable build has been produced yet.
- Working: PSVR2 eye tracking and DFR in Gunman Contracts, per user's physical test.
- Failing: eye tracked DFR in unspecified OpenVR game via OpenComposite, per user's report; specific action failure unverified.
- Last test: `git diff --check`, complete checkout of pinned submodules, static code comparison with OpenComposite revision `cff07db75c4823afe93ed7027b03d5f7bc86f164`. Windows compilation and physical PSVR2 A/B remain pending.
