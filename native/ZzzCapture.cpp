// SPDX-License-Identifier: GPL-3.0-or-later
#include "ZzzCapture.h"
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <locale.h>
#include <mutex>
#include <string>
#include <vector>

namespace ZzzCapture {
using Microsoft::WRL::ComPtr;
std::atomic<unsigned> captureMode{0};
std::atomic<bool> assessSwapChain{true};

namespace {
constexpr unsigned opCount = static_cast<unsigned>(Op::Count);
constexpr unsigned threadLimit = 32;
constexpr unsigned frameLimit = 60000;
constexpr unsigned gpuSlots = 8;
constexpr unsigned segmentLimit = 48;
constexpr unsigned gpuRowLimit = 100000;
constexpr const char* opNames[] = {
    "draw", "draw_indexed", "draw_instanced", "draw_indexed_instanced", "draw_indirect", "draw_auto",
    "dispatch", "dispatch_indirect", "shader_bind", "shader_resources", "constant_buffers", "samplers",
    "render_targets", "blend", "depth_stencil", "rasterizer", "vertex_buffers", "index_buffer",
    "map", "map_read", "map_do_not_wait", "map_driver", "unmap", "get_data", "get_data_do_not_flush", "query_marker",
    "update", "copy", "resolve", "flush", "clear", "execute_list", "finish_list",
    "create_buffer", "create_texture", "create_query",
    "xxmi_draw_overrides", "xxmi_dispatch_overrides", "xxmi_command_list",
    "xxmi_override_lookup", "xxmi_resource_hash", "xxmi_map_tracking"
};
static_assert(sizeof(opNames) / sizeof(*opNames) == opCount, "Mismatch in API labels");
constexpr const char* workNames[] = {"draw", "dispatch", "copy", "clear", "command_list"};
constexpr uint64_t histogramUs[] = {2, 8, 32, 128, 512, 1000, 4000, 16000};

int64_t Now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

struct Stat {
    uint64_t count = 0, samples = 0, ticks = 0, peak = 0, stalls = 0, amount = 0, repeats = 0;
    std::array<uint64_t, 9> histogram{};

    void merge(const Stat& rhs) noexcept
    {
        count += rhs.count;
        samples += rhs.samples;
        ticks += rhs.ticks;
        peak = (std::max)(peak, rhs.peak);
        stalls += rhs.stalls;
        amount += rhs.amount;
        repeats += rhs.repeats;
        for (unsigned i = 0; i < histogram.size(); ++i)
            histogram[i] += rhs.histogram[i];
    }
};

struct ThreadStats {
    std::mutex mutex;
    unsigned thread = 0;
    std::array<Stat, opCount * 2> frame{}, total{};
};

struct Frame {
    int64_t qpc = 0, interval = 0, present = 0, cost = 0;
    uint64_t apiSampleTicks = 0, apiSamples = 0;
    long hr = 0;
    unsigned syncInterval = 0, presentFlags = 0;
    uint64_t draws = 0, deferredDraws = 0, dispatches = 0, targetBinds = 0;
    uint64_t vertices = 0, indices = 0, stalls = 0, slowest = 0;
    int slowOp = -1;
    double gpuMs = -1.0;
};

struct Target {
    unsigned width = 0, height = 0, format = 0, count = 0;
    bool depthOnly = false;
};

struct Segment {
    Work work = Work::Draw;
    uint64_t shader = 0;
    Target target;
    unsigned binds = 0;
    unsigned calls = 0;
};

struct GpuSlot {
    ComPtr<ID3D11Query> disjoint;
    std::array<ComPtr<ID3D11Query>, 2 * segmentLimit + 2> stamp;
    std::array<Segment, segmentLimit> segments;
    unsigned frame = 0, num = 0;
    bool open = false, pending = false, truncated = false;
    // Timestamp 0/1 envelope the frame. Timestamp 2 onward mark segments.
};

struct GpuRow {
    unsigned frame = 0;
    Segment segment;
    double ms = 0;
};

struct Budget {
    unsigned frame = 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    bool hasLocal = false, hasNonlocal = false;
};

struct Config {
    unsigned mode = 0, seconds = 45, gpuEvery = 4, cpuEvery = 16, stallUs = 1000;
    uint64_t shader = 0;
};

struct State {
    std::mutex presentMutex;
    std::array<ThreadStats, threadLimit> threads;
    std::atomic<unsigned> token{0}, nextThread{0}, droppedCalls{0};
    std::atomic<const void*> device{nullptr};
    const void* chain = nullptr;
    const void* context = nullptr;
    DWORD owner = 0, pid = 0;
    Config config;
    std::wstring dir;
    bool initialized = false, keyWasDown = false, finishing = false, stopRequested = false;
    int64_t freq = 0, stallTicks = 0;
    int64_t started = 0, lastBoundary = 0, thisBoundary = 0, driverStart = 0, drainStart = 0;
    unsigned syncInterval = 0, presentFlags = 0, drainFrames = 0, serial = 0, contendedFrames = 0;
    unsigned gpuAttempts = 0, gpuDropped = 0, gpuDisjoint = 0, gpuTruncated = 0;
    int64_t profilerTicks = 0;
    HWND hwnd = nullptr;
    HANDLE startRequest = nullptr;
    DXGI_ADAPTER_DESC adapterDesc{};
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    LARGE_INTEGER driverVersion{};
    bool driverKnown = false;
    wchar_t cpuName[128]{};
    unsigned gameVersion[4]{};
    bool gameVersionKnown = false;
    ComPtr<IDXGIAdapter3> adapter;
    ID3D11DeviceContext* raw = nullptr; // borrowed from owning swap chain
    std::array<GpuSlot, gpuSlots> slots;
    GpuSlot* writing = nullptr;
    Target target;
    unsigned targetEpoch = 0;
    std::vector<Frame> frames;
    std::vector<GpuRow> gpuRows;
    std::vector<Budget> budgets;

