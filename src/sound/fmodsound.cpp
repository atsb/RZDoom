/*
** fmodsound.cpp
** System interface for sound; uses FMOD Ex.
**
**---------------------------------------------------------------------------
** Copyright 1998-2009 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

/* Adam (Gibbon) 2021
* So I ended up taking the following:
* 1. FMOD_STUDIO API from GZDoom 2.4 (credits to Graf Zahl)
* 2. Sound and Music code (i_sound, i_music, s_sound) from ZDoom32 (credits to DrFrag)
* 3. Other code I Myself modified in order to correctly merge it all together
*    and have it working as it should be.
* 4. All OpenAL code is now removed.
*/

// HEADER FILES ------------------------------------------------------------

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
extern HWND Window;
#define USE_WINDOWS_DWORD
#else
#define FALSE 0
#define TRUE 1
#endif
#if defined(__FreeBSD__) || defined(__APPLE__)
#include <stdlib.h>
#elif __sun
#include <alloca.h>
#else
#include <malloc.h>
#endif

#include "except.h"
#include "templates.h"
#include "fmodsound.h"
#include "c_cvars.h"
#include "i_system.h"
#include "i_music.h"
#include "v_text.h"
#include "v_video.h"
#include "v_palette.h"
#include "cmdlib.h"
#include "s_sound.h"
#include "files.h"

#if FMOD_VERSION > 0x42899 && FMOD_VERSION < 0x43400
#error You are trying to compile with an unsupported version of FMOD.
#endif

// MACROS ------------------------------------------------------------------

// killough 2/21/98: optionally use varying pitched sounds
#define PITCH(freq,pitch) (snd_pitched ? ((freq)*(pitch))/128.f : float(freq))

// Just some extra for music and whatever
#define NUM_EXTRA_SOFTWARE_CHANNELS		1

#define MAX_CHANNELS				1024

#define SPECTRUM_SIZE				512

// PUBLIC DATA DEFINITIONS -------------------------------------------------

ReverbContainer *ForcedEnvironment;

