/*
** -------------------------------------------------------------------------- -
**Copyright 1998 - 2008 Randy Heit
* *Copyright 2020 - 2026 atsb
* *All rights reserved.
* 
 * This file is part of RZDoom.
 *
 * RZDoom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RZDoom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RZDoom.  If not, see <https://www.gnu.org/licenses/>.
 */

// HEADER FILES ------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0501
#include <windows.h>
#define USE_WINDOWS_DWORD
#include "i_input.h"
#include "i_system.h"
#include "d_event.h"
#include "d_gui.h"
#include "c_cvars.h"
#include "doomdef.h"
#include "doomstat.h"
#include "win32iface.h"
#include "rawinput.h"
#include "menu/menu.h"

#ifndef GET_XBUTTON_WPARAM
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x20e
#endif

class FRawMouse : public FMouse
{
public:
    FRawMouse();
    ~FRawMouse();
    bool GetDevice();
    bool ProcessRawInput(RAWINPUT* rawinput, int code);
    bool WndProcHook(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);
    void Grab();
    void Ungrab();
protected:
    bool  Grabbed;
    POINT UngrabbedPointerPos;
};

class FWin32Mouse : public FMouse
{
public:
    FWin32Mouse();
    ~FWin32Mouse();
    bool GetDevice();
    void ProcessInput();
    bool WndProcHook(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);
    void Grab();
    void Ungrab();
protected:
    POINT UngrabbedPointerPos;
    LONG  PrevX, PrevY;
    bool  Grabbed;
};

enum EMouseMode
{
    MM_None,
    MM_Win32,
    MM_RawInput
};

static void SetCursorState(bool visible);
static FMouse* CreateWin32Mouse();
static FMouse* CreateRawMouse();
static void CenterMouse(int x, int y, LONG* centx, LONG* centy);

extern HWND Window;
extern bool GUICapture;
extern int  BlockMouseMove;

static EMouseMode MouseMode = MM_None;
static FMouse* (*MouseFactory[])() =
{
    CreateWin32Mouse,
    CreateRawMouse
};

FMouse* Mouse;
bool   NativeMouse;
bool   CursorState;

