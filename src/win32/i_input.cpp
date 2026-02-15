/*
** i_input.cpp
** Handles input from keyboard, mouse, and joystick
**
**---------------------------------------------------------------------------
** Copyright 1998-2008 Randy Heit
** Copyright 2020-2026 atsb
** All rights reserved.
/*
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

// Target: Windows XP+ for raw input + session notifications.
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0501)
#define _WIN32_WINNT 0x0501
#endif

#define WIN32_LEAN_AND_MEAN
#define __BYTEBOOL__
#ifndef __GNUC__
#define INITGUID
#endif

#include <windows.h>
#include <mmsystem.h>
#include <dbt.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

// Compensate for w32api's lack
#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#define WTS_CONSOLE_CONNECT 1
#define WTS_CONSOLE_DISCONNECT 2
#define WTS_SESSION_LOCK 7
#define WTS_SESSION_UNLOCK 8
#endif
#ifndef PBT_APMSUSPEND
#include <pbt.h>
#endif
#ifndef GET_RAWINPUT_CODE_WPARAM
#define GET_RAWINPUT_CODE_WPARAM(wParam) ((wParam) & 0xff)
#endif

#define USE_WINDOWS_DWORD
#include "c_dispatch.h"
#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "i_input.h"
#include "v_video.h"
#include "i_sound.h"
#include "g_game.h"
#include "d_main.h"
#include "d_gui.h"
#include "c_console.h"
#include "c_cvars.h"
#include "i_system.h"
#include "s_sound.h"
#include "m_misc.h"
#include "gameconfigfile.h"
#include "win32iface.h"
#include "templates.h"
#include "cmdlib.h"
#include "d_event.h"
#include "v_text.h"
#include "version.h"

// Prototypes and declarations.
#include "rawinput.h"

// Definitions to load raw input entry points dynamically
#define RIF(name, ret, args) \
    name##Proto My##name;
#include "rawinput.h"

// Compensate for w32api's lack
#ifndef GET_XBUTTON_WPARAM
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#endif

#ifdef _DEBUG
#define INGAME_PRIORITY_CLASS NORMAL_PRIORITY_CLASS
#else
#define INGAME_PRIORITY_CLASS NORMAL_PRIORITY_CLASS
#endif

static void FindRawInputFunctions();
FJoystickCollection* JoyDevices[NUM_JOYDEVICES];

extern HINSTANCE g_hInst;
extern DWORD     SessionID;
extern void      ShowEAXEditor();
extern bool      SpawnEAXWindow;

bool  GUICapture;
extern FMouse* Mouse;
extern FKeyboard* Keyboard;
bool  VidResizing;
extern bool      SpawnEAXWindow;
extern BOOL      vidactive;
extern HWND      Window, ConWindow;
extern HWND      EAXEditWindow;
EXTERN_CVAR(String, language)
EXTERN_CVAR(Bool, lookstrafe)
EXTERN_CVAR(Bool, use_joystick)
EXTERN_CVAR(Bool, use_mouse)

static int  WheelDelta;
extern bool CursorState;
extern BOOL paused;
static bool noidle = false;

BOOL AppActive = TRUE;
int  SessionState = 0;
int  BlockMouseMove;

CVAR(Bool, k_allowfullscreentoggle, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Bool, norawinput, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
    Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

extern int chatmodeon;
static void I_CheckGUICapture()
{
    bool wantCapt;
    if (menuactive == MENU_Off)
    {
        wantCapt = ConsoleState == c_down || ConsoleState == c_falling || chatmodeon;
    }
    else
    {
        wantCapt = (menuactive == MENU_On || menuactive == MENU_OnNoPause);
    }
    if (wantCapt != GUICapture)
    {
        GUICapture = wantCapt;
        if (wantCapt && Keyboard != NULL)
        {
            Keyboard->AllKeysUp();
        }
    }
}

void I_SetMouseCapture() { SetCapture(Window); }
void I_ReleaseMouseCapture() { ReleaseCapture(); }

bool GUIWndProcHook(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    event_t ev = { EV_GUI_Event };
    *result = 0;

    switch (message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (message == WM_KEYUP || message == WM_SYSKEYUP)
        {
            ev.subtype = EV_GUI_KeyUp;
        }
        else
        {
            ev.subtype = (lParam & 0x40000000) ? EV_GUI_KeyRepeat : EV_GUI_KeyDown;
        }
        if (GetKeyState(VK_SHIFT) & 0x8000) ev.data3 |= GKM_SHIFT;
        if (GetKeyState(VK_CONTROL) & 0x8000) ev.data3 |= GKM_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) ev.data3 |= GKM_ALT;

        if (wParam == VK_PROCESSKEY)
        {
            wParam = MapVirtualKey((lParam >> 16) & 255, 1);
        }
        if ((ev.data1 = MapVirtualKey((UINT)wParam, 2)))
        {
            D_PostEvent(&ev);
        }
        else
        {
            switch (wParam)
            {
            case VK_PRIOR:  ev.data1 = GK_PGUP;   break;
            case VK_NEXT:   ev.data1 = GK_PGDN;   break;
            case VK_END:    ev.data1 = GK_END;    break;
            case VK_HOME:   ev.data1 = GK_HOME;   break;
            case VK_LEFT:   ev.data1 = GK_LEFT;   break;
            case VK_RIGHT:  ev.data1 = GK_RIGHT;  break;
            case VK_UP:     ev.data1 = GK_UP;     break;
            case VK_DOWN:   ev.data1 = GK_DOWN;   break;
            case VK_DELETE: ev.data1 = GK_DEL;    break;
            case VK_ESCAPE: ev.data1 = GK_ESCAPE; break;
            case VK_F1:     ev.data1 = GK_F1;     break;
            case VK_F2:     ev.data1 = GK_F2;     break;
            case VK_F3:     ev.data1 = GK_F3;     break;
            case VK_F4:     ev.data1 = GK_F4;     break;
            case VK_F5:     ev.data1 = GK_F5;     break;
            case VK_F6:     ev.data1 = GK_F6;     break;
            case VK_F7:     ev.data1 = GK_F7;     break;
            case VK_F8:     ev.data1 = GK_F8;     break;
            case VK_F9:     ev.data1 = GK_F9;     break;
            case VK_F10:    ev.data1 = GK_F10;    break;
            case VK_F11:    ev.data1 = GK_F11;    break;
            case VK_F12:    ev.data1 = GK_F12;    break;
            case VK_BROWSER_BACK: ev.data1 = GK_BACK; break;
            }
            if (ev.data1 != 0) D_PostEvent(&ev);
        }
        return ev.subtype == EV_GUI_KeyUp;

    case WM_CHAR:
    case WM_SYSCHAR:
        if (wParam >= ' ')
        {
            ev.subtype = EV_GUI_Char;
            ev.data1 = (INT)wParam;
            ev.data2 = (message == WM_SYSCHAR);
            D_PostEvent(&ev);
            return true;
        }
        break;

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
    case WM_MOUSEMOVE:
        if (message >= WM_LBUTTONDOWN && message <= WM_LBUTTONDBLCLK)
        {
            ev.subtype = message - WM_LBUTTONDOWN + EV_GUI_LButtonDown;
        }
        else if (message >= WM_RBUTTONDOWN && message <= WM_RBUTTONDBLCLK)
        {
            ev.subtype = message - WM_RBUTTONDOWN + EV_GUI_RButtonDown;
        }
        else if (message >= WM_MBUTTONDOWN && message <= WM_MBUTTONDBLCLK)
        {
            ev.subtype = message - WM_MBUTTONDOWN + EV_GUI_MButtonDown;
        }
        else if (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONUP)
        {
            ev.subtype = message - WM_XBUTTONDOWN + EV_GUI_BackButtonDown;
            if (GET_XBUTTON_WPARAM(wParam) == 2)
                ev.subtype += EV_GUI_FwdButtonDown - EV_GUI_BackButtonDown;
            else if (GET_XBUTTON_WPARAM(wParam) != 1)
                break;
        }
        else if (message == WM_MOUSEMOVE)
        {
            ev.subtype = EV_GUI_MouseMove;
            if (BlockMouseMove > 0) return true;
        }
        ev.data1 = LOWORD(lParam);
        ev.data2 = HIWORD(lParam);
        if (screen != NULL)
        {
            screen->ScaleCoordsFromWindow(ev.data1, ev.data2);
        }
        if (wParam & MK_SHIFT)   ev.data3 |= GKM_SHIFT;
        if (wParam & MK_CONTROL) ev.data3 |= GKM_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) ev.data3 |= GKM_ALT;
        if (use_mouse) D_PostEvent(&ev);
        return true;

    case WM_MOUSEWHEEL:
        if (!use_mouse) return false;
        if (wParam & MK_SHIFT)   ev.data3 |= GKM_SHIFT;
        if (wParam & MK_CONTROL) ev.data3 |= GKM_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) ev.data3 |= GKM_ALT;

        WheelDelta += (SHORT)HIWORD(wParam);
        if (WheelDelta < 0)
        {
            ev.subtype = EV_GUI_WheelDown;
            while (WheelDelta <= -WHEEL_DELTA) { D_PostEvent(&ev); WheelDelta += WHEEL_DELTA; }
        }
        else
        {
            ev.subtype = EV_GUI_WheelUp;
            while (WheelDelta >= WHEEL_DELTA) { D_PostEvent(&ev); WheelDelta -= WHEEL_DELTA; }
        }
        return true;
    }
    return false;
}

static inline bool CallHook(FInputDevice* device, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    if (!device) return false;
    *result = 0;
    return device->WndProcHook(hWnd, message, wParam, lParam, result);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT result;

    if (message == WM_INPUT)
    {
        if (MyGetRawInputData != NULL)
        {
            UINT size;
            if (!MyGetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) && size != 0)
            {
                BYTE* buffer = (BYTE*)alloca(size);
                if (MyGetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size)
                {
                    int code = GET_RAWINPUT_CODE_WPARAM(wParam);
                    if (Keyboard == NULL || !Keyboard->ProcessRawInput((RAWINPUT*)buffer, code))
                    {
                        if (Mouse == NULL || !Mouse->ProcessRawInput((RAWINPUT*)buffer, code))
                        {
                            if (JoyDevices[INPUT_RawPS2] != NULL)
                            {
                                JoyDevices[INPUT_RawPS2]->ProcessRawInput((RAWINPUT*)buffer, code);
                            }
                        }
                    }
                }
            }
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    if (CallHook(Keyboard, hWnd, message, wParam, lParam, &result)) return result;
    if (CallHook(Mouse, hWnd, message, wParam, lParam, &result)) return result;
    for (int i = 0; i < NUM_JOYDEVICES; ++i)
    {
        if (CallHook(JoyDevices[i], hWnd, message, wParam, lParam, &result)) return result;
    }
    if (GUICapture && GUIWndProcHook(hWnd, message, wParam, lParam, &result)) return result;
    if ((gamestate == GS_DEMOSCREEN || gamestate == GS_TITLELEVEL) && message == WM_LBUTTONDOWN)
    {
        if (GUIWndProcHook(hWnd, message, wParam, lParam, &result)) return result;
    }

    switch (message)
    {
    case WM_DESTROY:
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        exit(0);
        break;

    case WM_HOTKEY:
        break;

    case WM_PAINT:
        if (screen != NULL && 0)
        {
            static_cast<BaseWinFB*> (screen)->PaintToWindow();
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_SETTINGCHANGE:
        if (wParam == 0 && lParam != 0 && strcmp((const char*)lParam, "intl") == 0)
        {
            language.Callback();
        }
        return 0;

    case WM_KILLFOCUS:
        I_CheckNativeMouse(true);
        break;

    case WM_SETFOCUS:
        I_CheckNativeMouse(false);
        break;

    case WM_SETCURSOR:
        if (!CursorState)
        {
            SetCursor(NULL);
            return TRUE;
        }
        else
        {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

    case WM_SIZE:
        InvalidateRect(Window, NULL, FALSE);
        break;

    case WM_KEYDOWN:
        if (EAXEditWindow != 0 && wParam == VK_TAB && !(lParam & 0x40000000) && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            SetForegroundWindow(EAXEditWindow);
        }
        break;

    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && k_allowfullscreentoggle && !(lParam & 0x40000000))
        {
            ToggleFullscreen = !ToggleFullscreen;
        }
        if (wParam == VK_F4 && !(lParam & 0x40000000))
        {
            PostQuitMessage(0);
        }
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) != SC_KEYMENU)
        {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

    case WM_DISPLAYCHANGE:
    case WM_STYLECHANGED:
        if (SpawnEAXWindow)
        {
            SpawnEAXWindow = false;
            ShowEAXEditor();
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_GETMINMAXINFO:
        if (screen && !VidResizing)
        {
            LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
            if (screen->IsFullscreen())
            {
                RECT rect = { 0, 0, screen->GetWidth(), screen->GetHeight() };
                AdjustWindowRectEx(&rect, WS_VISIBLE | WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
                mmi->ptMinTrackSize.x = rect.right - rect.left;
                mmi->ptMinTrackSize.y = rect.bottom - rect.top;
            }
            else
            {
                mmi->ptMinTrackSize.x = 320;
                mmi->ptMinTrackSize.y = 200;
            }
            return 0;
        }
        break;

    case WM_ACTIVATEAPP:
        AppActive = (BOOL)wParam;
        if (wParam) SetPriorityClass(GetCurrentProcess(), INGAME_PRIORITY_CLASS);
        else if (!noidle && !netgame) SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
        S_SetSoundPaused((BOOL)wParam);
        break;

    case WM_WTSSESSION_CHANGE:
    case WM_POWERBROADCAST:
    {
        int oldstate = SessionState;
        if (message == WM_WTSSESSION_CHANGE && lParam == (LPARAM)SessionID)
        {
            switch (wParam)
            {
            case WTS_SESSION_LOCK:    SessionState |= 1; break;
            case WTS_SESSION_UNLOCK:  SessionState &= ~1; break;
            case WTS_CONSOLE_DISCONNECT: SessionState |= 2; break;
            case WTS_CONSOLE_CONNECT:    SessionState &= ~2; break;
            }
        }
        else if (message == WM_POWERBROADCAST)
        {
            switch (wParam)
            {
            case PBT_APMSUSPEND:        SessionState |= 4; break;
            case PBT_APMRESUMESUSPEND:  SessionState &= ~4; break;
            }
        }
    }
    break;

    case WM_PALETTECHANGED:
        if ((HWND)wParam == Window) break;
        if (screen != NULL) { screen->PaletteChanged(); }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_QUERYNEWPALETTE:
        if (screen != NULL) return screen->QueryNewPalette();
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_ERASEBKGND:
        return true;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_CONFIGCHANGED)
        {
            event_t ev = { EV_DeviceChange };
            D_PostEvent(&ev);
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

bool I_InitInput(void* hwnd)
{
    Printf("I_InitInput\n");
    atterm(I_ShutdownInput);
    noidle = !!Args->CheckParm("-noidle");

    FindRawInputFunctions();

    Printf("I_StartupMouse\n");
    I_StartupMouse();

    Printf("I_StartupKeyboard\n");
    I_StartupKeyboard();

    Printf("I_StartupXInput\n");
    I_StartupXInput();

    Printf("I_StartupRawPS2\n");
    I_StartupRawPS2();

    return TRUE;
}

void I_ShutdownInput()
{
    if (Keyboard != NULL) { delete Keyboard; Keyboard = NULL; }
    if (Mouse != NULL) { delete Mouse;    Mouse = NULL; }
    for (int i = 0; i < NUM_JOYDEVICES; ++i)
    {
        if (JoyDevices[i] != NULL)
        {
            delete JoyDevices[i];
            JoyDevices[i] = NULL;
        }
    }
}

void I_GetEvent()
{
    MSG mess;

    SleepEx(0, TRUE);

    while (PeekMessage(&mess, NULL, 0, 0, PM_REMOVE))
    {
        if (mess.message == WM_QUIT) exit(mess.wParam);
        if (EAXEditWindow == 0 || !IsDialogMessage(EAXEditWindow, &mess))
        {
            if (GUICapture) TranslateMessage(&mess);
            DispatchMessage(&mess);
        }
    }
    if (Keyboard != NULL) { Keyboard->ProcessInput(); }
    if (Mouse != NULL) { Mouse->ProcessInput(); }
}

void I_StartTic()
{
    BlockMouseMove--;
    ResetButtonTriggers();
    I_CheckGUICapture();
    I_CheckNativeMouse(false);
    I_GetEvent();
}

void I_StartFrame()
{
    if (use_joystick)
    {
        for (int i = 0; i < NUM_JOYDEVICES; ++i)
        {
            if (JoyDevices[i] != NULL) { JoyDevices[i]->ProcessInput(); }
        }
    }
}

void I_GetAxes(float axes[NUM_JOYAXIS])
{
    for (int i = 0; i < NUM_JOYAXIS; ++i) axes[i] = 0;
    if (use_joystick)
    {
        for (int i = 0; i < NUM_JOYDEVICES; ++i)
        {
            if (JoyDevices[i] != NULL) { JoyDevices[i]->AddAxes(axes); }
        }
    }
}

void I_GetJoysticks(TArray<IJoystickConfig*>& sticks)
{
    sticks.Clear();
    for (int i = 0; i < NUM_JOYDEVICES; ++i)
    {
        if (JoyDevices[i] != NULL) { JoyDevices[i]->GetDevices(sticks); }
    }
}

IJoystickConfig* I_UpdateDeviceList()
{
    IJoystickConfig* newone = NULL;
    for (int i = 0; i < NUM_JOYDEVICES; ++i)
    {
        if (JoyDevices[i] != NULL)
        {
            IJoystickConfig* thisnewone = JoyDevices[i]->Rescan();
            if (newone == NULL) newone = thisnewone;
        }
    }
    return newone;
}

void I_PutInClipboard(const char* str)
{
    if (str == NULL || !OpenClipboard(Window)) return;

    EmptyClipboard();
    HGLOBAL cliphandle = GlobalAlloc(GMEM_DDESHARE, strlen(str) + 1);
    if (cliphandle != NULL)
    {
        char* ptr = (char*)GlobalLock(cliphandle);
        strcpy(ptr, str);
        GlobalUnlock(cliphandle);
        SetClipboardData(CF_TEXT, cliphandle);
    }
    CloseClipboard();
}

FString I_GetFromClipboard(bool return_nothing)
{
    FString retstr;
    HGLOBAL cliphandle;
    char* clipstr;
    char* nlstr;

    if (return_nothing || !IsClipboardFormatAvailable(CF_TEXT) || !OpenClipboard(Window)) return retstr;

    cliphandle = GetClipboardData(CF_TEXT);
    if (cliphandle != NULL)
    {
        clipstr = (char*)GlobalLock(cliphandle);
        if (clipstr != NULL)
        {
            for (nlstr = clipstr; *nlstr != '\0'; ++nlstr)
            {
                if (nlstr[0] == '\r' && nlstr[1] == '\n') { nlstr++; }
                retstr += *nlstr;
            }
            GlobalUnlock(clipstr);
        }
    }
    CloseClipboard();
    return retstr;
}

#include "i_movie.h"
CCMD(playmovie)
{
    if (argv.argc() != 2) { Printf("Usage: playmovie <movie name>\n"); return; }
    I_PlayMovie(argv[1]);
}

FInputDevice::~FInputDevice() {}
void FInputDevice::ProcessInput() {}
bool FInputDevice::ProcessRawInput(RAWINPUT* raw, int code) { return false; }
bool FInputDevice::WndProcHook(HWND, UINT, WPARAM, LPARAM, LRESULT*) { return false; }

static void FindRawInputFunctions()
{
    if (!norawinput)
    {
        HMODULE user32 = GetModuleHandle("user32.dll");
        if (user32 == NULL) return;

#define RIF(name,ret,args) My##name = (name##Proto)GetProcAddress(user32, #name);
#include "rawinput.h"
    }
}
