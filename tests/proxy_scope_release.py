#!/usr/bin/env python3
"""Compile actual app release, unchanged GPU helpers and post-release scope observer.
Check direct paths bypass GPU work and retained proxies preserve the base order.
"""
import ast
from pathlib import Path
import subprocess
import tempfile
from proxy_usage_source import block

ROOT=Path(__file__).resolve().parents[1]
layer=(ROOT/'XR_APILAYER_MBUCCHIA_toolkit/layer.cpp').read_text()
interfaces=(ROOT/'XR_APILAYER_MBUCCHIA_toolkit/interfaces.h').read_text()
base=subprocess.check_output(['git','show','70af55571d6665abb25cff4e543e504628263391:XR_APILAYER_MBUCCHIA_toolkit/layer.cpp'],cwd=ROOT,text=True)
tree=ast.parse((ROOT/'tests/proxy_timing_lifecycle.py').read_text())
harness=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
             and any(isinstance(t,ast.Name) and t.id=='harness' for t in n.targets))
harness=harness[:harness.index('int main()')]
harness=harness.replace('#include <stdexcept>', '''#include <stdexcept>
#include <set>
#include <sstream>
#define XR_FAILED(x) ((x)<0)
using XrSession=uintptr_t;
using ID3D12CommandQueue=void;
constexpr uintptr_t XR_NULL_HANDLE=0;
constexpr uint64_t XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT=2,XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT=16;
struct XrSwapchainCreateInfo {const void* next=nullptr;uint64_t createFlags=0,usageFlags=0;
    int64_t format=90;unsigned sampleCount=1,width=4128,height=2208,faceCount=1,arraySize=1,mipCount=1;};
#include "proxy_scope_diagnostic.h"
using namespace toolkit;
''')
harness=harness.replace('struct Device {', 'struct Device {\n    void* queue=(void*)0x123; bool badCompletion=false;')
harness=harness.replace('s.copyWaited=true;return s;', 's.copyWaited=true;s.queue=queue;s.bindingQueue=queue;if(badCompletion)s.completedAfterCopyWait=0;return s;')
harness=harness.replace('struct Fixture {', block(layer,'struct ProxyUsageState')+';\nstruct Fixture {')
harness=harness.replace('    RECORD', '''    bool m_proxyScopeEnabled=true,m_proxyScopeStrict=true;
    unsigned m_proxyScopeMode=0;
    std::string m_proxyScopeReason;
    ID3D12CommandQueue* m_proxyScopeQueue=(void*)0x123;
    std::map<uint64_t,proxy_scope::Decision> m_proxyScopeDecisions;
    std::map<uint64_t,uint32_t> m_proxyScopeReleaseLogs;
    bool m_proxyUsageEnabled=true,m_proxyUsageRunStrict=true;
    unsigned m_proxyUsageMode=0;
    uint64_t m_proxyUsageFrame=0;
    std::map<XrSwapchain,ProxyUsageState> m_proxyUsageSwapchains;
    RECORD''')
harness=harness.replace('    METHODS', '\n'.join(block(layer,m) for m in (
    'void invalidateProxyScope','void logProxyScopeRelease','void logProxyUsageRelease'))+'\n    METHODS')
harness=harness.replace('STRUCTS','\n'.join(block(interfaces,'struct '+n)+';' for n in ('ProxySyncReuse','ProxySyncSubmission')))
harness=harness.replace('RECORD',block(layer,'struct ProxyTimingCopy')+';')
harness=harness.replace('METHODS','\n'.join(block(layer,m) for m in (
    'graphics::ProxySyncSubmission copySubProxyImage','void logProxyTimingCopy','XrResult releaseSubProxyImage')))