    ~State()
    {
        if (startRequest)
            CloseHandle(startRequest);
    }
};

State state;
thread_local unsigned tlsToken = 0, tlsSlot = threadLimit;
thread_local uint32_t rng = 0;
thread_local bool presenting = false;
thread_local unsigned gpuToken = 0;

unsigned Clamped(unsigned value, unsigned low, unsigned high) noexcept
{
    return (std::min)((std::max)(value, low), high);
}

void Initialize()
{
    state.initialized = true;
    HMODULE self = nullptr;
    wchar_t path[32768];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&captureMode), &self))
        return;
    const DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path))
        return;
    state.dir.assign(path, length);
    const auto slash = state.dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return;
    state.dir.resize(slash + 1);
    const auto ini = state.dir + L"zzz-profiler.ini";
    const auto read = [&](const wchar_t* name, unsigned fallback) {
        return GetPrivateProfileIntW(L"Capture", name, fallback, ini.c_str());
    };
    auto& c = state.config;
    c.mode = Clamped(read(L"Level", 0), 0, 3);
    c.seconds = Clamped(read(L"Seconds", 45), 1, c.mode == 3 ? 20 : 120);
    c.gpuEvery = Clamped(read(L"GpuEveryNFrames", 4), 1, 120);
    c.cpuEvery = Clamped(read(L"CpuSampleEvery", 16), 1, 1024);
    c.stallUs = Clamped(read(L"StallMicroseconds", 1000), 100, 1000000);
    wchar_t hash[32]{};
    GetPrivateProfileStringW(L"Capture", L"ShaderHash", L"0", hash, 32, ini.c_str());
    wchar_t* end = nullptr;
    const auto parsed = wcstoull(hash, &end, 16);
    if (end && !*end)
        c.shader = parsed;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency)) {
        c.mode = 0;
        return;
    }
    state.freq = frequency.QuadPart;
    state.stallTicks = state.freq * c.stallUs / 1000000;
    state.pid = GetCurrentProcessId();
    if (c.mode) {
        wchar_t eventName[64];
        swprintf_s(eventName, L"Local\\ZZZ-DX11-Capture-%lu", state.pid);
        state.startRequest = CreateEventW(nullptr, TRUE, FALSE, eventName);
    }
}

HWND Window(IDXGISwapChain1* swap) noexcept
{
    HWND hwnd = nullptr;
    if (SUCCEEDED(swap->GetHwnd(&hwnd)))
        return hwnd;
    DXGI_SWAP_CHAIN_DESC desc{};
    return SUCCEEDED(swap->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
}

void CollectSystemMetadata() noexcept
{
    DWORD size = sizeof(state.cpuName);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, state.cpuName, &size);
    wchar_t exe[32768];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!length || length >= std::size(exe))
        return;
    DWORD ignored = 0;
    const DWORD versionSize = GetFileVersionInfoSizeW(exe, &ignored);
    if (!versionSize || versionSize > 65536)
        return;
    unsigned char data[65536];
    if (!GetFileVersionInfoW(exe, 0, versionSize, data))
        return;
    void* value = nullptr;
    UINT valueSize = 0;
    if (!VerQueryValueW(data, L"\\", &value, &valueSize) || valueSize < sizeof(VS_FIXEDFILEINFO))
        return;
    const auto* version = static_cast<VS_FIXEDFILEINFO*>(value);
    if (version->dwSignature != 0xfeef04bd)
        return;
    state.gameVersion[0] = HIWORD(version->dwFileVersionMS);
    state.gameVersion[1] = LOWORD(version->dwFileVersionMS);
    state.gameVersion[2] = HIWORD(version->dwFileVersionLS);
    state.gameVersion[3] = LOWORD(version->dwFileVersionLS);
    state.gameVersionKnown = true;
}