CVAR(Bool, use_mouse, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, m_noprescale, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, m_filter, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, m_hidepointer, true, 0)
CUSTOM_CVAR(Int, in_mouse, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
    if (self < 0) self = 0;
    else if (self > 2) self = 2;
    else I_StartupMouse();
}
CUSTOM_CVAR(Int, mouse_capturemode, 1, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
{
    if (self < 0) self = 0;
    else if (self > 2) self = 2;
}

static void SetCursorState(bool visible)
{
    CursorState = visible || !m_hidepointer;
    if (GetForegroundWindow() == Window)
    {
        if (CursorState) SetCursor((HCURSOR)(intptr_t)GetClassLongPtr(Window, GCLP_HCURSOR));
        else             SetCursor(NULL);
    }
}

static void CenterMouse(int curx, int cury, LONG* centxp, LONG* centyp)
{
    RECT rect;
    GetWindowRect(Window, &rect);
    int centx = (rect.left + rect.right) >> 1;
    int centy = (rect.top + rect.bottom) >> 1;
    if (centx != curx || centy != cury)
    {
        if (centxp) { *centxp = centx; *centyp = centy; }
        SetCursorPos(centx, centy);
    }
}

static bool CaptureMode_InGame()
{
    if (mouse_capturemode == 2) return true;
    else if (mouse_capturemode == 1)
        return gamestate == GS_LEVEL || gamestate == GS_INTERMISSION || gamestate == GS_FINALE;
    else
        return gamestate == GS_LEVEL;
}

void I_CheckNativeMouse(bool preferNative)
{
    bool windowed = (screen == NULL) || !screen->IsFullscreen();
    bool want_native;

    if (!windowed)
    {
        want_native = m_use_mouse && (menuactive == MENU_On || menuactive == MENU_OnNoPause);
    }
    else
    {
        if ((GetForegroundWindow() != Window) || preferNative || !use_mouse)
        {
            want_native = true;
        }
        else if (menuactive == MENU_WaitKey)
        {
            want_native = false;
        }
        else
        {
            want_native = ((!m_use_mouse || menuactive != MENU_WaitKey) &&
                (!CaptureMode_InGame() || GUICapture || paused || demoplayback));
        }
    }

    if (want_native != NativeMouse)
    {
        if (Mouse != NULL)
        {
            NativeMouse = want_native;
            if (want_native)
            {
                BlockMouseMove = 3;
                Mouse->Ungrab();
            }
            else
            {
                Mouse->Grab();
            }
        }
    }
}

FMouse::FMouse()
{
    LastX = LastY = 0;
    ButtonState = 0;
    WheelMove[0] = WheelMove[1] = 0;
}

void FMouse::PostMouseMove(int x, int y)
{
    event_t ev = { 0 };
    if (m_filter)
    {
        ev.x = (x + LastX) / 2;
        ev.y = (y + LastY) / 2;
    }
    else
    {
        ev.x = x;
        ev.y = y;
    }
    LastX = x;
    LastY = y;
    if (ev.x || ev.y)
    {
        ev.type = EV_Mouse;
        D_PostEvent(&ev);
    }
}

void FMouse::WheelMoved(int axis, int wheelmove)
{
    assert(axis == 0 || axis == 1);
    event_t ev = { 0 };
    int dir;
    WheelMove[axis] += wheelmove;
    if (WheelMove[axis] < 0) { dir = WHEEL_DELTA; ev.data1 = KEY_MWHEELDOWN; }
    else { dir = -WHEEL_DELTA; ev.data1 = KEY_MWHEELUP; }
    ev.data1 += axis * 2;

    if (!GUICapture)
    {
        while (abs(WheelMove[axis]) >= WHEEL_DELTA)
        {
            ev.type = EV_KeyDown; D_PostEvent(&ev);
            ev.type = EV_KeyUp;   D_PostEvent(&ev);
            WheelMove[axis] += dir;
        }
    }
    else
    {
        ev.type = EV_GUI_Event;
        ev.subtype = ev.data1 - KEY_MWHEELUP + EV_GUI_WheelUp;
        if (GetKeyState(VK_SHIFT) & 0x8000) ev.data3 |= GKM_SHIFT;
        if (GetKeyState(VK_CONTROL) & 0x8000) ev.data3 |= GKM_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) ev.data3 |= GKM_ALT;
        ev.data1 = 0;
        while (abs(WheelMove[axis]) >= WHEEL_DELTA)
        {
            D_PostEvent(&ev);
            WheelMove[axis] += dir;
        }
    }
}

void FMouse::PostButtonEvent(int button, bool down)
{
    event_t ev = { 0 };
    int mask = 1 << button;
    ev.data1 = KEY_MOUSE1 + button;
    if (down)
    {
        ButtonState |= mask;
        ev.type = EV_KeyDown;
        D_PostEvent(&ev);
    }
    else if (ButtonState & mask)
    {
        ButtonState &= ~mask;
        ev.type = EV_KeyUp;
        D_PostEvent(&ev);
    }
}

void FMouse::ClearButtonState()
{
    if (ButtonState != 0)
    {
        int i, mask;
        event_t ev = { 0 };
        ev.type = EV_KeyUp;
        for (i = sizeof(ButtonState) * 8, mask = 1; i > 0; --i, mask <<= 1)
        {
            if (ButtonState & mask)
            {
                ev.data1 = KEY_MOUSE1 + (int)sizeof(ButtonState) * 8 - i;
                D_PostEvent(&ev);
            }
        }
        ButtonState = 0;
    }
    WheelMove[0] = WheelMove[1] = 0;
}

static FMouse* CreateRawMouse() { return new FRawMouse; }

FRawMouse::FRawMouse()
{
    Grabbed = false;
    SetCursorState(true);
}

