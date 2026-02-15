/*
** win32video.cpp
** Code to let ZDoom draw to the screen
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

#ifdef _DEBUG
#define D3D_DEBUG_INFO
#endif

#define DIRECT3D_VERSION 0x0900
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>

// HEADER FILES ------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <ctype.h>

#define USE_WINDOWS_DWORD
#include "doomtype.h"
#include "c_dispatch.h"
#include "templates.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "v_pfx.h"
#include "stats.h"
#include "doomerrors.h"
#include "m_argv.h"
#include "r_defs.h"
#include "v_text.h"
#include "r_swrenderer.h"
#include "win32iface.h"

// MACROS ------------------------------------------------------------------
// TYPES -------------------------------------------------------------------
IMPLEMENT_ABSTRACT_CLASS(BaseWinFB)

typedef IDirect3D9* (WINAPI* DIRECT3DCREATE9FUNC)(UINT SDKVersion);

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------
void DoBlending(const PalEntry* from, PalEntry* to, int count, int r, int g, int b, int a);

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------
static void StopFPSLimit();

// EXTERNAL DATA DECLARATIONS ----------------------------------------------
extern HWND Window;
extern IVideo* Video;
extern BOOL AppActive;
extern int SessionState;
extern bool FullscreenReset;
extern bool VidResizing;

EXTERN_CVAR(Bool, fullscreen)
EXTERN_CVAR(Float, Gamma)
EXTERN_CVAR(Bool, cl_capfps)

// PRIVATE DATA DEFINITIONS ------------------------------------------------
static HMODULE D3D9_dll;
static UINT FPSLimitTimer;

// PUBLIC DATA DEFINITIONS -------------------------------------------------
IDirect3D9* D3D;
IDirect3DDevice9* D3Device;
HANDLE FPSLimitEvent;

CVAR(Bool, vid_forceddraw, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vid_adapter, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vid_maxfps, 200, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
    if (vid_maxfps < TICRATE && vid_maxfps != 0)
    {
        vid_maxfps = TICRATE;
    }
    else if (vid_maxfps > 1000)
    {
        vid_maxfps = 1000;
    }
    else if (cl_capfps == 0)
    {
        I_SetFPSLimit(vid_maxfps);
    }
}

#if VID_FILE_DEBUG
FILE* dbg;
#endif

Win32Video::Win32Video(int parm)
    : m_Modes(NULL),
    m_IsFullscreen(false),
    m_Adapter(D3DADAPTER_DEFAULT)
{
    I_SetWndProc();
    if (!InitD3D9()) {
        I_FatalError("Direct3D 9 initialization failed and DirectDraw fallback has been removed.");
    }
}

Win32Video::~Win32Video()
{
    FreeModes();
    if (D3D != NULL)
    {
        D3D->Release();
        D3D = NULL;
    }
    STOPLOG;
}

bool Win32Video::InitD3D9()
{
    DIRECT3DCREATE9FUNC direct3d_create_9;

    if (vid_forceddraw)
    {
        return false;
    }
    if ((D3D9_dll = LoadLibraryA("d3d9.dll")) == NULL)
    {
        return false;
    }
    // Obtain a IDirect3D interface.
    if ((direct3d_create_9 = (DIRECT3DCREATE9FUNC)GetProcAddress(D3D9_dll, "Direct3DCreate9")) == NULL)
    {
        goto closelib;
    }
    if ((D3D = direct3d_create_9(D3D_SDK_VERSION)) == NULL)
    {
        goto closelib;
    }

    m_Adapter = (vid_adapter < 1 || (UINT)vid_adapter > D3D->GetAdapterCount())
        ? D3DADAPTER_DEFAULT : (UINT)vid_adapter - 1u;

    D3DCAPS9 devcaps;
    if (FAILED(D3D->GetDeviceCaps(m_Adapter, D3DDEVTYPE_HAL, &devcaps)))
    {
        goto d3drelease;
    }
    if ((devcaps.PixelShaderVersion & 0xFFFF) < 0x104)
    {
        goto d3drelease;
    }
    if (!(devcaps.Caps2 & D3DCAPS2_DYNAMICTEXTURES))
    {
        goto d3drelease;
    }

    FreeModes();
    AddD3DModes(m_Adapter, D3DFMT_X8R8G8B8);
    AddD3DModes(m_Adapter, D3DFMT_R5G6B5);

    if (Args->CheckParm("-2"))
    {
        ScaleModes(1);
    }
    else if (Args->CheckParm("-4"))
    {
        ScaleModes(2);
    }
    else
    {
        AddLowResModes();
    }
    AddLetterboxModes();
    if (m_Modes == NULL)
    {
        goto d3drelease;
    }
    return true;

d3drelease:
    D3D->Release();
    D3D = NULL;
closelib:
    FreeLibrary(D3D9_dll);
    return false;
}

bool Win32Video::GoFullscreen(bool yes)
{
    if (m_IsFullscreen == yes) return yes;

    DWORD style = yes ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    DWORD exstyle = yes ? 0 : WS_EX_APPWINDOW;

    SetWindowLongPtr(Window, GWL_STYLE, style);
    SetWindowLongPtr(Window, GWL_EXSTYLE, exstyle);

    if (yes)
    {
        HMONITOR hm = MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hm, &mi);

        SetWindowPos(Window, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        BaseWinFB* oldfb = static_cast<BaseWinFB*>(screen);
        int cw = oldfb ? oldfb->Width : 640;
        int ch = oldfb ? oldfb->Height : 480;
        RECT rc = { 0, 0, (LONG)cw, (LONG)ch };
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HMONITOR hm = MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hm, &mi);
        int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

        SetWindowPos(Window, nullptr, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    m_IsFullscreen = yes;
    return yes;
}

void Win32Video::BlankForGDI()
{
    static_cast<BaseWinFB*> (screen)->Blank();
}

//==========================================================================
//
// Win32Video :: DumpAdapters
//
// Dumps the list of display adapters to the console. Only meaningful for
// Direct3D.
//
//==========================================================================
void Win32Video::DumpAdapters()
{
    if (D3D == NULL)
    {
        Printf("Multi-monitor support requires Direct3D.\n");
        return;
    }
    UINT num_adapters = D3D->GetAdapterCount();
    for (UINT i = 0; i < num_adapters; ++i)
    {
        D3DADAPTER_IDENTIFIER9 ai;
        char moreinfo[64] = "";
        if (FAILED(D3D->GetAdapterIdentifier(i, 0, &ai)))
        {
            continue;
        }
        for (char* p = ai.Description + strlen(ai.Description) - 1;
            p >= ai.Description && isspace(*p);
            --p)
        {
            *p = '\0';
        }
        HMONITOR hm = D3D->GetAdapterMonitor(i);
        MONITORINFOEX mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(hm, &mi))
        {
            mysnprintf(moreinfo, countof(moreinfo), " [%ldx%ld @ (%ld,%ld)]%s",
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.dwFlags & MONITORINFOF_PRIMARY ? " (Primary)" : "");
        }
        Printf("%s%u. %s%s\n",
            i == m_Adapter ? TEXTCOLOR_BOLD : "",
            i + 1, ai.Description, moreinfo);
    }
}

// Mode enumeration --------------------------------------------------------
void Win32Video::AddD3DModes(UINT adapter, D3DFORMAT format)
{
    UINT modecount, i;
    D3DDISPLAYMODE mode;
    modecount = D3D->GetAdapterModeCount(adapter, format);
    for (i = 0; i < modecount; ++i)
    {
        if (D3D_OK == D3D->EnumAdapterModes(adapter, format, i, &mode))
        {
            AddMode(mode.Width, mode.Height, 8, mode.Height, 0);
        }
    }
}

void Win32Video::AddLetterboxModes()
{
    ModeInfo* mode, * nextmode;
    for (mode = m_Modes; mode != NULL; mode = nextmode)
    {
        nextmode = mode->next;
        if (mode->realheight == mode->height && mode->height * 4 / 3 == mode->width)
        {
            if (mode->width >= 360)
            {
                AddMode(mode->width, mode->width * 9 / 16, mode->bits, mode->height, mode->doubling);
            }
            if (mode->width > 640)
            {
                AddMode(mode->width, mode->width * 10 / 16, mode->bits, mode->height, mode->doubling);
            }
        }
    }
}

void Win32Video::AddLowResModes()
{
    ModeInfo* mode, * nextmode;
    for (mode = m_Modes; mode != NULL; mode = nextmode)
    {
        nextmode = mode->next;
        if (mode->realheight == mode->height &&
            mode->doubling == 0 &&
            mode->height >= 200 * 2 &&
            mode->height <= 480 * 2 &&
            mode->width >= 320 * 2 &&
            mode->width <= 640 * 2)
        {
            AddMode(mode->width / 2, mode->height / 2, mode->bits, mode->height / 2, 1);
        }
    }
    for (mode = m_Modes; mode != NULL; mode = nextmode)
    {
        nextmode = mode->next;
        if (mode->realheight == mode->height &&
            mode->doubling == 0 &&
            mode->height >= 200 * 4 &&
            mode->height <= 480 * 4 &&
            mode->width >= 320 * 4 &&
            mode->width <= 640 * 4)
        {
            AddMode(mode->width / 4, mode->height / 4, mode->bits, mode->height / 4, 2);
        }
    }
}

void Win32Video::AddMode(int x, int y, int bits, int y2, int doubling)
{
    if ((x & 1) != 0
        || y > MAXHEIGHT
        || x > MAXWIDTH
        || y < 200
        || x < 320)
    {
        return;
    }

    ModeInfo** probep = &m_Modes;
    ModeInfo* probe = m_Modes;

    for (; probe != 0; probep = &probe->next, probe = probe->next)
    {
        if (probe->width > x)         break;
        if (probe->width < x)         continue;
        if (probe->height > y)        break;
        if (probe->height < y)        continue;
        if (probe->bits > bits)       break;
        if (probe->bits < bits)       continue;
        return;
    }

    *probep = new ModeInfo(x, y, bits, y2, doubling);
    (*probep)->next = probe;
}

void Win32Video::FreeModes()
{
    ModeInfo* mode = m_Modes;
    while (mode)
    {
        ModeInfo* tempmode = mode;
        mode = mode->next;
        delete tempmode;
    }
    m_Modes = NULL;
}

void Win32Video::ScaleModes(int doubling)
{
    ModeInfo* mode, ** prev;
    prev = &m_Modes;
    mode = m_Modes;
    while (mode != NULL)
    {
        assert(mode->doubling == 0);
        mode->width >>= doubling;
        mode->height >>= doubling;
        mode->realheight >>= doubling;
        mode->doubling = doubling;
        if ((mode->width & 7) != 0
            || mode->width < 320
            || mode->height < 200)
        {
            *prev = mode->next;
            delete mode;
        }
        else
        {
            prev = &mode->next;
        }
        mode = *prev;
    }
}

void Win32Video::StartModeIterator(int bits, bool fs)
{
    m_IteratorMode = m_Modes;
    m_IteratorBits = bits;
    m_IteratorFS = fs;
}

bool Win32Video::NextMode(int* width, int* height, bool* letterbox)
{
    if (m_IteratorMode)
    {
        while (m_IteratorMode && m_IteratorMode->bits != m_IteratorBits)
        {
            m_IteratorMode = m_IteratorMode->next;
        }
        if (m_IteratorMode)
        {
            *width = m_IteratorMode->width;
            *height = m_IteratorMode->height;
            if (letterbox != NULL) *letterbox = m_IteratorMode->realheight != m_IteratorMode->height;
            m_IteratorMode = m_IteratorMode->next;
            return true;
        }
    }
    return false;
}

DFrameBuffer* Win32Video::CreateFrameBuffer(int width, int height, bool fullscreen, DFrameBuffer* old)
{
    static int retry = 0;
    static int owidth, oheight;
    BaseWinFB* fb;
    PalEntry flashColor;
    int flashAmount;

    LOG4("CreateFB %d %d %d %p\n", width, height, fullscreen, old);

    if (old != NULL)
    {
        BaseWinFB* fb = static_cast<BaseWinFB*> (old);
        if (fb->Width == width &&
            fb->Height == height &&
            fb->Windowed == !fullscreen)
        {
            return old;
        }
        old->GetFlash(flashColor, flashAmount);
        old->ObjectFlags |= OF_YesReallyDelete;
        if (old == screen) screen = NULL;
        delete old;
    }
    else
    {
        flashColor = 0;
        flashAmount = 0;
    }

    if (D3D != NULL)
    {
        fb = new D3DFB(m_Adapter, width, height, fullscreen);
    }

    LOG1("New fb created @ %p\n", fb);

    while (fb == NULL || !fb->IsValid())
    {
        static HRESULT hr;
        if (fb != NULL)
        {
            if (retry == 0)
            {
                hr = fb->GetHR();
            }
            fb->ObjectFlags |= OF_YesReallyDelete;
            delete fb;
            LOG1("fb is bad: %08lx\n", hr);
        }
        else
        {
            LOG("Could not create fb at all\n");
        }
        screen = NULL;
        LOG1("Retry number %d\n", retry);
        switch (retry)
        {
        case 0:
            owidth = width;
            oheight = height;
            [[fallthrough]];
        case 2:
            I_ClosestResolution(&width, &height, 8);
            LOG2("Retry with size %d,%d\n", width, height);
            break;
        case 1:
            width = owidth;
            height = oheight;
            fullscreen = !fullscreen;
            LOG1("Retry with fullscreen %d\n", fullscreen);
            break;
        default:
            LOG3("Could not create new screen (%d x %d): %08lx", owidth, oheight, hr);
            I_FatalError("Could not create new screen (%d x %d): %08lx", owidth, oheight, hr);
        }
        ++retry;
        fb = static_cast<D3DFB*>(CreateFrameBuffer(width, height, fullscreen, NULL));
    }
    retry = 0;
    fb->SetFlash(flashColor, flashAmount);
    return fb;
}

void Win32Video::SetWindowedScale(float scale)
{

}

//==========================================================================
//
// SetFPSLimit
//
// Initializes an event timer to fire at a rate of <limit>/sec. The video
// update will wait for this timer to trigger before updating.
//
// Pass 0 as the limit for unlimited.
// Pass a negative value for the limit to use the value of vid_maxfps.
//
//==========================================================================
void I_SetFPSLimit(int limit)
{
    if (limit < 0)
    {
        limit = vid_maxfps;
    }
    if (FPSLimitTimer != 0)
    {
        timeKillEvent(FPSLimitTimer);
        FPSLimitTimer = 0;
    }
    if (limit == 0)
    {
        if (FPSLimitEvent != NULL)
        {
            CloseHandle(FPSLimitEvent);
            FPSLimitEvent = NULL;
        }
        DPrintf("FPS timer disabled\n");
    }
    else
    {
        if (FPSLimitEvent == NULL)
        {
            FPSLimitEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
            if (FPSLimitEvent == NULL)
            {
                Printf("Failed to create FPS limitter event\n");
                return;
            }
        }
        atterm(StopFPSLimit);
        UINT period = 1000 / limit;
        FPSLimitTimer = timeSetEvent(period, 0, (LPTIMECALLBACK)FPSLimitEvent, 0, TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
        if (FPSLimitTimer == 0)
        {
            CloseHandle(FPSLimitEvent);
            FPSLimitEvent = NULL;
            Printf("Failed to create FPS limitter timer\n");
            return;
        }
        DPrintf("FPS timer set to %u ms\n", period);
    }
}

//==========================================================================
//
// StopFPSLimit
//
// Used for cleanup during application shutdown.
//
//==========================================================================
static void StopFPSLimit()
{
    I_SetFPSLimit(0);
}