bool CreateQuery(ID3D11Device* device, D3D11_QUERY type, ComPtr<ID3D11Query>& query) noexcept
{
    const D3D11_QUERY_DESC desc{type, 0};
    return SUCCEEDED(device->CreateQuery(&desc, query.ReleaseAndGetAddressOf()));
}

void ReadBudget(unsigned frame)
{
    if (!state.adapter)
        return;
    Budget row;
    row.frame = frame;
    row.hasLocal = SUCCEEDED(state.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &row.local));
    row.hasNonlocal = SUCCEEDED(state.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &row.nonlocal));
    if (row.hasLocal || row.hasNonlocal)
        state.budgets.push_back(row);
}

void Start(const void* chain, const void* device, const void* context,
    IDXGISwapChain1* swap, ID3D11Device* rawDevice, ID3D11DeviceContext* rawContext)
{
    state.frames.clear();
    state.frames.reserve(frameLimit);
    state.gpuRows.clear();
    if (state.config.mode >= 2)
        state.gpuRows.reserve(gpuRowLimit);
    state.budgets.clear();
    state.budgets.reserve(frameLimit / 60 + 2);
    state.adapterDesc = {};
    state.driverVersion = {};
    state.driverKnown = false;
    state.swapDesc = {};
    swap->GetDesc1(&state.swapDesc);
    CollectSystemMetadata();
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> baseAdapter;
    if (rawDevice && SUCCEEDED(rawDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&baseAdapter))) {
        baseAdapter->GetDesc(&state.adapterDesc);
        state.driverKnown = SUCCEEDED(baseAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &state.driverVersion));
        baseAdapter.As(&state.adapter);
    }
    for (auto& slot : state.slots) {
        slot = {};
        if (state.config.mode < 2 || !rawDevice || !rawContext ||
                !CreateQuery(rawDevice, D3D11_QUERY_TIMESTAMP_DISJOINT, slot.disjoint))
            continue;
        const unsigned maxQueries = 2 * (state.config.mode == 3 ? segmentLimit : 20) + 2;
        for (unsigned i = 0; i < maxQueries; ++i) {
            if (!CreateQuery(rawDevice, D3D11_QUERY_TIMESTAMP, slot.stamp[i])) {
                slot = {};
                break;
            }
        }
    }
    for (auto& thread : state.threads) {
        const std::lock_guard<std::mutex> lock(thread.mutex);
        thread.thread = 0;
        thread.frame = {};
        thread.total = {};
    }
    state.nextThread.store(0, std::memory_order_relaxed);
    state.droppedCalls.store(0, std::memory_order_relaxed);
    state.contendedFrames = 0;
    state.gpuAttempts = state.gpuDropped = state.gpuDisjoint = state.gpuTruncated = 0;
    state.profilerTicks = 0;
    state.writing = nullptr;
    state.target = {};
    state.targetEpoch = 0;
    state.chain = chain;
    state.context = context;
    state.raw = rawContext;
    state.owner = GetCurrentThreadId();
    state.hwnd = Window(swap);
    state.finishing = false;
    state.started = state.lastBoundary = 0;
    state.thisBoundary = 0;
    state.driverStart = 0;
    ReadBudget(0);
    state.device.store(device, std::memory_order_relaxed);
    state.token.store(++state.serial, std::memory_order_release);
    gpuToken = state.serial;
    // Start counting only after the triggering Present returns.
}

void BeginGpu(unsigned frame) noexcept
{
    if (!state.raw || state.config.mode < 2 || frame % state.config.gpuEvery)
        return;
    ++state.gpuAttempts;
    for (auto& slot : state.slots) {
        if (slot.pending || !slot.disjoint)
            continue;
        state.target = {};
        state.targetEpoch = 0;
        slot.frame = frame;
        slot.num = 0;
        slot.truncated = false;
        slot.open = false;
        state.raw->Begin(slot.disjoint.Get());
        state.raw->End(slot.stamp[0].Get());
        state.writing = &slot;
        return;
    }
    ++state.gpuDropped;
}

void EndSegment(GpuSlot& slot) noexcept
{
    if (slot.open) {
        state.raw->End(slot.stamp[2 * slot.num + 1].Get());
        slot.open = false;
    }
}

