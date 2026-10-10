E-mu Darwin emulator
====================

An emulator of the E-mu Darwin 8-track digital audio disk recorder (1997). It runs the machine's
original firmware.

This program is independent. It is not affiliated with, endorsed by or connected to E-mu
Systems, or to Creative Technology Ltd, which owns the E-mu brand. E-mu, Darwin and the E-mu logo
are trademarks of their owners and are used here only to name the hardware being emulated.


What you need
-------------

The firmware is NOT included: use the ROM images of your own unit, as zip files named
emu_darwin_v2_50.zip (version 2.5) or emu_darwin_v2_02.zip (version 2.02). Put them in the
Firmware folder next to the program.

Keep this folder somewhere you can write to (not Program Files or /Applications): the emulator
keeps its settings here.


Starting
--------

Windows   Double-click Emu_Darwin.exe.

Linux     ./Emu_Darwin   (needs SDL2 and ALSA: sudo apt install libsdl2-2.0-0 libasound2)

macOS     Once, in Terminal in this folder:  xattr -dr com.apple.quarantine .
          (the download is not notarized), then double-click Emu_Darwin.app.


First steps
-----------

1. Click the E-MU logo on the panel (top left): it opens the emulator's settings. There you
   choose the audio and MIDI devices, the disks, the firmware version and the computer
   keyboard's keys.

2. Make a disk: in the settings, Disks page, press "New..." for SCSI ID 0 (1000 MB is fine) and
   then OK. The new image goes into the Disks folder.

3. Format it on the Darwin itself (System, then Disk), and create a project.

From there the emulator works like the real Darwin: follow E-mu's original Darwin owner's
manual, which is available online (archive.org has copies). The power switch is the round key
under the logo; clicking the drive bay (right) picks a disk image; a right-click holds a panel
key down, for things that need two keys at once (record: hold REC, press PLAY). Right-click
will hold given key pressed.


Licences
--------

The licences folder holds the notices of what is built in: Musashi (68000 core, MIT), SoftFloat
(John R. Hauser), Nuklear (public domain / MIT), the Droid Sans font (Apache 2.0); SDL2 (zlib
licence) is linked in.