CVAR (Int, snd_driver, 0, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (Int, snd_buffercount, 12, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (Bool, snd_waterreverb, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (String, snd_resampler, "Linear", CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (String, snd_speakermode, "Auto", CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (String, snd_output_format, "PCM-16", CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (String, snd_midipatchset, "", CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (Bool, snd_profile, false, 0)

// Underwater low-pass filter cutoff frequency. Set to 0 to disable the filter.
CUSTOM_CVAR (Float, snd_waterlp, 250, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	// Clamp to the DSP unit's limits.
	if (*self < 10 && *self != 0)
	{
		self = 10;
	}
	else if (*self > 22000)
	{
		self = 22000;
	}
}

CUSTOM_CVAR (Int, snd_streambuffersize, 512, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (self < 16)
	{
		self = 16;
	}
	else if (self > 1024)
	{
		self = 1024;
	}
}

// TYPES -------------------------------------------------------------------

struct FEnumList
{
	const char *Name;
	int Value;
};

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

static int Enum_NumForName(const FEnumList *list, const char *name);
static const char *Enum_NameForNum(const FEnumList *list, int num);

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

EXTERN_CVAR (String, snd_output)
EXTERN_CVAR (Float, snd_sfxvolume)
EXTERN_CVAR (Float, snd_musicvolume)
EXTERN_CVAR (Int, snd_buffersize)
EXTERN_CVAR (Int, snd_samplerate)
EXTERN_CVAR (Bool, snd_pitched)
EXTERN_CVAR (Int, snd_channels)

extern int sfx_empty;

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static const ReverbContainer *PrevEnvironment;
static bool ShowedBanner;

// The rolloff callback is called during FMOD::Sound::play, so we need this
// global variable to contain the sound info during that time for the
// callback.
static FRolloffInfo *GRolloff;
static float GDistScale;

// In the below lists, duplicate entries are for user selection. When
// queried, only the first one for the particular value is shown.
static const FEnumList OutputNames[] =
{
	{ "Auto",					FMOD_OUTPUTTYPE_AUTODETECT },
	{ "Default",				FMOD_OUTPUTTYPE_AUTODETECT },
	{ "No sound",				FMOD_OUTPUTTYPE_NOSOUND },

	// Windows
	{ "WASAPI",					FMOD_OUTPUTTYPE_WASAPI },
	{ "ASIO",					FMOD_OUTPUTTYPE_ASIO },

	//Android

	{ "OPENSL",					FMOD_OUTPUTTYPE_OPENSL },
	{ "Android Audio Track",	FMOD_OUTPUTTYPE_AUDIOTRACK },

	// Linux
	{ "ALSA",					FMOD_OUTPUTTYPE_ALSA },
	{ "PulseAudio",				FMOD_OUTPUTTYPE_PULSEAUDIO },
	{ "Pulse",					FMOD_OUTPUTTYPE_PULSEAUDIO },

	// Mac
	{ "Core Audio",				FMOD_OUTPUTTYPE_COREAUDIO },

	{ NULL, 0 }
};

static const FEnumList SpeakerModeNames[] =
{
	{ "Mono",					FMOD_SPEAKERMODE_MONO },
	{ "Stereo",					FMOD_SPEAKERMODE_STEREO },
	{ "Quad",					FMOD_SPEAKERMODE_QUAD },
	{ "Surround",				FMOD_SPEAKERMODE_SURROUND },
	{ "5.1",					FMOD_SPEAKERMODE_5POINT1 },
	{ "7.1",					FMOD_SPEAKERMODE_7POINT1 },
	{ "1",						FMOD_SPEAKERMODE_MONO },
	{ "2",						FMOD_SPEAKERMODE_STEREO },
	{ "4",						FMOD_SPEAKERMODE_QUAD },
	{ NULL, 0 }
};

static const FEnumList ResamplerNames[] =
{
	{ "No Interpolation",		FMOD_DSP_RESAMPLER_NOINTERP },
	{ "NoInterp",				FMOD_DSP_RESAMPLER_NOINTERP },
	{ "Linear",					FMOD_DSP_RESAMPLER_LINEAR },
	{ "Cubic",					FMOD_DSP_RESAMPLER_CUBIC },
	{ "Spline",					FMOD_DSP_RESAMPLER_SPLINE },
	{ NULL, 0 }
};

static const FEnumList SoundFormatNames[] =
{
	{ "None",					FMOD_SOUND_FORMAT_NONE },
	{ "PCM-8",					FMOD_SOUND_FORMAT_PCM8 },
	{ "PCM-16",					FMOD_SOUND_FORMAT_PCM16 },
	{ "PCM-24",					FMOD_SOUND_FORMAT_PCM24 },
	{ "PCM-32",					FMOD_SOUND_FORMAT_PCM32 },
	{ "PCM-Float",				FMOD_SOUND_FORMAT_PCMFLOAT },
	{ NULL, 0 }
};

static const char *OpenStateNames[] =
{
	"Ready",
	"Loading",
	"Error",
	"Connecting",
	"Buffering",
	"Seeking",
	"Streaming"
};

const FMODSoundRenderer::spk FMODSoundRenderer::SpeakerNames4[4] = { "L", "R", "BL", "BR" };
const FMODSoundRenderer::spk FMODSoundRenderer::SpeakerNamesMore[8] = { "L", "R", "C", "LFE", "BL", "BR", "SL", "SR" };

// CODE --------------------------------------------------------------------

//==========================================================================
//
// Enum_NumForName
//
// Returns the value of an enum name, or -1 if not found.
//
//==========================================================================

static int Enum_NumForName(const FEnumList *list, const char *name)
{
	while (list->Name != NULL)
	{
		if (stricmp(list->Name, name) == 0)
		{
			return list->Value;
		}
		list++;
	}
	return -1;
}

//==========================================================================
//
// Enum_NameForNum
//
// Returns the name of an enum value. If there is more than one name for a
// value, on the first one in the list is returned. Returns NULL if there
// was no match.
//
//==========================================================================

static const char *Enum_NameForNum(const FEnumList *list, int num)
{
	while (list->Name != NULL)
	{
		if (list->Value == num)
		{
			return list->Name;
		}
		list++;
	}
	return NULL;
}

//==========================================================================
//
// The container for a streaming FMOD::Sound, for playing music.
//
//==========================================================================

class FMODStreamCapsule : public SoundStream
{
public:
	FMODStreamCapsule(FMOD::Sound *stream, FMODSoundRenderer *owner, const char *url)
		: Owner(owner), Stream(NULL), Channel(NULL),
		  UserData(NULL), Callback(NULL), Reader(NULL), URL(url), Ended(false)
	{
		SetStream(stream);
	}

    FMODStreamCapsule(FMOD::Sound *stream, FMODSoundRenderer *owner, FileReader *reader)
        : Owner(owner), Stream(NULL), Channel(NULL),
          UserData(NULL), Callback(NULL), Reader(reader), Ended(false)
    {
        SetStream(stream);
    }

	FMODStreamCapsule(void *udata, SoundStreamCallback callback, FMODSoundRenderer *owner)
		: Owner(owner), Stream(NULL), Channel(NULL),
		  UserData(udata), Callback(callback), Reader(NULL), Ended(false)
	{}

	~FMODStreamCapsule()
	{
		if (Channel != NULL)
		{
			Channel->stop();
		}
		if (Stream != NULL)
		{
			Stream->release();
		}
		if (Reader != NULL)
		{
			delete Reader;
		}
	}

	void SetStream(FMOD::Sound *stream)
	{
		float frequency;

		Stream = stream;

		// As this interface is for music, make it super-high priority.
		if (FMOD_OK == stream->getDefaults(&frequency, NULL))
			stream->setDefaults(frequency, 1);
	}

	bool Play(bool looping, float volume)
	{
		FMOD_RESULT result;

		if (URL.IsNotEmpty())
		{ // Net streams cannot be looped, because they cannot be seeked.
			looping = false;
		}
		Stream->setMode((looping ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF) | FMOD_SOFTWARE | FMOD_2D);
		result = Owner->Sys->playSound(Stream,0, true, &Channel);
		if (result != FMOD_OK)
		{
			return false;
		}
		Channel->setChannelGroup(Owner->MusicGroup);
		Channel->setMixLevelsOutput(1, 1, 1, 1, 1, 1, 1, 1);
		Channel->setVolume(volume);
		// Ensure reverb is disabled.
		Channel->setReverbProperties(0,0.f);
		Channel->setPaused(false);
		Ended = false;
		JustStarted = true;
		Starved = false;
		Loop = looping;
		Volume = volume;
		return true;
	}

	void Stop()
	{
		if (Channel != NULL)
		{
			Channel->stop();
			Channel = NULL;
		}
	}

	bool SetPaused(bool paused)
	{
		if (Channel != NULL)
		{
			return FMOD_OK == Channel->setPaused(paused);
		}
		return false;
	}

	unsigned int GetPosition()
	{
		unsigned int pos;

		if (Channel != NULL && FMOD_OK == Channel->getPosition(&pos, FMOD_TIMEUNIT_MS))
		{
			return pos;
		}
		return 0;
	}

	bool IsEnded()
	{
		bool is;
		FMOD_OPENSTATE openstate = FMOD_OPENSTATE_MAX;
		bool starving;
		bool diskbusy;

		if (Stream == NULL)
		{
			return true;
		}
		if (FMOD_OK != Stream->getOpenState(&openstate, NULL, &starving, &diskbusy))
		{
			openstate = FMOD_OPENSTATE_ERROR;
		}
		if (openstate == FMOD_OPENSTATE_ERROR)
		{
			if (Channel != NULL)
			{
				Channel->stop();
				Channel = NULL;
			}
			return true;
		}
		if (Channel != NULL && (FMOD_OK != Channel->isPlaying(&is) || is == false))
		{
			return true;
		}
		if (Ended)
		{
			Channel->stop();
			Channel = NULL;
			return true;
		}
		if (URL.IsNotEmpty() && !JustStarted && openstate == FMOD_OPENSTATE_READY)
		{
			// Reconnect the stream, since it seems to have stalled.
			// The only way to do this appears to be to completely recreate it.
			FMOD_RESULT result;

			Channel->stop();
			Stream->release();
			Channel = NULL;
			Stream = NULL;
			// Open the stream asynchronously, so we don't hang the game while trying to reconnect.
			// (It would be nice to do the initial open asynchronously as well, but I'd need to rethink
			// the music system design to pull that off.)
			result = Owner->Sys->createSound(URL, (Loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF) | FMOD_SOFTWARE | FMOD_2D |
				FMOD_CREATESTREAM | FMOD_NONBLOCKING, NULL, &Stream);
			JustStarted = true;
			return result != FMOD_OK;
		}
		if (JustStarted && openstate == FMOD_OPENSTATE_PLAYING)
		{
			JustStarted = false;
		}
		if (JustStarted && Channel == NULL && openstate == FMOD_OPENSTATE_READY)
		{
			return !Play(Loop, Volume);
		}
		if (starving != Starved)
		{ // Mute the sound if it's starving.
			Channel->setVolume(starving ? 0 : Volume);
			Starved = starving;
		}
		return false;
	}

	void SetVolume(float volume)
	{
		if (Channel != NULL && !Starved)
		{
			Channel->setVolume(volume);
		}
		Volume = volume;
	}

	// Sets the position in ms.
	bool SetPosition(unsigned int ms_pos)
	{
		return FMOD_OK == Channel->setPosition(ms_pos, FMOD_TIMEUNIT_MS);
	}

	// Sets the order number for MOD formats.
	bool SetOrder(int order_pos)
	{
		return FMOD_OK == Channel->setPosition(order_pos, FMOD_TIMEUNIT_MODORDER);
	}

	FString GetStats()
	{
		FString stats;
		FMOD_OPENSTATE openstate;
		unsigned int percentbuffered;
		unsigned int position;
		bool starving;
		bool diskbusy;
		float volume;
		float frequency;
		bool paused;
		bool isplaying;

		if (FMOD_OK == Stream->getOpenState(&openstate, &percentbuffered, &starving, &diskbusy))
		{
			stats = (openstate <= FMOD_OPENSTATE_PLAYING ? OpenStateNames[openstate] : "Unknown state");
			stats.AppendFormat(",%3d%% buffered, %s", percentbuffered, starving ? "Starving" : "Well-fed");
		}
		if (Channel == NULL)
		{
			stats += ", not playing";
		}
		if (Channel != NULL && FMOD_OK == Channel->getPosition(&position, FMOD_TIMEUNIT_MS))
		{
			stats.AppendFormat(", %d", position);
			if (FMOD_OK == Stream->getLength(&position, FMOD_TIMEUNIT_MS))
			{
				stats.AppendFormat("/%d", position);
			}
			stats += " ms";
		}
		if (Channel != NULL && FMOD_OK == Channel->getVolume(&volume))
		{
			stats.AppendFormat(", %d%%", int(volume * 100));
		}
		if (Channel != NULL && FMOD_OK == Channel->getPaused(&paused) && paused)
		{
			stats += ", paused";
		}
		if (Channel != NULL && FMOD_OK == Channel->isPlaying(&isplaying) && isplaying)
		{
			stats += ", playing";
		}
		if (Channel != NULL && FMOD_OK == Channel->getFrequency(&frequency))
		{
			stats.AppendFormat(", %g Hz", frequency);
		}
		if (JustStarted)
		{
			stats += " JS";
		}
		if (Ended)
		{
			stats += " XX";
		}
		return stats;
	}

	static FMOD_RESULT PCMReadCallback(FMOD_SOUND *sound, void *data, unsigned int datalen)
	{
		FMOD_RESULT result;
		FMODStreamCapsule *self;
		
		result = ((FMOD::Sound *)sound)->getUserData((void **)&self);
		if (result != FMOD_OK || self == NULL || self->Callback == NULL || self->Ended)
		{
			// Contrary to the docs, this return value is completely ignored.
			return FMOD_OK;
		}
		if (!self->Callback(self, data, datalen, self->UserData))
		{
			self->Ended = true;
		}
		return FMOD_OK;
	}

	static FMOD_RESULT PCMSetPosCallback(FMOD_SOUND *sound, int subsound, unsigned int position, FMOD_TIMEUNIT postype)
	{
		// This is useful if the user calls Channel::setPosition and you want
		// to seek your data accordingly.
		return FMOD_OK;
	}

private:
	FMODSoundRenderer *Owner;
	FMOD::Sound *Stream;
	FMOD::Channel *Channel;
	void *UserData;
	SoundStreamCallback Callback;
    FileReader *Reader;
	FString URL;
	bool Ended;
	bool JustStarted;
	bool Starved;
	bool Loop;
	float Volume;
};

//==========================================================================
//
// The interface the game uses to talk to FMOD.
//
//==========================================================================

FMODSoundRenderer::FMODSoundRenderer()
{
	InitSuccess = Init();
}

FMODSoundRenderer::~FMODSoundRenderer()
{
	Shutdown();
}

bool FMODSoundRenderer::IsValid()
{
	return InitSuccess;
}

//==========================================================================
//
// FMODSoundRenderer :: Init
//
//==========================================================================

bool FMODSoundRenderer::Init()
{
	FMOD_RESULT result;
	unsigned int version;
	FMOD_SPEAKERMODE speakermode;
	FMOD_SOUND_FORMAT format;
	FMOD_DSP_RESAMPLER resampler;
	FMOD_INITFLAGS initflags;
	int samplerate;
	int driver;

	int eval;

	SFXPaused = 0;
	DSPLocked = false;
	MusicGroup = NULL;
	SfxGroup = NULL;
	PausableSfx = NULL;
	SfxConnection = NULL;
	WaterLP = NULL;
	WaterReverb = NULL;
	PrevEnvironment = DefaultEnvironments[0];
	DSPClock.AsOne = 0;
	ChannelGroupTargetUnit = NULL;
	ChannelGroupTargetUnitOutput = NULL;
	SfxReverbHooked = false;
	SfxReverbPlaceholder = NULL;
	OutputPlugin = 0;

	Printf("I_InitSound: Initializing FMOD\n");

	// This is just for safety. Normally this should never be called if FMod Ex cannot be found.
	if (!IsFModExPresent())
	{
		Sys = NULL;
		Printf(TEXTCOLOR_ORANGE"Failed to load fmodex"
#ifdef _WIN64
			"64"
#endif
			".dll\n");
		return false;
	}

	// Create a System object and initialize.
	result = FMOD::System_Create(&Sys);
	if (result != FMOD_OK)
	{
		Sys = NULL;
		Printf(TEXTCOLOR_ORANGE"Failed to create FMOD system object: Error %d\n", result);
		return false;
	}

	result = Sys->getVersion(&version);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_ORANGE"Could not validate FMOD version: Error %d\n", result);
		return false;
	}

	const char *wrongver = NULL;
	if (version < (FMOD_VERSION & 0xFFFF00))
	{
		wrongver = "an old";
	}
	else if ((version & 0xFFFF00) > (FMOD_VERSION & 0xFFFF00))
	{
		wrongver = "a new";
	}
	if (wrongver != NULL)
	{
		Printf (" " TEXTCOLOR_ORANGE "Error! You are using %s version of FMOD (%x.%02x.%02x).\n"
				" " TEXTCOLOR_ORANGE "This program was built for version %x.%02x.%02x\n",
				wrongver,
				version >> 16, (version >> 8) & 255, version & 255,
				FMOD_VERSION >> 16, (FMOD_VERSION >> 8) & 255, FMOD_VERSION & 255);
		return false;
	}
	ActiveFMODVersion = version;

	if (!ShowedBanner)
	{
		// '\xa9' is the copyright symbol in the Windows-1252 code page.
		Printf("FMOD Studio Sound System, copyright \xa9 Firelight Technologies Pty, Ltd., 1994-2023.\n");
		Printf("Loaded FMOD Studio version %x.%02x.%02x\n", version >> 16, (version >> 8) & 255, version & 255);
		ShowedBanner = true;
	}

	// Set the user specified output mode.
	eval = Enum_NumForName(OutputNames, snd_output);
	if (eval >= 0)
	{
		if (eval == 666 && OutputPlugin != 0)
		{
			result = Sys->setOutputByPlugin(OutputPlugin);
		}
		else
		{
			result = Sys->setOutput(FMOD_OUTPUTTYPE(eval));
		}
		if (result != FMOD_OK)
		{
			Printf(TEXTCOLOR_BLUE"Setting output type '%s' failed. Using default instead. (Error %d)\n", *snd_output, result);
			eval = FMOD_OUTPUTTYPE_AUTODETECT;
			Sys->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
		}
	}
	
	result = Sys->getNumDrivers(&driver);
	if (result == FMOD_OK)
	{
		if (driver == 0)
		{
			Printf(TEXTCOLOR_ORANGE"No working sound devices found. Try a different snd_output?\n");
			return false;
		}
		if (snd_driver >= driver)
		{
			Printf(TEXTCOLOR_BLUE"Driver %d does not exist. Using 0.\n", *snd_driver);
			driver = 0;
		}
		else
		{
			driver = snd_driver;
		}
		result = Sys->setDriver(driver);
	}
	result = Sys->getDriver(&driver);

	// We were built with an FMOD Studio that only returns the control panel frequency
	result = Sys->getDriverInfo(driver, nullptr, 0, nullptr, &Driver_MinFrequency, &speakermode, nullptr);
	Driver_MaxFrequency = Driver_MinFrequency;

	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"Could not ascertain driver capabilities. Some things may be weird. (Error %d)\n", result);
		// Fill in some default to pretend it worked. (But as long as we specify a valid driver,
		// can this call actually fail?)
		Driver_MinFrequency = 4000;
		Driver_MaxFrequency = 48000;
		speakermode = FMOD_SPEAKERMODE_STEREO;
	}

	// Set the user selected speaker mode.
	eval = Enum_NumForName(SpeakerModeNames, snd_speakermode);
	if (eval >= 0)
	{
		speakermode = FMOD_SPEAKERMODE(eval);
	}

	// Set software format
	eval = Enum_NumForName(SoundFormatNames, snd_output_format);
	format = eval >= 0 ? FMOD_SOUND_FORMAT(eval) : FMOD_SOUND_FORMAT_PCM16;
	if (format == FMOD_SOUND_FORMAT_PCM8)
	{
		// PCM-8 sounds like garbage with anything but DirectSound.
		FMOD_OUTPUTTYPE output;
		if (FMOD_OK != Sys->getOutput(&output) || output != FMOD_OUTPUTTYPE_WASAPI)
		{
			format = FMOD_SOUND_FORMAT_PCM16;
		}
	}
	eval = Enum_NumForName(ResamplerNames, snd_resampler);
	resampler = eval >= 0 ? FMOD_DSP_RESAMPLER(eval) : FMOD_DSP_RESAMPLER_LINEAR;
	// These represented the frequency limits for hardware channels, which we never used anyway.
//	samplerate = clamp<int>(snd_samplerate, Driver_MinFrequency, Driver_MaxFrequency);
	samplerate = snd_samplerate;
	if (samplerate == 0 || snd_samplerate == 0)
	{ // Creative's ASIO drivers report the only supported frequency as 0!
		if (FMOD_OK != Sys->getSoftwareFormat(&samplerate, NULL, NULL))
		{
			samplerate = 48000;
		}
	}
	if (samplerate != snd_samplerate && snd_samplerate != 0)
	{
		Printf(TEXTCOLOR_BLUE"Sample rate %d is unsupported. Trying %d.\n", *snd_samplerate, samplerate);
	}
	result = Sys->setSoftwareFormat(samplerate, speakermode, 0);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"Could not set mixing format. Defaults will be used. (Error %d)\n", result);
	}

	FMOD_ADVANCEDSETTINGS advSettings = {};
	advSettings.cbSize = sizeof advSettings;
	advSettings.resamplerMethod = resampler;
	result = Sys->setAdvancedSettings(&advSettings);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"Could not set resampler method. Defaults will be used. (Error %d)\n", result);
	}

	// Set software channels according to snd_channels
	result = Sys->setSoftwareChannels(snd_channels + NUM_EXTRA_SOFTWARE_CHANNELS);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"Failed to set the preferred number of channels. (Error %d)\n", result);
	}

	if (snd_buffersize != 0 || snd_buffercount != 0)
	{
		int buffersize = snd_buffersize ? snd_buffersize : 1024;
		int buffercount = snd_buffercount ? snd_buffercount : 4;
		result = Sys->setDSPBufferSize(buffersize, buffercount);
	}
	else
	{
		result = FMOD_OK;
	}
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"Setting DSP buffer size failed. (Error %d)\n", result);
	}

	// Try to init
	initflags = FMOD_INIT_NORMAL;

	if (snd_profile)
	{
#ifdef FMOD_INIT_PROFILE_ENABLE
		initflags |= FMOD_INIT_PROFILE_ENABLE;
#else
		initflags |= FMOD_INIT_ENABLE_PROFILE;
#endif
	}
	for (;;)
	{
		result = Sys->init(MAX(*snd_channels, MAX_CHANNELS), initflags, 0);
		if (result == FMOD_ERR_OUTPUT_CREATEBUFFER)
		{ 
			// Possible causes of a buffer creation failure:
			// 1. The speaker mode selected isn't supported by this soundcard. Force it to stereo.
			// 2. The output format is unsupported. Force it to 16-bit PCM.
			// 3. ???
			result = Sys->getSoftwareFormat(nullptr, &speakermode, nullptr);
			if (result == FMOD_OK &&
				speakermode != FMOD_SPEAKERMODE_STEREO &&
				FMOD_OK == Sys->setSoftwareFormat(samplerate, FMOD_SPEAKERMODE_STEREO, 0))
			{
				Printf(TEXTCOLOR_RED"  Buffer creation failed. Retrying with stereo output.\n");
				continue;
			}
		}
		else if (result == FMOD_ERR_NET_SOCKET_ERROR &&
#ifdef FMOD_INIT_PROFILE_ENABLE
				 (initflags & FMOD_INIT_PROFILE_ENABLE))
#else
				 (initflags & FMOD_INIT_ENABLE_PROFILE))
