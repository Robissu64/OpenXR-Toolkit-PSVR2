#!/usr/bin/env python3
"""Compile actual U0/U1 release dispatch plus actual post-release observer.
Reuse the existing mocks, not an independent reimplementation of the GPU path.
"""
import ast
from pathlib import Path
import subprocess
import tempfile
from proxy_usage_source import block

ROOT = Path(__file__).resolve().parents[1]
layer = (ROOT/'XR_APILAYER_MBUCCHIA_toolkit/layer.cpp').read_text()
interfaces = (ROOT/'XR_APILAYER_MBUCCHIA_toolkit/interfaces.h').read_text()
base = subprocess.check_output(['git','show','70af55571d6665abb25cff4e543e504628263391:XR_APILAYER_MBUCCHIA_toolkit/layer.cpp'],cwd=ROOT,text=True)
tree = ast.parse((ROOT/'tests/proxy_timing_lifecycle.py').read_text())
harness = next(ast.literal_eval(node.value) for node in tree.body if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='harness' for t in node.targets))
harness = harness[:harness.index('int main()')]
harness = harness.replace('#include <stdexcept>','#include <stdexcept>\n#include <set>\n#define XR_FAILED(x) ((x)<0)\nusing XrSession=uintptr_t;\nconstexpr uintptr_t XR_NULL_HANDLE=0;')
harness = harness.replace('struct Fixture {', block(layer,'struct ProxyUsageState')+';\nstruct Fixture {')
harness = harness.replace('    RECORD', '''    bool m_proxyScopeEnabled=false;
    void logProxyScopeRelease(XrSwapchain,XrResult,bool) {}
    bool m_proxyUsageEnabled=true,m_proxyUsageRunStrict=true;
    unsigned m_proxyUsageMode=0;
    uint64_t m_proxyUsageFrame=0;
    std::map<XrSwapchain,ProxyUsageState> m_proxyUsageSwapchains;
    RECORD''')
harness = harness.replace('    METHODS', block(layer,'void logProxyUsageRelease')+'\n    METHODS')
harness = harness.replace('STRUCTS','\n'.join(block(interfaces,'struct '+name)+';' for name in ('ProxySyncReuse','ProxySyncSubmission')))
harness = harness.replace('RECORD',block(layer,'struct ProxyTimingCopy')+';')
harness = harness.replace('METHODS','\n'.join(block(layer,m) for m in ('graphics::ProxySyncSubmission copySubProxyImage','void logProxyTimingCopy','XrResult releaseSubProxyImage')))
harness = harness.replace('BASEMETHOD',block(base,'XrResult releaseSubProxyImage').replace('releaseSubProxyImage','baseRelease',1))
harness = harness.replace('APPRELEASE',block(layer[layer.index('XrResult xrReleaseSwapchainImage'):], 'if (m_proxyUsageEnabled)'))
harness = harness.replace('ENDFRAME',block(layer[layer.index('XrResult xrEndFrame'):], 'if (m_proxyTimingEnabled && m_proxyTimingMode > 0)'))
harness += r'''
int main() {
    for(unsigned mode : {0,1}) for(bool depthFirst : {false,true}) {
        Fixture base;events.clear();
        const auto first=depthFirst?2:9,second=depthFirst?9:2;
        base.baseRelease(first,"release");base.baseRelease(second,"release");
        const auto expected=visibleEvents();
        Fixture f;f.m_proxyUsageMode=mode;
        f.m_proxyUsageSwapchains[9].hasProxy=true;f.m_proxyUsageSwapchains[9].strict=true;
        f.m_proxyUsageSwapchains[2].strict=true;
        events.clear();assert(f.appRelease(first)==0 && f.appRelease(second)==0);
        assert(visibleEvents()==expected);
        const auto wait=std::find(events.begin(),events.end(),"wait_current");
        assert(wait!=events.end() && *(wait+1)=="release9");
        assert(f.m_proxyUsageSwapchains[9].releases==1 && f.m_proxyUsageSwapchains[2].releases==1);
        assert(f.m_proxyUsageRunStrict && f.m_proxyUsageSwapchains[9].strict);
        f.endFrame();assert(count("copy")==1 && count("release9")==1 && count("release2")==1);
        assert(f.m_proxyTimingPendingOrder.empty());
        std::cout<<"PASS: U"<<mode<<" GPU/release order equals timing Mode 0; no I/O between completion/release; depthFirst="<<depthFirst<<"\n";
    }
    Fixture failed;failed.m_proxyUsageSwapchains[9].hasProxy=true;failed.m_proxyUsageSwapchains[9].strict=true;
    events.clear();failRelease=true;assert(failed.appRelease(9)<0);
    assert(!failed.m_proxyUsageSwapchains[9].strict && !failed.m_proxyUsageRunStrict);
    failRelease=false;assert(failed.appRelease(9)==0);assert(count("copy")==1);
    Fixture fence;fence.m_proxyUsageSwapchains[9].hasProxy=true;
    events.clear();failFlush=true;
    try {fence.appRelease(9);assert(false);}catch(const std::runtime_error&){}
    assert(count("release9")==0);
    std::cout<<"PASS: downstream failure invalidates strict comparison, retry does not recopy, fence failure prevents release\n";
}
'''
with tempfile.TemporaryDirectory(prefix='proxy-usage-release-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
