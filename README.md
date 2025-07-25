# Sampler Feedback Streaming With DirectStorage

*Latest Updates*:
- Many performance improvements reduce main-thread overhead and improve internal thread performance with large numbers of resources
- Resolve directly from GPU feedback buffers to CPU readback buffers, avoiding temporary memory and per-frame copies
- Enabled multi-threaded resource creation and destruction 
- Uses GPU Upload Heaps (new D3D12 Agility SDK feature), requiring developer mode.
- Added application frustum culling and screen-area heuristic to not QueueFeedback to off-screen or small objects that wouldn't benefit from higher-resolution textures.

## Introduction

This repository contains an [MIT licensed](LICENSE) __implementation library__ and demo of _DirectX12 Sampler Feedback Streaming_, a technique that dramatically improves visual quality by enabling incredible scene detail: on-screen textures that, if fully loaded, may require _100 to 1000 times_ the GPU physical memory. That is, 1TB of imagery using just 1GB of a GPU's 8GB of physical memory.

Here's an example with 939 _gigabytes_ of textures (4,122 16k x 16k) using 412 _megabytes_ of memory (within a 1.5GB heap) on a GPU with 8GB of physical memory (the hardware is limited to 40 bits of address space == 1TB). GPU time spent on feedback is limited to 2ms (settable via a slider under the UI "More Options").

