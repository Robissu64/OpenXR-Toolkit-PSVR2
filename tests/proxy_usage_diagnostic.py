#!/usr/bin/env python3
"""Native x64 mocks compile the production policy, resource queries and role observer.
No Windows DLL/hardware validation is claimed. Requires g++ on Linux.
"""
from pathlib import Path
import subprocess
import tempfile
from proxy_usage_source import block

ROOT = Path(__file__).resolve().parents[1]
layer = (ROOT / 'XR_APILAYER_MBUCCHIA_toolkit/layer.cpp').read_text()
methods = '\n'.join(block(layer, marker) for marker in (
    'template <typename T>\n        static void usageField',
    'void writeProxyUsageReference', 'void beginProxyUsageSession',
    'bool captureProxyUsageResource', 'void recordProxyUsageCreation',
    'static const char* proxyUsageRole', 'void observeProxyUsageRole(',
    'void observeProxyUsageRoles', 'void summarizeProxyUsage', 'void finishProxyUsageSession'))
record = block(layer, 'struct ProxyUsageState') + ';'
create = block(layer[layer.index('XrResult xrCreateSwapchain'):], 'if (m_gfxSubBisectMode == 2 || m_gfxSubBisectMode == 3)')
create_dispatch = create[create.index('const bool isDepth'):create.index('uint32_t imageCount = 0;')]
harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using XrSession=uintptr_t; using XrSwapchain=uintptr_t; using XrResult=int;
constexpr uintptr_t XR_NULL_HANDLE=0;
constexpr uint64_t XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT=2, XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT=16;
struct XrSwapchainCreateInfo {
    int type=1; const void* next=nullptr; uint64_t createFlags=0, usageFlags=0;
    int64_t format=91; uint32_t sampleCount=1,width=4128,height=2208,faceCount=1,arraySize=1,mipCount=1;
};
#include "proxy_usage_diagnostic.h"
#include "proxy_scope_diagnostic.h"
using namespace toolkit;
using ID3D12CommandQueue=void;
using D3D12_RESOURCE_STATES=unsigned;
constexpr unsigned D3D12_RESOURCE_STATE_COPY_SOURCE=0x800,D3D12_RESOURCE_STATE_COPY_DEST=0x400;
using D3D12_HEAP_FLAGS=unsigned;
struct D3D12_HEAP_PROPERTIES {unsigned Type=1,CPUPageProperty=0,MemoryPoolPreference=0,CreationNodeMask=1,VisibleNodeMask=1;};
struct Desc {
    unsigned Dimension=3; uint64_t Alignment=0,Width=4128; unsigned Height=2208;
    unsigned DepthOrArraySize=1,MipLevels=1,Format=90;
    struct {unsigned Count=1,Quality=0;} SampleDesc;
    unsigned Layout=0,Flags=1;
};
struct D3D12_FEATURE_DATA_FORMAT_INFO {unsigned Format; unsigned char PlaneCount;};
constexpr unsigned D3D12_FEATURE_FORMAT_INFO=1;
#define SUCCEEDED(hr) ((hr)>=0)
#define XR_FAILED(hr) ((hr)<0)
#define IID_PPV_ARGS(x) x
struct ID3D12Device {
    struct Luid {int HighPart=1;unsigned LowPart=2;};
    Luid GetAdapterLuid() {return {};}
    int hr=0;
    int CheckFeatureSupport(unsigned, D3D12_FEATURE_DATA_FORMAT_INFO* out,size_t) {out->PlaneCount=1;return hr;}
};
struct ID3D12Resource {
    Desc desc; D3D12_HEAP_PROPERTIES heap; unsigned heapFlags=2; int heapHr=0,deviceHr=0;
    ID3D12Device device;
    Desc GetDesc() const {return desc;}
    int GetHeapProperties(D3D12_HEAP_PROPERTIES* h, unsigned* flags) {*h=heap;*flags=heapFlags;return heapHr;}
    int GetDevice(ID3D12Device** out) {*out=&device;return deviceHr;}
};
template<class T> struct ComPtr {T* ptr=nullptr;T* operator->(){return ptr;}};
template<class T> T** set(ComPtr<T>& ptr) {return &ptr.ptr;}
namespace graphics {struct D3D12 {};}
struct Texture {
    ID3D12Resource resource;
    template<class T> ID3D12Resource* getAs() const {return const_cast<ID3D12Resource*>(&resource);}
};
struct Image {std::shared_ptr<Texture> appTexture=std::make_shared<Texture>(),runtimeTexture=std::make_shared<Texture>();};
struct State {std::vector<Image> images=std::vector<Image>(3);};
namespace fmt {
    template<class T> std::string str(const T& value) {std::ostringstream out;out<<value;return out.str();}
    inline std::string format(std::string pattern) {return pattern;}
    template<class T,class... A> std::string format(std::string pattern,const T& value,const A&... args) {
        const auto at=pattern.find("{}");assert(at!=std::string::npos);
        return pattern.substr(0,at)+str(value)+format(pattern.substr(at+2),args...);
    }
}
std::filesystem::path localAppData=std::filesystem::current_path()/"mock-appdata";
constexpr unsigned MOVEFILE_REPLACE_EXISTING=1,MOVEFILE_WRITE_THROUGH=2;
unsigned GetCurrentProcessId() {return 42;}
int MoveFileExW(const std::filesystem::path::value_type* from,
                const std::filesystem::path::value_type* to,unsigned) {
    std::error_code error;
    std::filesystem::remove(to,error);
    error.clear();
    std::filesystem::rename(from,to,error);
    return !error;
}
std::vector<std::string> logs;
template<class... A> void Log(const char* pattern,const A&...) {logs.push_back(pattern);}
struct XrRect2Di {struct {int x=0,y=0;} offset;struct {int width=0,height=0;} extent;};
struct XrSwapchainSubImage {XrSwapchain swapchain=0;XrRect2Di imageRect;unsigned imageArrayIndex=0;};
constexpr int XR_TYPE_COMPOSITION_LAYER_PROJECTION=1,XR_TYPE_COMPOSITION_LAYER_QUAD=2,
    XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR=3,XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR=4,
    XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR=5,XR_TYPE_COMPOSITION_LAYER_CUBE_KHR=6;
