# Roadmap

**How it works (D5):** `FPEB.exe` is a Windows program that runs the real EarthBound inside the Snes9x libretro core. All game logic (story, text, battles, items, saves) is the original game. Every frame, the program reads the game's memory (where Ness is, which way he faces, nearby NPCs, whether a battle or menu is open) and draws a 3D first-person scene instead of the top-down one. Text boxes, menus, and battle enemies come straight from the emulator's picture, layered over the 3D scene.

**Current phase: 1** (Phase 0 passed the user's playtest on 2026-10-10)

🟡 = checkpoint where the user plays a build or makes a creative call before work continues. Claude writes all code (D3). Effort is given in working sessions.

Major creative choices ship as **switchable options inside the build** (D20). The user flips between them while playing, picks one, and only then does work move on.

## Phase 0 — Foundation (1–2 sessions)
- ✅ RAM symbol table generated from ebsrc (D14): `src/eb_ram.h`.
- ✅ `FPEB.exe`, version 0: loads `snes9x_libretro.dll` from RetroArch plus the ROM, and plays EarthBound normally (video, audio, gamepad and keyboard). Tab/F1 debug panel (on by default, D16) showing live values: game state, Ness's X/Y/direction, battle flag.
- ✅ The ROM check is built into the program, which rejects Mother 2 and other games with a clear message.
- ✅ GitHub Actions builds the Windows .exe on every push, so the user just downloads a zip.
- ✅ 🟡 **User playtest passed:** game, sound, X/Y, and facing (0 up, 2 right, 4 down, 6 left) all accurate.

## Phase 1 — The world in 3D (2–3 sessions)
- ✅ ROM decoder (`src/eb_world.c`): map, sectors, 20 tilesets, palettes (including story-event variants), and collision, decoded at startup from the user's ROM. HAL decompression is implemented. `ebmapdump` renders any area top-down for checking.
- ✅ 3D builder (`src/world3d.c`): floor from map tiles; walls from solid collision, in two styles: **Facades** (each solid column stands up so the oblique art becomes the front wall) and **Blocks** (diorama). Texture atlas filled on demand, built per sector with fog distance culling.
- ✅ Free-fly viewer (`src/viewer.c`), opened with V. Six switchable options (D20): resolution, wall style, wall height, fog, mood, palette.
- ✅ 🟡 User picks: 640x360, Facades, Light fog (D21). Wall height follows context (D23); mood and palette follow context (D24).
- ✅ "Rounded out" pass (D22). Solid areas are grouped into objects and drawn by kind:
  - **Props** (small trees, bushes, signs, posts) become camera-facing cut-out sprites keyed against the ground colour, and the copy painted on the floor is erased.
  - **Foliage and thin things** (groves, hedges, fences) become one cut-out strip per column.
  - **Buildings** get one uniform height in whole storeys, and their side walls borrow the siding art.
  - **Terrain** (cliffs) is kept as low ledges.
  - Debug flags: `--probe px py` and `--class-map file tx ty tw th`.
- ✅ User: "the entire town is a jumbled mess". Rebuilt around the projection rule (D26) after an 825-shot reference survey (D27).
- 🟡 **User:** fly around the new build and point out the worst remaining spots.
- Known leftovers: tree rows that touch a cliff merge into it and render as ledge; staircase edges on diagonal cliff lips; close-up hedge strips look flat; terrain has no real elevation (plateaus aren't raised).

## Phase 2 — Seeing through Ness's eyes (2–3 sessions)
- The camera sits at Ness's real position. Free movement and turning (D7): mouse or right stick turns, and movement is fed to the game so its own collision, doors, and triggers still fire.
- Text boxes and menus from the game are overlaid on the 3D view.
- Ness's house interior, the stairs, the front door, and the walk up to the hilltop.
- 🟡 **User:** playtest the opening walk. Tune turn speed, field of view, and head-bob.

## Phase 3 — 2D characters in the 3D world (2 sessions)
- NPCs, enemies, presents, and objects as billboards using the original sprites, with the correct facing frame relative to the camera.
- Party followers walk behind Ness, visible when you turn around (D10). King, Pokey, and Picky in the opening.
- "Talk to" and "Check" target whatever is in front of you.
- 🟡 **User:** review how characters look and read in first person.

## Phase 4 — Trippy battles (2 sessions)
- Battle transition swirl into 3D.
- New 3D battle backgrounds behind the original enemy sprites and battle UI (D9). For example, each enemy's original pattern and palette wrapped into tunnels, vortexes, and warped rooms around the camera, still palette-cycling and wobbling.
- 🟡 **User:** pick from several background concepts (Q14) and set the tone for the rest of the game.

## Phase 5 — Vertical slice v0.1 (1–2 sessions)
- The full opening night, from bedroom to meteor to Pokey's house, playable start to finish in first person (D8).
- Settings menu, controls screen, crash and edge-case fixes.
- 🟡 **User:** v0.1 release decision (Q12).

## Phase 6 — Scale out (ongoing, area by area)
- Turn the slice's hand fixes into per-area override files, so each new area is mostly automatic.
- Story order: Onett by day, Twoson, Peaceful Rest Valley, Happy Happy Village, and on. Each area gets a 🟡 playtest.
- Known hard spots: Moonside, Lost Underworld's giant scale, Magicant, Fourside's skyscrapers, the bicycle, the Sky Runner and other vehicle scenes, and cutscenes that move the camera.

## Risks
- **Movement vs. the game's own logic:** EarthBound moves Ness in 8 directions. Free 3D movement either gets translated into the nearest game input, or we write Ness's position into memory directly. Phase 2 decides which, by testing whether doors and triggers still fire.
- **Wall heights:** the top-down maps don't store how tall things are. Generated walls get per-area height overrides.
- **Overlay separation:** we need the game's text boxes and battle enemies without the old top-down background. Plan: decode the relevant layer ourselves from video memory. Fallback: hide layers with a tiny ROM patch.