FRawMouse::~FRawMouse()
{
    Ungrab();
}

bool FRawMouse::GetDevice()
{
    if (MyRegisterRawInputDevices == NULL) return false;

    RAWINPUTDEVICE rid;
    rid.usUsagePage = HID_GENERIC_DESKTOP_PAGE;
    rid.usUsage = HID_GDP_MOUSE;
    rid.dwFlags = 0;
    rid.hwndTarget = Window;

    if (!MyRegisterRawInputDevices(&rid, 1, sizeof(rid))) return false;

    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = NULL;
    MyRegisterRawInputDevices(&rid, 1, sizeof(rid));

    return true;
}

void FRawMouse::Grab()
{
    if (!Grabbed)
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = HID_GENERIC_DESKTOP_PAGE;
        rid.usUsage = HID_GDP_MOUSE;
        rid.dwFlags = RIDEV_CAPTUREMOUSE | RIDEV_NOLEGACY;
        rid.hwndTarget = Window;
        if (MyRegisterRawInputDevices(&rid, 1, sizeof(rid)))
        {
            GetCursorPos(&UngrabbedPointerPos);
            Grabbed = true;
            SetCursorState(false);
            CenterMouse(-1, -1, NULL, NULL);
        }
    }
}

void FRawMouse::Ungrab()
{
    if (Grabbed)
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = HID_GENERIC_DESKTOP_PAGE;
        rid.usUsage = HID_GDP_MOUSE;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = NULL;
        if (MyRegisterRawInputDevices(&rid, 1, sizeof(rid)))
        {
            Grabbed = false;
            ClearButtonState();
        }
        SetCursorState(true);
        SetCursorPos(UngrabbedPointerPos.x, UngrabbedPointerPos.y);
    }
}

