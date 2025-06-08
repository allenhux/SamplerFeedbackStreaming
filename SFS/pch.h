//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <string>
#include <vector>
#include <memory>      // for unique_ptr
#include <list>
#include <wrl.h>
#include <shellapi.h>
#include <sstream>
#include <crtdbg.h>
#include <fstream>
#include <algorithm>
#include <thread>
#include <filesystem>
#include <set>

#include "d3dx12.h"
