#!/usr/bin/env python3
"""Compile actual scope selection, image creation loop, profile/role/resource observers.
Reuse the usage resource mocks; exercise production code rather than a second policy.
Linux g++ x64 mocks do not substitute for the Windows DLL or hardware tests.
"""
import ast
from pathlib import Path
import subprocess
import tempfile
from proxy_usage_source import block

ROOT = Path(__file__).resolve().parents[1]
layer = (ROOT/'XR_APILAYER_MBUCCHIA_toolkit/layer.cpp').read_text()
tree = ast.parse((ROOT/'tests/proxy_usage_diagnostic.py').read_text())
harness = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'harness' for t in n.targets))
harness = harness[:harness.index('int main()')]
start = harness.index('    proxy_scope::Decision selectProxyScope')
end = harness.index('    uint32_t m_proxyUsageMode', start)
harness = harness[:start] + '''    bool m_proxyScopeStrict=false;
    uint32_t m_proxyScopeMode=0;
    std::string m_proxyScopeReason;
    ID3D12CommandQueue* m_proxyScopeQueue=nullptr;
    std::map<uint64_t,uint32_t> m_proxyScopeReleaseLogs;
''' + harness[end:]
harness = harness.replace('    ID3D12Resource resource;', '    ID3D12Resource resource; ID3D12Resource* wrapped=nullptr;')
harness = harness.replace('return const_cast<ID3D12Resource*>(&resource);', 'return wrapped ? wrapped : const_cast<ID3D12Resource*>(&resource);')
harness = harness.replace('struct State {std::vector<Image> images=std::vector<Image>(3);};', '''
using SwapchainImages=Image;
struct State {std::vector<Image> images=std::vector<Image>(3);};
struct GraphicsDevice {
    unsigned allocations=0;std::vector<XrSwapchainCreateInfo> recipes;
    std::shared_ptr<Texture> createTexture(const XrSwapchainCreateInfo& info,const std::string&,int64_t nativeFormat) {
        ++allocations;recipes.push_back(info);auto t=std::make_shared<Texture>();
        t->resource.desc.Width=info.width;t->resource.desc.Height=info.height;
        t->resource.desc.DepthOrArraySize=info.arraySize;t->resource.desc.MipLevels=info.mipCount;
        t->resource.desc.SampleDesc.Count=info.sampleCount;t->resource.desc.Format=nativeFormat;
        return t;
    }
};
namespace graphics {
    std::shared_ptr<Texture> WrapD3D12Texture(GraphicsDevice*,const XrSwapchainCreateInfo&,
                                             ID3D12Resource* r,unsigned,const std::string&) {
        auto t=std::make_shared<Texture>();t->wrapped=r;return t;
    }
}
''')
harness = harness.replace('    METHODS', '''    GraphicsDevice device;GraphicsDevice* m_bisectDevice=&device;
    std::map<XrSwapchain,std::vector<ID3D12Resource>> ownedRuntime;
    void prepare(XrSwapchain swapchain,const XrSwapchainCreateInfo& info) {
        const auto* createInfo=&info;
        const bool isDepth=(info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)!=0;
        const auto decision=m_proxyScopeEnabled ? selectProxyScope(info) : proxy_scope::Decision{};
        const bool hasProxy=!isDepth && (!m_proxyScopeEnabled || decision.hasProxy);
        const unsigned initialState=isDepth?0x10:(info.usageFlags&1)?4:0;
        const unsigned imageCount=3;
        auto& owned=ownedRuntime[swapchain];owned.resize(imageCount);
        struct RuntimeImage {ID3D12Resource* texture;};std::vector<RuntimeImage> runtimeImages;
        for(auto& r : owned) {
            r.desc.Width=info.width;r.desc.Height=info.height;r.desc.DepthOrArraySize=info.arraySize;
            r.desc.MipLevels=info.mipCount;r.desc.SampleDesc.Count=info.sampleCount;
            runtimeImages.push_back({&r});
        }
        State state;state.images.clear();
        IMAGELOOP
        m_swapchains[swapchain]=std::move(state);m_subProxySwapchains[swapchain]=hasProxy;
        if(m_proxyScopeEnabled) m_proxyScopeDecisions.emplace(m_proxyUsageCreationId+1,decision);
        recordProxyUsageCreation(3,swapchain,info,info,initialState);
    }
    METHODS''')
methods = '\n'.join(block(layer, marker) for marker in (
    'void invalidateProxyScope', 'void beginProxyScopeSession', 'proxy_scope::Decision selectProxyScope',
    'void recordProxyScopeCreation', 'void validateProxyScopeRole', 'void finishProxyScopeSession',
    'template <typename T>\n        static void usageField', 'void writeProxyUsageReference',
    'void beginProxyUsageSession', 'bool captureProxyUsageResource', 'void recordProxyUsageCreation',
    'static const char* proxyUsageRole', 'void observeProxyUsageRole(', 'void observeProxyUsageRoles',
    'void summarizeProxyUsage', 'void finishProxyUsageSession'))