void CloseGpu() noexcept
{
    if (!state.writing)
        return;
    auto& slot = *state.writing;
    EndSegment(slot);
    state.raw->End(slot.stamp[1].Get());
    state.raw->End(slot.disjoint.Get());
    slot.pending = true;
    if (slot.truncated)
        ++state.gpuTruncated;
    state.writing = nullptr;
}

template<class T>
HRESULT GetQuery(ID3D11Query* query, T& data) noexcept
{
    return state.raw->GetData(query, &data, sizeof(T), D3D11_ASYNC_GETDATA_DONOTFLUSH);
}

void ReadGpu(unsigned frame, bool draining)
{
    for (auto& slot : state.slots) {
        if (!slot.pending || (!draining && frame < slot.frame + 4))
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clocks{};
        const HRESULT disjoint = GetQuery(slot.disjoint.Get(), clocks);
        if (disjoint == S_FALSE)
            continue;
        if (FAILED(disjoint) || clocks.Disjoint || !clocks.Frequency) {
            slot.pending = false;
            ++state.gpuDisjoint;
            continue;
        }
        std::array<UINT64, 2 * segmentLimit + 2> stamps{};
        HRESULT hr = S_OK;
        const unsigned count = 2 * slot.num + 2;
        for (unsigned i = 0; i < count && hr == S_OK; ++i)
            hr = GetQuery(slot.stamp[i].Get(), stamps[i]);
        if (hr == S_FALSE)
            continue;
        slot.pending = false;
        if (hr != S_OK || slot.frame >= state.frames.size() || stamps[1] < stamps[0]) {
            ++state.gpuDropped;
            continue;
        }
        const double toMs = 1000.0 / static_cast<double>(clocks.Frequency);
        state.frames[slot.frame].gpuMs = (stamps[1] - stamps[0]) * toMs;
        for (unsigned i = 0; i < slot.num; ++i) {
            if (stamps[2 * i + 3] < stamps[2 * i + 2] || state.gpuRows.size() >= gpuRowLimit)
                continue;
            state.gpuRows.push_back({slot.frame, slot.segments[i], (stamps[2 * i + 3] - stamps[2 * i + 2]) * toMs});
        }
    }
}

bool Pending() noexcept
{
    for (const auto& slot : state.slots) {
        if (slot.pending)
            return true;
    }
    return false;
}

double Us(int64_t ticks) noexcept
{
    return static_cast<double>(ticks) * 1000000.0 / static_cast<double>(state.freq);
}

std::string Utf8(const wchar_t* value)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &result[0], length, nullptr, nullptr);
    result.pop_back();
    return result;
}

void JsonString(FILE* f, const std::string& text) noexcept
{
    std::fputc('"', f);
    for (const unsigned char c : text) {
        if (c == '\\' || c == '"')
            std::fputc('\\', f);
        if (c < 0x20)
            std::fprintf(f, "\\u%04x", static_cast<unsigned>(c));
        else
            std::fputc(c, f);
    }
    std::fputc('"', f);
}

bool CompleteFile(FILE* stream) noexcept
{
    const bool written = std::fflush(stream) == 0 && std::ferror(stream) == 0;
    return std::fclose(stream) == 0 && written;
}

struct NumberLocale {
    _locale_t value = _create_locale(LC_NUMERIC, "C");
    ~NumberLocale() { if (value) _free_locale(value); }
};

template<class... Args>
int Print(FILE* file, const char* format, Args... args) noexcept
{
    static const NumberLocale locale;
    return _fprintf_l(file, format, locale.value, args...);
}

