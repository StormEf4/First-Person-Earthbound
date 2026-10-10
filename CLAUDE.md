# First-Person EarthBound — working rules

Read these before doing anything, in this order:
1. `docs/PROJECT_LOG.md` — every decision made so far, open questions, and session history.
2. `docs/ROADMAP.md` — phases, current phase, and the checkpoints where the user gives input.

Rules:
- Do not re-ask a question already answered in the log. If an answer seems wrong for new facts, propose a change and record it as a new entry; don't silently override.
- When the user answers a question or a decision is made, add it to the Decision Log with the date before continuing.
- At the end of each session, append a short Session History entry: what was done, what's next, anything blocked.
- Never commit ROM files, ROM-derived binaries, or emulator binaries. The user supplies the ROM and the Snes9x libretro core (from their RetroArch install).