create = block(layer[layer.index('XrResult xrCreateSwapchain'):], 'if (m_gfxSubBisectMode == 2 || m_gfxSubBisectMode == 3)')
harness = harness.replace('RECORD', block(layer, 'struct ProxyUsageState')+';')
harness = harness.replace('METHODS', methods)
harness = harness.replace('CREATE', create[create.index('const bool isDepth'):create.index('uint32_t imageCount = 0;')])
harness = harness.replace('IMAGELOOP', block(create, 'for (uint32_t i = 0; i < imageCount; ++i)'))
harness += r'''
std::string fileBytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
void observe(Fixture& f, bool mismatch=false) {
    XrCompositionLayerProjectionView v;v.subImage.swapchain=11;
    XrCompositionLayerProjection p;p.viewCount=1;p.views=&v;
    XrCompositionLayerQuad q;q.subImage.swapchain=13;
    XrCompositionLayerQuad q2;q2.subImage.swapchain=14;
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&p),
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&q),reinterpret_cast<XrCompositionLayerBaseHeader*>(&q2)};
    XrFrameEndInfo frame{3,layers};f.observeProxyUsageRoles(&frame);
    if(mismatch) {q.subImage.swapchain=11;frame.layerCount=1;frame.layers=layers+1;f.observeProxyUsageRoles(&frame);}
}
int main() {
    for(unsigned mode : {0,1,2}) {auto text=std::to_string(mode);assert(proxy_scope::ParseMode(text.c_str(),1)==mode);}
    for(auto text : {"3","-1","1x","", " 1"}) assert(proxy_scope::ParseMode(text,std::string(text).size())==0);
    std::vector<XrSwapchainCreateInfo> infos(4);
    infos[0].usageFlags=0x61;infos[1].usageFlags=0x62;
    infos[2].usageFlags=0x61;infos[2].width=infos[2].height=512;
    infos[3].usageFlags=0x61;infos[3].width=infos[3].height=64;
    ID3D12Device device;Fixture reference;reference.beginProxyUsageSession(3,&device);
    for(unsigned i=0;i<4;i++) reference.prepare(11+i,infos[i]);
    observe(reference);reference.finishProxyUsageSession();
    const auto bytes=fileBytes(reference.m_proxyUsagePath);
    assert(reference.m_proxyUsageCurrent.complete && !bytes.empty());
    // This is the real old profile schema: no runtime descriptor/heap fields.
    assert(!reference.m_proxyUsageCurrent.swapchains.at(1).count("image0.runtime.Width"));
    for(unsigned mode : {0,1,2}) {
        Fixture f;f.m_proxyScopeEnabled=true;f.m_proxyScopeMode=mode;
        f.beginProxyUsageSession(3,&device);f.beginProxyScopeSession(nullptr);
        assert(f.m_proxyScopeStrict && f.m_proxyUsageReferenceValid);
        for(unsigned i=0;i<4;i++) {
            f.prepare(11+i,infos[i]);
            const bool expected=i!=1 && (mode==0 || (mode==1 && i==0) || (mode==2 && i>=2));
            assert(f.m_subProxySwapchains.at(11+i)==expected);
            for(const auto& image : f.m_swapchains.at(11+i).images) {
                if(expected) assert(image.appTexture!=image.runtimeTexture);
                else {
                    assert(image.appTexture==image.runtimeTexture);
                    assert(image.appTexture->getAs<graphics::D3D12>()==image.runtimeTexture->getAs<graphics::D3D12>());
                }
            }
        }
        assert(f.device.allocations==(mode==0?9:mode==1?3:6));
        observe(f);const auto after=logs.size();for(int n=0;n<500;n++)observe(f);
        assert(logs.size()==after);
        f.writeProxyUsageReference();f.finishProxyUsageSession();
        assert(f.m_proxyScopeStrict && fileBytes(f.m_proxyUsagePath)==bytes);
        std::cout<<"PASS: scope "<<mode<<" actual creation loop/identity/private allocation count, roles, rate limit, U0 file read-only\n";
    }
    // Every signature field matters. Size is validation only, never a role heuristic.
    const auto snapshot=reference.m_proxyUsageCurrent.swapchains.at(1);
    for(const auto& field : proxy_scope::Signature(infos[0])) {
        auto changed=snapshot;changed[field.first]+="changed";
        for(unsigned mode : {0,1,2}) {
            const auto d=proxy_scope::Select(infos[0],mode,&changed,true);
            assert(d.hasProxy && !d.strict && !d.signatureMatch && d.reason=="signature_"+field.first);
        }
        auto missing=snapshot;missing.erase(field.first);
        const auto d=proxy_scope::Select(infos[0],2,&missing,true);
        assert(d.hasProxy && !d.strict && !d.signatureMatch);
    }
    for(const char* role : {"0","3","9","garbage"}) {
        auto changed=snapshot;changed["observedRoles"]=role;
        const auto d=proxy_scope::Select(infos[0],2,&changed,true);
        assert(d.hasProxy && !d.strict && !d.reason.empty());
    }
    auto absent=snapshot;absent.erase("observedRoles");
    assert(!proxy_scope::Select(infos[0],2,&absent,true).strict);
    assert(proxy_scope::Select(infos[0],2,nullptr,true).hasProxy);
    assert(!proxy_scope::Select(infos[0],2,&snapshot,false).strict);
    auto extension=infos[0];extension.next=(void*)1;
    assert(!proxy_scope::Select(extension,2,&snapshot,true).strict);
    for(unsigned mode : {0,1,2}) assert(!proxy_scope::Select(infos[1],mode,nullptr,false).hasProxy);
    std::cout<<"PASS: nine signature mutations, unknown/ambiguous/missing roles, bad context/id/extension fall back to control; depth always direct\n";
    for(unsigned invalid=0;invalid<6;invalid++) {
        Fixture f;f.m_proxyScopeEnabled=true;f.m_proxyScopeMode=2;
        f.beginProxyUsageSession(3,&device);
        if(invalid==0) f.m_proxyUsageReferenceValid=false; // missing/incomplete/context handled by reader
        if(invalid==1) f.m_proxyUsageReference.swapchains.erase(1);
        if(invalid==2) f.m_proxyUsageReference.swapchains.at(1)["requestedFormat"]="changed";
        if(invalid==3) f.m_proxyUsageReference.swapchains.at(1)["observedRoles"]="3";
        f.beginProxyScopeSession(nullptr);
        for(unsigned i=0;i<(invalid==5?3u:4u);i++) f.prepare(11+i,infos[i]);
        if(invalid<4) assert(f.m_subProxySwapchains.at(11) && !f.m_proxyScopeStrict);
        const bool selected=f.m_subProxySwapchains.at(11);const auto image=f.m_swapchains.at(11).images.at(0).appTexture;
        observe(f,invalid==4);f.finishProxyUsageSession();
        assert(!f.m_proxyScopeStrict && !f.m_proxyScopeReason.empty());
        assert(f.m_subProxySwapchains.at(11)==selected && f.m_swapchains.at(11).images.at(0).appTexture==image);
        assert(fileBytes(f.m_proxyUsagePath)==bytes);
    }
    std::cout<<"PASS: production fallback, role mismatch and missing creation invalidate final strict status without changing selection\n";
    for(unsigned bad=0;bad<4;bad++) {
        std::ofstream out(reference.m_proxyUsagePath);auto ref=reference.m_proxyUsageCurrent;
        if(bad==0) ref.complete=false;
        if(bad==1) ref.identity="different context";
        if(bad==2) out<<"malformed profile";else assert(proxy_usage::WriteReference(out,ref));
        out.close();if(bad==3)std::filesystem::remove(reference.m_proxyUsagePath);
        Fixture f;f.m_proxyScopeEnabled=true;f.m_proxyScopeMode=2;
        f.beginProxyUsageSession(3,&device);f.beginProxyScopeSession(nullptr);f.prepare(11,infos[0]);
        assert(!f.m_proxyUsageReferenceValid && !f.m_proxyScopeStrict && f.m_subProxySwapchains.at(11));
    }
    auto current=snapshot;current["hasProxy"]="0";
    for(auto it=current.begin();it!=current.end();) {
        if(it->first.find(".proxy.")!=std::string::npos) it=current.erase(it);else ++it;
    }
    assert(proxy_scope::CompareResources(snapshot,current,false).empty());
    for(const auto& key : {"imageCount","image0.runtimeFormat","copyPolicy","initialLogicalState"}) {
        auto changed=current;changed[key]+="bad";assert(!proxy_scope::CompareResources(snapshot,changed,false).empty());
    }
    auto withRuntime=snapshot;withRuntime["image0.runtime.HeapType"]="1";
    assert(!proxy_scope::CompareResources(withRuntime,current,false).empty());
    current["image0.runtime.HeapType"]="1";assert(proxy_scope::CompareResources(withRuntime,current,false).empty());
    current["image0.runtime.HeapType"]="2";assert(!proxy_scope::CompareResources(withRuntime,current,false).empty());
    auto changed=snapshot;changed["image0.proxy.Layout"]="99";
    assert(!proxy_scope::CompareResources(snapshot,changed,true).empty());
    std::cout<<"PASS: actual profile reader rejects incomplete/context/malformed; deliberate direct proxy disappearance accepted; constant runtime/private invariants enforced\n";
    for(unsigned mode : {0,1,2}) {
        Fixture f;f.m_proxyScopeEnabled=true;f.m_proxyScopeMode=mode;f.m_proxyUsageMode=1;
        XrSwapchain output=0;OpenXrApi::calls=0;OpenXrApi::result=-7;
        assert(f.create(3,&infos[0],&output)==-7 && OpenXrApi::calls==1);
        assert(OpenXrApi::received.usageFlags==infos[0].usageFlags && !f.m_proxyScopeStrict);
    }
    std::cout<<"PASS: scope downstream usage original regardless of legacy usage mode; one create call, no retry\n";
}
'''
with tempfile.TemporaryDirectory(prefix='proxy-scope-test-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-I',str(ROOT/'XR_APILAYER_MBUCCHIA_toolkit'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,cwd=path)