void Export(const char* reason)
{
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t suffix[96];
    swprintf_s(suffix, L"capture-%04u%02u%02uT%02u%02u%02uZ-%lu-%u",
        static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay),
        static_cast<unsigned>(utc.wHour), static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
        state.pid, state.serial);
    const auto path = state.dir + L"zzz-captures";
    CreateDirectoryW(path.c_str(), nullptr);
    const auto stem = path + L"\\" + suffix;
    // Write metadata last. It is the marker that the full capture was written.
    FILE* f = _wfopen((stem + L"-frames.csv").c_str(), L"wb");
    if (!f)
        return;
    std::fputs("frame,present_start_qpc,interval_ms,driver_present_ms,profiler_boundary_us,outer_api_sampled_us,outer_api_timed_calls,present_hr,sync_interval,present_flags,immediate_draw_calls,deferred_draw_calls,dispatch_calls,rt_bind_calls,vertices_requested,indices_requested,stall_calls,slowest_stall_api,slowest_stall_us,gpu_envelope_ms\n", f);
    unsigned validGpu = 0;
    for (unsigned i = 0; i < state.frames.size(); ++i) {
        const auto& row = state.frames[i];
        Print(f, "%u,%lld,%.6f,%.6f,%.3f,%.3f,%llu,%ld,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s,%.3f,",
            i, static_cast<long long>(row.qpc), Us(row.interval) / 1000, Us(row.present) / 1000, Us(row.cost),
            Us(row.apiSampleTicks), row.apiSamples, row.hr, row.syncInterval, row.presentFlags, row.draws, row.deferredDraws, row.dispatches,
            row.targetBinds, row.vertices, row.indices, row.stalls, row.slowOp < 0 ? "" : opNames[row.slowOp], Us(row.slowest));
        if (row.gpuMs >= 0) {
            Print(f, "%.6f", row.gpuMs);
            ++validGpu;
        }
        std::fputc('\n', f);
    }
    if (!CompleteFile(f))
        return;

    f = _wfopen((stem + L"-apis.csv").c_str(), L"wb");
    if (!f)
        return;
    std::fputs("thread,context_kind,api,calls,timed_calls,total_sampled_us,max_sampled_us,stall_calls,amount,possible_repeats,le_2us,le_8us,le_32us,le_128us,le_512us,le_1000us,le_4000us,le_16000us,gt_16000us\n", f);
    for (const auto& thread : state.threads) {
        for (unsigned i = 0; i < thread.total.size(); ++i) {
            const auto& row = thread.total[i];
            if (!row.count)
                continue;
            const char* kind = i >= opCount ? "deferred" : "immediate_or_device";
            Print(f, "%u,%s,%s,%llu,%llu,%.3f,%.3f,%llu,%llu,%llu",
                thread.thread, kind, opNames[i % opCount], row.count, row.samples,
                Us(row.ticks), Us(row.peak), row.stalls, row.amount, row.repeats);
            for (const auto h : row.histogram)
                Print(f, ",%llu", h);
            std::fputc('\n', f);
        }
    }
    if (!CompleteFile(f))
        return;

    f = _wfopen((stem + L"-gpu.csv").c_str(), L"wb");
    if (!f)
        return;
    std::fputs("frame,work,shader_hash,rt_width,rt_height,view_format,rt_slots,depth_only,rt_epoch,wrapped_calls,gpu_ms\n", f);
    for (const auto& row : state.gpuRows) {
        const auto& s = row.segment;
        Print(f, "%u,%s,%016llx,%u,%u,%u,%u,%u,%u,%u,%.6f\n", row.frame,
            workNames[static_cast<unsigned>(s.work)], s.shader, s.target.width, s.target.height, s.target.format,
            s.target.count, static_cast<unsigned>(s.target.depthOnly), s.binds, s.calls, row.ms);
    }
    if (!CompleteFile(f))
        return;

    f = _wfopen((stem + L"-memory.csv").c_str(), L"wb");
    if (!f)
        return;
    std::fputs("frame,local_usage,local_budget,nonlocal_usage,nonlocal_budget\n", f);
    for (const auto& row : state.budgets) {
        Print(f, "%u,", row.frame);
        if (row.hasLocal)
            Print(f, "%llu,%llu", row.local.CurrentUsage, row.local.Budget);
        else
            std::fputc(',', f);
        std::fputc(',', f);
        if (row.hasNonlocal)
            Print(f, "%llu,%llu", row.nonlocal.CurrentUsage, row.nonlocal.Budget);
        else
            std::fputc(',', f);
        std::fputc('\n', f);
    }
    if (!CompleteFile(f))
        return;

    const auto meta = stem + L".json";
    const auto temporary = meta + L".tmp";
    f = _wfopen(temporary.c_str(), L"wb");
    if (!f)
        return;
    const auto adapterName = Utf8(state.adapterDesc.Description);
    const auto cpuName = Utf8(state.cpuName);
    Print(f,
        "{\n  \"schema_version\": 1, \"base_xxmi\": \"v1.3.3\", \"capture_end\": \"%s\","
        "\n  \"pid\": %lu, \"qpc_frequency\": %lld, \"frames\": %zu, \"duration_ms\": %.6f,"
        "\n  \"level\": %u, \"requested_seconds\": %u, \"cpu_sample_every\": %u, \"gpu_sample_every\": %u,"
        "\n  \"stall_us\": %u, \"target_shader\": \"%016llx\", \"width\": %u, \"height\": %u,"
        "\n  \"gpu_attempts\": %u, \"gpu_valid\": %u, \"gpu_unavailable_or_lost\": %u, \"gpu_disjoint_or_error\": %u,"
        "\n  \"gpu_partial_frames\": %u, \"gpu_segment_rows\": %zu,"
        "\n  \"contended_frames\": %u, \"thread_cap_dropped_calls\": %u, \"profiler_boundary_us\": %.3f,"
        "\n  \"vendor_id\": %u, \"device_id\": %u, \"dedicated_video_bytes\": %zu, \"adapter\": ",
        reason, state.pid, state.freq, state.frames.size(), Us(state.lastBoundary - state.started) / 1000,
        state.config.mode, state.config.seconds, state.config.cpuEvery, state.config.gpuEvery, state.config.stallUs,
        state.config.shader, state.swapDesc.Width, state.swapDesc.Height, state.gpuAttempts, validGpu,
        state.gpuDropped, state.gpuDisjoint, state.gpuTruncated, state.gpuRows.size(), state.contendedFrames,
        state.droppedCalls.load(std::memory_order_relaxed), Us(state.profilerTicks),
        state.adapterDesc.VendorId, state.adapterDesc.DeviceId, state.adapterDesc.DedicatedVideoMemory);
    JsonString(f, adapterName);
    std::fputs(", \"cpu\": ", f);
    JsonString(f, cpuName);
    std::fputs(", \"dxgi_driver_version\": ", f);
    if (state.driverKnown)
        Print(f, "\"%u.%u.%u.%u\"", HIWORD(state.driverVersion.HighPart), LOWORD(state.driverVersion.HighPart),
            HIWORD(state.driverVersion.LowPart), LOWORD(state.driverVersion.LowPart));
    else
        std::fputs("null", f);
    std::fputs(", \"exe_file_version\": ", f);
    if (state.gameVersionKnown)
        Print(f, "\"%u.%u.%u.%u\"", state.gameVersion[0], state.gameVersion[1],
            state.gameVersion[2], state.gameVersion[3]);
    else
        std::fputs("null", f);
    std::fputs("\n}\n", f);
    if (CompleteFile(f))
        MoveFileExW(temporary.c_str(), meta.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool IsStallCandidate(Op op) noexcept
{
    switch (op) {
    case Op::MapDriver: case Op::GetData: case Op::GetDataNoFlush: case Op::Unmap: case Op::Update:
    case Op::Copy: case Op::Resolve: case Op::Flush: case Op::ExecuteList: case Op::FinishList:
    case Op::CreateBuffer: case Op::CreateTexture: case Op::CreateQuery:
        return true;
    default:
        return false;
    }
}

void FoldFrame(Frame& frame, bool last)
{
    bool contended = false;
    for (auto& thread : state.threads) {
        std::unique_lock<std::mutex> lock(thread.mutex, std::defer_lock);
        if (last)
            lock.lock();
        else if (!lock.try_lock()) {
            contended = true;
            continue;
        }
        for (unsigned i = 0; i < thread.frame.size(); ++i) {
            auto& stat = thread.frame[i];
            if (!stat.count)
                continue;
            const auto op = static_cast<Op>(i % opCount);
            if (op <= Op::DrawAuto) {
                if (i >= opCount)
                    frame.deferredDraws += stat.count;
                else
                    frame.draws += stat.count;
                if (op == Op::Draw || op == Op::DrawInstanced)
                    frame.vertices += stat.amount;
                if (op == Op::DrawIndexed || op == Op::DrawIndexedInstanced)
                    frame.indices += stat.amount;
            } else if (op == Op::Dispatch || op == Op::DispatchIndirect) {
                frame.dispatches += stat.count;
            } else if (op == Op::RenderTargets) {
                frame.targetBinds += stat.count;
            }
            if (op < Op::DrawOverrides && op != Op::MapDriver) {
                frame.apiSampleTicks += stat.ticks;
                frame.apiSamples += stat.samples;
            }
            frame.stalls += stat.stalls;
            if (stat.stalls && stat.peak >= frame.slowest) {
                frame.slowest = stat.peak;
                frame.slowOp = static_cast<int>(op);
            }
            thread.total[i].merge(stat);
            stat = {};
        }
    }
    state.contendedFrames += contended;
}

void Reset() noexcept
{
    captureMode.store(0, std::memory_order_relaxed);
    state.token.store(0, std::memory_order_release);
    state.device.store(nullptr, std::memory_order_relaxed);
    state.writing = nullptr;
    state.chain = nullptr;
    state.raw = nullptr;
    state.adapter.Reset();
    for (auto& slot : state.slots)
        slot = {};
}

void Finish(const char* reason)
{
    for (const auto& slot : state.slots) {
        if (slot.pending)
            ++state.gpuDropped;
    }
    Reset();
    if (!state.frames.empty())
        Export(reason);
}

bool HotkeyHeld() noexcept
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F8) & 0x8000);
}

