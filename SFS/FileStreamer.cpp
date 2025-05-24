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

#include "pch.h"

#include "FileStreamer.h"
#include "SFSResourceDU.h"

//-----------------------------------------------------------------------------
// color lookup table
//-----------------------------------------------------------------------------
float SFS::FileStreamer::m_lut[16][3] =
{
    { 1, 1, 1 },             // white
    { 1, 0.25f, 0.25f },     // light red
    { 0.25f, 1, 0.25f },     // light green
    { 0.25f, 0.25f, 1 },     // light blue

    { 1, 0.25f, 1 },         // light magenta
    { 1, 1, 0.25f },         // light yellow
    { 0.25f, 1, 1 },         // light cyan
    { 0.9f, 0.5f, 0.2f },    // orange

    { 0.59f, 0.48f, 0.8f },  // dark magenta
    { 0.53f, 0.25f, 0.11f },
    { 0.8f, 0.48f, 0.53f},
    { 0.64f, 0.8f, 0.48f },

    { 0.48f, 0.75f, 0.8f },
    { 0.5f, 0.25f, 0.75f },
    { 0.99f, 0.68f, 0.42f },
    { .4f, .5f, .6f }
};

//-----------------------------------------------------------------------------
// palettes used for visualization tile data
//-----------------------------------------------------------------------------
BYTE SFS::FileStreamer::m_BC7[SFS::FileStreamer::m_lutSize][D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES];
BYTE SFS::FileStreamer::m_BC1[SFS::FileStreamer::m_lutSize][D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES];

//-----------------------------------------------------------------------------
// constructor
//-----------------------------------------------------------------------------
SFS::FileStreamer::FileStreamer(ID3D12Device* in_pDevice, bool in_traceCaptureMode) : m_traceCaptureMode(in_traceCaptureMode)
{
    in_pDevice->CreateFence(m_copyFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence));
    m_copyFence->SetName(L"FileStreamer::m_copyFence");
    m_copyFenceValue++;

    static bool firstTimeInit = true;
    if (firstTimeInit)
    {
        firstTimeInit = false;
        // add contrast to lut
        UINT numColors = _ARRAYSIZE(m_lut);
        for (UINT i = 0; i < numColors; i++)
        {
            m_lut[i][0] = pow(m_lut[i][0], 1.5f);
            m_lut[i][1] = pow(m_lut[i][1], 1.5f);
            m_lut[i][2] = pow(m_lut[i][2], 1.5f);
        }

        // FIXME? add more formats
        InitializeBC7();
        InitializeBC1();
    }

    if (m_traceCaptureMode)
    {
        m_traces.resize(1);
    }
}

SFS::FileStreamer::~FileStreamer()
{
    // write trace file
    if (m_captureTrace)
    {
        UINT resourceIndex = 0;
        for (const auto& d : m_tracingResources)
        {
            auto& r = m_trace.GetRoot()["resources"][resourceIndex];
            r["rsrc"] = (UINT64)d.first;
            const D3D12_RESOURCE_DESC& desc = d.second;
            r["fmt"] = (UINT32)desc.Format;
            r["dim"][0] = (UINT32)desc.Width;
            r["dim"][1] = (UINT32)desc.Height;
            r["dim"][2] = (UINT32)desc.MipLevels;
            resourceIndex++;
        }

        UINT submitIndex = 0;
        for (const auto& l : m_traces)
        {
            UINT traceIndex = 0;
            for (const auto& t : l)
            {
                auto& r = m_trace.GetRoot()["submits"][submitIndex][traceIndex++];
                r["rsrc"] = (UINT64)t.m_pDstResource;
                r["coord"][0] = t.m_coord.X;
                r["coord"][1] = t.m_coord.Y;
                r["coord"][2] = t.m_coord.Subresource;
                r["file"] = m_files[t.m_pFileHandle];
                r["off"] = t.m_offset;
                r["size"] = t.m_numBytes;
                if (t.m_compressionFormat) { r["comp"] = t.m_compressionFormat; }
            }
            submitIndex++;
        }

        int index = 0;
        while (1)
        {
            std::wstring unique = L"uploadTraceFile_" + std::to_wstring(++index) + L".json";
            if (!std::filesystem::exists(unique))
            {
                std::ofstream ofs(unique, std::ios::out);
                m_trace.Write(ofs);
                break;
            }
        }
    }
}

