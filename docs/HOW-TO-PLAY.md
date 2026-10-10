First-Person EarthBound — how to play a test build
==================================================

You need:
  * Your own US EarthBound ROM (.sfc or .smc). It is never uploaded anywhere.
  * RetroArch with the Snes9x core installed (you already have this).

First run:
  1. Unzip this folder anywhere, e.g. your Desktop.
  2. Double-click FPEB.exe.
  3. If it says "Emulator core: missing", drag snes9x_libretro.dll onto the window.
     It's in your RetroArch folder, inside "cores".
  4. Drag your EarthBound ROM onto the window. The game starts.
  Your choices are saved in fpeb.ini, so next time it starts straight away.

Controls (same as RetroArch's defaults; any gamepad also works):
  Arrows = D-pad     X = A     Z = B     S = X     A = Y
  Q = L     W = R     Enter = Start     Right Shift = Select

Extra keys:
  Tab (or F1) = show/hide the debug panel in the top-left corner
                (Ness's X/Y position, facing direction, battle flags).
                It's on by default and remembers your choice.
  F5 = save state    F9 = load state

Saves: in-game saves go in a .srm file next to your ROM. To continue a save from
RetroArch, copy your EarthBound .srm from RetroArch's "saves" folder to sit next
to the ROM, with the same name as the ROM.

If something goes wrong, send the fpeb.log file from this folder.
