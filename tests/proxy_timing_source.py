#!/usr/bin/env python3
"""Static invariants for the proxy copy timing diagnostic.

This complements tests/proxy_sync_ring.py. It does not emulate OpenXR; it checks
that the production layer keeps synchronization pinned to Proxy Sync Mode 2 and
that the three timing modes retain the intended copy/release split.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
layer = (root / "XR_APILAYER_MBUCCHIA_toolkit" / "layer.cpp").read_text(encoding="utf-8")
workflow = (root / ".github" / "workflows" / "psvr2-diagnostic.yml").read_text(encoding="utf-8")
doc = (root / "docs" / "PROXY_TIMING_TESTING.txt").read_text(encoding="utf-8")

checks = {
    "timing env is production input": 'GetEnvironmentVariableA("OXRTK_PROXY_TIMING_TEST"' in layer,
    "sync pinned to mode 2": "m_proxySyncMode = 2;" in layer,
    "mode 0 immediate path": 'm_proxyTimingMode == 0' in layer and 'releaseSubProxyImage(swapchain, "release", releaseInfo)' in layer,
    "mode 1 copies at app release": 'm_proxyTimingMode == 1 && hasProxy' in layer and 'copySubProxyImage(swapchain, "release")' in layer,
    "modes 1/2 defer": 'm_proxyTimingMode > 0' in layer and 'release_deferred' in layer,
    "endFrame completes pending releases": 'releaseSubProxyImage(m_proxyTimingPendingOrder.front(), "endFrame")' in layer,
    "scope is exactly the two games": 'return m_applicationName == "Impact" || m_applicationName == "TheMidnightWalk";' in layer,
    "pending release FIFO exists": 'm_proxyTimingPendingOrder.push_back(swapchain)' in layer,
    "frame boundary logged": 'event=xrEndFrame' in layer,
    "copy dedup state exists": "m_subProxyCopyCompleted" in layer,
    "timing branch builds": "diag/proxy-copy-timing-bisect" in workflow,
    "timing artifact named": "OpenXR-Toolkit-PSVR2-ProxyTimingDiag-x64" in workflow,
    "test guide names all modes": all(f"{n} -" in doc for n in range(3)),
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
if failed:
    raise SystemExit("proxy timing invariant failure: " + ", ".join(failed))
print("PASS: proxy timing source invariants")

for file in ('d3d12.cpp', 'interfaces.h'):
    relative = 'XR_APILAYER_MBUCCHIA_toolkit/' + file
    base = subprocess.check_output(['git', 'show',
        'acd28ea578b4171c1ed1fd01eab034513b244431:' + relative], cwd=root)
    current = subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=root)
    assert current == base, f'synchronization changed: {file}'
print('PASS: D3D12 allocator/fence implementation byte-identical to published base')