#endif
		{
			Printf(TEXTCOLOR_RED"  Could not create socket. Retrying without profiling.\n");
#ifdef FMOD_INIT_PROFILE_ENABLE
			initflags &= ~FMOD_INIT_PROFILE_ENABLE;
#else
			initflags &= ~FMOD_INIT_ENABLE_PROFILE;
#endif
			continue;
		}
#ifdef _WIN32
		else if (result == FMOD_ERR_OUTPUT_INIT)
		{
			FMOD_OUTPUTTYPE output;
			result = Sys->getOutput(&output);
			if (result == FMOD_OK && output != FMOD_OUTPUTTYPE_WASAPI)
			{
				Printf(TEXTCOLOR_BLUE"  Init failed for output type %s. Retrying with Auto Detection.\n",
					Enum_NameForNum(OutputNames, output));
				if (FMOD_OK == Sys->setOutput(FMOD_OUTPUTTYPE_AUTODETECT))
				{
					continue;
				}
			}
		}
#endif
		break;
	}
	if (result != FMOD_OK)
	{ // Initializing FMOD failed. Cry cry.
		Printf(TEXTCOLOR_ORANGE"  System::init returned error code %d\n", result);
		return false;
	}

	// Create channel groups
	result = Sys->createChannelGroup("Music", &MusicGroup);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_ORANGE"  Could not create music channel group. (Error %d)\n", result);
		return false;
	}

	result = Sys->createChannelGroup("SFX", &SfxGroup);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_ORANGE"  Could not create sfx channel group. (Error %d)\n", result);
		return false;
	}

	result = Sys->createChannelGroup("Pausable SFX", &PausableSfx);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_ORANGE"  Could not create pausable sfx channel group. (Error %d)\n", result);
		return false;
	}

	result = SfxGroup->addGroup(PausableSfx);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"  Could not attach pausable sfx to sfx channel group. (Error %d)\n", result);
	}

	// Create DSP units for underwater effect
	result = Sys->createDSPByType(FMOD_DSP_TYPE_LOWPASS, &WaterLP);
	if (result != FMOD_OK)
	{
		Printf(TEXTCOLOR_BLUE"  Could not create underwater lowpass unit. (Error %d)\n", result);
	}
	else
	{
		result = Sys->createDSPByType(FMOD_DSP_TYPE_SFXREVERB, &WaterReverb);
		if (result != FMOD_OK)
		{
			Printf(TEXTCOLOR_BLUE"  Could not create underwater reverb unit. (Error %d)\n", result);
		}
	}

	// Connect underwater DSP unit between PausableSFX and SFX groups, while
	// retaining the connection established by SfxGroup->addGroup().
	if (WaterLP != NULL)
	{
		FMOD::DSP *sfx_head, *pausable_head;

		result = SfxGroup->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &sfx_head);
		if (result == FMOD_OK)
		{
			result = sfx_head->getInput(0, &pausable_head, &SfxConnection);
			if (result == FMOD_OK)
			{
				// The placeholder mixer is for reference to where to connect the SFX
				// reverb unit once it gets created.
				result = Sys->createDSPByType(FMOD_DSP_TYPE_MIXER, &SfxReverbPlaceholder);
				if (result == FMOD_OK)
				{
					// Replace the PausableSFX->SFX connection with
					// PausableSFX->ReverbPlaceholder->SFX.
					result = SfxReverbPlaceholder->addInput(pausable_head, NULL);
					if (result == FMOD_OK)
					{
						FMOD::DSPConnection *connection;
						result = sfx_head->addInput(SfxReverbPlaceholder, &connection);
						if (result == FMOD_OK)
						{
							sfx_head->disconnectFrom(pausable_head);
							SfxReverbPlaceholder->setActive(true);
							SfxReverbPlaceholder->setBypass(true);
							// The placeholder now takes the place of the pausable_head
							// for the following connections.
							pausable_head = SfxReverbPlaceholder;
							SfxConnection = connection;
						}
					}
					else
					{
						SfxReverbPlaceholder->release();
						SfxReverbPlaceholder = NULL;
					}
				}
				result = WaterLP->addInput(pausable_head, NULL);
				WaterLP->setActive(false);
				WaterLP->setParameterFloat(FMOD_DSP_LOWPASS_CUTOFF, snd_waterlp);
				WaterLP->setParameterFloat(FMOD_DSP_LOWPASS_RESONANCE, 2);

				if (WaterReverb != NULL)
				{
					result = WaterReverb->addInput(WaterLP, NULL);
					if (result == FMOD_OK)
					{
						result = sfx_head->addInput(WaterReverb, NULL);
						if (result == FMOD_OK)
						{
							// These parameters are entirely empirical and can probably
							// stand some improvement, but it sounds remarkably close
							// to the old reverb unit's output.
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_LOWSHELFFREQUENCY, 150);
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_HFREFERENCE, 10000);
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_DRYLEVEL, 0);
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_HFDECAYRATIO, 100);
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_DECAYTIME, 0.25f);
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_DENSITY, 100);
							WaterReverb->setParameterFloat(FMOD_DSP_SFXREVERB_DIFFUSION, 100);
							WaterReverb->setActive(false);
						}
					}
				}
				else
				{
					result = sfx_head->addInput(WaterLP, NULL);
				}
			}
		}
	}
	LastWaterLP = snd_waterlp;

	// Find the FMOD Channel Group Target Unit. To completely eliminate sound
	// while the program is deactivated, we can deactivate this DSP unit, and
	// all audio processing will cease. This is not directly exposed by the
	// API but can be easily located by getting the master channel group and
	// tracing its single output, since it is known to hook up directly to the
	// Channel Group Target Unit. (See FMOD Profiler for proof.)
	FMOD::ChannelGroup *master_group;
	result = Sys->getMasterChannelGroup(&master_group);
	if (result == FMOD_OK)
	{
		FMOD::DSP *master_head;

		result = master_group->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &master_head);
		if (result == FMOD_OK)
		{
			result = master_head->getOutput(0, &ChannelGroupTargetUnit, NULL);
			if (result != FMOD_OK)
			{
				ChannelGroupTargetUnit = NULL;
			}
			else
			{
				FMOD::DSP *dontcare;
				result = ChannelGroupTargetUnit->getOutput(0, &dontcare, &ChannelGroupTargetUnitOutput);
				if (result != FMOD_OK)
				{
					ChannelGroupTargetUnitOutput = NULL;
				}
			}
		}
	}

	if (FMOD_OK != Sys->getSoftwareFormat(&OutputRate, NULL, NULL))
	{
		OutputRate = 48000;		// Guess, but this should never happen.
	}
	Sys->set3DSettings(0.5f, 96.f, 1.f);
	Sys->set3DRolloffCallback(RolloffCallback);
	Sys->setStreamBufferSize(snd_streambuffersize * 1024, FMOD_TIMEUNIT_RAWBYTES);
	snd_sfxvolume.Callback ();
	return true;
}

//==========================================================================
//
// FMODSoundRenderer :: Shutdown
//
//==========================================================================

void FMODSoundRenderer::Shutdown()
{
	if (Sys != NULL)
	{
		if (MusicGroup != NULL)
		{
			MusicGroup->release();
			MusicGroup = NULL;
		}
		if (PausableSfx != NULL)
		{
			PausableSfx->release();
			PausableSfx = NULL;
		}
		if (SfxGroup != NULL)
		{
			SfxGroup->release();
			SfxGroup = NULL;
		}
		if (WaterLP != NULL)
		{
			WaterLP->release();
			WaterLP = NULL;
		}
		if (WaterReverb != NULL)
		{
			WaterReverb->release();
			WaterReverb = NULL;
		}
		if (SfxReverbPlaceholder != NULL)
		{
			SfxReverbPlaceholder->release();
			SfxReverbPlaceholder = NULL;
		}

		Sys->close();
		if (OutputPlugin != 0)
		{
			Sys->unloadPlugin(OutputPlugin);
			OutputPlugin = 0;
		}
		Sys->release();
		Sys = NULL;
	}
}

//==========================================================================
//
// FMODSoundRenderer :: GetOutputRate
//
//==========================================================================

float FMODSoundRenderer::GetOutputRate()
{
	return (float)OutputRate;
}

//==========================================================================
//
// FMODSoundRenderer :: PrintStatus
//
//==========================================================================

