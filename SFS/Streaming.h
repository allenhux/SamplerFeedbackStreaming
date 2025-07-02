//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <set>
#include <thread>

#include "DebugHelper.h"
#include "d3dx12.h"
#include "SynchronizationUtils.h"

namespace SFS
{
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    //==================================================
    //==================================================
    class UploadBuffer
    {
    public:
        ~UploadBuffer() { if (m_resource.Get()) m_resource->Unmap(0, nullptr); }
        ID3D12Resource* GetResource() const { return m_resource.Get(); }
        ID3D12Resource* Detach() { return m_resource.Detach(); } // release ComPtr reference
        void* GetData() const { return m_pData; }

        void Allocate(ID3D12Device* in_pDevice, UINT in_numBytes,
            D3D12_HEAP_PROPERTIES in_uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD))
        {
            if (m_pData && m_resource.Get())
            {
                m_resource->Unmap(0, nullptr);
            }

            const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(in_numBytes);
            in_pDevice->CreateCommittedResource(
                &in_uploadHeapProperties,
                D3D12_HEAP_FLAG_NONE, &resourceDesc,
                // debug layer says: Buffers are effectively created in state D3D12_RESOURCE_STATE_COMMON
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&m_resource));
            m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_pData));
        }
    private:
        ComPtr<ID3D12Resource> m_resource;
        void* m_pData{ nullptr };
    };

    //==================================================
    //==================================================
    // default is both UINT32 SIMD8 and D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
    // allocator must implement allocate, deallocate, and value_type
    template<typename T, std::size_t ALIGNMENT = 256>
    class AlignedAllocator
    {
    public:
        constexpr AlignedAllocator() noexcept = default;
        constexpr AlignedAllocator(const AlignedAllocator&) noexcept = default;
        template<typename U>
        constexpr AlignedAllocator(AlignedAllocator<U, ALIGNMENT> const&) noexcept {}

        using value_type = T;

        [[nodiscard]] T* allocate(std::size_t n)
        {
            return reinterpret_cast<T*>(::operator new[](n * sizeof(T), (std::align_val_t)ALIGNMENT));
        }
        void deallocate(T* p, [[maybe_unused]] std::size_t n) { ::operator delete[](p, (std::align_val_t)ALIGNMENT); }

        template<class OtherT>
        struct rebind
        {
            using other = AlignedAllocator<OtherT, ALIGNMENT>;
        };
    private:
        static_assert(ALIGNMENT >= alignof(T), "alignment less than sizeof(type)");
    };

    //==================================================
    // hybrid CPU helper. 0 = default policy, 1 = prefer performance, -1 = prefer power efficiency
    // https://www.intel.com/content/www/us/en/developer/articles/guide/12th-gen-intel-core-processor-gamedev-guide.html
    //==================================================
    inline void SetThreadPriority(std::thread& in_thread, int in_priority)
    {
        // default power policy, mask and state = 0
        THREAD_POWER_THROTTLING_STATE throttlingState{ THREAD_POWER_THROTTLING_CURRENT_VERSION, 0, 0 };
        if (in_priority) // 0 = default (do nothing). -1 = efficiency. otherwise, performance.
        {
            // state set to 0 prefers performance (hint to OS to use p-cores)
            throttlingState = { THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0 };
            //  state to "THROTTLING_EXECUTION_SPEED" to hint to OS that it use low-speed processors (e-cores)
            if (-1 == in_priority) { throttlingState.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED; }
        }
        ::SetThreadInformation(in_thread.native_handle(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState));
    }

    //==================================================
    //==================================================
    // remove from container all values matching provided set
    template<typename C, typename V> void ContainerRemove(C& container, const std::set<V>& values)
    {
        std::erase_if(container, [&](auto p) { return values.contains(p); });
    }

    // container remove but not preserving order
    // there must not be duplicates in the container
    template<typename C, typename V> void ContainerRemoveUO(std::vector<C>& container, std::set<V> values)
    {
        UINT numToFind = (UINT)values.size();

        for (UINT i = (UINT)container.size(); (numToFind != 0) && (i != 0);)
        {
            i--;
            if (values.contains(container[i]))
            {
                container[i] = container.back();
                container.resize(container.size() - 1);
                numToFind--;
            }
        }
    }

    // container accessed via lock
    // e.g. LockedContainer<std::vector<Object*>> objects;
    template<typename T> class LockedContainer
    {
    public:
        auto& Acquire() { m_lock.Acquire(); return m_values; }
        void Release() { m_size = m_values.size(); m_lock.Release(); }
        size_t size() { return  m_size; }
        void swap(T& v) { Acquire().swap(v); Release(); }
    protected:
        Lock m_lock;
        T m_values;
        std::atomic<size_t> m_size{ 0 }; // maintained so lock not required
    };
}
