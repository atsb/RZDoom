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
#include <dinput.h>
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

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------
class FRawKeyboard : public FKeyboard
{
public:
    FRawKeyboard();
    ~FRawKeyboard();
    bool GetDevice();
    bool ProcessRawInput(RAWINPUT* rawinput, int code);
protected:
    USHORT E1Prefix;
};

// EXTERNAL DATA DECLARATIONS ----------------------------------------------
extern HWND Window;
extern bool GUICapture;

// PRIVATE DATA DEFINITIONS ------------------------------------------------
static const BYTE Convert[256] =
{
    //  0    1    2    3    4    5    6    7    8    9    A    B    C    D    E    F
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',  8,  9, // 0
       'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,  0, 'a', 's', // 1
       'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39,  '`',  0,'\\', 'z', 'x', 'c', 'v', // 2
       'b', 'n', 'm', ',', '.', '/',  0, '*',  0, ' ',  0,  0,  0,  0,  0,  0,       // 3
        0,  0,   0,   0,  0,  0,  0, '7', '8', '9', '-', '4', '5', '6', '+', '1',     // 4
       '2', '3', '0', '.',  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,           // 5
        0,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,             // 6
        0,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,             // 7
        0,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0, '=',  0,  0,            // 8
        0, '@', ':', '_',  0,  0,  0,  0,  0,  0,  0,  0, 13,  0,  0,  0,            // 9
        0,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,             // A
        0,  0,   0,  ',',  0, '/',  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,           // B
        0,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,             // C
        0,  0,   0,   0,  0,  0,  0,  0
};

// PUBLIC DATA DEFINITIONS -------------------------------------------------
FKeyboard* Keyboard;
CVAR(Bool, k_mergekeys, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

FKeyboard::FKeyboard()
{
    memset(KeyStates, 0, sizeof(KeyStates));
}
FKeyboard::~FKeyboard()
{
    AllKeysUp();
}

bool FKeyboard::CheckAndSetKey(int keynum, INTBOOL down)
{
    BYTE* statebyte = &KeyStates[keynum >> 3];
    BYTE mask = 1 << (keynum & 7);
    if (down)
    {
        if (*statebyte & mask) return true;
        *statebyte |= mask;
        return false;
    }
    else
    {
        if (*statebyte & mask)
        {
            *statebyte &= ~mask;
            return false;
        }
        return true;
    }
}

void FKeyboard::AllKeysUp()
{
    event_t ev = { 0 };
    ev.type = EV_KeyUp;
    for (int i = 0; i < 256 / 8; ++i)
    {
        if (KeyStates[i] != 0)
        {
            BYTE states = KeyStates[i];
            int j = 0;
            KeyStates[i] = 0;
            do
            {
                if (states & 1)
                {
                    ev.data1 = (i << 3) + j;
                    ev.data2 = Convert[ev.data1];
                    D_PostEvent(&ev);
                }
                states >>= 1;
                ++j;
            } while (states != 0);
        }
    }
}

void FKeyboard::PostKeyEvent(int key, INTBOOL down, bool foreground)
{
    event_t ev = { 0 };

    if (k_mergekeys)
    {
        if (key == DIK_NUMPADENTER || key == DIK_RMENU || key == DIK_RCONTROL)
        {
            k_mergekeys = false;
            PostKeyEvent(key, false, foreground);
            k_mergekeys = true;
            key &= 0x7F;
        }
        else if (key == DIK_RSHIFT)
        {
            k_mergekeys = false;
            PostKeyEvent(key, false, foreground);
            k_mergekeys = true;
            key = DIK_LSHIFT;
        }
    }
    if (key == 0x59)
    {
        key = DIK_NUMPADEQUALS;
    }

    if (down)
    {
        if (!foreground || GUICapture) return;
        ev.type = EV_KeyDown;
    }
    else
    {
        ev.type = EV_KeyUp;
    }
    if (CheckAndSetKey(key, down)) return;

    ev.data1 = key;
    ev.data2 = Convert[key];
    D_PostEvent(&ev);
}

FRawKeyboard::FRawKeyboard()
{
    E1Prefix = 0;
}
FRawKeyboard::~FRawKeyboard()
{
    if (MyRegisterRawInputDevices != NULL)
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = HID_GENERIC_DESKTOP_PAGE;
        rid.usUsage = HID_GDP_KEYBOARD;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = NULL;
        MyRegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
}

bool FRawKeyboard::GetDevice()
{
    if (MyRegisterRawInputDevices == NULL) return false;

    RAWINPUTDEVICE rid;
    rid.usUsagePage = HID_GENERIC_DESKTOP_PAGE;
    rid.usUsage = HID_GDP_KEYBOARD;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = Window;

    return MyRegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
}

bool FRawKeyboard::ProcessRawInput(RAWINPUT* raw, int code)
{
    if (raw->header.dwType != RIM_TYPEKEYBOARD) return false;

    int keycode = raw->data.keyboard.MakeCode;
    if (keycode == 0 && (raw->data.keyboard.Flags & RI_KEY_E0))
    {
        if (raw->data.keyboard.VKey >= VK_BROWSER_BACK && raw->data.keyboard.VKey <= VK_LAUNCH_APP2)
        {
            static const BYTE MediaKeys[VK_LAUNCH_APP2 - VK_BROWSER_BACK + 1] =
            {
                DIK_WEBBACK, DIK_WEBFORWARD, DIK_WEBREFRESH, DIK_WEBSTOP,
                DIK_WEBSEARCH, DIK_WEBFAVORITES, DIK_WEBHOME,
                DIK_MUTE, DIK_VOLUMEDOWN, DIK_VOLUMEUP,
                DIK_NEXTTRACK, DIK_PREVTRACK, DIK_MEDIASTOP, DIK_PLAYPAUSE,
                DIK_MAIL, DIK_MEDIASELECT, DIK_MYCOMPUTER, DIK_CALCULATOR
            };
            keycode = MediaKeys[raw->data.keyboard.VKey - VK_BROWSER_BACK];
        }
    }

    if (keycode < 1 || keycode > 0xFF) return false;

    if (raw->data.keyboard.Flags & RI_KEY_E1)
    {
        E1Prefix = raw->data.keyboard.MakeCode;
        return false;
    }
    if (raw->data.keyboard.Flags & RI_KEY_E0)
    {
        if (keycode == DIK_LSHIFT || keycode == DIK_RSHIFT) return false;
        keycode |= 0x80;
    }

    if (E1Prefix)
    {
        if (E1Prefix == 0x1D && keycode == DIK_NUMLOCK)
        {
            keycode = DIK_PAUSE;
            E1Prefix = 0;
        }
        else
        {
            E1Prefix = 0;
            return false;
        }
    }
    if (keycode == 0xC6)
    {
        keycode = DIK_PAUSE;
    }
    if (keycode == 0x54)
    {
        keycode = DIK_SYSRQ;
    }

    PostKeyEvent(keycode, !(raw->data.keyboard.Flags & RI_KEY_BREAK), code == RIM_INPUT);
    return true;
}

void I_StartupKeyboard()
{
    Keyboard = new FRawKeyboard;
    if (!Keyboard->GetDevice())
    {
        delete Keyboard;
        Keyboard = NULL;
    }
}