//-----------------------------------------------------------------------------
// append an upload request to the trace file
//-----------------------------------------------------------------------------
void SFS::FileStreamer::TraceRequest(
    ID3D12Resource* in_pDstResource, const D3D12_TILED_RESOURCE_COORDINATE& in_dstCoord,
    const DSTORAGE_REQUEST& in_request)
{
    m_traces.back().push_back({ in_pDstResource, in_dstCoord,
        in_request.Source.File.Source, in_request.Source.File.Offset,
        in_request.Source.File.Size, in_request.Options.CompressionFormat });

    // find atlas it is writing to
    if (!m_tracingResources.contains(in_pDstResource))
    {
        m_tracingResources[in_pDstResource] = in_pDstResource->GetDesc();
    }
}

//-----------------------------------------------------------------------------
// capture submit() to the trace file
// increments index to next array of requests
//-----------------------------------------------------------------------------
void SFS::FileStreamer::TraceSubmit()
{
    if (m_firstSubmit) // ignore first submit while tracing in case enabled mid-frame.
    {
        m_firstSubmit = false;
        return;
    }
    ASSERT(0 != m_traces.size()); // should never call Submit() without any requests
    ASSERT(0 != m_traces.back().size()); // should never call Submit() without any requests
    m_traces.resize(m_traces.size() + 1);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::FileStreamer::InitializeBC7()
{
    for (UINT i = 0; i < m_lutSize; i++)
    {
        UINT64 block[2] = { 0, 0 };
        // 10 bits for mode
        // first 4 bits 0001 is mode 3 of BC7: 7 bits per channel no alpha
        // 6 bits of partition
        // colors start at bit 10. 
        // 4*7 bits each channel r0..3, g0..3, b0..3
        // then 4 p bits, then indices
        block[0] = 0x08;

        float* color = m_lut[i];

        UINT64 r = std::min(UINT(0x7f * color[0]), (UINT)0x7f);
        UINT64 g = std::min(UINT(0x7f * color[1]), (UINT)0x7f);
        UINT64 b = std::min(UINT(0x7f * color[2]), (UINT)0x7f);

        UINT64 r4 = r | (r << 7) | (r << 14) | (r << 21);
        UINT64 g4 = g | (g << 7) | (g << 14) | (g << 21);
        UINT64 b4 = b | (b << 7) | (b << 14) | (b << 21);

        block[0] |= (r4 << 10);

        // g bit position: 10 + 4*7 bits = 38 bits
        block[0] |= (g4 << 38);
        block[1] = (g4 >> 26); // 28 + 38 = 66, so there were 2 overflow bits. 28 - 2 = 26

        // b is 28 bits after g = 66 bits - 64 = 2
        block[1] |= (b4 << 2);

        UINT64* pDst = (UINT64*)m_BC7[i];

        UINT numBlocks = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES / 16;
        for (UINT j = 0; j < numBlocks; j++)
        {
            pDst[0] = block[0];
            pDst[1] = block[1];
            pDst += 2;
        }
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::FileStreamer::InitializeBC1()
{
    UINT64 block = 0;

    for (UINT i = 0; i < m_lutSize; i++)
    {
        float* color = m_lut[i];

        UINT r = UINT(0x1f * color[0]);
        UINT g = UINT(0x3f * color[1]);
        UINT b = UINT(0x1f * color[2]);

        {
            UINT16* pDst = (UINT16*)&block;
            pDst[0] = (UINT16)((b << 11) | (g << 5) | r);
            //pDst[1] = (UINT16)((b << 11) | (g << 5) | r);
        }

        UINT64* pSrc = &block;
        UINT64* pDst = (UINT64*)m_BC1[i];

        UINT numBlocks = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES / 8;
        for (UINT j = 0; j < numBlocks; j++)
        {
            pDst[j] = pSrc[0];
        }
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void* SFS::FileStreamer::GetVisualizationData(const D3D12_TILED_RESOURCE_COORDINATE& in_coord, DXGI_FORMAT in_format)
{
    static UINT randomColorIndex = 7;

    UINT colorIndex = in_coord.Subresource;
    if (VisualizationMode::DATA_VIZ_TILE == m_visualizationMode)
    {
        colorIndex = randomColorIndex++ & 0x0f;
    }

    colorIndex = std::min(colorIndex, (UINT)15);

    // FIXME? support more formats
    BYTE* pSrc = nullptr;
    switch (in_format)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        pSrc = m_BC1[colorIndex];
        break;
    default:
        pSrc = m_BC7[colorIndex];
    }
    return pSrc;
}