bool IsForeground(HWND hwnd) noexcept
{
    return hwnd && GetForegroundWindow() == GetAncestor(hwnd, GA_ROOT);
}

bool Trigger() noexcept
{
    const bool held = HotkeyHeld();
    const bool pressed = held && !state.keyWasDown;
    state.keyWasDown = held;
    return pressed;
}
} // namespace

bool BeforePresent(const void* chain, const void* device, const void* context,
    IDXGISwapChain1* swap, ID3D11Device* rawDevice, ID3D11DeviceContext* rawContext,
    unsigned syncInterval, unsigned flags) noexcept
{
    if (presenting)
        return false;
    try {
        const std::lock_guard<std::mutex> lock(state.presentMutex);
        if (!state.initialized)
            Initialize();
        if (!state.config.mode) {
            assessSwapChain.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!state.chain) {
            const bool requested = state.startRequest &&
                WaitForSingleObject(state.startRequest, 0) == WAIT_OBJECT_0;
            const bool held = HotkeyHeld();
            if (!held)
                state.keyWasDown = false;
            if (!requested && (!held || state.keyWasDown))
                return false;

            const HWND hwnd = Window(swap);
            if (!IsForeground(hwnd) || !rawDevice || !rawContext)
                return false;
            if (requested)
                ResetEvent(state.startRequest);
            state.keyWasDown = held;
            Start(chain, device, context, swap, rawDevice, rawContext);
        }
        if (state.chain != chain)
            return false;
        if (state.owner != GetCurrentThreadId()) {
            Finish("present_thread_changed");
            return false;
        }
        presenting = true;
        state.thisBoundary = Now();
        state.syncInterval = syncInterval;
        state.presentFlags = flags;
        state.stopRequested = Trigger() && IsForeground(state.hwnd);
        CloseGpu();
        state.driverStart = Now();
        return true;
    } catch (...) {
        Reset();
        presenting = false;
        return false;
    }
}