![Sample screenshot](./readme-images/sampler-feedback-streaming-1TB.jpg "Sample screenshot")
Textures derived from [Hubble Images](https://www.nasa.gov/mission_pages/hubble/multimedia/index.html), see the [Hubble Copyright](https://hubblesite.org/copyright)

Note: textures appear to repeat because the files are re-used. They are all treated as separate D3D resources in the sample, though Sampler Feedback resources _can_ be used by multiple objects.

## How? (light version)
The library brings together 3 systems:
1. [DirectStorage](https://devblogs.microsoft.com/directx/directstorage-api-downloads/) "a feature intended to allow games to make full use of high-speed storage (such as NVMe SSDs) that can can deliver multiple gigabytes a second of small (eg 64kb) data reads with minimal CPU overhead."
2. [DirectX12 Sampler Feedback](https://microsoft.github.io/DirectX-Specs/d3d/SamplerFeedback.html) "a Direct3D feature for capturing and recording texture sampling information and locations. Without sampler feedback, these details would be opaque to the developer".
3. [Reserved Resources](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource) "a resource with virtual memory only, no backing store" also known as _tiled_ or _sparse_ textures.

The scene is rendered, with "feedback" resources capturing which texels were sampled. The per-texel feedback is "resolved" to texture regions that match the tiled resource size, e.g. 512x256 for BC1 format textures. The required tiles are then loaded from disk - yes, the image is drawn before the required data has been loaded! While the (compressed) tiles are loaded via DirectStorage, they are simultaneously mapped into the address space of the target texture (loads _do not wait_ for UpdateTileMappings to complete). When tiles arrive or are evicted, a texture sampler clamp is altered (via CPU write to upload heap) so sampling only accesses resident portions of the texture.

The image settles quickly with a fast SSD, and the delay is usually imperceptable because lower-resolution mips are being averaged with higher-resolution mips as a natural part of texture filtering (anisotropic or trilinear). The weight of the higher resolution data is low when an object is further away, giving time for the data to arrive before the weight increases until blurring would be apparent.

See also:
- [Intel's 1.1 announcement blog](https://www.intel.com/content/www/us/en/developer/articles/news/directstorage-on-intel-gpus.html)
- [Microsoft's 1.1 announcement blog](https://devblogs.microsoft.com/directx/directstorage-1-1-now-available/)

- [GDC 2021 video](https://software.intel.com/content/www/us/en/develop/events/gdc.html?videoid=6264595860001) [(alternate link)](https://www.youtube.com/watch?v=VDDbrfZucpQ) which provides an overview of Sampler Feedback and discusses this sample [starting at about 15:30.](https://www.youtube.com/watch?v=VDDbrfZucpQ&t=936s)
- [GDC 2021 presentation](https://software.intel.com/content/dam/develop/external/us/en/documents/pdf/july-gdc-2021-sampler-feedback-texture-space-shading-direct-storage.pdf) in PDF form

Notes:
- while multiple objects can share the same DX texture and source file, this sample aims to demonstrate the possibility of every object having a unique resource. Hence, every texture is treated as though unique, though the same source file may be used multiple times.
- the repo does not include all textures shown above (they total over 13GB). A few 16k x 16k textures are available as [release 1](https://github.com/GameTechDev/SamplerFeedbackStreaming/releases/tag/1) and  [release 2](https://github.com/GameTechDev/SamplerFeedbackStreaming/releases/tag/2)
- the file format has changed since large textures were provided as "releases." See the [log](#log) below.
- this repository depends on DirectStorage for Windows&reg; version 1.1.0 from https://www.nuget.org/packages/Microsoft.Direct3D.DirectStorage/
- at build time, BCx textures (BC7 and BC1 tested) in the dds/ directory are converted into the custom .XET format and placed in the ($TargetDir)/media directory (e.g. x64/Release/media). A few dds files are included.

## Requirements:
- minimum:
    - Windows 11
    - GPU with D3D12 Sampler Feedback Support
    - nvme SSD with PCIe gen3 or later
- recommended:
    - GPU supporting [Upload Heaps](https://microsoft.github.io/DirectX-Specs/d3d/D3D12GPUUploadHeaps.html)

Tested on Win11 24H2, nvidia 4xxx, amd 780m

## Build Instructions

Download the source. Build (release mode recommended) [SamplerFeedbackStreaming.sln](SamplerFeedbackStreaming.sln). Run the __Expanse__ project.

All executables, scripts, configurations, and media files will be found in the x64/Release or x64/Debug directories. You can run from within the Visual Studio IDE or from the command line, e.g.:

    c:\SamplerFeedbackStreaming\x64\Release> expanse.exe

By default (no command line options) the view will focus on a "terrain" object which allows for exploring sampler feedback streaming. Note the UI region labeled "Terrain Object Feedback Viewer."

In the top right find 2 views:
- the solid color texture represents the texture clamp on the terrain texture, with colors representing mip levels: white is mip 0, red is mip 1, and green is mip 2.
- the next column is the first 3 mip levels of the texture. Note how the white region of the left image corresponds to the non-black region of the top-right image; those are the tiles that have been loaded into the sparse texure. The "scrambled" tiles were previously evicted and recycled for a different texture with a different format (BC7 vs. BC1).

If you click-drag the screen or navigate (arrow keys and WASD) you can watch the tiles dynamically load.

Note that navigation in this mode has the up direction locked, which can be disabled in the UI.

![default startup](./readme-images/default-startup.jpg "default startup")

Try:
- Press the DEMO MODE button or run the batch file _demo.bat_ to see streaming in action.
- While in demo mode, toggle _Roller Coaster_ mode (page down) to fly through the scene.
- Toggle _Tile Min Mip Overlay_ (page up) to toggle a visualization of the tiles loading
- Press "home" to toggle the UI
- Press "end" to toggle the feedback viewer

Note: keyboard controls are inactive while the _Camera_ slider is non-zero.

Benchmark mode generates massive disk traffic by cranking up the animation rate, dialing up the sampler bias, and rapidly switching between two camera paths to force eviction of all the current texture tiles. This mode is designed to stress the whole platform, from storage to PCIe interface to CPU and GPU.

## Keyboard controls

* `qwe / asd` : strafe left, forward, strafe right / rotate left, back, rotate right
* `z c` : roll left (counter/clockwise), roll right (clockwise) *unless up-lock engaged*
* `f v` : vertical up, down
* `arrow keys` : rotate left/right, pitch down/up
* `shift` : move faster
* `mouse left-click drag` : rotate view
* `page up` : toggle the min mip map overlay onto every object (visualize tiles loading)
* `page down` : while camera animation is non-zero, toggles fly-through "rollercoaster" vs. fly-around "orbit"
* `space` : toggles camera animation on/off.
* `home` : toggles UI.
* `shift-home`: toggles mini-UI.
* `insert` : toggles frustum visualization
* `esc` : while windowed, exit. while full-screen, return to windowed mode

## Configuration files and command lines

For a full list of command line options, pass the command line "?", e.g.

    c:> expanse.exe ?

Most of the detailed settings for the system can be found in the default configuration file [config.json](config/config.json). You can replace this configuration with a custom configuration filewith the '-config' command line:

    -config myconfig.json

The options in the json have corresponding command lines, e.g.:

json:

    "mediaDir" : "media"

equivalent command line:

    -mediadir media

## Creating Your Own Textures

The executable `DdsToXet.exe` converts BCn DDS textures to the custom XET format. Only BC1 and BC7 textures have been tested. Usage:

    c:> ddstoxet.exe -in myfile.dds -out myfile.xet

The batch file [convert.bat](scripts/convert.bat) will read all the DDS files in one directory and write XET files to a second directory. The output directory must exist.

    c:> convert c:\myDdsFiles c:\myXetFiles

## Sample Textures
Two sets of high resolution textures are available for use with "demo-hubble.bat": [hubble-16k.zip](https://github.com/GameTechDev/SamplerFeedbackStreaming/releases/tag/1) and [hubble-16k-bc1.zip](https://github.com/GameTechDev/SamplerFeedbackStreaming/releases/tag/2)). BUT they are in an older file format. Simply drop them into the "dds" directory and rebuild DdsToXet, or convert them to the new file format with `convert.bat` (see below). Make sure the mediadir in the batch file is set properly, or override it on the command line as follows:

    c:\SamplerFeedbackStreaming\x64\Release> demo-hubble.bat -mediadir c:\hubble-16k

## Performance Profiling
A new DirectStorage trace capture and playback utility has been added so DirectStorage performance can be analyzed without the overhead of rendering. For example, to capture and play back the DirectStorage requests and submits for 500 "stressful" frames with a staging buffer size of 128MB, cd to the build directory and:
```
stress.bat -timingstart 200 -timingstop 700 -capturetrace
traceplayer.exe -file uploadTraceFile_1.json -mediadir media -staging 128
```
# SFS: a library for streaming textures

The sample includes a library **SFS** with a minimal set of APIs defined in [SamplerFeedbackStreaming.h](SFS/SamplerFeedbackStreaming.h). The central object, *SFSManager*, allows for the creation of streaming textures and heaps. These objects handle all the feedback resource creation, readback, processing, and file/IO.

The application creates an **SFSManager** and 1 or more heaps in Scene.cpp:

```cpp
m_pSFSManager = SFSManager::Create(desc);

// create 1 or more heaps to contain our StreamingResources
for (UINT i = 0; i < m_args.m_numHeaps; i++)
{
    m_sharedHeaps.push_back(m_pSFSManager->CreateHeap(m_args.m_sfsHeapSizeMB));
}

```

Each SceneObject has its own SFSResource. Note **an SFSResource can be used by multiple objects**, but this sample was designed to emphasize the ability to manage many resources and so objects are 1:1 with SFSResources.

```cpp
m_pSFSManager->CreateResource(m_sfsResourceDescs[fileIndex], pHeap, textureFilename)
```

## How It Works

This implementation of Sampler Feedback Streaming uses DX12 Sampler Feedback in combination with DX12 Reserved Resources, aka Tiled Resources. A multi-threaded CPU library processes feedback from the GPU, makes decisions about which tiles to load and evict, loads data from disk storage, and submits mapping and uploading requests via GPU copy queues.

Frame rate is not dependent on completion of copy commands; the GPU never waits, even while textures are modified mid-frame. The CPU writes asynchronously to buffers that clamp texture sampling so the GPU always reads valid data. GPU time is mostly a function of the Sampler Feedback Resolve() operations (described below) and GPU decompression when using DirectStorage. The CPU threads run continuously and asynchronously from the GPU (pausing when there's no work to do), polling fence completion states to determine when feedback is ready to process and when copies and memory mapping has completed.

All the magic can be found in  the **SFS** library (see  [SamplerFeedbackStreaming.h](SFS/SamplerFeedbackStreaming.h)), which abstracts the creation of streaming resources and heaps while internally managing feedback resources, file I/O, and GPU memory mapping.

The technique works as follows:

## 1. Create a Texture to be Streamed

The streaming textures are allocated as DX12 [Reserved Resources](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource), which behave like [VirtualAlloc](https://docs.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) in C. Each resource takes no physical GPU memory until 64KB regions of the resource are committed in 1 or more GPU [heaps](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createheap). The x/y dimensions of a reserved resource tile is a function of the texture format, such that it fills a 64KB GPU memory page. For example, BC7 textures have 256x256 tiles, while BC1 textures have 512x256 tiles.

In Expanse, each tiled resource corresponds to a single .XeT file on a hard drive (though multiple resources can point to the same file). The file contains dimensions and format, but also information about how to access the tiles within the file.

## 2. Create and Pair a Min-Mip Feedback Map

To use sampler feedback, we create a feedback resource corresponding to each streaming resource, with identical dimensions to record information about which texels were sampled.

For this streaming usage, we use the min mip feedback feature by [creating the resource](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device8-createcommittedresource2) with the format DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE. We set the region size of the feedback to match the tile dimensions of the tiled resource (streaming resource) through the SamplerFeedbackRegion member of [D3D12_RESOURCE_DESC1](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc1).

For the feedback to be written by GPU shaders (in this case, pixel shaders) the texture and feedback resources must be paired through a view created with [CreateSamplerFeedbackUnorderedAccessView](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device8-createsamplerfeedbackunorderedaccessview).

## 3. Draw Objects While Recording Feedback

For expanse, there is a "normal" non-feedback shader named [terrainPS.hlsl](src/shaders/terrainPS.hlsl) and a "feedback-enabled" version of the same shader, [terrainPS-FB.hlsl](src/shaders/terrainPS-FB.hlsl). The latter simply writes feedback using [WriteSamplerFeedback](https://microsoft.github.io/DirectX-Specs/d3d/SamplerFeedback.html) HLSL intrinsic, using the same sampler and texture coordinates, then calls the prior shader. Compare the WriteSamplerFeedback() call below to to the Sample() call above.

To add feedback to an existing shader:

1. include the original shader hlsl
2. add binding for the paired feedback resource
3. call the WriteSamplerFeedback intrinsic with the resource and sampler defined in the original shader
4. call the original shader

```cpp
#include "terrainPS.hlsl"

FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> g_feedback : register(u0);

float4 psFB(VS_OUT input) : SV_TARGET0
{
    g_feedback.WriteSamplerFeedback(g_streamingTexture, g_sampler, input.tex.xy);

    return ps(input);
}
```
## 4. Process Feedback
Sampler Feedback resources are opaque, and must be *Resolved* before interpretting on the CPU.

Resolving feedback for one resource is inexpensive, but adds up when there are 1000s of objects. Expanse has a configurable time limit for the amount of feedback resolved each frame. Feedback-enabled shaders are only used for a subset of resources such that the amount of feedback produced can be resolved within the time limit. You can find the time limit estimation, the eviction optimization, and the request to gather sampler feedback by searching [Scene.cpp](src/Scene.cpp) for the following:

- SFSManager **GetGpuTexelsPerMs()** provides a metric to help the application decide how many resources to request feedback from, based on the resource dimensions
- SFSResource **QueueEviction** tells the runtime to evict all tiles for this resource, e.g. if the object is outside the view frustum or far enough away as to not need higher resolution texture data (determined with an estimate of on-screen area)
- SceneObject **SetFeedbackEnabled** results in 2 actions:
    1. tell the runtime to collect feedback for this object via SFSResource::QueueFeedback()
    2. use the feedback-enabled pixel shader for this object

## 5. Determine Which Tiles to Load & Evict
The resolved Min mip feedback tells us the minimum mip tile that should be loaded. The min mip feedback is traversed, updating an internal reference count for each tile. If a tile previously was unused (ref count = 0), it is queued for loading from the bottom (highest mip) up. If a tile is not needed for a particular region, its ref count is decreased (from the top down). When its ref count reaches 0, it might be ready to evict.

Data structures for tracking reference count, residency state, and heap usage can be found in [ResourceBase.cpp](SFS/ResourceBase.cpp) and [ResourceBase.h](SFS/ResourceBase.h), look for TileMappingState. This class also has methods for interpreting the feedback buffer (ProcessFeedback) and updating the residency map (UpdateMinMipMap), which execute concurrently in separate CPU threads.

Tiles can only be evicted if there are no lower-mip-level tiles that depend on them, e.g. a mip 1 tile may have four mip 0 tiles "above" it in the mip hierarchy, and may only be evicted if all 4 of those tiles have also been evicted. The ref count helps us determine this dependency.

A tile also cannot be evicted if it is being used by an outstanding draw command. We prevent this by  delaying evictions a frame or two depending on swap chain buffer count (i.e. double or triple buffering). If a tile is needed before the eviction delay completes, the tile is simply rescued from the pending eviction data structure instead of being re-loaded.

The mechanics of loading, mapping, and unmapping tiles is all contained within the DataUploader class, which depends on a [FileStreamer](SFS/FileStreamer.h) class to do the actual tile loads.

### 6. Update Residency Map

Because textures are only partially resident, we only want the pixel shader to sample resident portions. Sampling texels that are not physically mapped that returns 0s, resulting in undesirable visual artifacts. To prevent this, we clamp all sampling operations based on a **residency map**. The residency map is relatively tiny: for a 16k x 16k BC7 texture, which would take 350MB of GPU memory, we only need a 4KB residency map. Note that the lowest-resolution "packed" mips are loaded for all objects, so there is always something available to sample. See also [GetResourceTiling](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getresourcetiling).

When a texture tile has been loaded or evicted by SFSManager, it updates the corresponding residency map. The residency map is an application-generated representation of the minimum mip available for each region in the texture, and is described in the [Sample Feedback spec](https://microsoft.github.io/DirectX-Specs/d3d/SamplerFeedback.html) as follows:

```
The MinMip map represents per-region mip level clamping values for the tiled texture; it represents what is actually loaded.
```

Below, the Visualization mode was set to "Color = Mip" and labels were added. SFSManager processes the Min Mip Feedback (left window in top right), uploads and evicts tiles to form a Residency map, which is a proper min-mip-map (right window in top right). The contents of memory can be seen in the partially resident mips along the bottom (black is not resident). The last 3 mip levels are never evicted because they are packed mips (all fit within a 64KB tile). In this visualization mode, the colors of the texture on the bottom correspond to the colors of the visualization windows in the top right. Notice how the resident tiles do not exactly match what feedback says is required.
![Expanse UI showing feedback and residency maps](./readme-images/labels.jpg "Expanse UI showing Min Mip Feedback, Residency Map, and Texture Mips (labels added)")

To reduce GPU memory, a single combined buffer contains all the residency maps for all the resources. The pixel shader samples the corresponding residency map to clamp the sampling function to the minimum available texture data available, thereby avoiding sampling tiles that have not been mapped.

We can see the lookup into the residency map in the pixel shader [terrainPS.hlsl](src/shaders/terrainPS.hlsl). Resources are defined at the top of the shader, including the reserved (tiled) resource g_streamingTexture, the residency map g_minmipmap, and the sampler:

```cpp
Texture2D g_streamingTexture : register(t0);
Buffer<uint> g_minmipmap: register(t1);
SamplerState g_sampler : register(s0);
```

The shader offsets into its region of the residency map (g_minmipmapOffset) and loads the minimum mip value for the region to be sampled.

```cpp
    int2 uv = input.tex * g_minmipmapDim;
    uint index = g_minmipmapOffset + uv.x + (uv.y * g_minmipmapDim.x);
    uint mipLevel = g_minmipmap.Load(index);
```

The sampling operation is clamped to the minimum mip resident (mipLevel).

```cpp
    float3 color = g_streamingTexture.Sample(g_sampler, input.tex, 0, mipLevel).rgb;
```

## 7. Putting it all Together

There is some work that needs to be done before drawing objects that use feedback (clearing feedback resources), and some work that needs to be done after (resolving feedback resources). SFSManager creates these commands, but does not execute them. Each frame, a command list must be built and submitted with the application draw commands, which you can find just before the call to Present() in [Scene.cpp](src/Scene.cpp) as follows:

```cpp
auto pCommandList = m_pSFSManager->EndFrame(minmipmapDescriptor);

ID3D12CommandList* pCommandLists[] = { m_commandList.Get(), pCommandList };

m_commandQueue->ExecuteCommandLists(_countof(pCommandLists), pCommandLists);
```

# Known issues
* On some architectures (AMD 780M) GPU feedback is not stable frame-to-frame when there is no motion.
* full-screen with multi-gpu or remote desktop is not borderless
* entering full screen in a multi-gpu system moves the window to a monitor attached to the GPU by design. However, if the window starts on a different monitor, it "disappears" on the first maximization. Hit *escape* then maximize again, and it should work fine.

# Old Issues (no longer encountered)

## Performance Degradation

(2025: Mostly fixed via Microsoft improvements to Tiled Resource mapping operations?)

On some architectures, performance degrades over time. This can be seen in the bandwidth graph in benchmark mode after a few minutes. Compare the following healthy graph to the graph containing stalls below:

- healthy: ![Healthy Streaming](./readme-images/streamingHealthy.png "Healthy Streaming")
- stalling: ![Streaming Stalls](./readme-images/streamingStalls.png "Streaming Stalls")

As a workaround, try the command line `-config fragmentationWA.json` , e.g.:

    c:\SamplerFeedbackStreaming\x64\Release> demo.bat -config fragmentationWA.json
    c:\SamplerFeedbackStreaming\x64\Release> stress.bat -mediadir c:\hubble-16k -config fragmentationWA.json

The issue (which does not affect Intel GPUs) is the tile allocations in the heap becoming fragmented relative to resources. Specifically, the CPU time for [UpdateTileMappings](https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-updatetilemappings) gradually increases causing the streaming system to stall waiting for pending operations to complete. The workaround reduces fragmentation by distributing streaming resources across multiple small heaps (vs. a single large heap), which can result in visual artifacts if the small heaps fill. To mitigate the small heaps filling, more total heap memory is allocated. There may be other (unexplored) solutions, e.g. perhaps by implementing a hash in the tiled heap allocator. This workaround adjusts two properties:

    "heapSizeTiles": 512, // size for each heap. 64KB per tile * 512 tiles -> 32MB heap
    "numHeaps": 127, // number of heaps. streaming resources will be distributed among heaps

## Cracks between tiles

The demo exhibits texture cracks due to the way feedback is used. Feedback is always read *after* drawing, resulting in loads and evictions corresponding to that frame only becoming available for a future frame. That means we never have exactly the texture data we need when we draw (unless no new data is needed). Most of the time this isn't perceptible, but sometimes a fast-moving object enters the view resulting in visible artifacts.

In this case, the hardware sampler is reaching across tile boundaries to perform anisotropic sampling, but encounters tiles that are not physically mapped. D3D12 Reserved Resource tiles that are not physically mapped return black to the sampler. This could be mitigated by dilating or eroding the min mip map such that there is no more than 1 mip level difference between neighboring tiles. That visual optimization is TBD.

The following image shows an exaggerated version of the problem, created by disabling streaming completely then moving the camera:

![Streaming Cracks](./readme-images/streaming-cracks.jpg "Streaming Cracks")
## Log

- 2025-07-10: Many improvements have been added over the last few months, with a 10x increase in the number of resources supported at lower FPS! Improvements include reduced GPU time by resolving directly to CPU memory, reduced CPU overhead on the render thread, reduced CPU feedback processing time, and reduced CPU and GPU memory usage. SFS APIs for resource creation & destruction are thread-safe, allowing the application to create the scene faster. Better application heuristics including frustum culling and screen-space area demonstrate how to effectively use SFS, focusing work on the objects that benefit most. And many, many smaller improvements.
- 2025-01-04: GPU Heaps, OS improvements for tile mapping, and Frustum culling provide nice performance benefits. The library has been renamed to look more production-ready.
- 2024-05-23: Hemi-spherical texture coordinate projection in pixel shader provides super crisp, low distortion text and images across geometry LoDs. Sort of novel technique?
- 2022-10-24: Added DirectStorage trace playback utility to measure performance of file upload independent of rendering. For example, to capture and playback the DirectStorage requests and submits for 500 "stressful" frames with a staging buffer size of 128MB, cd to the build directory and:
```
stress.bat -timingstart 200 -timingstop 700 -capturetrace
traceplayer.exe -file uploadTraceFile_1.json -mediadir media -staging 128
```
- 2022-06-10: File format (.xet) change. DdsToXet can upgrade old Xet files to the new format. Assets in the DDS directory are exported at build time into media directory. Upgrade to DirectStorage v1.0.2. Many misc. improvements.
- 2022-05-05: Workaround for rare race condition. Many tweaks and improvements.
- 2022-03-14: DirectStorage 1.0.0 integrated into mainline
- 2021-12-15: "-addAliasingBarriers" command line option to add an aliasing barrier to assist PIX analysis. Can also be enabled in config.json.
- 2021-12-03: added BC1 asset collection as "release 2." All texture assets (.xet files) can reside in the same directory despite format differences, and can co-exist in the same GPU heap. Also minor source tweaks, including fix to not cull base "terrain" object.
- 2021-10-21: code refactor to improve sampler feedback streaming library API
- 2021-08-10: Added some 16k x 16k textures (BC7 format) posted as "release 1".

## License

Sample and its code provided under MIT license, please see [LICENSE](/LICENSE). All third-party source code provided under their own respective and MIT-compatible Open Source licenses.

Copyright (C) 2021, Intel Corporation  
