# Rules for coding agents

Every agent brief points here. These rules are owner decisions; they override an agent's own habits.

## Waiting costs tokens: never poll

- Builds and emulator runs are local and cost no tokens. **An agent that waits by polling** (`sleep N`, then check, repeated) **pays tokens on every wake.**
- Run a long job one of two ways:
  1. **Foreground, one call:** `tools/port3ds/build.sh` or `run_azahar.py` as a single Bash call with a timeout long enough for the whole job (up to 600000 ms). One call, one result.
  2. **Background, then end the turn:** start the job with `run_in_background`, then stop. The harness wakes the agent when the job finishes. Do not read its output file while waiting.
- Forbidden: `sleep` loops, `while ! test -f ...; do sleep; done`, repeated status checks, re-reading a log to see whether a job has finished.
- A build may wait for a build slot (at most 2 machine-wide). That is normal; it is not stuck.

## Emulator runs

- Targeted and short: usually under 1 minute. Use warps (`map=`, `story=`, `battle=`, `storyjump=`) and `run_azahar.py --frames N` instead of long boots and hand-written `--until` patterns.
- Long runs (`--long N`) only when the question needs them; say why in the report.
- At most 3 Azahar and 2 Dolphin instances machine-wide, through the slot lock. The lock reaps stuck runs automatically. Never kill other processes yourself.
- Each Azahar instance gets its own devupdate port from `run_azahar.py`; do not hard-code 5050.

## Console

- Never push to the console (`devupdate.py push/restart/queue`, FBI install). The owner gates every console push. Put the exact commands in the report instead.

## Repository

- Work in your worktree; start with `git reset --hard port3ds`; commit there; never push.
- One command per Bash call: a hook rewrites chained commands, and the auto-mode check then refuses them. Prefer the Read tool or python3 over `cat`/`grep`/`ls` in the shell.
- No game assets in the CIA or 3dsx (`docs/RELEASE.md`, standing rule). No large static arrays: the image must end below 0x01000000 (the build checks).
- The GameCube matching build stays 100% after any `src/` change (`python3 configure.py --version G8ME01`, then `ninja`).
- Stay inside the files your brief assigns; other agents own the rest.

## Reports

- Short and factual: commits, measured numbers, what was not done. Raw data first; no speculation presented as fact.