void AfterPresent(const void* chain, HRESULT result) noexcept
{
    if (!presenting)
        return;
    const int64_t driverEnd = Now();
    try {
        const std::lock_guard<std::mutex> lock(state.presentMutex);
        presenting = false;
        if (state.chain != chain)
            return;
        if (state.finishing) {
            // A delayed readback never flushes or waits on the GPU.
            ReadGpu(static_cast<unsigned>(state.frames.size()), true);
            if (!Pending() || ++state.drainFrames >= 120 || driverEnd - state.drainStart >= 4 * state.freq)
                Finish("completed");
            return;
        }
        if (!state.started) {
            if (result != S_OK) {
                Reset();
                return;
            }
            state.started = state.lastBoundary = state.thisBoundary;
            captureMode.store(state.config.mode, std::memory_order_release);
            BeginGpu(0);
            return;
        }
        const unsigned frameIndex = static_cast<unsigned>(state.frames.size());
        const bool terminalError = FAILED(result) && result != DXGI_ERROR_WAS_STILL_DRAWING;
        const bool last = terminalError || state.stopRequested || frameIndex + 1 >= frameLimit ||
            state.thisBoundary - state.started >= state.config.seconds * state.freq;
        if (last) {
            captureMode.store(0, std::memory_order_relaxed);
            state.token.store(0, std::memory_order_release);
        }
        Frame row;
        row.qpc = state.thisBoundary;
        row.interval = state.thisBoundary - state.lastBoundary;
        row.present = driverEnd - state.driverStart;
        row.hr = result;
        row.syncInterval = state.syncInterval;
        row.presentFlags = state.presentFlags;
        state.lastBoundary = state.thisBoundary;
        FoldFrame(row, last);
        state.frames.push_back(row);
        ReadGpu(frameIndex, false);
        if (frameIndex % 60 == 59)
            ReadBudget(frameIndex);
        if (!last)
            BeginGpu(frameIndex + 1);
        state.frames.back().cost = state.driverStart - state.thisBoundary + Now() - driverEnd;
        state.profilerTicks += state.frames.back().cost;
        if (terminalError) {
            Finish("present_error");
        } else if (last) {
            state.finishing = true;
            state.drainStart = driverEnd;
            state.drainFrames = 0;
            if (!Pending())
                Finish("completed");
        }
    } catch (...) {
        Reset();
        presenting = false;
    }
}

void ReleaseSwapChain(const void* chain) noexcept
{
    try {
        const std::lock_guard<std::mutex> lock(state.presentMutex);
        if (state.chain == chain)
            Finish("swapchain_released");
    } catch (...) {
        Reset();
    }
}

void BeginScope(unsigned mode, Op op, const void* device, bool, unsigned& token, int64_t& start) noexcept
{
    if (device && device != state.device.load(std::memory_order_relaxed))
        return;
    const unsigned active = state.token.load(std::memory_order_acquire);
    if (!active)
        return;
    // XXMI timing-only regions beget overlapping measurements and stay outside counts-only mode.
    if ((op >= Op::DrawOverrides || op == Op::MapDriver) && mode < 2)
        return;
    token = active;
    if (mode < 2)
        return;
    // Avoid phase-locking the sample cadence to repeating API sequences.
    if (!rng)
        rng = GetCurrentThreadId() | 1u;
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    if (IsStallCandidate(op) || op >= Op::DrawOverrides || rng % state.config.cpuEvery == 0)
        start = Now();
}