void FMODSoundRenderer::PrintStatus()
{
	FMOD_OUTPUTTYPE output;
	FMOD_SPEAKERMODE speakermode;
	int driver;
	int samplerate;
	unsigned int bufferlength;
	int numbuffers;

	Printf ("Loaded FMOD version: " TEXTCOLOR_GREEN "%x.%02x.%02x\n", ActiveFMODVersion >> 16,
		(ActiveFMODVersion >> 8) & 255, ActiveFMODVersion & 255);
	if (FMOD_OK == Sys->getOutput(&output))
	{
		Printf ("Output type: " TEXTCOLOR_GREEN "%s\n", Enum_NameForNum(OutputNames, output));
	}
	if (FMOD_OK == Sys->getSoftwareFormat(&samplerate, &speakermode, nullptr))
	{
		Printf ("Speaker mode: " TEXTCOLOR_GREEN "%s\n", Enum_NameForNum(SpeakerModeNames, speakermode));
		Printf (TEXTCOLOR_LIGHTBLUE "Software mixer sample rate: " TEXTCOLOR_GREEN "%d\n", samplerate);
	}
	if (FMOD_OK == Sys->getDriver(&driver))
	{
		char name[256];
		if (FMOD_OK != Sys->getDriverInfo(driver, name, sizeof(name), nullptr, nullptr, nullptr, nullptr))
		{
			strcpy(name, "Unknown");
		}
		Printf ("Driver: " TEXTCOLOR_GREEN "%d" TEXTCOLOR_NORMAL " (" TEXTCOLOR_ORANGE "%s" TEXTCOLOR_NORMAL ")\n", driver, name);
	}
	if (FMOD_OK == Sys->getDSPBufferSize(&bufferlength, &numbuffers))
	{
		Printf (TEXTCOLOR_LIGHTBLUE "DSP buffers: " TEXTCOLOR_GREEN "%u samples x %d\n", bufferlength, numbuffers);
	}
}

//==========================================================================
//
// FMODSoundRenderer :: PrintDriversList
//
//==========================================================================

void FMODSoundRenderer::PrintDriversList()
{
	int numdrivers;
	int i;
	char name[256];
	
	if (FMOD_OK == Sys->getNumDrivers(&numdrivers))
	{
		for (i = 0; i < numdrivers; ++i)
		{
			if (FMOD_OK == Sys->getDriverInfo(i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr))
			{
				Printf("%d. %s\n", i, name);
			}
		}
	}
}

//==========================================================================
//
// FMODSoundRenderer :: GatherStats
//
//==========================================================================

FString FMODSoundRenderer::GatherStats()
{
	int channels;
	float dsp, stream, update, geometry, total;
	FString out;

	channels = 0;
	total = update = geometry = stream = dsp = 0;
	Sys->getChannelsPlaying(&channels);
		((FMOD_RESULT (F_API *)(FMOD_SYSTEM *, float *, float *, float *, float *))
			FMOD_System_GetCPUUsage)((FMOD_SYSTEM *)Sys, &dsp, &stream, &update, &total);

	out.Format ("%d channels," TEXTCOLOR_YELLOW "%5.2f" TEXTCOLOR_NORMAL "%% CPU "
		"(DSP:" TEXTCOLOR_YELLOW "%5.2f" TEXTCOLOR_NORMAL "%% "
		"Stream:" TEXTCOLOR_YELLOW "%5.2f" TEXTCOLOR_NORMAL "%% "
		"Geometry:" TEXTCOLOR_YELLOW "%5.2f" TEXTCOLOR_NORMAL "%% "
		"Update:" TEXTCOLOR_YELLOW "%5.2f" TEXTCOLOR_NORMAL "%%)",
		channels, total, dsp, stream, geometry, update);
	return out;
}

//==========================================================================
//
// FMODSoundRenderer :: SetSfxVolume
//
//==========================================================================

void FMODSoundRenderer::SetSfxVolume(float volume)
{
	SfxGroup->setVolume(volume);
}

//==========================================================================
//
// FMODSoundRenderer :: SetMusicVolume
//
//==========================================================================

void FMODSoundRenderer::SetMusicVolume(float volume)
{
	MusicGroup->setVolume(volume);
}

//==========================================================================
//
// FMODSoundRenderer :: CreateStream
//
// Creates a streaming sound that receives PCM data through a callback.
//
//==========================================================================