harness=harness.replace('BASEMETHOD',block(base,'XrResult releaseSubProxyImage').replace('releaseSubProxyImage','baseRelease',1))
harness=harness.replace('APPRELEASE',block(layer[layer.index('XrResult xrReleaseSwapchainImage'):],'if (m_proxyUsageEnabled)'))
harness=harness.replace('ENDFRAME',block(layer[layer.index('XrResult xrEndFrame'):],'if (m_proxyTimingEnabled && m_proxyTimingMode > 0)'))
harness+=r'''
void setup(Fixture& f,unsigned mode) {
    f.m_proxyScopeMode=mode;
    for(unsigned i=1;i<=4;i++) {
        f.m_swapchains[i];
        const bool proxy=i!=2 && (mode==0 || (mode==1 && i==1) || (mode==2 && i>=3));
        f.m_subProxySwapchains[i]=proxy;
        auto& id=f.m_proxyUsageSwapchains[i];id.id=i;id.hasProxy=proxy;id.strict=true;
        auto& d=f.m_proxyScopeDecisions[i];d.hasProxy=proxy;d.strict=true;d.referenceRole=i==1?1:i==2?0:2;
        if(!proxy) for(auto& image : f.m_swapchains[i].images) image.appTexture=image.runtimeTexture;
    }
}
int main() {
    for(unsigned mode : {0,1,2}) for(bool depthFirst : {false,true}) {
        const std::vector<unsigned> order=depthFirst?std::vector<unsigned>{2,1,3,4}:std::vector<unsigned>{1,2,3,4};
        Fixture baseline;setup(baseline,mode);events.clear();
        for(auto sc : order) baseline.baseRelease(sc,"release");
        const auto expected=visibleEvents();
        Fixture f;setup(f,mode);events.clear();
        for(auto sc : order) {
            const auto copies=count("copy"),waits=count("wait_current");
            assert(f.appRelease(sc)==0);
            const bool proxy=f.m_subProxySwapchains.at(sc);
            assert(count("copy")==copies+(proxy?1:0) && count("wait_current")==waits+(proxy?1:0));
            if(proxy) {
                auto wait=std::find(events.rbegin(),events.rend(),"wait_current").base();
                assert(wait!=events.end() && *wait=="release"+std::to_string(sc));
            }
        }
        assert(visibleEvents()==expected && f.m_proxyScopeStrict);
        const auto expectedCopies=mode==0?3:mode==1?1:2;
        assert(count("copy")==expectedCopies && count("wait_current")==expectedCopies);
        f.endFrame();assert(count("copy")==expectedCopies && f.m_proxyTimingPendingOrder.empty());
        for(auto sc : order) assert(count("release"+std::to_string(sc))==1);
        std::cout<<"PASS: scope "<<mode<<" direct has no GPU work; proxies use unchanged base copy/wait/release; depthFirst="<<depthFirst<<"\n";
    }
    Fixture failure;setup(failure,0);events.clear();failRelease=true;
    assert(failure.appRelease(1)<0 && !failure.m_proxyScopeStrict);
    failRelease=false;assert(failure.appRelease(1)==0 && count("copy")==1);
    Fixture fence;setup(fence,0);events.clear();failFlush=true;
    try {fence.appRelease(1);assert(false);}catch(const std::runtime_error&){}
    assert(count("release1")==0);failFlush=false;
    for(bool badQueue : {false,true}) {
        Fixture f;setup(f,1);events.clear();
        if(badQueue)f.m_bisectDevice->queue=(void*)0x456;else f.m_bisectDevice->badCompletion=true;
        f.appRelease(1);assert(!f.m_proxyScopeStrict && f.m_proxyScopeReason=="queue_or_copy_completion_mismatch");
    }
    Fixture direct;setup(direct,2);events.clear();direct.m_bisectDevice->badCompletion=true;
    assert(direct.appRelease(1)==0 && direct.m_proxyScopeStrict && count("copy")==0 && count("signal")==0);
    std::cout<<"PASS: failed runtime release invalidates, retry never recopies, fence failure stops release; queue/completion verified only on retained proxies\n";
}
'''
with tempfile.TemporaryDirectory(prefix='proxy-scope-release-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-I',str(ROOT/'XR_APILAYER_MBUCCHIA_toolkit'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
