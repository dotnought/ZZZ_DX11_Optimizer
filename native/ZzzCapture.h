// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstdint>

namespace ZzzCapture {

enum class Op : unsigned {
    Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawIndirect, DrawAuto,
    Dispatch, DispatchIndirect, Shader, ShaderResources, ConstantBuffers, Samplers,
    RenderTargets, Blend, DepthStencil, Rasterizer, VertexBuffers, IndexBuffer,
    Map, MapRead, MapNoWait, MapDriver, Unmap, GetData, GetDataNoFlush, QueryMarker,
    Update, Copy, Resolve, Flush, Clear, ExecuteList, FinishList,
    CreateBuffer, CreateTexture, CreateQuery,
    DrawOverrides, DispatchOverrides, CommandList, OverrideLookup, ResourceHash, MapTracking,
    Count
};

enum class Work : unsigned { Draw, Dispatch, Copy, Clear, CommandList };

extern std::atomic<unsigned> captureMode;
extern std::atomic<bool> assessSwapChain;

inline bool EnabledOrUninitialized() noexcept
{
    return assessSwapChain.load(std::memory_order_relaxed);
}

// Frames and query submission are driven by the owning swap chain's Present thread.
bool BeforePresent(const void* chain, const void* device, const void* context,
    IDXGISwapChain1* swap, ID3D11Device* rawDevice, ID3D11DeviceContext* rawContext,
    unsigned syncInterval, unsigned flags) noexcept;
void AfterPresent(const void* chain, HRESULT result) noexcept;
void ReleaseSwapChain(const void* chain) noexcept;

void BeginScope(unsigned mode, Op op, const void* device, bool deferred,
    unsigned& token, int64_t& start) noexcept;
void EndScope(unsigned token, Op op, bool deferred, int64_t start, uint64_t amount, bool repeat) noexcept;

class Scope {
public:
    explicit Scope(Op op, const void* device, bool deferred = false, uint64_t amount = 0) noexcept
        : op_(op), deferred_(deferred), amount_(amount)
    {
        const unsigned mode = captureMode.load(std::memory_order_relaxed);
        if (mode)
            BeginScope(mode, op, device, deferred, token_, start_);
    }

    ~Scope()
    {
        if (token_)
            EndScope(token_, op_, deferred_, start_, amount_, repeat_);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void possibleRepeat(bool repeat) noexcept { repeat_ = repeat; }

private:
    unsigned token_ = 0;
    int64_t start_ = 0;
    Op op_;
    bool deferred_;
    uint64_t amount_;
    bool repeat_ = false;
};

void WorkMarker(const void* context, Work work, uint64_t shader = 0) noexcept;
void Targets(const void* context, UINT count, ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* depth) noexcept;

inline void MarkWork(const void* context, Work work, uint64_t shader = 0) noexcept
{
    if (captureMode.load(std::memory_order_relaxed) >= 2)
        WorkMarker(context, work, shader);
}

inline void MarkTargets(const void* context, UINT count, ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* depth) noexcept
{
    if (captureMode.load(std::memory_order_relaxed) >= 2)
        Targets(context, count, views, depth);
}

} // namespace ZzzCapture