SoundStream *FMODSoundRenderer::CreateStream (SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
{
	FMODStreamCapsule *capsule;
	FMOD::Sound *sound;
	FMOD_RESULT result;
	FMOD_CREATESOUNDEXINFO exinfo;
	FMOD_MODE mode;
	int sample_shift;
	int channel_shift;

	InitCreateSoundExInfo(&exinfo);
	capsule = new FMODStreamCapsule (userdata, callback, this);

	mode = FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL | FMOD_SOFTWARE | FMOD_CREATESTREAM | FMOD_OPENONLY;
	sample_shift = (flags & (SoundStream::Bits32 | SoundStream::Float)) ? 2 : (flags & SoundStream::Bits8) ? 0 : 1;
	channel_shift = (flags & SoundStream::Mono) ? 0 : 1;

	// Chunk size of stream update in samples. This will be the amount of data
	// passed to the user callback.
	exinfo.decodebuffersize	 = buffbytes >> (sample_shift + channel_shift);

	// Number of channels in the sound.
	exinfo.numchannels		 = 1 << channel_shift;

	// Length of PCM data in bytes of whole song (for Sound::getLength).
	// This pretends it's extremely long.
	exinfo.length			 = ~0u;

	// Default playback rate of sound. */
	exinfo.defaultfrequency	 = samplerate;

	// Data format of sound.
	if (flags & SoundStream::Float)
	{
		exinfo.format = FMOD_SOUND_FORMAT_PCMFLOAT;
	}
	else if (flags & SoundStream::Bits32)
	{
		exinfo.format = FMOD_SOUND_FORMAT_PCM32;
	}
	else if (flags & SoundStream::Bits8)
	{
		exinfo.format = FMOD_SOUND_FORMAT_PCM8;
	}
	else
	{
		exinfo.format = FMOD_SOUND_FORMAT_PCM16;
	}

	// User callback for reading.
	exinfo.pcmreadcallback	 = FMODStreamCapsule::PCMReadCallback;

	// User callback for seeking.
	exinfo.pcmsetposcallback = FMODStreamCapsule::PCMSetPosCallback;

	// User data to be attached to the sound during creation.  Access via Sound::getUserData.
	exinfo.userdata = capsule;

	result = Sys->createSound(NULL, mode, &exinfo, &sound);
	if (result != FMOD_OK)
	{
		delete capsule;
		return NULL;
	}
	capsule->SetStream(sound);
	return capsule;
}

//==========================================================================
//
// GetTagData
//
// Checks for a string-type tag, and returns its data.
//
//==========================================================================

const char *GetTagData(FMOD::Sound *sound, const char *tag_name)
{
	FMOD_TAG tag;

	if (FMOD_OK == sound->getTag(tag_name, 0, &tag) &&
		(tag.datatype == FMOD_TAGDATATYPE_STRING || tag.datatype == FMOD_TAGDATATYPE_STRING_UTF8))
	{
		return (const char *)tag.data;
	}
	return NULL;
}

//==========================================================================
//
// SetCustomLoopPts
//
// Sets up custom sound loops by checking for these tags:
//    LOOP_START
//    LOOP_END
//    LOOP_BIDI
//
//==========================================================================

static void SetCustomLoopPts(FMOD::Sound *sound)
{
#if 0
	FMOD_TAG tag;
	int numtags;
	if (FMOD_OK == stream->getNumTags(&numtags, NULL))
	{
		for (int i = 0; i < numtags; ++i)
		{
			if (FMOD_OK == sound->getTag(NULL, i, &tag))
			{
				Printf("Tag %2d. %d %s = %s\n", i, tag.datatype, tag.name, tag.data);
			}
		}
	}
#endif
	const char *tag_data;
	unsigned int looppt[2];
	bool looppt_as_samples[2], have_looppt[2] = { false };
	static const char *const loop_tags[2] = { "LOOP_START", "LOOP_END" };

	for (int i = 0; i < 2; ++i)
	{
		if (NULL != (tag_data = GetTagData(sound, loop_tags[i])))
		{
			if (S_ParseTimeTag(tag_data, &looppt_as_samples[i], &looppt[i]))
			{
				have_looppt[i] = true;
			}
			else
			{
				Printf("Invalid %s tag: '%s'\n", loop_tags[i], tag_data);
			}
		}
	}
	if (have_looppt[0] && !have_looppt[1])
	{ // Have a start tag, but not an end tag: End at the end of the song.
		have_looppt[1] = (FMOD_OK == sound->getLength(&looppt[1], FMOD_TIMEUNIT_PCM));
		looppt_as_samples[1] = true;
	}
	else if (!have_looppt[0] && have_looppt[1])
	{ // Have an end tag, but no start tag: Start at beginning of the song.
		looppt[0] = 0;
		looppt_as_samples[0] = true;
		have_looppt[0] = true;
	}
	if (have_looppt[0] && have_looppt[1])
	{ // Have both loop points: Try to set the loop.
		FMOD_RESULT res = sound->setLoopPoints(
			looppt[0], looppt_as_samples[0] ? FMOD_TIMEUNIT_PCM : FMOD_TIMEUNIT_MS,
			looppt[1] - 1, looppt_as_samples[1] ? FMOD_TIMEUNIT_PCM : FMOD_TIMEUNIT_MS);
		if (res != FMOD_OK)
		{
			Printf("Setting custom loop points failed. Error %d\n", res);
		}
	}
	// Check for a bi-directional loop.
	if (NULL != (tag_data = GetTagData(sound, "LOOP_BIDI")) &&
		(stricmp(tag_data, "on") == 0 ||
		 stricmp(tag_data, "true") == 0 ||
		 stricmp(tag_data, "yes") == 0 ||
		 stricmp(tag_data, "1") == 0))
	{
		FMOD_MODE mode;
		if (FMOD_OK == (sound->getMode(&mode)))
		{
			sound->setMode((mode & ~(FMOD_LOOP_OFF | FMOD_LOOP_NORMAL)) | FMOD_LOOP_BIDI);
		}
	}
}

//==========================================================================
//
// open_reader_callback
// close_reader_callback
// read_reader_callback
// seek_reader_callback
//
// FMOD_CREATESOUNDEXINFO callbacks to handle reading resource data from a
// FileReader.
//
//==========================================================================

static FMOD_RESULT open_reader_callback(const char *name, unsigned int *filesize, void **handle, void *userdata)
{
    FileReader *reader = NULL;
    if(sscanf(name, "_FileReader_%p", &reader) != 1)
    {
        Printf("Invalid name in callback: %s\n", name);
        return FMOD_ERR_FILE_NOTFOUND;
    }

    *filesize = reader->GetLength();
    *handle = reader;
    return FMOD_OK;
}

static FMOD_RESULT close_reader_callback(void *handle, void *userdata)
{
    return FMOD_OK;
}

static FMOD_RESULT read_reader_callback(void *handle, void *buffer, unsigned int sizebytes, unsigned int *bytesread, void *userdata)
{
    FileReader *reader = reinterpret_cast<FileReader*>(handle);
    *bytesread = reader->Read(buffer, sizebytes);
    if(*bytesread > 0) return FMOD_OK;
    return FMOD_ERR_FILE_EOF;
}

static FMOD_RESULT seek_reader_callback(void *handle, unsigned int pos, void *userdata)
{
    FileReader *reader = reinterpret_cast<FileReader*>(handle);
    if(reader->Seek(pos, SEEK_SET) == 0)
        return FMOD_OK;
    return FMOD_ERR_FILE_COULDNOTSEEK;
}


//==========================================================================
//
// FMODSoundRenderer :: OpenStream
//
// Creates a streaming sound from a FileReader.
//
//==========================================================================

SoundStream *FMODSoundRenderer::OpenStream(FileReader *reader, int flags)
{
    FMOD_MODE mode;
    FMOD_CREATESOUNDEXINFO exinfo;
    FMOD::Sound *stream;
    FMOD_RESULT result;
    FString patches;
    FString name;

    InitCreateSoundExInfo(&exinfo);
    exinfo.fileuseropen  = open_reader_callback;
    exinfo.fileuserclose = close_reader_callback;
    exinfo.fileuserread  = read_reader_callback;
    exinfo.fileuserseek  = seek_reader_callback;

    mode = FMOD_SOFTWARE | FMOD_2D | FMOD_CREATESTREAM;
    if(flags & SoundStream::Loop)
        mode |= FMOD_LOOP_NORMAL;
    if((*snd_midipatchset)[0] != '\0')
    {
#ifdef _WIN32
        // If the path does not contain any path separators, automatically
        // prepend $PROGDIR to the path.
        if (strcspn(snd_midipatchset, ":/\\") == strlen(snd_midipatchset))
        {
            patches << "$PROGDIR/" << snd_midipatchset;
            patches = NicePath(patches);
        }
        else
#endif
        {
            patches = NicePath(snd_midipatchset);
        }
        exinfo.dlsname = patches;
    }

    name.Format("_FileReader_%p", reader);
    result = Sys->createSound(name, mode, &exinfo, &stream);
    if(result == FMOD_ERR_FORMAT && exinfo.dlsname != NULL)
    {
        // FMOD_ERR_FORMAT could refer to either the main sound file or
        // to the DLS instrument set. Try again without special DLS
        // instruments to see if that lets it succeed.
        exinfo.dlsname = NULL;
        result = Sys->createSound(name, mode, &exinfo, &stream);
        if (result == FMOD_OK)
        {
            Printf("%s is an unsupported format.\n", *snd_midipatchset);
        }
    }
    if(result != FMOD_OK)
        return NULL;

    SetCustomLoopPts(stream);
    return new FMODStreamCapsule(stream, this, reader);
}

SoundStream *FMODSoundRenderer::OpenStream(const char *url, int flags)
{
    FMOD_MODE mode;
    FMOD_CREATESOUNDEXINFO exinfo;
    FMOD::Sound *stream;
    FMOD_RESULT result;
    FString patches;

    InitCreateSoundExInfo(&exinfo);
    mode = FMOD_SOFTWARE | FMOD_2D | FMOD_CREATESTREAM;
    if(flags & SoundStream::Loop)
        mode |= FMOD_LOOP_NORMAL;
    if((*snd_midipatchset)[0] != '\0')
    {
#ifdef _WIN32
        // If the path does not contain any path separators, automatically
        // prepend $PROGDIR to the path.
        if (strcspn(snd_midipatchset, ":/\\") == strlen(snd_midipatchset))
        {
            patches << "$PROGDIR/" << snd_midipatchset;
            patches = NicePath(patches);
        }
        else
#endif
        {
            patches = NicePath(snd_midipatchset);
        }
        exinfo.dlsname = patches;
    }

    result = Sys->createSound(url, mode, &exinfo, &stream);
    if(result == FMOD_ERR_FORMAT && exinfo.dlsname != NULL)
    {
        exinfo.dlsname = NULL;
        result = Sys->createSound(url, mode, &exinfo, &stream);
        if(result == FMOD_OK)
        {
            Printf("%s is an unsupported format.\n", *snd_midipatchset);
        }
    }

    if(result != FMOD_OK)
        return NULL;

    SetCustomLoopPts(stream);
    return new FMODStreamCapsule(stream, this, url);
}

//==========================================================================
//
// FMODSoundRenderer :: StartSound
//
//==========================================================================

FISoundChannel *FMODSoundRenderer::StartSound(SoundHandle sfx, float vol, int pitch, int flags, FISoundChannel *reuse_chan)
{
	FMOD_RESULT result;
	FMOD_MODE mode;
	FMOD::Channel *chan;
	float freq;

	if (FMOD_OK == ((FMOD::Sound *)sfx.data)->getDefaults(&freq, NULL))
	{
		freq = PITCH(freq, pitch);
	}
	else
	{
		freq = 0;
	}

	GRolloff = NULL;	// Do 2D sounds need rolloff?
	result = Sys->playSound((FMOD::Sound *)sfx.data, (flags & SNDF_NOPAUSE) ? SfxGroup : PausableSfx, true, &chan);
	if (FMOD_OK == result)
	{
		result = chan->getMode(&mode);

		if (result != FMOD_OK)
		{
			assert(0);
			mode = FMOD_SOFTWARE;
		}
		mode = (mode & ~FMOD_3D) | FMOD_2D;
		if (flags & SNDF_LOOP)
		{
			mode &= ~FMOD_LOOP_OFF;
			if (!(mode & (FMOD_LOOP_NORMAL | FMOD_LOOP_BIDI)))
			{
				mode |= FMOD_LOOP_NORMAL;
			}
		}
		else
		{
			mode |= FMOD_LOOP_OFF;
		}
		chan->setMode(mode);
		if (freq != 0)
		{
			chan->setFrequency(freq);
		}
		chan->setVolume(vol);
		if (!HandleChannelDelay(chan, reuse_chan, flags & (SNDF_ABSTIME | SNDF_LOOP), freq))
		{
			chan->stop();
			return NULL;
		}
		if (flags & SNDF_NOREVERB)
		{
			chan->setReverbProperties(0,0.f);
		}
		chan->setPaused(false);
		return CommonChannelSetup(chan, reuse_chan);
	}

	//DPrintf (DMSG_WARNING, "Sound %s failed to play: %d\n", sfx->name.GetChars(), result);
	return NULL;
}

//==========================================================================
//
// FMODSoundRenderer :: StartSound3D
//
//==========================================================================

FISoundChannel *FMODSoundRenderer::StartSound3D(SoundHandle sfx, SoundListener *listener, float vol, 
	FRolloffInfo *rolloff, float distscale,
	int pitch, int priority, const FVector3 &pos, const FVector3 &vel,
	int channum, int flags, FISoundChannel *reuse_chan)
{
	FMOD_RESULT result;
	FMOD_MODE mode;
	FMOD::Channel *chan;
	float freq;
	float def_freq;
	int numchans;
	int def_priority;

	if (FMOD_OK == ((FMOD::Sound *)sfx.data)->getDefaults(&def_freq, &def_priority))
	{
		freq = PITCH(def_freq, pitch);
		// Change the sound's default priority before playing it.
		((FMOD::Sound *)sfx.data)->setDefaults(def_freq, clamp(def_priority - priority, 1, 256));
	}
	else
	{
		freq = 0;
		def_priority = -1;
	}

	// Play it.
	GRolloff = rolloff;
	GDistScale = distscale;

	// Experiments indicate that playSound will ignore priorities and always succeed
	// as long as the parameters are set properly. It will first try to kick out sounds
	// with the same priority level but has no problem with kicking out sounds at
	// higher priority levels if it needs to.
	result = Sys->playSound((FMOD::Sound *)sfx.data, (flags & SNDF_NOPAUSE) ? SfxGroup : PausableSfx, true, &chan);

	// Then set the priority back.
	if (def_priority >= 0)
	{
		((FMOD::Sound *)sfx.data)->setDefaults(def_freq,  def_priority);
	}

	if (FMOD_OK == result)
	{
		result = chan->getMode(&mode);
		if (result != FMOD_OK)
		{
			mode = FMOD_3D | FMOD_SOFTWARE;
		}
		if (flags & SNDF_LOOP)
		{
			mode &= ~FMOD_LOOP_OFF;
			if (!(mode & (FMOD_LOOP_NORMAL | FMOD_LOOP_BIDI)))
			{
				mode |= FMOD_LOOP_NORMAL;
			}
		}
		else
		{
			// FMOD_LOOP_OFF overrides FMOD_LOOP_NORMAL and FMOD_LOOP_BIDI
			mode |= FMOD_LOOP_OFF;
		}
		mode = SetChanHeadSettings(listener, chan, pos, !!(flags & SNDF_AREA), mode);
		chan->setMode(mode);

		if (mode & FMOD_3D)
		{
			// Reduce volume of stereo sounds, because each channel will be summed together
			// and is likely to be very similar, resulting in an amplitude twice what it
			// would have been had it been mixed to mono.
			if (FMOD_OK == ((FMOD::Sound *)sfx.data)->getFormat(NULL, NULL, &numchans, NULL))
			{
				if (numchans > 1)
				{
					vol *= 0.5f;
				}
			}
		}
		if (freq != 0)
		{
			chan->setFrequency(freq);
		}
		chan->setVolume(vol);
		if (mode & FMOD_3D)
		{
			chan->set3DAttributes((FMOD_VECTOR *)&pos[0], (FMOD_VECTOR *)&vel[0]);
		}
		if (!HandleChannelDelay(chan, reuse_chan, flags & (SNDF_ABSTIME | SNDF_LOOP), freq))
		{
			// FMOD seems to get confused if you stop a channel right after
			// starting it, so hopefully this function will never fail.
			// (Presumably you need an update between them, but I haven't
			// tested this hypothesis.)
			chan->stop();
			return NULL;
		}
		if (flags & SNDF_NOREVERB)
		{
			chan->setReverbProperties(0,0.f);
		}
		chan->setPaused(false);
		chan->getPriority(&def_priority);
		FISoundChannel *schan = CommonChannelSetup(chan, reuse_chan);
		schan->Rolloff = *rolloff;
		return schan;
	}

	GRolloff = NULL;
	//DPrintf (DMSG_WARNING, "Sound %s failed to play: %d\n", sfx->name.GetChars(), result);
	return 0;
}

//==========================================================================
//
// FMODSoundRenderer :: MarkStartTime
//
// Marks a channel's start time without actually playing it.
//
//==========================================================================

void FMODSoundRenderer::MarkStartTime(FISoundChannel *chan)
{
	unsigned long long int dsp_time;
	((FMOD::Channel *)chan->SysChannel)->getDSPClock(&dsp_time,NULL);
	chan->StartTime.Lo = dsp_time & 0xFFFFFFFF;
	chan->StartTime.Hi = dsp_time >> 32;
}

//==========================================================================
//
// FMODSoundRenderer :: HandleChannelDelay
//
// If the sound is restarting, seek it to its proper place. Returns false
// if the sound would have ended.
//
// Otherwise, record its starting time, and return true.
//
//==========================================================================

bool FMODSoundRenderer::HandleChannelDelay(FMOD::Channel *chan, FISoundChannel *reuse_chan, int flags, float freq) const
{
	if (reuse_chan != NULL)
	{ // Sound is being restarted, so seek it to the position
	  // it would be in now if it had never been evicted.
		QWORD_UNION nowtime;
		unsigned long long int delay;
		chan->getDelay(&delay,NULL,NULL);
		nowtime.Lo = delay & 0xFFFFFFFF;
		nowtime.Hi = delay >> 32;

		// If abstime is set, the sound is being restored, and
		// the channel's start time is actually its seek position.
		if (flags & SNDF_ABSTIME)
		{
			unsigned int seekpos = reuse_chan->StartTime.Lo;
			if (seekpos > 0)
			{
				chan->setPosition(seekpos, FMOD_TIMEUNIT_PCM);
			}
			reuse_chan->StartTime.AsOne = QWORD(nowtime.AsOne - seekpos * OutputRate / freq);
		}
		else if (reuse_chan->StartTime.AsOne != 0)
		{
			QWORD difftime = nowtime.AsOne - reuse_chan->StartTime.AsOne;
			if (difftime > 0)
			{
				// Clamp the position of looping sounds to be within the sound.
				// If we try to start it several minutes past its normal end,
				// FMOD doesn't like that.
				// FIXME: Clamp this right for loops that don't cover the whole sound.
				if (flags & SNDF_LOOP)
				{
					FMOD::Sound *sound;
					if (FMOD_OK == chan->getCurrentSound(&sound))
					{
						unsigned int len;
						if (FMOD_OK == sound->getLength(&len, FMOD_TIMEUNIT_MS) && len != 0)
						{
							difftime %= len;
						}
					}
				}
				return chan->setPosition((unsigned int)(difftime / OutputRate), FMOD_TIMEUNIT_MS) == FMOD_OK;
			}
		}
	}
	else
	{
//		chan->setDelay(FMOD_DELAYTYPE_DSPCLOCK_START, DSPClock.Hi, DSPClock.Lo);
	}
	return true;
}

//==========================================================================
//
// FMODSoundRenderer :: SetChanHeadSettings
//
// If this sound is played at the same coordinates as the listener, make
// it head relative. Also, area sounds should use no 3D panning if close
// enough to the listener.
//
//==========================================================================

FMOD_MODE FMODSoundRenderer::SetChanHeadSettings(SoundListener *listener, FMOD::Channel *chan,
												 const FVector3 &pos, bool areasound, 
												 FMOD_MODE oldmode) const
{
	if (!listener->valid)
	{
		return oldmode;
	}
	FVector3 cpos, mpos;

	cpos = listener->position;

	if (areasound)
	{
		float level, old_level;

		// How far are we from the perceived sound origin? Within a certain
		// short distance, we interpolate between 2D panning and full 3D panning.
		const double interp_range = 32.0;
		double dist_sqr = (cpos - pos).LengthSquared();

		if (dist_sqr == 0)
		{
			level = 0;
		}
		else if (dist_sqr <= interp_range * interp_range)
		{ // Within interp_range: Interpolate between none and full 3D panning.
			level = float(1 - (interp_range - sqrt(dist_sqr)) / interp_range);
		}
		else
		{ // Beyond interp_range: Normal 3D panning.
			level = 1;
		}
		if (chan->get3DLevel(&old_level) == FMOD_OK && old_level != level)
		{ // Only set it if it's different.
			chan->set3DLevel(level);
			if (level < 1)
			{ // Let the noise come from all speakers, not just the front ones.
			  // A centered 3D sound does not play at full volume, so neither should the 2D-panned one.
			  // This is sqrt(0.5), which is the result for a centered equal power panning.
				chan->setMixLevelsOutput(0.70711f,0.70711f,0.70711f,0.70711f,0.70711f,0.70711f,0.70711f,0.70711f);
			}
		}
		return oldmode;
	}
	else if ((cpos - pos).LengthSquared() < (0.0004 * 0.0004))
	{ // Head relative
		return (oldmode & ~FMOD_3D) | FMOD_2D;
	}
	// World relative
	return (oldmode & ~FMOD_2D) | FMOD_3D;
}

//==========================================================================
//
// FMODSoundRenderer :: CommonChannelSetup
//
// Assign an end callback to the channel and allocates a game channel for
// it.
//
//==========================================================================

FISoundChannel *FMODSoundRenderer::CommonChannelSetup(FMOD::Channel *chan, FISoundChannel *reuse_chan) const
{
	FISoundChannel *schan;
	
	if (reuse_chan != NULL)
	{
		schan = reuse_chan;
		schan->SysChannel = chan;
	}
	else
	{
		schan = S_GetChannel(chan);
		unsigned long long int time;
		chan->getDelay(&time,NULL,NULL);
		schan->StartTime.Lo = time & 0xFFFFFFFF;
		schan->StartTime.Hi = time >> 32;
	}
	chan->setUserData(schan);
	chan->setCallback(ChannelCallback);
	GRolloff = NULL;
	return schan;
}

//==========================================================================
//
// FMODSoundRenderer :: StopChannel
//
//==========================================================================

void FMODSoundRenderer::StopChannel(FISoundChannel *chan)
{
	if (chan != NULL && chan->SysChannel != NULL)
	{
		if (((FMOD::Channel *)chan->SysChannel)->stop() == FMOD_ERR_INVALID_HANDLE)
		{ // The channel handle was invalid; pretend it ended.
			S_ChannelEnded(chan);
		}
	}
}

//==========================================================================
//
// FMODSoundRenderer :: ChannelVolume
//
//==========================================================================

void FMODSoundRenderer::ChannelVolume(FISoundChannel *chan, float volume)
{
	if (chan != NULL && chan->SysChannel != NULL)
	{
		((FMOD::Channel *)chan->SysChannel)->setVolume(volume);
	}
}

//==========================================================================
//
// FMODSoundRenderer :: GetPosition
//
// Returns position of sound on this channel, in samples.
//
//==========================================================================

unsigned int FMODSoundRenderer::GetPosition(FISoundChannel *chan)
{
	unsigned int pos;

	if (chan == NULL || chan->SysChannel == NULL)
	{
		return 0;
	}
	((FMOD::Channel *)chan->SysChannel)->getPosition(&pos, FMOD_TIMEUNIT_PCM);
	return pos;
}

//==========================================================================
//
// FMODSoundRenderer :: GetAudibility
//
// Returns the audible volume of the channel, after rollof and any other
// factors are applied.
//
//==========================================================================

float FMODSoundRenderer::GetAudibility(FISoundChannel *chan)
{
	float aud;

	if (chan == NULL || chan->SysChannel == NULL)
	{
		return 0;
	}
	((FMOD::Channel *)chan->SysChannel)->getAudibility(&aud);
	return aud;
}

//==========================================================================
//
// FMODSoundRenderer :: SetSfxPaused
//
//==========================================================================

void FMODSoundRenderer::SetSfxPaused(bool paused, int slot)
{
	int oldslots = SFXPaused;

	if (paused)
	{
		SFXPaused |= 1 << slot;
	}
	else
	{
		SFXPaused &= ~(1 << slot);
	}
	//Printf("%d\n", SFXPaused);
	if (oldslots != 0 && SFXPaused == 0)
	{
		PausableSfx->setPaused(false);
	}
	else if (oldslots == 0 && SFXPaused != 0)
	{
		PausableSfx->setPaused(true);
	}
}

//==========================================================================
//
// FMODSoundRenderer :: SetInactive
//
// This is similar to SetSfxPaused but will *pause* everything, including
// the global reverb effect. This is meant to be used only when the
// game is deactivated, not for general sound pausing.
//
//==========================================================================

void FMODSoundRenderer::SetInactive(SoundRenderer::EInactiveState inactive)
{
	float mix;
	bool active;

	if (inactive == INACTIVE_Active)
	{
		mix = 1;
		active = true;
	}
	else if (inactive == INACTIVE_Complete)
	{
		mix = 1;
		active = false;
	}
	else // inactive == INACTIVE_Mute
	{
		mix = 0;
		active = true;
	}
	if (ChannelGroupTargetUnitOutput != NULL)
	{
		ChannelGroupTargetUnitOutput->setMix(mix);
	}
	if (ChannelGroupTargetUnit != NULL)
	{
		ChannelGroupTargetUnit->setActive(active);
	}
}

//==========================================================================
//
// FMODSoundRenderer :: UpdateSoundParams3D
//
//==========================================================================

void FMODSoundRenderer::UpdateSoundParams3D(SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel)
{
	if (chan == NULL || chan->SysChannel == NULL)
		return;

	FMOD::Channel *fchan = (FMOD::Channel *)chan->SysChannel;
	FMOD_MODE oldmode, mode;

	if (fchan->getMode(&oldmode) != FMOD_OK)
	{
		oldmode = FMOD_3D | FMOD_SOFTWARE;
	}
	mode = SetChanHeadSettings(listener, fchan, pos, areasound, oldmode);
	if (mode != oldmode)
	{ // Only set the mode if it changed.
		fchan->setMode(mode);
	}
	fchan->set3DAttributes((FMOD_VECTOR *)&pos[0], (FMOD_VECTOR *)&vel[0]);
}

//==========================================================================
//
// FMODSoundRenderer :: UpdateListener
//
//==========================================================================

void FMODSoundRenderer::UpdateListener(SoundListener *listener)
{
	FMOD_VECTOR pos, vel;
	FMOD_VECTOR forward;
	FMOD_VECTOR up;

	if (!listener->valid)
	{
		return;
	}

	// Set velocity to 0 to prevent crazy doppler shifts just from running.

	vel.x = listener->velocity.X;
	vel.y = listener->velocity.Y;
	vel.z = listener->velocity.Z;
	pos.x = listener->position.X;
	pos.y = listener->position.Y;
	pos.z = listener->position.Z;

	float angle = listener->angle;
	forward.x = cosf(angle);
	forward.y = 0;
	forward.z = sinf(angle);

	up.x = 0;
	up.y = 1;
	up.z = 0;

	Sys->set3DListenerAttributes(0, &pos, &vel, &forward, &up);

	bool underwater = false;
	const ReverbContainer *env;

	underwater = (listener->underwater && snd_waterlp);
	if (ForcedEnvironment)
	{
		env = ForcedEnvironment;
	}
	else
	{
		env = listener->Environment;
		if (env == NULL)
		{
			env = DefaultEnvironments[0];
		}
	}
	if (env != PrevEnvironment || env->Modified)
	{
		DPrintf ("Reverb Environment %s\n", env->Name);
		const_cast<ReverbContainer*>(env)->Modified = false;
		SetSystemReverbProperties(&env->Properties);
		PrevEnvironment = env;

		if (!SfxReverbHooked)
		{
			SfxReverbHooked = ReconnectSFXReverbUnit();
		}
	}

	if (underwater || env->SoftwareWater)
	{
		//PausableSfx->setPitch(0.64171f);		// This appears to be what Duke 3D uses
		PausableSfx->setPitch(0.7937005f);		// Approx. 4 semitones lower; what Nash suggested
		if (WaterLP != NULL)
		{
			if (LastWaterLP != snd_waterlp)
			{
				LastWaterLP = snd_waterlp;
				WaterLP->setParameterFloat(FMOD_DSP_LOWPASS_CUTOFF, snd_waterlp);
			}
			WaterLP->setActive(true);
			if (WaterReverb != NULL && snd_waterreverb)
			{
				WaterReverb->setActive(true);
				WaterReverb->setBypass(false);
				SfxConnection->setMix(0);
			}
			else
			{
				// Let some of the original mix through so that high frequencies are
				// not completely lost. The reverb unit has its own connection and
				// preserves dry sounds itself if used.
				SfxConnection->setMix(0.1f);
				if (WaterReverb != NULL)
				{
					WaterReverb->setActive(true);
					WaterReverb->setBypass(true);
				}
			}
		}
	}
	else
	{
		PausableSfx->setPitch(1);
		if (WaterLP != NULL)
		{
			SfxConnection->setMix(1);
			WaterLP->setActive(false);
			if (WaterReverb != NULL)
			{
				WaterReverb->setActive(false);
			}
		}
	}
}

//==========================================================================
//
// FMODSoundRenderer :: ReconnectSFXReverbUnit
//
// Locates the DSP unit responsible for software 3D reverb. There is only
// one, and it by default is connected directly to the ChannelGroup Target
// Unit. Older versions of FMOD created this at startup; newer versions
// delay creating it until the first call to setReverbProperties, at which
// point it persists until the system is closed.
//
// Upon locating the proper DSP unit, reconnects it to serve as an input to
// our water DSP chain after the Pausable SFX ChannelGroup.
//
//==========================================================================

bool FMODSoundRenderer::ReconnectSFXReverbUnit()
{
	FMOD::DSP *unit;
	FMOD_DSP_TYPE type;
	int numinputs, i;

	if (ChannelGroupTargetUnit == NULL || SfxReverbPlaceholder == NULL)
	{
		return false;
	}
	// Look for SFX Reverb unit
	if (FMOD_OK != ChannelGroupTargetUnit->getNumInputs(&numinputs))
	{
		return false;
	}
	for (i = numinputs - 1; i >= 0; --i)
	{
		if (FMOD_OK == ChannelGroupTargetUnit->getInput(i, &unit, NULL) &&
			FMOD_OK == unit->getType(&type))
		{
			if (type == FMOD_DSP_TYPE_SFXREVERB)
			{
				break;
			}
		}
	}
	if (i < 0)
	{
		return false;
	}

	// Found it! Now move it in the DSP graph to be done before the water
	// effect.
	if (FMOD_OK != ChannelGroupTargetUnit->disconnectFrom(unit))
	{
		return false;
	}
	if (FMOD_OK != SfxReverbPlaceholder->addInput(unit, NULL))
	{
		return false;
	}
	return true;
}

//==========================================================================
//
// FMODSoundRenderer :: Sync
//
// Used by the save/load code to restart sounds at the same position they
// were in at the time of saving. Must not be nested.
//
//==========================================================================

void FMODSoundRenderer::Sync(bool sync)
{
	DSPLocked = sync;
	if (sync)
	{
		Sys->lockDSP();
		unsigned long long int clock;
		SfxGroup->getDSPClock(&clock,NULL);
		DSPClock.Lo = clock & 0xFFFFFFFF;
		DSPClock.Hi = clock >> 32;
	}
	else
	{
		Sys->unlockDSP();
	}
}

//==========================================================================
//
// FMODSoundRenderer :: UpdateSounds
//
//==========================================================================

void FMODSoundRenderer::UpdateSounds()
{
	// Any sounds played between now and the next call to this function
	// will start exactly one tic from now.
	unsigned long long int clock;
	SfxGroup->getDSPClock(&clock,NULL);
	DSPClock.Lo = clock & 0xFFFFFFFF;
	DSPClock.Hi = clock >> 32;
	DSPClock.AsOne += OutputRate / TICRATE;
	Sys->update();
}

//==========================================================================
//
// FMODSoundRenderer :: LoadSoundRaw
//
//==========================================================================

SoundHandle FMODSoundRenderer::LoadSoundRaw(BYTE* sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
	FMOD_CREATESOUNDEXINFO exinfo;
	SoundHandle retval = { NULL };
	int numsamples;

	if (length <= 0)
	{
		return retval;
	}

	InitCreateSoundExInfo(&exinfo);
	exinfo.length = length;
	exinfo.numchannels = channels;
	exinfo.defaultfrequency = frequency;
	switch (bits)
	{
	case -8:
		// Need to convert sample data from signed to unsigned.
		for (int i = 0; i < length; i++)
		{
			sfxdata[i] ^= 0x80;
		}

	case 8:
		exinfo.format = FMOD_SOUND_FORMAT_PCM8;
		numsamples = length;
		break;

	case 16:
		exinfo.format = FMOD_SOUND_FORMAT_PCM16;
		numsamples = length >> 1;
		break;

	case 32:
		exinfo.format = FMOD_SOUND_FORMAT_PCM32;
		numsamples = length >> 2;
		break;

	default:
		return retval;
	}

	const FMOD_MODE samplemode = FMOD_3D | FMOD_OPENMEMORY | FMOD_SOFTWARE | FMOD_OPENRAW;
	FMOD::Sound *sample;
	FMOD_RESULT result;

	result = Sys->createSound((char *)sfxdata, samplemode, &exinfo, &sample);
	if (result != FMOD_OK)
	{
		DPrintf("Failed to allocate sample: Error %d\n", result);
		return retval;
	}

	if (loopstart >= 0)
	{
		if (loopend == -1)
			loopend = numsamples - 1;
		sample->setLoopPoints(loopstart, FMOD_TIMEUNIT_PCM, loopend, FMOD_TIMEUNIT_PCM);
	}

	retval.data = sample;
	return retval;
}

//==========================================================================
//
// FMODSoundRenderer :: LoadSound
//
//==========================================================================

SoundHandle FMODSoundRenderer::LoadSound(BYTE* sfxdata, int length)
{
	FMOD_CREATESOUNDEXINFO exinfo;
	SoundHandle retval = { NULL };

	if (length == 0) return retval;

	InitCreateSoundExInfo(&exinfo);
	exinfo.length = length;

	const FMOD_MODE samplemode = FMOD_3D | FMOD_OPENMEMORY | FMOD_SOFTWARE;
	FMOD::Sound *sample;
	FMOD_RESULT result;

	result = Sys->createSound((char *)sfxdata, samplemode, &exinfo, &sample);
	if (result != FMOD_OK)
	{
		DPrintf("Failed to allocate sample: Error %d\n", result);
		return retval;;
	}
	SetCustomLoopPts(sample);
	retval.data = sample;
	return retval;
}

//==========================================================================
//
// FMODSoundRenderer :: UnloadSound
//
//==========================================================================

void FMODSoundRenderer::UnloadSound(SoundHandle sfx)
{
	if (sfx.data != NULL)
	{
		((FMOD::Sound *)sfx.data)->release();
	}
}

//==========================================================================
//
// FMODSoundRenderer :: GetMSLength
//
//==========================================================================

unsigned int FMODSoundRenderer::GetMSLength(SoundHandle sfx)
{
	if (sfx.data != NULL)
	{
		unsigned int length;

		if (((FMOD::Sound *)sfx.data)->getLength(&length, FMOD_TIMEUNIT_MS) == FMOD_OK)
		{
			return length;
		}
	}
	return 0;	// Don't know.
}


//==========================================================================
//
// FMODSoundRenderer :: GetMSLength
//
//==========================================================================

unsigned int FMODSoundRenderer::GetSampleLength(SoundHandle sfx)
{
	if (sfx.data != NULL)
	{
		unsigned int length;

		if (((FMOD::Sound *)sfx.data)->getLength(&length, FMOD_TIMEUNIT_PCM) == FMOD_OK)
		{
			return length;
		}
	}
	return 0;	// Don't know.
}


//==========================================================================
//
// FMODSoundRenderer :: ChannelCallback								static
//
// Handles when a channel finishes playing. This is only called when
// System::update is called and is therefore asynchronous with the actual
// end of the channel.
//
//==========================================================================

FMOD_RESULT FMODSoundRenderer::ChannelCallback
	(FMOD_CHANNELCONTROL *channel, FMOD_CHANNELCONTROL_TYPE controltype, FMOD_CHANNELCONTROL_CALLBACK_TYPE type, void *data1, void *data2)
{
	FMOD::ChannelControl *chan = (FMOD::ChannelControl *)channel;
	FISoundChannel *schan;

	if (chan->getUserData((void **)&schan) == FMOD_OK && schan != NULL)
	{
		if (type == FMOD_CHANNELCONTROL_CALLBACK_END)
		{
			S_ChannelEnded(schan);
		}
		else if (type == FMOD_CHANNELCONTROL_CALLBACK_VIRTUALVOICE)
		{
			S_ChannelVirtualChanged(schan, data1 != 0);
		}
	}
	return FMOD_OK;
}


//==========================================================================
//
// FMODSoundRenderer :: RolloffCallback								static
//
// Calculates a volume for the sound based on distance.
//
//==========================================================================

float FMODSoundRenderer::RolloffCallback(FMOD_CHANNELCONTROL *channel, float distance)
{
	FMOD::ChannelControl *chan = (FMOD::ChannelControl *)channel;
	FISoundChannel *schan;

	if (GRolloff != NULL)
	{
		return S_GetRolloff(GRolloff, distance * GDistScale, true);
	}
	else if (chan->getUserData((void **)&schan) == FMOD_OK && schan != NULL)
	{
		return S_GetRolloff(&schan->Rolloff, distance * schan->DistanceScale, true);
	}
	else
	{
		return 0;
	}
}

//==========================================================================
//
// FMODSoundRenderer :: DrawWaveDebug
//
// Bit 0: ( 1) Show oscilloscope for sfx.
// Bit 1: ( 2) Show spectrum for sfx.
// Bit 2: ( 4) Show oscilloscope for music.
// Bit 3: ( 8) Show spectrum for music.
// Bit 4: (16) Show oscilloscope for all sounds.
// Bit 5: (32) Show spectrum for all sounds.
//
//==========================================================================

void FMODSoundRenderer::DrawWaveDebug(int mode)
{
	const int window_height = 100;
	int window_size;
	int numoutchans;
	int y, yy;
	const spk *labels;
	int labelcount;

	if (FMOD_OK != Sys->getSoftwareFormat(NULL, NULL, &numoutchans))
	{
		return;
	}

	// Decide on which set of labels to use.
	labels = (numoutchans == 4) ? SpeakerNames4 : SpeakerNamesMore;
	labelcount = MIN<int>(numoutchans, countof(SpeakerNamesMore));

	// Scale all the channel windows so one group fits completely on one row, with
	// 16 pixels of padding between each window.
	window_size = (screen->GetWidth() - 16) / numoutchans - 16;

	float *wavearray = (float*)alloca(MAX(SPECTRUM_SIZE,window_size)*sizeof(float));
	y = 16;

	yy = DrawChannelGroupOutput(SfxGroup, wavearray, window_size, window_height, y, mode);
	if (y != yy)
	{
		DrawSpeakerLabels(labels, yy-14, window_size, labelcount);
	}
	y = DrawChannelGroupOutput(MusicGroup, wavearray, window_size, window_height, yy, mode >> 2);
	if (y != yy)
	{
		DrawSpeakerLabels(labels, y-14, window_size, labelcount);
	}
	yy = DrawSystemOutput(wavearray, window_size, window_height, y, mode >> 4);
	if (y != yy)
	{
		DrawSpeakerLabels(labels, yy-14, window_size, labelcount);
	}
}

//==========================================================================
//
// FMODSoundRenderer :: DrawSpeakerLabels
//
//==========================================================================

void FMODSoundRenderer::DrawSpeakerLabels(const spk *labels, int y, int width, int count)
{
	if (labels == NULL)
	{
		return;
	}
	for (int i = 0, x = 16; i < count; ++i)
	{
		screen->DrawText(SmallFont, CR_LIGHTBLUE, x, y, labels[i], TAG_DONE);
		x += width + 16;
	}
}

//==========================================================================
//
// FMODSoundRenderer :: DrawChannelGroupOutput
//
// Draws an oscilloscope and/or a spectrum for a channel group.
//
//==========================================================================

int FMODSoundRenderer::DrawChannelGroupOutput(FMOD::ChannelGroup *group, float *wavearray, int width, int height, int y, int mode)
{
	int y1, y2;

	switch (mode & 0x03)
	{
	case 0x01:		// Oscilloscope only
		return DrawChannelGroupWaveData(group, wavearray, width, height, y, false);

	case 0x02:		// Spectrum only
		return DrawChannelGroupSpectrum(group, wavearray, width, height, y, false);

	case 0x03:		// Oscilloscope + Spectrum
		width = (width + 16) / 2 - 16;
		y1 = DrawChannelGroupSpectrum(group, wavearray, width, height, y, true);
		y2 = DrawChannelGroupWaveData(group, wavearray, width, height, y, true);
		return MAX(y1, y2);
	}
	return y;
}

//==========================================================================
//
// FMODSoundRenderer :: DrawSystemOutput
//
// Like DrawChannelGroupOutput(), but uses the system object.
//
//==========================================================================

int FMODSoundRenderer::DrawSystemOutput(float *wavearray, int width, int height, int y, int mode)
{
	int y1, y2;

	switch (mode & 0x03)
	{
	case 0x01:		// Oscilloscope only
		return DrawSystemWaveData(wavearray, width, height, y, false);

	case 0x02:		// Spectrum only
		return DrawSystemSpectrum(wavearray, width, height, y, false);

	case 0x03:		// Oscilloscope + Spectrum
		width = (width + 16) / 2 - 16;
		y1 = DrawSystemSpectrum(wavearray, width, height, y, true);
		y2 = DrawSystemWaveData(wavearray, width, height, y, true);
		return MAX(y1, y2);
	}
	return y;
}

//==========================================================================
//
// FMODSoundRenderer :: DrawChannelGroupWaveData
//
// Draws all the output channels for a specified channel group.
// Setting skip to true causes it to skip every other window.
//
//==========================================================================

int FMODSoundRenderer::DrawChannelGroupWaveData(FMOD::ChannelGroup *group, float *wavearray, int width, int height, int y, bool skip)
{
	int drawn = 0;
	int x = 16;

	if (drawn)
	{
		y += height + 16;
	}
	return y;
}

//==========================================================================
//
// FMODSoundRenderer::DrawSystemWaveData
//
// Like DrawChannelGroupWaveData, but it uses the system object to get the
// complete output.
//
//==========================================================================

int FMODSoundRenderer::DrawSystemWaveData(float *wavearray, int width, int height, int y, bool skip)
{
	int drawn = 0;
	int x = 16;

	if (drawn)
	{
		y += height + 16;
	}
	return y;
}

//==========================================================================
//
// FMODSoundRenderer :: DrawWave
//
// Draws an oscilloscope at the specified coordinates on the screen. Each
// entry in the wavearray buffer has its own column. IOW, there are <width>
// entries in wavearray.
//
//==========================================================================

void FMODSoundRenderer::DrawWave(float *wavearray, int x, int y, int width, int height)
{
	float scale = height / 2.f;
	float mid = y + scale;
	int i;

	// Draw a box around the oscilloscope.
	screen->DrawLine(x - 1, y - 1, x + width, y - 1, -1, MAKEARGB(160, 0, 40, 200));
	screen->DrawLine(x + width, y - 1, x + width, y + height, -1, MAKEARGB(160, 0, 40, 200));
	screen->DrawLine(x + width, y + height, x - 1, y + height, -1, MAKEARGB(160, 0, 40, 200));
	screen->DrawLine(x - 1, y + height, x - 1, y - 1, -1, MAKEARGB(160, 0, 40, 200));

	// Draw the actual oscilloscope.
	if (screen->Accel2D)
	{ // Drawing this with lines is super-slow without hardware acceleration, at least with
	  // the debug build.
		float lasty = mid - wavearray[0] * scale;
		float newy;
		for (i = 1; i < width; ++i)
		{
			newy = mid - wavearray[i] * scale;
			screen->DrawLine(x + i - 1, int(lasty), x + i, int(newy), -1, MAKEARGB(255,255,248,248));
			lasty = newy;
		}
	}
	else
	{
		for (i = 0; i < width; ++i)
		{
			float y = wavearray[i] * scale + mid;
			screen->DrawPixel(x + i, int(y), -1, MAKEARGB(255,255,255,255));
		}
	}
}

//==========================================================================
//
// FMODSoundRenderer :: DrawChannelGroupSpectrum
//
// Draws all the spectrum for a specified channel group.
// Setting skip to true causes it to skip every other window, starting at
// the second one.
//
//==========================================================================

int FMODSoundRenderer::DrawChannelGroupSpectrum(FMOD::ChannelGroup *group, float *spectrumarray, int width, int height, int y, bool skip)
{
	int drawn = 0;
	int x = 16;

	if (skip)
	{
		x += width + 16;
	}

	if (drawn)
	{
		y += height + 16;
	}
	return y;
}

//==========================================================================
//
// FMODSoundRenderer::DrawSystemSpectrum
//
// Like DrawChannelGroupSpectrum, but it uses the system object to get the
// complete output.
//
//==========================================================================

int FMODSoundRenderer::DrawSystemSpectrum(float *spectrumarray, int width, int height, int y, bool skip)
{
	int drawn = 0;
	int x = 16;

	if (skip)
	{
		x += width + 16;
	}

	if (drawn)
	{
		y += height + 16;
	}
	return y;
}

//==========================================================================
//
// FMODSoundRenderer :: DrawSpectrum
//
// Draws a spectrum at the specified coordinates on the screen.
//
//==========================================================================

void FMODSoundRenderer::DrawSpectrum(float *spectrumarray, int x, int y, int width, int height)
{
	float scale = height / 2.f;
	float mid = y + scale;
	float db;
	int top;

	// Draw a border and dark background for the spectrum.
	screen->DrawLine(x - 1, y - 1, x + width, y - 1, -1, MAKEARGB(160, 0, 40, 200));
	screen->DrawLine(x + width, y - 1, x + width, y + height, -1, MAKEARGB(160, 0, 40, 200));
	screen->DrawLine(x + width, y + height, x - 1, y + height, -1, MAKEARGB(160, 0, 40, 200));
	screen->DrawLine(x - 1, y + height, x - 1, y - 1, -1, MAKEARGB(160, 0, 40, 200));
	screen->Dim(MAKERGB(0,0,0), 0.3f, x, y, width, height);

	// Draw the actual spectrum.
	for (int i = 0; i < width; ++i)
	{
		db = spectrumarray[i * (SPECTRUM_SIZE - 2) / width + 1];
		db = MAX(-150.f, 10 * log10f(db) * 2);		// Convert to decibels and clamp
		db = 1.f - (db / -150.f);
		db *= height;
		top = (int)db;
		if (top >= height)
		{
			top = height - 1;
		}
//		screen->Clear(x + i, int(y + height - db), x + i + 1, y + height, -1, MAKEARGB(255, 255, 255, 40));
		screen->Dim(MAKERGB(255,255,40), 0.65f, x + i, y + height - top, 1, top);
	}
}

//==========================================================================
//
// FMODSoundRenderer :: DecodeSample
//
// Uses FMOD to decode a compressed sample to a 16-bit buffer. This is used
// by the DUMB XM reader to handle FMOD's OggMods.
//
//==========================================================================

short *FMODSoundRenderer::DecodeSample(int outlen, const void *coded, int sizebytes, ECodecType type)
{
	FMOD_CREATESOUNDEXINFO exinfo;
	FMOD::Sound *sound;
	FMOD_SOUND_FORMAT format;
	int channels;
	unsigned int len, amt_read;
	FMOD_RESULT result;
	short *outbuf;

	InitCreateSoundExInfo(&exinfo);
	if (type == CODEC_Vorbis)
	{
		exinfo.suggestedsoundtype = FMOD_SOUND_TYPE_OGGVORBIS;
	}
	exinfo.length = sizebytes;
	result = Sys->createSound((const char *)coded,
		FMOD_2D | FMOD_SOFTWARE | FMOD_CREATESTREAM |
		FMOD_OPENMEMORY_POINT | FMOD_OPENONLY | FMOD_LOWMEM,
		&exinfo, &sound);
	if (result != FMOD_OK)
	{
		return NULL;
	}
	result = sound->getFormat(NULL, &format, &channels, NULL);
	// TODO: Handle more formats if it proves necessary.
	if (result != FMOD_OK || format != FMOD_SOUND_FORMAT_PCM16 || channels != 1)
	{
		sound->release();
		return NULL;
	}
	len = outlen;
	// Must be malloc'ed for DUMB, which is C.
	outbuf = (short *)malloc(len);
	result = sound->readData(outbuf, len, &amt_read);
	sound->release();
	if (result == FMOD_ERR_FILE_EOF)
	{
		memset((BYTE *)outbuf + amt_read, 0, len - amt_read);
	}
	else if (result != FMOD_OK || amt_read != len)
	{
		free(outbuf);
		return NULL;
	}
	return outbuf;
}

//==========================================================================
//
// FMODSoundRenderer :: InitCreateSoundExInfo
//
// Allow for compiling with 4.26 APIs while still running with older DLLs.
//
//==========================================================================

void FMODSoundRenderer::InitCreateSoundExInfo(FMOD_CREATESOUNDEXINFO *exinfo) const
{
	memset(exinfo, 0, sizeof(*exinfo));
    {
        exinfo->cbsize = sizeof(*exinfo);
    }
}

//==========================================================================
//
// FMODSoundRenderer :: SetSystemReverbProperties
//
// Set the global reverb properties.
//
//==========================================================================

FMOD_RESULT FMODSoundRenderer::SetSystemReverbProperties(const REVERB_PROPERTIES *props)
{
	// The reverb format changed when hardware mixing support was dropped, because
	// all EAX-only properties were removed from the structure.
	FMOD_REVERB_PROPERTIES fr;

	const float LateEarlyRatio = powf(10.f, (props->Reverb - props->Reflections)/2000.f);
	const float EarlyAndLatePower = powf(10.f, props->Reflections/1000.f) + powf(10, props->Reverb/1000.f);
	const float HFGain = powf(10.f, props->RoomHF/2000.f);
	fr.DecayTime = props->DecayTime*1000.f;
	fr.EarlyDelay = props->ReflectionsDelay*1000.f;
	fr.LateDelay = props->ReverbDelay*1000.f;
	fr.HFReference = props->HFReference;
	fr.HFDecayRatio = clamp<float>(props->DecayHFRatio*100.f, 0.f, 100.f);
	fr.Diffusion = props->Diffusion;
	fr.Density = props->Density;
	fr.LowShelfFrequency = props->DecayLFRatio;
	fr.LowShelfGain = clamp<float>(props->RoomLF/100.f, -48.f, 12.f);
	fr.HighCut = clamp<float>(props->RoomLF < 0 ? props->HFReference/sqrtf((1.f-HFGain)/HFGain) : 20000.f, 20.f, 20000.f);
	fr.EarlyLateMix = props->Reflections > -10000.f ? LateEarlyRatio/(LateEarlyRatio + 1)*100.f : 100.f;
	fr.WetLevel = clamp<float>(10*log10f(EarlyAndLatePower)+props->Room/100.f, -80.f, 20.f);

	return Sys->setReverbProperties(0, &fr);
}

//==========================================================================
//
// IsFModExPresent
//
// Check if FMod can be used
//
//==========================================================================

bool IsFModExPresent()
{
#ifdef NO_FMOD
	return false;
#elif !defined _MSC_VER
	return true;	// on non-MSVC we cannot delay load the library so it has to be present.
#else
	static bool cached_result;
	static bool done = false;

	if (!done)
	{
		done = true;

		FMOD::System *Sys;
		FMOD_RESULT result;
		__try
		{
			result = FMOD::System_Create(&Sys);
		}
		__except (CheckException(GetExceptionCode()))
		{
			// FMod could not be delay loaded
			return false;
		}
		if (result == FMOD_OK) Sys->release();
		cached_result = true;
	}
	return cached_result;
#endif
}
