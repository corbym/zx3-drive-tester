# Menu User Flow Plan (Current Behavior and Validation)

Date: 2026-03-18
Scope: Document current menu behavior, key flow, card visibility timing, and whether the current layout is optimal.
Status: Planning and review only. No behavior changes applied.

## 1. Current User Flow (As Implemented)

### Startup
1. Program initializes font and safe motor state.
2. Main menu is rendered with status line (`STATUS: NO TESTS RUN` or `STATUS: x/5 PASS`).
3. Selected menu row starts at index 0 (`M`).

### Main Menu Navigation
1. Up selection:
- `MENU_KEY_UP` from `W`, `F`, or `CAPS+7`.
2. Down selection:
- `MENU_KEY_DOWN` from `S`, `V`, or `CAPS+6`.
3. Select highlighted item:
- `ENTER`.
4. Direct shortcut select and execute:
- Pressing a menu hotkey (`M,P,K,I,T,D,H,A,R,C,Q`) selects that item and runs it.
5. Quit:
- `Q`.

### Per-Action Flow
1. Single-shot tests (`M,P,K,T`) render a test card and then wait for Enter/Escape before returning.
2. Loop tests (`D,H`) run until `ENTER` or `X` (or break key combo) exits.
3. Interactive seek (`I`) uses `K` up, `J` down, `Q` exit.
4. `A` (Run all core tests) runs all core tests and returns after final report wait.
5. `R` shows report card and waits for Enter/Escape.
6. `C` clears results, shows `RESULTS CLEARED`, then waits for Enter/Escape.

## 2. Timing Model (As Implemented)

### Fixed UI Delays
- `TEST_CARD_STATE_DELAY_MS = 90` (READY -> RUNNING transition for single-shot cards).
- `RUN_ALL_READY_DELAY_MS = 150`.
- `RUN_ALL_RUNNING_DELAY_MS = 120`.
- `RUN_ALL_RESULT_DELAY_MS = 220`.
- Loop pacing:
- Read loop: 4 x 6 ms (24 ms total pacing slices).
- RPM loop: 180 ms between successful samples, 450 ms for fail displays.

### Visibility Behavior
1. Manual single-shot test run:
- READY card shown briefly (90 ms), then RUNNING, then PASS/FAIL card persists until user confirms.
2. Run-all mode:
- Individual test cards are intentionally suppressed while status is `RUNNING`.
- User sees report card transitions with short timed pauses.

## 3. Layout and Keying Assessment

### Is Current Menu Layout Good?
Verdict: Mostly yes, with one keying clarity gap.

Why it is good now:
1. Menu item order follows practical diagnostic progression from simple status checks to loop diagnostics.
2. Arrow-style movement plus direct hotkeys supports both novice and expert usage.
3. Highlighted row and Enter-to-select behavior are consistent.
4. Report card and clear-results are logically grouped near end-of-list.

Gap to address in documentation/UI text:
1. On-screen hint shows `^: UP  v: DOWN`, but physical equivalents are `W/F` up and `S/V` down (plus CAPS+6/7).
2. This is discoverable only from source or habit, not from explicit menu text.

## 4. Timing Assessment

### Are cards visible long enough?
Verdict: Functional and test-stable, but short for human readability in run-all transitions.

Details:
1. Manual mode is acceptable because final PASS/FAIL cards block on Enter/Escape.
2. READY/RUNNING pre-state flashes are intentionally brief (90 ms), which is fine for progress indication.
3. In run-all mode, 120-220 ms transitions are optimized for CI speed and OCR gating, not for human reading comfort.

Recommendation (plan only, not applied):
1. Keep manual-mode behavior unchanged.
2. Add a configurable profile for run-all pacing:
- `FAST` (current values, default for CI/headless).
- `HUMAN` (for example: 250/250/500 ms).
3. If no profile system is desired, increase only `RUN_ALL_RESULT_DELAY_MS` to improve readability with minimal run-time cost.

## 5. Key Consistency Assessment

Verdict: Mostly consistent with minor wording mismatch.

Consistent behaviors:
1. Enter confirms/returns in menu and post-test waits.
2. Loop tests consistently advertise and use `ENTER/X EXIT`.
3. Interactive seek consistently uses `K/J` for movement and `Q` to exit.

Inconsistencies to document/fix later:
1. Single-shot cards display `KEYS  : ENTER MENU`, but implementation accepts Enter or Escape.
2. Menu navigation helper text does not explicitly list `W/S` fallback keys.

## 6. Proposed Menu/User-Flow Plan (No Code Change Yet)

Phase 1: Documentation-only updates
1. Document key aliases in README or in-menu helper line.
2. Document that Enter and Escape both return from post-test waits.

Phase 2: Optional UX polish
1. Add timing profile switch (`FAST` vs `HUMAN`).
2. Standardize control hint strings to match actual accepted keys.

Phase 3: Optional stricter consistency
1. Normalize all card control lines to a shared wording template.
2. Add a short human-visible dwell before any auto-return in non-interactive runs (if desired).

## 7. Confirmation Summary

1. Current structure is fundamentally sound and suitable for diagnostics.
2. Core keys are consistent in behavior, but not fully explicit in labels.
3. Timings are good for automated runs and acceptable manually, but run-all readability can be improved.
4. No changes applied in this plan document.
