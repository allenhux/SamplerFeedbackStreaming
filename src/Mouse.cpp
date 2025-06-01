//*********************************************************
// Copyright 2024 Allen Hux 
//
// SPDX-License-Identifier: MIT
//*********************************************************

#include "pch.h"
#include "Mouse.h"

//-----------------------------------------------------------------------------
// Call for every wm_mousemove
// Internally tracks last mouse position
//-----------------------------------------------------------------------------
void Mouse::Update(WPARAM wParam, LPARAM lParam)
{
    // MSDN: lparam: low-order is x, high-order is y, relative to top-left of client rect
    auto points = MAKEPOINTS(lParam);
    int posX = points.x;
    int posY = points.y;

    UINT mouseButton = (GetSystemMetrics(SM_SWAPBUTTON) ? MK_RBUTTON : MK_LBUTTON);
    if (wParam & mouseButton)
    {
        m_deltaX += posX - m_posX;
        m_deltaY += posY - m_posY;
    }

    m_posX = posX;
    m_posY = posY;
}

//-----------------------------------------------------------------------------
// after all windows messages processed,
// call to process the accumulated delta.
// 
// also clears the delta.
//-----------------------------------------------------------------------------
POINT Mouse::GetDelta()
{
    POINT p{ m_deltaX, m_deltaY };
    m_deltaX = 0;
    m_deltaY = 0;
    return p;
}
