//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once
#include "FileStreamer.h"
#include "SimpleAllocator.h"

//=======================================================================================
//=======================================================================================
namespace SFS
{
    class FileStreamerReference : public FileStreamer
    {
    public:
        FileStreamerReference(ID3D12Device* in_pDevice,
            UINT in_maxNumCopyBatches,               // maximum number of in-flight batches
            UINT in_maxTileCopiesInFlight);          // upload buffer size. 1024 would become a 64MB upload buffer
        virtual ~FileStreamerReference();

        virtual FileHandle* OpenFile(const std::wstring& in_path) override;
        virtual void StreamTexture(SFS::UpdateList& in_updateList) override;
        virtual void StreamPackedMips(SFS::UpdateList& in_updateList) override;

        virtual void Signal() override {} // reference auto-submits

        static const UINT MEDIA_SECTOR_SIZE = 4096; // see https://docs.microsoft.com/en-us/windows/win32/fileio/file-buffering
    private:
        class FileHandleReference : public FileHandle
        {
        public:
            FileHandleReference(HANDLE in_handle) : m_handle(in_handle) {}
            virtual ~FileHandleReference() { ::CloseHandle(m_handle); }

            HANDLE GetHandle() const { return m_handle; }
        private:
            const HANDLE m_handle;
        };

        std::wstring m_fileName; // only used for packed mips hack

        ComPtr<ID3D12CommandQueue> m_copyCommandQueue;
        ComPtr<ID3D12GraphicsCommandList> m_copyCommandList;

        class CopyBatch
        {
        public:
            enum class State : UINT
            {
                FREE = 0,
                ALLOCATED,
                COPY_TILES,
                WAIT_COMPLETE,
            };

            std::atomic<State> m_state{ State::FREE };

            SFS::UpdateList* m_pUpdateList{ nullptr };
            std::vector<UINT> m_uploadIndices; // indices into upload buffer. also serves as indices into the shared array of event handles.
            UINT64 m_copyFenceValue{ 0 }; // tracked independently from UpdateList so CopyBatch lifetime can be independent

            ID3D12CommandAllocator* GetCommandAllocator()
            {
                m_commandAllocator->Reset();
                return m_commandAllocator.Get();
            }

            // call only once
            void Init(ID3D12Device* in_pDevice);

            UINT m_copyStart{ 0 };
            UINT m_copyEnd{ 0 };

            UINT m_numEvents{ 0 };
            UINT m_lastSignaled{ 0 };

        private:
            ComPtr<ID3D12CommandAllocator> m_commandAllocator;
        };

        struct Request : public OVERLAPPED
        {
            Request() { hEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr); }
            ~Request() { ::CloseHandle(hEvent); }
        };
        std::vector<Request> m_requests;

        // close command list, execute on m_copyCommandQueue, signal fence, increment fence value
        void ExecuteCopyCommandList(ID3D12GraphicsCommandList* in_pCmdList);

        std::vector<CopyBatch> m_copyBatches;
        UINT m_batchAllocIndex{ 0 }; // allocation optimization

        // structure for finding space to upload tiles
        SFS::SimpleAllocator m_uploadAllocator;
        SFS::UploadBuffer m_uploadBuffer;

        void CopyThread();
        std::atomic<bool> m_copyThreadRunning{ false };
        std::thread m_copyThread;

        void LoadTexture(CopyBatch& in_copyBatch, UINT in_numtilesToLoad);
        void CopyTiles(ID3D12GraphicsCommandList* out_pCopyCmdList, ID3D12Resource* in_pSrcResource,
            const UpdateList* in_pUpdateList, const std::vector<UINT>& in_indices);

        static HANDLE GetFileHandle(const FileHandle* in_pHandle) { return dynamic_cast<const FileHandleReference*>(in_pHandle)->GetHandle(); }
    };
}
