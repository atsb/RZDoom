NOTE: RZDoom now lives at sourceforge: https://sourceforge.net/projects/rzdoom/
This repository is thus archived and all future work will take place there.

# So what is RZDoom?
RZDoom is a ZDoom 2.8.1 fork with bug fixes, my additions and my optimisations for many systems.  Consider it the true successor to ZDoom, without all the ancient cruft that nobody has removed in other forks except myself.  It runs better and it is future-proofed.

RZDoom is also the only ZDoom based source port that puts speed runners first.  Always on stats that aren't intruding and pistol starting options, the vsync implementation is designed to cause zero stuttering and mouse lag.  In short, it is the best damn thing if this is what you like to do.

# Why is it needed?
In my own opinion, ZDoom is too big with too many additions for one person to properly maintain. I'm not interested in pleasing everyone, I don't care about supporting impossibly old systems (XP) or having pointless X64 assembly when our processors are plenty fast enough without it.

RZDoom is 'the' premier and most performant ZDoom derivative (I am not counting GZ, that is its own thing) that exists today.  MacOS users are also treated very well and after I finally removed the last vestiges of OpenGL, Mac users will find their performance is drastically increased.  Slaughter maps with Brutal Doom are possible at 60fps.

You'll also find that (unlike most ZDoom developers) I don't bloat the source port more than it already is.  I modernise or remove things that are either deprecated or plain outdated and either replace them with a modern derivative or deprecate that feature entirely.  One of which was OpenAL which was replaced with FMOD Studio (which also handles mod music), so two things removed for one update.

# Differences

You will notice differences to ZDoom 2.8.1, largely about the difference in Sound Systems / Backend Choices.

As RZDoom now uses FMOD Studio (not the legacy FMOD Ex) sound API as standard, support was dropped for a number of sound backends and outputs (because FMOD Studio removed them).  These are the 'HRTF' lowpass output, XMA, Direct Sound etc..

Windows only WASAPI, and ASIO remain (WASAPI being the default on all modern Windows).
Linux only ALSA and Pulse Audio.
macOS only Core Audio.

Wildmidi, Timidity, Fluidsynth, OPL etc..  remain as you remember them.

# Notices
Master branch is inherently unstable, expect bugs if you compile from there. Feel free to report anything you wish, I'll be happy to work with you on them. Of course, I do debug it myself often.

# Where do I find more information
RZDoom has a slowly evolving wiki page on GitHub.  Stop by and see if anything there helps you out.