struct XrCompositionLayerBaseHeader {int type;};
struct XrCompositionLayerProjectionView {XrSwapchainSubImage subImage;};
struct XrCompositionLayerProjection {int type=1;unsigned viewCount=0;const XrCompositionLayerProjectionView* views=nullptr;};
struct XrCompositionLayerQuad {int type=2;XrSwapchainSubImage subImage;};
using XrCompositionLayerCylinderKHR=XrCompositionLayerQuad;
using XrCompositionLayerEquirectKHR=XrCompositionLayerQuad;
using XrCompositionLayerEquirect2KHR=XrCompositionLayerQuad;
struct XrCompositionLayerCubeKHR {int type=6;XrSwapchain swapchain;unsigned imageArrayIndex=0;};
struct XrFrameEndInfo {unsigned layerCount;const XrCompositionLayerBaseHeader* const* layers;};
namespace xr {
    const char* ToCString(int) {return "result";}
    std::string ToString(const XrRect2Di& r) {return std::to_string(r.offset.x)+","+std::to_string(r.offset.y)+","+std::to_string(r.extent.width)+","+std::to_string(r.extent.height);}
}
RECORD
struct OpenXrApi {
    static inline int calls=0,result=0;static inline XrSwapchainCreateInfo received;
    static XrResult xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
        ++calls;received=*info;if(result>=0)*out=5;return result;
    }
};
struct Fixture {
    bool m_proxyScopeEnabled=false;
    std::map<uint64_t,proxy_scope::Decision> m_proxyScopeDecisions;
    proxy_scope::Decision selectProxyScope(const XrSwapchainCreateInfo&) {return {};}
    void invalidateProxyScope(const std::string&) {}
    void recordProxyScopeCreation(XrSwapchain,const XrSwapchainCreateInfo&,const proxy_usage::Snapshot&,ProxyUsageState&) {}
    void validateProxyScopeRole(uint64_t,uint32_t,bool) {}
    void finishProxyScopeSession() {}
    uint32_t m_proxyUsageMode=0;bool m_proxyUsageEnabled=true,m_proxyUsageRunStrict=true,m_proxyUsageReferenceValid=false;
    uint64_t m_proxyUsageCreationId=0,m_proxyUsageFrame=0;
    proxy_usage::Reference m_proxyUsageReference,m_proxyUsageCurrent;
    std::map<XrSwapchain,ProxyUsageState> m_proxyUsageSwapchains;
    std::map<XrSwapchain,uint64_t> m_proxyUsageGenerations;
    std::map<XrSwapchain,State> m_swapchains;
    std::map<XrSwapchain,bool> m_subProxySwapchains;
    std::string m_proxyUsageAppIdentity="TheMidnightWalk:1:UE5:1",m_applicationName="TheMidnightWalk";
    std::string m_runtimeName="mock runtime",m_systemName="mock system";
    std::filesystem::path m_proxyUsagePath;
    METHODS
    XrResult create(XrSession session,const XrSwapchainCreateInfo* createInfo,XrSwapchain* swapchain) {
        CREATE
        return result;
    }
    void resources(unsigned mode=0) {
        m_proxyUsageMode=mode;m_swapchains[5];m_subProxySwapchains[5]=true;
    }
};
int main() {
    assert(proxy_usage::ParseMode("1",1)==1);
    for(auto text : {"0","2","-1","1x","", " 1"}) assert(proxy_usage::ParseMode(text,std::string(text).size())==0);
    assert(proxy_usage::ParseMode("1",20)==0);
    for(uint64_t usage=0;usage<128;usage++) for(bool proxy : {false,true}) for(unsigned mode : {0,1}) {
        XrSwapchainCreateInfo original;original.usageFlags=usage;original.next=(void*)0x123;
        const auto downstream=proxy_usage::DownstreamCreateInfo(original,mode,proxy);
        const auto expected=usage|((mode && proxy && !(usage&2)) ? 16:0);
        assert(downstream.usageFlags==expected && original.usageFlags==usage);
        assert(downstream.next==original.next && downstream.format==original.format && downstream.width==original.width);
    }
    std::cout<<"PASS: U0/U1, depth/direct, existing bit, invalid input, original fields unchanged\n";
    for(int result : {0,-7}) for(unsigned mode : {0,1}) {
        Fixture f;f.m_proxyUsageMode=mode;
        XrSwapchainCreateInfo original;original.usageFlags=0x61;XrSwapchain out=0;
        OpenXrApi::calls=0;OpenXrApi::result=result;
        assert(f.create(3,&original,&out)==result && OpenXrApi::calls==1);
        assert(OpenXrApi::received.usageFlags==(mode?0x71:0x61) && original.usageFlags==0x61);
    }
    std::cout<<"PASS: production downstream call forwards failure with exactly one call, no retry\n";
    Fixture f;f.resources();XrSwapchainCreateInfo original;original.usageFlags=0x61;
    f.recordProxyUsageCreation(3,5,original,original,4);
    assert(f.m_proxyUsageSwapchains[5].strict && f.m_proxyUsageSwapchains[5].roles==0);
    auto snapshot=f.m_proxyUsageCurrent.swapchains[1];
    assert(snapshot.at("image0.proxy.Format")=="90" && snapshot.at("requestedFormat")=="91");
    for(auto role : {"runtime","proxy"}) {
        proxy_usage::Snapshot probe;ProxyUsageState identity;
        ID3D12Resource r;r.heapHr=-1;r.device.hr=-2;
        const auto ok=f.captureProxyUsageResource(&r,role,identity,5,0,probe,true);
        assert(!ok && !probe.count(std::string("image0.")+role+".HeapType") && !probe.count(std::string("image0.")+role+".PlaneCount"));
    }
    std::cout<<"PASS: production GetDesc/heap/plane query, failed queries do not invent properties\n";
    for(int mutation=0;mutation<6;mutation++) {
        Fixture u;u.resources(1);u.m_proxyUsageReference=f.m_proxyUsageCurrent;u.m_proxyUsageReferenceValid=true;
        if(mutation==1) u.m_swapchains[5].images[0].runtimeTexture->resource.desc.Format=91;
        if(mutation==2) u.m_swapchains[5].images[0].appTexture->resource.desc.Layout=1;
        if(mutation==3) u.m_swapchains[5].images[0].appTexture->resource.heapFlags=0;
        if(mutation==4) u.m_swapchains[5].images.pop_back();
        if(mutation==5) u.m_proxyUsageReferenceValid=false;
        auto downstream=proxy_usage::DownstreamCreateInfo(original,1,true);
        u.recordProxyUsageCreation(3,5,original,downstream,4);
        assert(u.m_proxyUsageSwapchains[5].strict==(mutation==0));
    }
    std::cout<<"PASS: production cross-run comparison rejects native format, proxy desc/heap, image count, missing reference\n";
    ID3D12Device device;
    Fixture reference;reference.beginProxyUsageSession(3,&device);reference.resources();
    reference.recordProxyUsageCreation(3,5,original,original,4);
    Fixture incomplete;incomplete.m_proxyUsageMode=1;incomplete.beginProxyUsageSession(3,&device);
    assert(!incomplete.m_proxyUsageReferenceValid && !incomplete.m_proxyUsageRunStrict);
    reference.finishProxyUsageSession();
    Fixture loaded;loaded.m_proxyUsageMode=1;loaded.beginProxyUsageSession(3,&device);
    assert(loaded.m_proxyUsageReferenceValid && loaded.m_proxyUsageRunStrict);
    loaded.resources(1);loaded.recordProxyUsageCreation(3,5,original,proxy_usage::DownstreamCreateInfo(original,1,true),4);
    loaded.finishProxyUsageSession();assert(loaded.m_proxyUsageRunStrict);
    Fixture different;different.m_proxyUsageMode=1;different.m_runtimeName="different runtime";
    different.beginProxyUsageSession(3,&device);assert(!different.m_proxyUsageReferenceValid);
    Fixture missing;missing.m_proxyUsageMode=1;missing.beginProxyUsageSession(3,&device);
    missing.finishProxyUsageSession();assert(!missing.m_proxyUsageRunStrict);
    // Starting another U0 invalidates the old completed profile.
    Fixture interrupted;interrupted.beginProxyUsageSession(3,&device);
    Fixture next;next.m_proxyUsageMode=1;next.beginProxyUsageSession(3,&device);assert(!next.m_proxyUsageReferenceValid);
    std::cout<<"PASS: production reference files, incomplete/context mismatch rejection, final count check, interrupted U0 invalidation\n";
    auto clean=f.m_proxyUsageCurrent;
    for(auto& entry : clean.swapchains) for(const auto& field : entry.second) {
        auto changed=entry.second;changed[field.first]+="changed";
        assert(!proxy_usage::Compare(entry.second,changed).empty());
    }
    clean.identity="test identity";clean.complete=true;
    std::stringstream file;assert(proxy_usage::WriteReference(file,clean));
    proxy_usage::Reference read;assert(proxy_usage::ReadReference(file,read));
    assert(read.identity==clean.identity && read.complete && read.swapchains==clean.swapchains);
    for(auto malformed : {"bad\n", "PROXY_USAGE_V1\n\"x\"\n1 99999\n", "PROXY_USAGE_V1\n\"x\"\n1 0\ntrailing"}) {
        std::stringstream bad(malformed);assert(!proxy_usage::ReadReference(bad,read));
    }
    std::cout<<"PASS: named-field roundtrip, each observed invariant mutation rejected, malformed profiles rejected\n";
    XrCompositionLayerProjectionView views[2];views[0].subImage.swapchain=5;views[1].subImage.swapchain=5;
    views[1].subImage.imageArrayIndex=1;
    XrCompositionLayerProjection projection;projection.viewCount=2;projection.views=views;
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo frame{1,layers};
    const auto texture=f.m_swapchains[5].images[0].appTexture;
    f.observeProxyUsageRoles(&frame);assert(f.m_proxyUsageSwapchains[5].roles==1 && f.m_proxyUsageFrame==1);
    const auto afterFirst=logs.size();for(int i=0;i<1000;i++)f.observeProxyUsageRoles(&frame);
    assert(logs.size()==afterFirst);
    XrCompositionLayerQuad quad;quad.subImage.swapchain=5;layers[0]=reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
    f.observeProxyUsageRoles(&frame);assert(f.m_proxyUsageSwapchains[5].roles==3);
    XrCompositionLayerBaseHeader unknown{999};layers[0]=&unknown;
    f.observeProxyUsageRoles(&frame);assert(f.m_proxyUsageSwapchains[5].roles==3);
    assert(f.m_subProxySwapchains[5] && f.m_swapchains[5].images[0].appTexture==texture && quad.subImage.swapchain==5);
    std::cout<<"PASS: production role unknown/projection/both, dedup/rate limit, unknown layers ignored, selection untouched\n";
    Fixture depth;depth.resources();depth.m_subProxySwapchains[5]=false;
    for(auto& image : depth.m_swapchains[5].images) image.appTexture=image.runtimeTexture;
    original.usageFlags=0x62;depth.recordProxyUsageCreation(3,5,original,original,0x10);
    assert(!depth.m_proxyUsageSwapchains[5].hasProxy && depth.m_proxyUsageCurrent.swapchains[1].at("originalUsage")=="98");
    std::cout<<"PASS: direct depth resource identity and original metadata preserved\n";
}
'''
harness = harness.replace('RECORD',record).replace('METHODS',methods).replace('CREATE',create_dispatch)
with tempfile.TemporaryDirectory(prefix='proxy-usage-test-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-I',str(ROOT/'XR_APILAYER_MBUCCHIA_toolkit'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,cwd=path)
