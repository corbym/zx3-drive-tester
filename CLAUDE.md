# Claude Code — Project Notes

See **AGENTS.md** for full project architecture, build system, test structure, port assignments, and optimization rules. That file is the primary reference for all project context.

## Key commands

```bash
./build.sh                            # Build TAP + DSK (always run before tests)
go test ./tests -run TestTapCodeSizeBudget        # Check CODE.bin ≤ 32000 bytes
go test ./tests -run TestMapHeapStackHeadroom     # Check heap/stack headroom
go test ./tests -run TestScreenCaptureStages -v   # Screenshot regression tests
ZX3_UPDATE_APPROVED=1 go test ./tests -run TestScreenCaptureStages  # Refresh baselines
./run_tests.sh                        # Full suite (requires ZEsarUX + Go)
```

Always run `./build.sh` before any test. Budget and screenshot tests evaluate generated output files; stale builds produce misleading failures.

## Memory budget

`out/disk_tester_CODE.bin` must stay ≤ 32000 bytes. Check after every non-trivial change:

```bash
./build.sh && go test ./tests -run TestTapCodeSizeBudget
```

If the budget is tight, consult the **Optimizations** section in AGENTS.md — in particular the note about large struct locals, which are handled efficiently as-is by SCCZ80 at `-SO3`.

## Screenshot baselines

Approved baselines live in `tests/approved/screen-check/`. After any UI layout change (row shifts, content changes), refresh them:

```bash
ZX3_UPDATE_APPROVED=1 go test ./tests -run TestScreenCaptureStages
```

Then rebuild and re-run the full test to confirm the new baselines pass cleanly.

## What AGENTS.md vs CLAUDE.md are for

**AGENTS.md** is read by any AI coding agent (Claude, Cursor, Copilot, etc.). It documents the project for any automated tool: architecture decisions, hardware constraints, build flags, test patterns, and optimization rules. It is the authoritative technical reference.

**CLAUDE.md** is read specifically by Claude Code (this tool). It is for workflow shortcuts, reminders about common commands, and any preferences specific to how Claude Code should interact with this project. It does not duplicate AGENTS.md — it complements it.

In short: AGENTS.md is the *what* and *why* of the project; CLAUDE.md is the *how to work with this project using Claude Code*.
