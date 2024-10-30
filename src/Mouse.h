//*********************************************************
//
// Copyright 2024 Allen Hux 
//
// Keeps most recent mouse position
// If dragging, accumulates delta
// 
//*********************************************************

#pragma once

class Mouse
{
public:
    // call within windows message loop
    void Update(WPARAM wParam, LPARAM lParam);
    // call outside loop to get accumulated delta. clears delta.
    POINT GetDelta();
    int GetPosX() const { return m_posX; }
    int GetPosY() const { return m_posY; }
private:
    int m_posX{ 0 };
    int m_posY{ 0 };

    int m_deltaX{ 0 };
    int m_deltaY{ 0 };
};
