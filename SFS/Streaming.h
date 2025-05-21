//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************

#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <thread>
#include <synchapi.h>
#pragma comment(lib, "Synchronization.lib")

#include "DebugHelper.h"
#include "d3dx12.h"

namespace SFS
{
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    using BarrierList = std::vector<D3D12_RESOURCE_BARRIER>;

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
        constexpr AlignedAllocator(AlignedAllocator<U, ALIGNMENT> const&) noexcept
        {}

        using value_type = T;

        [[nodiscard]] T* allocate(std::size_t n)
        {
            return reinterpret_cast<T*>(::operator new[](n * sizeof(T), m_alignment));
        }
        void deallocate(T* p, [[maybe_unused]] std::size_t n) { ::operator delete[](p, m_alignment); }

        template<class OtherT>
        struct rebind
        {
            using other = AlignedAllocator<OtherT, ALIGNMENT>;
        };
    private:
        static_assert(ALIGNMENT >= alignof(T), "alignment less than sizeof(type)");
        static std::align_val_t constexpr m_alignment{ ALIGNMENT };
    };

    //==================================================
    //==================================================
    inline void SetThreadPriority(std::thread& in_thread, int in_priority)
    {
        if (in_priority) // 0 = default (do nothing). -1 = efficiency. otherwise, performance.
        {
            THREAD_POWER_THROTTLING_STATE throttlingState{ THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0 };
            if (-1 == in_priority) { throttlingState.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED; } // speed, speed = prefer e cores
            ::SetThreadInformation(in_thread.native_handle(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState));
        }
    }

    //==================================================
    // multiple threads may wait on this flag, which may be set by any number of threads
    //==================================================
    class SynchronizationFlag
    {
    public:
        void Set()
        {
            m_flag = true;
            WakeByAddressAll(&m_flag);
        }

        void Wait()
        {
            // note: msdn recommends verifying that the value really changed, but we're not.
            bool undesiredValue = false;
            WaitOnAddress(&m_flag, &undesiredValue, sizeof(bool), INFINITE);
            m_flag = false;
        };
    private:
        bool m_flag{ false };
    };

    //==================================================
    // Spin Lock using expensive wait
    //==================================================
    class SpinLock
    {
    public:
        void Acquire()
        {
            const std::uint32_t desired = 1;
            bool waiting = true;

            while (waiting)
            {
                std::uint32_t expected = 0;
                waiting = !m_lock.compare_exchange_weak(expected, desired);
            }
        }

        void Release()
        {
            ASSERT(0 != m_lock);
            m_lock = 0;
        }

        bool TryAcquire()
        {
            std::uint32_t expected = 0;
            const std::uint32_t desired = 2; //  different value useful for debugging
            return m_lock.compare_exchange_weak(expected, desired);
        }
    private:
        std::atomic<uint32_t> m_lock{ 0 };
    };

    //==================================================
    // Efficient Lock using Sync Flag
    //==================================================
    class Lock
    {
    public:
        void Acquire()
        {
            const std::uint32_t desired = 1;
            bool waiting = true;

            while (waiting)
            {
                std::uint32_t expected = 0;
                waiting = !m_lock.compare_exchange_weak(expected, desired);
                if (waiting)
                {
                    m_flag.Wait();
                }
            }
        }

        void Release()
        {
            ASSERT(0 != m_lock);
            m_lock = 0;
            m_flag.Set();
        }

        bool TryAcquire()
        {
            std::uint32_t expected = 0;
            const std::uint32_t desired = 2; //  different value if just trying
            return m_lock.compare_exchange_weak(expected, desired);
        }
    private:
        std::atomic<uint32_t> m_lock{ 0 };
        SynchronizationFlag m_flag;
    };

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
}
