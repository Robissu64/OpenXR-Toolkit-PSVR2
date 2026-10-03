#!/usr/bin/env python3
"""Source invariants runnable on Windows Actions without a native test compiler."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = '70af55571d6665abb25cff4e543e504628263391'
def blob(path):
    return subprocess.check_output(['git', 'show', BASE + ':' + path], cwd=ROOT)

def block(src, marker):
    start = src.index(marker)
    opening = src.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]

def release_without_index_diagnostic(layer):
    """Extract the pre-existing dispatch for legacy GPU mock tests."""
    release = block(layer[layer.index('XrResult xrReleaseSwapchainImage'):], 'if (m_proxyUsageEnabled)')
    start = release.index('const auto indexTracker = m_proxyIndexTrackers.find(swapchain);')
    end = release.index('const bool copiedNow =', start)
    release = release[:start] + release[end:]
    start = release.index('if (indexTracker != m_proxyIndexTrackers.end() && m_proxyIndexMode == 1)')
    end = release.index('if (indexTracker != m_proxyIndexTrackers.end())', start + 1)
    release = release[:start] + release[end:]
    start = release.index('if (indexTracker != m_proxyIndexTrackers.end())', release.index('logProxyUsageRelease'))
    end = start + len(block(release[start:], 'if (indexTracker != m_proxyIndexTrackers.end())'))
    return release[:start] + release[end:]

relative = 'XR_APILAYER_MBUCCHIA_toolkit/'
layer = (ROOT / relative / 'layer.cpp').read_text()
base = blob(relative + 'layer.cpp').decode()
header = (ROOT / relative / 'proxy_usage_diagnostic.h').read_text()
workflow = (ROOT / '.github/workflows/psvr2-diagnostic.yml').read_text()
doc = (ROOT / 'docs/PROXY_USAGE_TESTING.txt').read_text()
for name in ('d3d12.cpp', 'interfaces.h'):
    # read Git HEAD and working tree normalized lines: catches uncommitted changes
    # without Windows checkout line endings producing false positives.
    expected = blob(relative + name)
    current = subprocess.check_output(['git', 'show', 'HEAD:' + relative + name], cwd=ROOT)
    assert current == expected, name + ' HEAD synchronization changed'
    assert (ROOT / relative / name).read_bytes().replace(b'\r\n', b'\n') == expected.replace(b'\r\n', b'\n')
print('PASS: D3D12 and interfaces unchanged in Git and working tree')

for marker in ('graphics::ProxySyncSubmission copySubProxyImage',
               'void logProxyTimingCopy', 'XrResult releaseSubProxyImage'):
    assert block(layer, marker) == block(base, marker), marker + ' changed'
print('PASS: copy/barriers/submission/wait/runtime release helpers byte-identical to Mode 0 base')

observer = '                if (m_proxyUsageEnabled) {\n                    observeProxyUsageRoles(frameEndInfo);\n                }\n'
assert block(layer, 'XrResult xrEndFrame').replace(observer, '') == block(base, 'XrResult xrEndFrame')
assert block(layer, 'XrResult xrEnumerateSwapchainImages') == block(base, 'XrResult xrEnumerateSwapchainImages')
wait = block(layer, 'XrResult xrWaitSwapchainImage')
assert 'const XrResult result = OpenXrApi::xrWaitSwapchainImage(swapchain, waitInfo);' in wait
assert wait.index('OpenXrApi::xrWaitSwapchainImage(swapchain, waitInfo)') < wait.index('tracker->second.waitSucceeded()')
assert 'result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING' in wait
assert 'result == XR_TIMEOUT_EXPIRED' in wait
assert 'chainWaitInfo.timeout = XR_INFINITE_DURATION;' in wait
assert 'return OpenXrApi::xrWaitSwapchainImage(swapchain, &chainWaitInfo);' in wait
print('PASS: EndFrame and enumeration unchanged; wait forwards original info before diagnostic state update')

startup = block(layer, 'if (isGraphicsNeutralApp())')
assert 'OXRTK_PROXY_USAGE_TEST' in startup and startup.rindex('m_proxyTimingMode = 0;') > startup.index('OXRTK_PROXY_USAGE_TEST')
assert 'return m_applicationName == "Impact" || m_applicationName == "TheMidnightWalk";' in layer
dispatch = block(layer[layer.index('XrResult xrReleaseSwapchainImage'):], 'if (m_proxyUsageEnabled)')
assert dispatch.index('releaseSubProxyImage(swapchain, "release", releaseInfo)') < dispatch.index('logProxyUsageRelease')
assert 'copySubProxyImage' not in dispatch and 'delayedRelease = true' not in dispatch
print('PASS: only two games, old timing mode overridden, post-release logging')

create = block(layer[layer.index('XrResult xrCreateSwapchain'):], 'if (m_gfxSubBisectMode == 2 || m_gfxSubBisectMode == 3)')
assert create.count('OpenXrApi::xrCreateSwapchain(') == 1
assert '&downstreamCreateInfo' in create and 'return result;' in block(create, 'if (XR_FAILED(result))')
assert 'usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT' in create
creation_loop = block(create, 'for (uint32_t i = 0; i < imageCount; ++i)')
if 'm_proxyScopeEnabled' in create:
    # Scope deliberately changes selection; the private resource recipe stays identical.
    creation_loop = creation_loop.replace('if (!hasProxy)', 'if (isDepth)')
    assert 'm_subProxySwapchains.insert_or_assign(*swapchain, hasProxy)' in create
else:
    assert 'm_subProxySwapchains.insert_or_assign(*swapchain, !isDepth)' in create
assert creation_loop == block(base[base.index('XrResult xrCreateSwapchain'):], 'for (uint32_t i = 0; i < imageCount; ++i)')
policy = block(header, 'inline XrSwapchainCreateInfo DownstreamCreateInfo')
assert 'auto downstream = original;' in policy
assert 'hasProxy' in policy and 'XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT' in policy
assert 'downstream.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;' in policy
assert policy.count('downstream.') == 1
print('PASS: legacy usage policy, no retry and private proxy recipe unchanged; scope selection tested separately')

for marker in ('void observeProxyUsageRoles', 'void observeProxyUsageRole'):
    method = block(layer, marker)
    assert all(token not in method for token in ('CopyResource(', 'pushState(', 'releaseSub', 'OpenXrApi::', 'm_subProxySwapchains', 'appTexture ='))
assert 'm_proxyUsageRunStrict = m_proxyUsageReferenceValid;' in layer
assert 'baseline.erase("observedRoles")' in layer and 'total_swapchain_creation_count' in layer
assert 'native_CopyResource_precondition_mismatch' in layer
assert 'heap=unavailable' in layer and 'plane_count=unavailable' in layer
assert 'states=wrapper_tracked_logical' in layer
assert 'strict_comparison=0' in layer and 'proxy_usage::Compare' in layer
assert 'diag/proxy-transfer-dst-bisect' in workflow and 'OpenXR-Toolkit-PSVR2-ProxyUsageDiag-x64' in workflow
assert 'python tests/proxy_usage_source.py' in workflow and 'fetch-depth: 0' in workflow
assert all(s in doc for s in ('U0:', 'U1:', 'strict_comparison=0', 'strict_comparison=1'))
print('PASS: observation only, conservative reference comparison, resources, artifact and guide')