bool FRawMouse::ProcessRawInput(RAWINPUT* raw, int code)
{
    if (!Grabbed || raw->header.dwType != RIM_TYPEMOUSE || !use_mouse) return false;
    for (int i = 0, mask = 1; i < 5; ++i)
    {
        if (raw->data.mouse.usButtonFlags & mask) { PostButtonEvent(i, true); }
        mask <<= 1;
        if (raw->data.mouse.usButtonFlags & mask) { PostButtonEvent(i, false); }
        mask <<= 1;
    }

    if (raw->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
    {
        WheelMoved(0, (SHORT)raw->data.mouse.usButtonData);
    }
    else if (raw->data.mouse.usButtonFlags & 0x800) // horizontal wheel
    {
        WheelMoved(1, (SHORT)raw->data.mouse.usButtonData);
    }

    int x = m_noprescale ? raw->data.mouse.lLastX : (raw->data.mouse.lLastX << 2);
    int y = -raw->data.mouse.lLastY;

    PostMouseMove(x, y);
    if (x || y) CenterMouse(-1, -1, NULL, NULL);
    return true;
}

bool FRawMouse::WndProcHook(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    if (!Grabbed) return false;
    if (message == WM_SYSCOMMAND)
    {
        wParam &= 0xFFF0;
        if (wParam == SC_MOVE || wParam == SC_SIZE) return true;
    }
    return false;
}

static FMouse* CreateWin32Mouse() { return new FWin32Mouse; }

FWin32Mouse::FWin32Mouse()
{
    GetCursorPos(&UngrabbedPointerPos);
    Grabbed = false;
    SetCursorState(true);
}

FWin32Mouse::~FWin32Mouse()
{
    Ungrab();
}

bool FWin32Mouse::GetDevice()
{
    return true;
}

void FWin32Mouse::ProcessInput()
{
    POINT pt;
    int x, y;
    if (!Grabbed || !use_mouse || !GetCursorPos(&pt)) return;

    x = pt.x - PrevX;
    y = PrevY - pt.y;
    if (!m_noprescale) { x *= 3; y *= 2; }

    if (x || y) CenterMouse(pt.x, pt.y, &PrevX, &PrevY);
    PostMouseMove(x, y);
}

bool FWin32Mouse::WndProcHook(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    if (!Grabbed) return false;

    if (message == WM_SIZE)
    {
        if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED)
        {
            CenterMouse(-1, -1, &PrevX, &PrevY);
            return true;
        }
    }
    else if (message == WM_MOVE)
    {
        CenterMouse(-1, -1, &PrevX, &PrevY);
        return true;
    }
    else if (message == WM_SYSCOMMAND)
    {
        wParam &= 0xFFF0;
        if (wParam == SC_MOVE || wParam == SC_SIZE) return true;
    }
    else if (!use_mouse)
    {
        return false;
    }
    else if (message == WM_MOUSEWHEEL)
    {
        WheelMoved(0, (SHORT)HIWORD(wParam));
        return true;
    }
    else if (message == WM_MOUSEHWHEEL)
    {
        WheelMoved(1, (SHORT)HIWORD(wParam));
        return true;
    }
    else if (message >= WM_LBUTTONDOWN && message <= WM_MBUTTONUP)
    {
        int action = (message - WM_LBUTTONDOWN) % 3;
        int button = (message - WM_LBUTTONDOWN) / 3;
        if (action == 2) return false;
        event_t ev = { 0 };
        ev.type = action ? EV_KeyUp : EV_KeyDown;
        ev.data1 = KEY_MOUSE1 + button;
        if (action) ButtonState &= ~(1 << button);
        else        ButtonState |= (1 << button);
        D_PostEvent(&ev);
        return true;
    }
    else if (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONUP)
    {
        WORD xbuttons = GET_XBUTTON_WPARAM(wParam);
        event_t ev = { 0 };
        ev.type = (message == WM_XBUTTONDOWN) ? EV_KeyDown : EV_KeyUp;
        for (int i = 0; i < 5; ++i, xbuttons >>= 1)
        {
            if (xbuttons & 1)
            {
                ev.data1 = KEY_MOUSE4 + i;
                if (ev.type == EV_KeyDown) ButtonState |= (1 << (i + 4));
                else                       ButtonState &= ~(1 << (i + 4));
                D_PostEvent(&ev);
            }
        }
        *result = TRUE;
        return true;
    }
    return false;
}

void FWin32Mouse::Grab()
{
    if (Grabbed) return;

    RECT rect;
    GetCursorPos(&UngrabbedPointerPos);
    ClipCursor(NULL);
    GetClientRect(Window, &rect);
    ClientToScreen(Window, (LPPOINT)&rect.left);
    ClientToScreen(Window, (LPPOINT)&rect.right);
    ClipCursor(&rect);
    SetCursorState(false);
    CenterMouse(-1, -1, &PrevX, &PrevY);
    Grabbed = true;
}

void FWin32Mouse::Ungrab()
{
    if (!Grabbed) return;

    ClipCursor(NULL);
    SetCursorPos(UngrabbedPointerPos.x, UngrabbedPointerPos.y);
    SetCursorState(true);
    Grabbed = false;
    ClearButtonState();
}

void I_StartupMouse()
{
    EMouseMode new_mousemode;
    switch (in_mouse)
    {
    case 0:
    default:
        new_mousemode = (MyRegisterRawInputDevices != NULL) ? MM_RawInput : MM_Win32;
        break;
    case 1: new_mousemode = MM_Win32;    break;
    case 2: new_mousemode = MM_RawInput; break;
    }

    if (new_mousemode != MouseMode)
    {
        if (Mouse != NULL) { delete Mouse; Mouse = NULL; }
        do
        {
            Mouse = MouseFactory[new_mousemode - MM_Win32]();
            if (Mouse && Mouse->GetDevice()) break;
            if (Mouse) { delete Mouse; Mouse = NULL; }
            new_mousemode = (EMouseMode)(new_mousemode - 1);
        } while (new_mousemode != MM_None);

        MouseMode = new_mousemode;
        NativeMouse = true;
    }
}
