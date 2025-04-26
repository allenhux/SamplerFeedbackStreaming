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

#include "d3dx12.h"
#include "DDS.h"
#include "XeTexture.h"
#include "DebugHelper.h"

static void Error(std::wstring in_s)
{
    MessageBox(0, in_s.c_str(), L"Error", MB_OK);
    exit(-1);
}

/*-----------------------------------------------------------------------------
DDS format:
UINT32 magic number
DDS_HEADER structure
DDS_HEADER_DXT10 structure
-----------------------------------------------------------------------------*/
SFS::XeTexture::XeTexture(const std::wstring& in_fileName, const XetFileHeader* in_pFileHeader)
    : m_fileName(in_fileName)
{
    if (in_pFileHeader)
    {
        m_fileHeader = *in_pFileHeader;
    }
    else
    {
        std::ifstream inFile(in_fileName.c_str(), std::ios::binary);
        ASSERT(!inFile.fail()); // File doesn't exist?

        inFile.read((char*)&m_fileHeader, sizeof(m_fileHeader));
        ASSERT(inFile.good()); // Unexpected Error reading header
        inFile.close();
    }

    ASSERT(m_fileHeader.m_magic == XetFileHeader::GetMagic()); // valid XET file?
    ASSERT(m_fileHeader.m_version = XetFileHeader::GetVersion()); // correct XET version?
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::XeTexture::LoadTileInfo()
{
    std::ifstream inFile(m_fileName.c_str(), std::ios::binary);
    ASSERT(!inFile.fail()); // File doesn't exist?

    inFile.seekg(sizeof(m_fileHeader), std::ios::beg); // skip the header...

    m_subresourceInfo.resize(m_fileHeader.m_ddsHeader.mipMapCount);
    inFile.read((char*)m_subresourceInfo.data(), m_subresourceInfo.size() * sizeof(m_subresourceInfo[0]));
    ASSERT(inFile.good()); // Unexpected Error reading subresource info

    m_tileOffsets.resize(m_fileHeader.m_mipInfo.m_numTilesForStandardMips + 1); // plus 1 for the packed mips offset & size
    inFile.read((char*)m_tileOffsets.data(), m_tileOffsets.size() * sizeof(m_tileOffsets[0]));
    ASSERT(inFile.good()); // Unexpected Error reading packed mip info

    inFile.close();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
UINT SFS::XeTexture::GetPackedMipFileOffset(UINT* out_pNumBytesTotal, UINT* out_pNumBytesUncompressed) const
{
    UINT packedOffset = m_tileOffsets[m_fileHeader.m_mipInfo.m_numTilesForStandardMips].m_offset;
    *out_pNumBytesTotal = m_tileOffsets[m_fileHeader.m_mipInfo.m_numTilesForStandardMips].m_numBytes;
    *out_pNumBytesUncompressed = m_fileHeader.m_mipInfo.m_numUncompressedBytesForPackedMips;
    return packedOffset;
}

//-----------------------------------------------------------------------------
// compute linear tile index from subresource info
//-----------------------------------------------------------------------------
UINT SFS::XeTexture::GetLinearIndex(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) const
{
    const auto& data = m_subresourceInfo[in_coord.Subresource].m_standardMipInfo;
    return data.m_subresourceTileIndex + (in_coord.Y * data.m_widthTiles) + in_coord.X;
}

//-----------------------------------------------------------------------------
// return value is byte offset into file
//-----------------------------------------------------------------------------
SFS::XeTexture::FileOffset SFS::XeTexture::GetFileOffset(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) const
{
    // use index to look up file offset and number of bytes
    UINT index = GetLinearIndex(in_coord);
    FileOffset fileOffset;
    fileOffset.numBytes = m_tileOffsets[index].m_numBytes;
    fileOffset.offset = m_tileOffsets[index].m_offset;
    return fileOffset;
}
