// i_dijoy.cpp
// -----------------------------------------------------------------------------

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

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#define USE_WINDOWS_DWORD
#include "i_input.h"
#include "i_system.h"
#include "d_event.h"
#include "d_gui.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "doomdef.h"
#include "doomstat.h"
#include "win32iface.h"
#include "templates.h"
#include "gameconfigfile.h"
#include "cmdlib.h"
#include "v_text.h"
#include "m_argv.h"

extern HWND Window;
extern void UpdateJoystickMenu(IJoystickConfig* current);

CUSTOM_CVAR(Bool, joy_dinput, false,
    CVAR_GLOBALCONFIG | CVAR_ARCHIVE | CVAR_NOINITCALL)
{
    I_StartupDirectInputJoystick();

    event_t ev = { EV_DeviceChange };
    D_PostEvent(&ev);
}

void I_StartupDirectInputJoystick()
{
    if (!use_joystick || Args->CheckParm("-nojoy"))
    {
        if (JoyDevices[INPUT_DIJoy] != nullptr)
        {
            delete JoyDevices[INPUT_DIJoy];
            JoyDevices[INPUT_DIJoy] = nullptr;
        }
        UpdateJoystickMenu(nullptr);
        return;
    }

    if (JoyDevices[INPUT_DIJoy] != nullptr)
    {
        delete JoyDevices[INPUT_DIJoy];
        JoyDevices[INPUT_DIJoy] = nullptr;
    }

    UpdateJoystickMenu(nullptr);

    event_t ev = { EV_DeviceChange };
    D_PostEvent(&ev);
}
