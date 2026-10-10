# Roadmap (DRAFT — finalized after the Q1–Q10 answers)

The plan is to prove the hardest technical risk (a playable first-person renderer on the SNES) early, ship one small area that is fully playable, then scale to the rest of the game with tools rather than by hand.

🟡 = checkpoint where the user's input or testing is needed before continuing.

## Phase 0 — Foundation (≈1 week)
- Verify the user's ROM by checksum. Set up the toolchain: assembler, CoilSnake, ebsrc for reference, and a patch builder.
- A one-command build: `clean ROM → patched ROM → .bps`.
- If an enhancement chip is chosen: convert the ROM header/mapping and confirm the unmodified game still boots and plays.
- 🟡 User boots the build on their emulator or hardware.

## Phase 1 — Renderer prototype (≈2–3 weeks)
- A standalone first-person renderer on a hand-made test map: walls, floor/ceiling, movement.
- Measure framerate and resolution on the target platform.
- 🟡 User chooses between resolution, framerate, and view-size presets, and approves the look and feel.

## Phase 2 — Hook into EarthBound (≈2–3 weeks)
- Replace the overworld view on one map with the renderer, driven by the game's real player position and facing.
- Generate walls from EarthBound's collision data. Doors and map transitions still work.
- Keep the pause/menu, text boxes, and battle transitions working on top of the 3D view.
- 🟡 User playtests walking around the slice area.

## Phase 3 — 2D in the 3D world (≈2 weeks)
- NPCs, enemies, items, and presents as scaled sprite billboards, using the original sprite art and direction frames.
- "Talk to" and "Check" act on whatever the player faces.
- Followers handled per the Q8 decision.
- 🟡 User reviews how characters read in first person.

## Phase 4 — Vertical slice polish (≈2 weeks)
- Textures for the slice area (per the Q9 decision), HUD/minimap if wanted, battle swirl into the original first-person battles.
- 🟡 Release v0.1 patch; user and friends playtest; collect feedback.

## Phase 5 — Scale out (ongoing, area by area)
- Automated map-to-3D conversion pipeline (collision → walls, tileset → textures, with per-area override files for hand fixes).
- Areas in story order; each area is its own small milestone with a 🟡 playtest.
- Special cases: interiors, stairs and ladders, ropes, Moonside, Lost Underworld scale, Magicant.

## Risks to watch
- CPU budget: rendering, plus game logic, plus NPC updates every frame.
- EarthBound's oblique maps don't encode wall height, so some areas will need hand-made overrides.
- Cutscenes that depend on the top-down camera (scripted movement, screen-position effects).
