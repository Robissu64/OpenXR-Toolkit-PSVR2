#!/usr/bin/env python3
"""Guard the index experiment's scope and ordering without changing GPU helpers."""
from pathlib import Path
import subprocess
from proxy_usage_source import block

root = Path(__file__).resolve().parents[1]
layer = (root / 'XR_APILAYER_MBUCCHIA_toolkit/layer.cpp').read_text()
base = subprocess.check_output(['git', 'show',
    'a55ddfe226383f05c1cb22777c286ca8b0880a3c:XR_APILAYER_MBUCCHIA_toolkit/layer.cpp'],
    cwd=root, text=True)
for marker in ('graphics::ProxySyncSubmission copySubProxyImage',
               'void logProxyTimingCopy', 'XrResult releaseSubProxyImage'):
    assert block(layer, marker) == block(base, marker), marker

startup = block(layer, 'if (isGraphicsNeutralApp())')
assert 'OXRTK_PROXY_INDEX_TEST' in startup
assert "indexText[0] == '1' ? 1u : 0u" in startup
assert 'm_proxyIndexEnabled = m_proxyScopeMode == 1;' in startup
assert 'm_proxyIndexEnabled && hasProxy && scope.strict && scope.referenceRole == 1' in layer

acquire = block(layer, 'XrResult xrAcquireSwapchainImage')
assert acquire.index('OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);',
    acquire.index('auto swapchainIt = m_swapchains.find(swapchain)')) < acquire.index('tracker->second.acquire(*index')
assert acquire.index('tracker->second.acquire(*index') < acquire.index(
    '} else if (XR_FAILED(result) && m_proxyIndexTrackers.count(swapchain))')
assert 'invalidateProxyIndex("downstream_acquire_failed");' in acquire
assert acquire.rindex('return result;') > acquire.index('invalidateProxyIndex("downstream_acquire_failed");')
acquire_failure = acquire[acquire.index('} else if (XR_FAILED(result) && m_proxyIndexTrackers.count(swapchain))'):]
assert 'tracker->second.acquire(' not in acquire_failure  # failed acquire leaves the queue intact
wait = block(layer, 'XrResult xrWaitSwapchainImage')
assert wait.index('OpenXrApi::xrWaitSwapchainImage(swapchain, waitInfo);') < wait.index('tracker->second.waitSucceeded()')
assert 'result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING' in wait
assert 'result == XR_TIMEOUT_EXPIRED' in wait
assert wait.index('tracker->second.waitSucceeded()') < wait.index('result == XR_TIMEOUT_EXPIRED') < wait.index(
    'invalidateProxyIndex("downstream_wait_failed");')
assert 'else if (XR_FAILED(result))' in wait
assert wait.index('invalidateProxyIndex("downstream_wait_failed");') < wait.index('return result;')
timeout_branch = wait[wait.index('} else if (result == XR_TIMEOUT_EXPIRED)'):wait.index('} else if (XR_FAILED(result))')]
assert 'waitSucceeded()' not in timeout_branch  # timeout keeps the same wait target
wait_failure = wait[wait.index('} else if (XR_FAILED(result))'):wait.index('if (m_proxyIndexWaitLogs++')]
assert 'waitSucceeded()' not in wait_failure  # failed wait leaves the queue intact

release = block(layer, 'XrResult xrReleaseSwapchainImage')
selection = release.index('swapchainIt->second.acquiredImageIndex = selectedCopyIndex;')
copy = release.index('releaseSubProxyImage(swapchain, "release", releaseInfo)', selection)
pop = release.index('tracker.releaseSucceeded()', copy)
assert selection < copy < pop
restore = release.index('swapchainIt->second.acquiredImageIndex = legacyIndex;', copy)
assert copy < release.index('logProxyUsageRelease(swapchain, result)', copy) < restore < pop
assert 'if (!validIndex)' in release[selection - 600:selection]
assert 'if (XR_SUCCEEDED(result) && validIndex)' in release
assert release.index('if (validIndex && XR_FAILED(result))') < release.index(
    'if (XR_SUCCEEDED(result) && validIndex)') < pop
assert 'invalidateProxyIndex("downstream_release_failed");' in release
assert release.rindex('return result;') > release.index('invalidateProxyIndex("downstream_release_failed");')
release_failure = release[release.index('if (validIndex && XR_FAILED(result))'):
                          release.index('if (XR_SUCCEEDED(result) && validIndex)')]
assert 'tracker.releaseSucceeded()' not in release_failure  # failed release keeps FIFO front
assert 'selectedCopyIndex = tracker.selectedCopyIndex(m_proxyIndexMode)' in release
assert '++m_proxyIndexLegacyMismatchCount' in release
assert 'logProxyIndexRelease' in release
invalidate = block(layer, 'void invalidateProxyIndex')
assert '++m_proxyIndexFifoErrors;' in invalidate and 'm_proxyIndexStrict = false;' in invalidate

destroy = block(layer, 'XrResult xrDestroySwapchain')
finish = block(layer, 'void finishProxyIndexSession')
assert 'm_proxyIndexPendingAtDestroy += pending;' in destroy
assert 'invalidateProxyIndex' not in destroy
assert 'pendingAtSessionEnd += tracker.second.queue.size();' in finish
assert 'invalidateProxyIndex' not in finish
assert 'pending_at_session_end=%zu' in finish and 'pending_at_destroy=%llu' in finish

workflow = (root / '.github/workflows/psvr2-diagnostic.yml').read_text()
assert 'diag/proxy-index-bisect' in workflow
assert 'tests/proxy_index_source.py' in workflow
assert 'tests/proxy_index_diagnostic.cpp' in workflow
assert 'OpenXR-Toolkit-PSVR2-ProxyIndexDiag-x64' in workflow
assert "docs/PROXY_INDEX_TESTING.txt' -Destination" in workflow
assert workflow.index('python tests/proxy_index_source.py') < workflow.index('Build unsigned x64 layer')

assert block(layer, 'XrResult xrEndFrame') == block(base, 'XrResult xrEndFrame')
assert block(layer, 'XrResult xrCreateSwapchain') != block(base, 'XrResult xrCreateSwapchain')
print('PASS: scope 1 only, unchanged GPU helpers/EndFrame, FIFO selection/restoration, downstream failures invalidate without queue advancement, observational teardown, CI and packaging')
