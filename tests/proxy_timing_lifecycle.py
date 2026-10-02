"""Compile production layer helpers/dispatch branches with mock OpenXR/D3D12.
Checks behavior, error retries and exact Mode 0 versus the published base.
This is a native x64 harness, not a Windows DLL or real headset validation.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def block(src, marker):
    start = src.index(marker)
    opening = src.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]
layer = (ROOT / 'XR_APILAYER_MBUCCHIA_toolkit/layer.cpp').read_text()
interfaces = (ROOT / 'XR_APILAYER_MBUCCHIA_toolkit/interfaces.h').read_text()
base = subprocess.check_output(['git','show','acd28ea578b4171c1ed1fd01eab034513b244431:XR_APILAYER_MBUCCHIA_toolkit/layer.cpp'],cwd=ROOT,text=True)
methods = '\n'.join(block(layer, m) for m in (
    'graphics::ProxySyncSubmission copySubProxyImage',
    'void logProxyTimingCopy', 'XrResult releaseSubProxyImage'))
app_release = block(layer[layer.index('XrResult xrReleaseSwapchainImage'):], 'if (m_proxyTimingEnabled)')
endframe = block(layer[layer.index('XrResult xrEndFrame'):], 'if (m_proxyTimingEnabled && m_proxyTimingMode > 0)')
structs = '\n'.join(block(interfaces, 'struct '+n)+';' for n in ('ProxySyncReuse','ProxySyncSubmission'))
record = block(layer, 'struct ProxyTimingCopy')+';'
harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
using ULONGLONG = uint64_t;
using D3D12_RESOURCE_STATES = unsigned;
constexpr unsigned D3D12_RESOURCE_STATE_COMMON=0, D3D12_RESOURCE_STATE_COPY_SOURCE=1, D3D12_RESOURCE_STATE_COPY_DEST=2;
using XrSwapchain = uintptr_t;
using XrResult = int;
constexpr int XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO=1, XR_SUCCESS=0, XR_ERROR_CALL_ORDER_INVALID=-1;
struct XrSwapchainImageReleaseInfo { int type; const void* next=nullptr; };
#define XR_SUCCEEDED(x) ((x)>=0)
#define CHECK_XRCMD(x) do { if ((x)<0) throw std::runtime_error("XR failure"); } while(0)
std::vector<std::string> events;
bool failRelease=false, failFlush=false;
uint64_t tick=0;
uint64_t GetTickCount64() { return ++tick; }
template<class... Args> void Log(const char*, Args...) {events.push_back("log");}
namespace xr { const char* ToCString(XrResult) { return "result"; } }
namespace graphics {
STRUCTS
struct D3D12 {};
}
struct Texture {
    unsigned state=0;
    std::vector<unsigned> stack;
    D3D12_RESOURCE_STATES getTrackedStateForDiagnostics() const {return state;}
    void* getNativePtr() const {return (void*)this;}
    template<class T> void* getAs() {return this;}
    void pushState(unsigned s) {stack.push_back(state);state=s;events.push_back("push"+std::to_string(s));}
    void popState() {state=stack.back();stack.pop_back();events.push_back("pop");}
};
struct Context {void CopyResource(void*,void*) {events.push_back("copy");}};
struct Device {
    Context context;
    template<class T> Context* getContextAs() {return &context;}
    graphics::ProxySyncSubmission flushProxySyncCopy() {
        events.push_back("allocator_safe");events.push_back("execute");events.push_back("signal");
        events.push_back("wait_current");
        if(failFlush) throw std::runtime_error("fence failed");
        graphics::ProxySyncSubmission s;s.submissionFence=1;s.completedAfterCopyWait=1;s.copyWaited=true;return s;
    }
    void flushContext() {events.push_back("flush");}
};
struct Image {std::shared_ptr<Texture> appTexture=std::make_shared<Texture>(),runtimeTexture=std::make_shared<Texture>();};
struct State {unsigned acquiredImageIndex=0,requestedArraySize=2;bool delayedRelease=false;std::vector<Image> images=std::vector<Image>(3);};
struct OpenXrApi {
    static XrResult xrReleaseSwapchainImage(XrSwapchain s,const XrSwapchainImageReleaseInfo*) {
        events.push_back("release"+std::to_string(s));return failRelease ? -2 : 0;
    }
};
struct Fixture {
    std::string m_applicationName="Impact";
    uint32_t m_proxyTimingMode=0,m_proxySyncMode=2,m_gfxSubBisectMode=2;
    bool m_proxyTimingEnabled=true,m_proxySyncEnabled=true;
    std::atomic<uint32_t> m_proxyTimingLogs{0},m_subReleaseLogs{0},m_proxySyncReleaseLogs{0};
    static constexpr unsigned MetroGfxLogSamples=128;
    std::shared_ptr<Device> m_bisectDevice=std::make_shared<Device>();
    std::map<XrSwapchain,State> m_swapchains;
    std::map<XrSwapchain,bool> m_subProxySwapchains,m_subProxyCopyCompleted;
    RECORD
    std::map<XrSwapchain,ProxyTimingCopy> m_proxyTimingCopies;
    std::vector<XrSwapchain> m_proxyTimingPendingOrder;
    uint64_t m_proxyTimingFrame=0;
    Fixture() { m_swapchains[9];m_swapchains[2];m_subProxySwapchains[9]=true;m_subProxySwapchains[2]=false; }
    METHODS
    BASEMETHOD
    XrResult appRelease(XrSwapchain swapchain) {
        XrSwapchainImageReleaseInfo info{1};const auto* releaseInfo=&info;
        auto swapchainIt=m_swapchains.find(swapchain);
        APPRELEASE
        return -3;
    }
    void endFrame() {ENDFRAME events.push_back("endFrame");}
};
std::vector<std::string> visibleEvents() {
    auto e=events;e.erase(std::remove(e.begin(),e.end(),"log"),e.end());return e;
}
int count(const std::string& e) {return std::count(events.begin(),events.end(),e);}
int main() {
    Fixture baseline;
    events.clear();baseline.baseRelease(9,"release");baseline.baseRelease(2,"release");
    const auto expected=visibleEvents();
    for(unsigned mode=0;mode<3;mode++) {
        for(bool depthFirst : {false,true}) {
            Fixture f;f.m_proxyTimingMode=mode;events.clear();
            const auto first=depthFirst?2:9,second=depthFirst?9:2;
            assert(f.appRelease(first)==0);assert(f.appRelease(second)==0);
            if(mode==0) {
                if(!depthFirst) assert(visibleEvents()==expected);
                const auto wait=std::find(events.begin(),events.end(),"wait_current");
                assert(wait!=events.end() && *(wait+1)=="release9"); // no hidden pre-release log
            } else {
                assert(count("release9")==0 && count("release2")==0);
                assert(count("copy")== (mode==1?1:0));
                assert(f.appRelease(9)==XR_ERROR_CALL_ORDER_INVALID);
            }
            f.endFrame();
            assert(count("copy")==1 && count("wait_current")==1);
            assert(count("release9")==1 && count("release2")==1);
            const auto r1=std::find(events.begin(),events.end(),"release"+std::to_string(first));
            const auto r2=std::find(events.begin(),events.end(),"release"+std::to_string(second));
            assert(r1<r2 && r2<std::find(events.begin(),events.end(),"endFrame"));
            assert(f.m_proxyTimingPendingOrder.empty());
            assert(f.m_swapchains[9].images[0].appTexture->state==0);
            assert(f.m_swapchains[9].images[0].runtimeTexture->state==0);
            events.clear();f.endFrame();assert(count("copy")==0 && count("release9")==0);
        }
        std::cout<<"Mode "<<mode<<": copy/wait/release points, both color/depth orders PASS\n";
    }
    for(unsigned mode : {1,2}) {
        Fixture f;f.m_proxyTimingMode=mode;events.clear();f.appRelease(9);
        failRelease=true;try{f.endFrame();assert(false);}catch(const std::runtime_error&){}
        assert(f.m_swapchains[9].delayedRelease && f.m_subProxyCopyCompleted[9]);
        failRelease=false;f.endFrame();assert(count("copy")==1 && count("wait_current")==1);
        assert(f.m_proxyTimingPendingOrder.empty());
        // Explicit early-reacquire safety path must not leave stale pending entries or recopy.
        events.clear();f.appRelease(9);f.releaseSubProxyImage(9,"acquire");f.endFrame();
        assert(count("copy")==1 && count("release9")==1 && f.m_proxyTimingPendingOrder.empty());
    }
    Fixture failed;failed.m_proxyTimingMode=2;events.clear();failed.appRelease(9);failFlush=true;
    try{failed.endFrame();assert(false);}catch(const std::runtime_error&){}
    assert(count("release9")==0 && failed.m_swapchains[9].delayedRelease);
    assert(!failed.m_subProxyCopyCompleted[9]);
    std::cout<<"Mode 0 exact base GPU/runtime order PASS\nRetry without recopy, forced reacquire and fence failure PASS\n";
}
'''
harness = harness.replace('STRUCTS', structs).replace('RECORD',record).replace('METHODS',methods)
harness = harness.replace('BASEMETHOD',block(base,'XrResult releaseSubProxyImage').replace('releaseSubProxyImage','baseRelease',1))
harness = harness.replace('APPRELEASE',app_release).replace('ENDFRAME',endframe)
with tempfile.TemporaryDirectory(prefix='proxy-timing-test-') as d:
    src=Path(d)/'test.cpp';src.write_text(harness)
    exe=Path(d)/'test'
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
