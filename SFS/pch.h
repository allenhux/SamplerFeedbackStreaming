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
#include <set>
#include <list>
#include <memory>      // for unique_ptr
#include <wrl.h>
#include <shellapi.h>
#include <sstream>
#include <crtdbg.h>
#include <fstream>
#include <algorithm>
#include <thread>
#include <filesystem>
#include <synchapi.h>

#include "d3dx12.h"
