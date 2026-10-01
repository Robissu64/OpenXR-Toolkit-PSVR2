"""Exercise the production flush methods against a deliberately stalled mock GPU.

No D3D12 algorithm is duplicated: the C++ methods and observation structs are
extracted from production source, compiled with g++, and run with fake objects.
This checks ordering/lifetime, not Windows ABI, real D3D12 or headset behavior.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end].replace(" override", "")


source = (ROOT / "XR_APILAYER_MBUCCHIA_toolkit/d3d12.cpp").read_text()
baseline_source = subprocess.run(
    ["git", "show", "15533b8e8e79d63daa4894282d75aef75b1e3fc7:XR_APILAYER_MBUCCHIA_toolkit/d3d12.cpp"],
    cwd=ROOT, capture_output=True, text=True, check=True,
).stdout
interfaces = (ROOT / "XR_APILAYER_MBUCCHIA_toolkit/interfaces.h").read_text()
structs = "\n".join(block(interfaces, "struct " + name) + ";" for name in
                    ("ProxySyncReuse", "ProxySyncSubmission"))
methods = "\n".join(block(source, signature) for signature in (
    "void flushContext(bool blocking, bool isEndOfFrame = false)",
    "void configureProxySyncDiagnostic(uint32_t mode, void* bindingQueue)",
    "ProxySyncSubmission flushProxySyncCopy()",
    "uint64_t proxySyncCompletedValue() const",
    "double waitProxySyncFence(uint64_t value) const",
    "void flushProxySyncContext(bool blocking, bool isEndOfFrame)",
))
# Extract the production member declarations too, to keep the fixture aligned.
members = re.search(r"bool m_proxySyncEnabled\{false\};.*?uint32_t m_proxySyncFlushLogs\{0\};",
                    source, re.S).group()

harness = r'''
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using UINT64 = uint64_t;
using DWORD = unsigned long;
constexpr int EVENT_ALL_ACCESS = 0, INFINITE = -1, D3D12_QUERY_TYPE_TIMESTAMP = 0;
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_FAILED = 0xffffffff;
#define ARRAYSIZE(x) (sizeof(x) / sizeof((x)[0]))
#define CHECK_HRCMD(x) do { if ((x) != 0) throw std::runtime_error("mock HRESULT"); } while (0)
struct Event { std::string kind; int slot; uint64_t value; };
std::vector<Event> events;
uint64_t gpuSubmitted = 0, gpuCompleted = 0, fenceCompleted = 0, fenceSignaled = 0;
int unsafeResets = 0;
bool failSignal = false;
bool failWait = false;
uint64_t waitTarget = 0;
std::array<uint64_t, 32> allocatorLastWork{};
struct Allocator {
    int slot;
    int Reset() {
        if (gpuCompleted < allocatorLastWork[slot]) ++unsafeResets;
        events.push_back({"allocator_reset", slot, gpuCompleted});
        return 0;
    }
};
struct List {
    int slot;
    int Close() { events.push_back({"close", slot, 0}); return 0; }
    int Reset(Allocator* allocator, void*) {
        assert(allocator->slot == slot);
        events.push_back({"list_reset", slot, 0}); return 0;
    }
    void ResolveQueryData(void*, int, int, uint32_t, void*, int) {}
};
using ID3D12CommandList = List;
struct Fence {
    uint64_t GetCompletedValue() { return fenceCompleted; }
    int SetEventOnCompletion(uint64_t value, void*) { waitTarget = value; return 0; }
};
struct Queue {
    void ExecuteCommandLists(size_t count, List* const* lists) {
        assert(count == 1);
        allocatorLastWork[lists[0]->slot] = ++gpuSubmitted;
        events.push_back({"execute", lists[0]->slot, gpuSubmitted});
    }
    int Signal(Fence*, uint64_t value) {
        events.push_back({"signal", -1, value});
        if (failSignal) return -1;
        fenceSignaled = value;
        assert(value == gpuSubmitted); // one Signal per submission in safe modes
        return 0;
    }
};
template<class T> struct Ptr {
    T* value = nullptr;
    T* operator->() const { return value; }
    Ptr& operator=(T* p) { value = p; return *this; }
};
template<class T> T* get(const Ptr<T>& p) { return p.value; }
template<class... A> void Log(const char*, A...) {}
namespace wil {
    struct unique_handle {
        void* value = nullptr;
        void** put() { return &value; }
        void* get() { return value; }
        explicit operator bool() const { return value != nullptr; }
    };
}
struct Device { int GetDeviceRemovedReason() { return -1; } };
int GetLastError() { return 1; }
int HRESULT_FROM_WIN32(int value) { return value; }
void* CreateEventEx(void*, const wchar_t*, int, int) { return reinterpret_cast<void*>(1); }
DWORD WaitForSingleObject(void*, int) {
    assert(waitTarget <= fenceSignaled);
    events.push_back({"wait", -1, waitTarget});
    if (failWait) return WAIT_FAILED;
    fenceCompleted = gpuCompleted = waitTarget;
    return WAIT_OBJECT_0;
}
'''
harness += structs
harness += r'''
class MockDevice {
public:
    static constexpr size_t NumInflightContexts = 32;
    std::array<Allocator, 32> allocators;
    std::array<List, 32> lists;
    Ptr<Allocator> m_commandAllocator[32];
    Ptr<List> m_commandList[32], m_context;
    Queue queue; Fence fence; Device device;
    Ptr<Queue> m_queue; Ptr<Fence> m_fence;
    Ptr<Device> m_device;
    Ptr<void> m_queryHeap, m_queryReadbackBuffer;
    uint32_t m_nextGpuTimestampIndex = 0;
    size_t m_currentContext = 0;
    uint64_t m_fenceValue = 0;
'''
harness += members
harness += r'''
    MockDevice() {
        for (int i = 0; i < 32; ++i) {
            allocators[i].slot = lists[i].slot = i;
            m_commandAllocator[i] = &allocators[i]; m_commandList[i] = &lists[i];
        }
        m_context = &lists[0]; m_queue = &queue; m_fence = &fence;
        m_device = &device;
    }
'''
harness += methods
harness += "\n};\nclass BaselineDevice : public MockDevice { public:\n"
harness += block(baseline_source, "void flushContext(bool blocking, bool isEndOfFrame = false)")
harness += r'''
};
void resetGpu() {
    events.clear(); allocatorLastWork.fill(0);
    gpuSubmitted = gpuCompleted = fenceCompleted = fenceSignaled = 0;
    unsafeResets = 0; failSignal = failWait = false;
}
int main() {
    resetGpu(); BaselineDevice baseline;
    for (int i = 0; i < 240; ++i) baseline.flushContext(false, i % 3 == 2);
    const auto baselineEvents = events;
    resetGpu(); MockDevice control; control.configureProxySyncDiagnostic(0, &control.queue);
    for (int i = 0; i < 240; ++i) control.flushContext(false, i % 3 == 2);
    assert(events.size() == baselineEvents.size());
    for (size_t i = 0; i < events.size(); ++i) {
        assert(events[i].kind == baselineEvents[i].kind);
        assert(events[i].slot == baselineEvents[i].slot);
        assert(events[i].value == baselineEvents[i].value);
    }
    std::cout << "Mode 0 versus validated commit: identical GPU/reset event order PASS\n";
    for (uint32_t mode = 0; mode <= 2; ++mode) {
        resetGpu(); MockDevice d; d.configureProxySyncDiagnostic(mode, &d.queue);
        int reuseWaits = 0;
        for (int frame = 0; frame < 80; ++frame) {
            // The original non-copy begin flush and end flush also consume ring slots.
            for (int phase = 0; phase < 3; ++phase) {
                const size_t start = events.size();
                ProxySyncSubmission s;
                if (phase == 1) s = d.flushProxySyncCopy();
                else d.flushContext(false, phase == 2);
                events.push_back({"release_or_return", -1, 0});
                size_t execute = start;
                for (size_t i = start; i < events.size(); ++i)
                    if (events[i].kind == "execute") execute = i;
                if (mode > 0) {
                    assert(unsafeResets == 0);
                    assert(d.m_proxySyncSlotFence[events[execute].slot] == gpuSubmitted);
                    for (size_t i = execute + 1; i < events.size(); ++i) {
                        assert(events[i].kind != "allocator_reset" && events[i].kind != "list_reset");
                        if (mode == 1 || phase != 1) assert(events[i].kind != "wait");
                    }
                    for (size_t i = start; i < execute; ++i)
                        if (events[i].kind == "wait") {
                            ++reuseWaits;
                            assert(events[i].value < gpuSubmitted);
                        }
                }
                if (phase == 1 && mode == 1) {
                    assert(!s.copyWaited);
                    assert(s.completedBeforeRelease < s.submissionFence);
                }
                if (phase == 1 && mode == 2) {
                    assert(s.copyWaited);
                    assert(s.completedBeforeRelease >= s.submissionFence);
                }
                if (mode == 0) assert(fenceSignaled == 0);
            }
        }
        if (mode == 0) assert(unsafeResets > 0);
        if (mode == 1) assert(reuseWaits > 0);
        std::cout << "Mode " << mode << ": PASS (240 submissions, unsafe_resets="
                  << unsafeResets << ", reuse_waits=" << reuseWaits << ")\n";
    }
    resetGpu(); MockDevice d; d.configureProxySyncDiagnostic(2, &d.queue);
    d.flushContext(false);
    // Already-completed current submission must skip a redundant wait.
    fenceCompleted = gpuCompleted = 1000;
    auto s = d.flushProxySyncCopy();
    assert(!s.copyWaited && s.completedBeforeRelease >= s.submissionFence);
    resetGpu(); MockDevice e; e.configureProxySyncDiagnostic(1, &e.queue);
    failSignal = true;
    bool threw = false;
    try { e.flushProxySyncCopy(); } catch (const std::runtime_error&) { threw = true; }
    assert(threw && !e.m_proxySyncCopySubmission);
    resetGpu(); MockDevice f; f.configureProxySyncDiagnostic(2, &f.queue);
    failWait = true; threw = false;
    try { f.flushProxySyncCopy(); } catch (const std::runtime_error&) { threw = true; }
    assert(threw && !f.m_proxySyncCopySubmission);
    resetGpu(); MockDevice removed; removed.configureProxySyncDiagnostic(1, &removed.queue);
    fenceCompleted = UINT64_MAX; threw = false;
    try { removed.flushProxySyncCopy(); } catch (const std::runtime_error&) { threw = true; }
    assert(threw && !removed.m_proxySyncCopySubmission);
    resetGpu(); MockDevice disabled;
    for (int i = 0; i < 40; ++i) disabled.flushContext(false);
    assert(fenceSignaled == 0 && unsafeResets > 0);
    std::cout << "Already-complete, Signal/wait/device errors and disabled path: PASS\n";
}
'''

with tempfile.TemporaryDirectory(prefix="proxy-sync-ring-") as tmp:
    cpp = Path(tmp) / "ring.cpp"
    exe = Path(tmp) / "ring-test"
    cpp.write_text(harness)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
