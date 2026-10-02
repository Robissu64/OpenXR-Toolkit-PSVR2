#!/usr/bin/env python3
"""Scope invariants for Windows Actions (no native mock compiler required)."""
from pathlib import Path
import subprocess
from proxy_usage_source import block

ROOT=Path(__file__).resolve().parents[1]
relative='XR_APILAYER_MBUCCHIA_toolkit/'
layer=(ROOT/relative/'layer.cpp').read_text()
header=(ROOT/relative/'proxy_scope_diagnostic.h').read_text()
# The three GPU helpers are already checked against 70af555 by proxy_usage_source.
# That published ancestor is also used when an exported patch has new committer metadata.
base=subprocess.check_output(['git','show','70af55571d6665abb25cff4e543e504628263391:'+relative+'layer.cpp'],cwd=ROOT,text=True)
assert 'OXRTK_PROXY_SCOPE_TEST' in layer
startup=block(layer,'if (isGraphicsNeutralApp())')
assert startup.index('m_proxyScopeEnabled = true;') < startup.index('m_proxyUsageMode = 0;')
assert 'm_proxyTimingMode = 0;' in startup and 'm_proxySyncMode = 2;' in startup
create=block(layer[layer.index('XrResult xrCreateSwapchain'):], 'if (m_gfxSubBisectMode == 2 || m_gfxSubBisectMode == 3)')
assert 'm_proxyScopeEnabled ? 0 :' in create
assert 'const bool hasProxy = !isDepth && (!m_proxyScopeEnabled || scope.hasProxy);' in create
assert 'if (!hasProxy)' in create and 'images.appTexture = images.runtimeTexture;' in create
assert 'm_subProxySwapchains.insert_or_assign(*swapchain, hasProxy)' in create
assert create.count('OpenXrApi::xrCreateSwapchain(')==1
selection=block(header,'inline Decision Select')
assert 'for (const auto& field : Signature(info))' in selection
assert 'role->second == "1"' in selection and 'role->second == "2"' in selection
assert 'info.width ==' not in selection and 'info.height ==' not in selection
assert all(k in block(header,'inline proxy_usage::Snapshot Signature') for k in ('width','height','arraySize','faceCount','mipCount','sampleCount','requestedFormat','createFlags','originalUsage'))
assert 'm_proxyUsageCreationId + 1' in block(layer,'proxy_scope::Decision selectProxyScope')
print('PASS: original usage, fixed timing/sync, all-field signature, persisted-role selection and direct native resources')

write=block(layer,'void writeProxyUsageReference')
assert write.index('if (m_proxyScopeEnabled) return;') < write.index('std::ofstream')
begin=block(layer,'void beginProxyUsageSession')
assert 'm_proxyUsageMode == 1 || m_proxyScopeEnabled' in begin
for marker in ('void validateProxyScopeRole','void observeProxyUsageRole','void observeProxyUsageRoles'):
    method=block(layer,marker)
    assert all(token not in method for token in ('hasProxy =','appTexture =','CopyResource(', 'pushState(', 'releaseSub', 'm_subProxySwapchains'))
assert 'invalidateProxyScope("role_mismatch")' in layer
assert 'm_proxyUsageReference.swapchains.size() != m_proxyUsageCurrent.swapchains.size()' in layer
compare=block(header,'inline std::string CompareResources')
assert 'hasProxy && field.first.find(".proxy.")' in compare
assert 'field.first.find(".runtime.")' in compare
assert 'if (!check(key)) return key;' in compare
print('PASS: U0 reference read-only in all modes, no dynamic selection, available runtime invariants and final role/count validation')

release=block(layer[layer.index('XrResult xrReleaseSwapchainImage'):], 'if (m_proxyUsageEnabled)')
assert release.index('releaseSubProxyImage(swapchain, "release", releaseInfo)') < release.index('logProxyScopeRelease')
for marker in ('graphics::ProxySyncSubmission copySubProxyImage','XrResult releaseSubProxyImage','void logProxyTimingCopy'):
    assert block(layer,marker)==block(base,marker)
assert 'if (!hasProxy)' in block(layer,'graphics::ProxySyncSubmission copySubProxyImage')
assert 'if (hasProxy && !m_subProxyCopyCompleted[swapchain])' in block(layer,'XrResult releaseSubProxyImage')
print('PASS: direct path never copies; proxy path, barriers/fence/completion/release unchanged; new logs after release')

workflow=(ROOT/'.github/workflows/psvr2-diagnostic.yml').read_text()
doc=(ROOT/'docs/PROXY_SCOPE_TESTING.txt').read_text()
assert all(s in workflow for s in ('diag/proxy-scope-bisect','OpenXR-Toolkit-PSVR2-ProxyScopeDiag-x64','python tests/proxy_scope_source.py','fetch-depth: 0'))
assert 'needs: validate_scope_native' in workflow
assert all(s in workflow for s in ('tests/proxy_scope_diagnostic.py','tests/proxy_scope_release.py','tests/proxy_sync_ring.py','tests/proxy_usage_diagnostic.py'))
assert all(s in doc for s in ('0 = C','1 = A','2 = B','DO NOT DELETE','unavailable_in_reference','strict_scope_comparison=0'))
print('PASS: workflow and test guide; runtime descriptor/heap reference coverage explicitly limited')