void EndScope(unsigned token, Op op, bool deferred, int64_t start, uint64_t amount, bool repeat) noexcept
{
    const int64_t duration = start ? (std::max)(Now() - start, int64_t{0}) : 0;
    if (token != state.token.load(std::memory_order_acquire))
        return;
    if (tlsToken != token) {
        tlsToken = token;
        tlsSlot = state.nextThread.fetch_add(1, std::memory_order_relaxed);
    }
    if (tlsSlot >= threadLimit) {
        state.droppedCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& thread = state.threads[tlsSlot];
    const std::lock_guard<std::mutex> lock(thread.mutex);
    if (token != state.token.load(std::memory_order_relaxed))
        return;
    if (!thread.thread)
        thread.thread = GetCurrentThreadId();
    auto& stats = thread.frame[static_cast<unsigned>(op) + (deferred ? opCount : 0)];
    ++stats.count;
    stats.amount += amount;
    stats.repeats += repeat;
    if (!start)
        return;
    ++stats.samples;
    stats.ticks += duration;
    stats.peak = (std::max)(stats.peak, static_cast<uint64_t>(duration));
    stats.stalls += IsStallCandidate(op) && duration >= state.stallTicks;
    const uint64_t us = duration * 1000000ULL / state.freq;
    unsigned bucket = 0;
    while (bucket < std::size(histogramUs) && us > histogramUs[bucket])
        ++bucket;
    ++stats.histogram[bucket];
}

bool GpuActive(const void* context) noexcept
{
    if (!gpuToken || captureMode.load(std::memory_order_relaxed) < 2 ||
        gpuToken != state.token.load(std::memory_order_acquire))
        return false;
    return context == state.context && state.writing;
}

void WorkMarker(const void* context, Work work, uint64_t shader) noexcept
{
    if (!GpuActive(context))
        return;
    auto& slot = *state.writing;
    const uint64_t focus = state.config.shader;
    if (state.config.mode == 3 && focus && (shader != focus ||
            (work != Work::Draw && work != Work::Dispatch))) {
        EndSegment(slot);
        return;
    }
    // Level 2 groups graphics work by bound target sequence. Level 3 also splits it by observed shader.
    if (state.config.mode == 2 && work == Work::Draw)
        shader = 0;
    const auto target = (work == Work::Draw || work == Work::Clear) ? state.target : Target{};
    const unsigned epoch = (work == Work::Draw || work == Work::Clear) ? state.targetEpoch : 0;
    if (slot.open) {
        auto& prev = slot.segments[slot.num - 1];
        if (prev.work == work && prev.shader == shader && prev.binds == epoch) {
            ++prev.calls;
            return;
        }
        EndSegment(slot);
    }
    if (slot.num >= (state.config.mode == 3 ? segmentLimit : 20)) {
        slot.truncated = true;
        return;
    }
    state.raw->End(slot.stamp[2 * slot.num + 2].Get());
    slot.segments[slot.num++] = {work, shader, target, epoch, 1};
    slot.open = true;
}

template<class Desc>
void TargetSize(ID3D11View* view, const Desc& desc, unsigned mip) noexcept
{
    state.target.format = desc.Format;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource && SUCCEEDED(resource.As(&texture))) {
        D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);
        state.target.width = (std::max)(1u, textureDesc.Width >> (std::min)(31u, mip));
        state.target.height = (std::max)(1u, textureDesc.Height >> (std::min)(31u, mip));
    }
}

void Targets(const void* context, UINT count, ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* depth) noexcept
{
    if (!GpuActive(context) || count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        return;
    EndSegment(*state.writing);
    ++state.targetEpoch;
    state.target = {};
    state.target.count = count;
    for (unsigned i = 0; views && i < (std::min)(count, 8u); ++i) {
        if (views[i]) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            views[i]->GetDesc(&desc);
            const unsigned mip = desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? desc.Texture2D.MipSlice :
                desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ? desc.Texture2DArray.MipSlice : 0;
            TargetSize(views[i], desc, mip);
            return;
        }
    }
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        depth->GetDesc(&desc);
        const unsigned mip = desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D ? desc.Texture2D.MipSlice :
            desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? desc.Texture2DArray.MipSlice : 0;
        state.target.depthOnly = true;
        TargetSize(depth, desc, mip);
    }
}
} // namespace ZzzCapture
