# Test Card Layout Plan and Consistency Review

Date: 2026-03-18
Scope: Document every current test card layout, evaluate effectiveness, and identify consistency improvements.
Status: Planning and review only. No layout changes applied.

## 1. Card Inventory (Current)

### A. MOTOR AND DRIVE STATUS (`M`)
- Title: `MOTOR AND DRIVE STATUS`
- Controls line: `KEYS  : ENTER MENU` (manual) or `KEYS  : AUTO RETURN MENU` (run-all)
- Data lines:
1. `MOTOR : OFF/ON`
2. `ST3   : ...`
3. `READY : YES/NO`
4. `WPROT : YES/NO`
5. `TRACK0: YES/NO`
6. `FAULT : YES/NO`
- Result line: `RESULT: READY/RUNNING/PASS/FAIL`

### B. DRIVE READ ID PROBE (`P`)
- Title: `DRIVE READ ID PROBE`
- Controls line: single-shot controls
- Data lines:
1. `ST3   : ...`
2. `READY : YES/NO`
3. `WPROT : YES/NO`
4. `TRACK0: YES/NO`
5. `FAULT : YES/NO`
6. `RECAL : ...` then `ID    : ...`
- Result line: `RESULT: READY/RUNNING/PASS/FAIL`

### C. RECALIBRATE AND SEEK TRACK 2 (`K`)
- Title: `RECALIBRATE AND SEEK TRACK 2`
- Controls line: single-shot controls
- Data lines:
1. `READY : ...`
2. `RECAL : ...`
3. `SEEK  : ...`
4. `TRACK : ...` or `INFO  : ...`
- Result line: `RESULT: READY/RUNNING/PASS/FAIL`

### D. INTERACTIVE STEP SEEK (`I`)
- Title: `INTERACTIVE STEP SEEK`
- Controls line: `KEYS  : K UP  J DOWN  Q EXIT`
- Data lines:
1. `TRACK : ...`
2. `LAST  : ...`
3. `PCN   : ...`
- Result line: `RESULT: ACTIVE/STOPPED/FAIL`

### E. READ ID ON TRACK 0 (`T`)
- Title: `READ ID ON TRACK 0`
- Controls line: single-shot controls
- Data lines:
1. `MEDIA : READABLE DISK REQUIRED`
2. `STS   : st0/st1/st2` or drive-ready diagnostic
3. `CHRN  : ...`
4. `REASON:` or `INFO  :`
- Result line: `RESULT: READY/RUNNING/PASS/FAIL`

### F. READ TRACK DATA LOOP (`D`)
- Title: `READ TRACK DATA LOOP`
- Controls line: `KEYS  : J/K TRACK  ENTER/X EXIT`
- Data lines:
1. `TRACK : ...`
2. `PASS  : ...`
3. `FAIL  : ...`
4. `LAST  : ...`
5. `INFO  : ...`
- Result line: `RESULT: ACTIVE/FAIL/STOPPED`

### G. DISK RPM CHECK LOOP (`H`)
- Title: `DISK RPM CHECK LOOP`
- Controls line: `KEYS  : ENTER/X EXIT`
- Data lines:
1. `RPM   : ...` or `---`
2. `PASS  : ...`
3. `FAIL  : ...`
4. `LAST  : ...`
5. `INFO  : ...`
- Result line: `RESULT: PASS/OUT-OF-RANGE/FAIL/STOPPED`

### H. TEST REPORT CARD (`R` and run-all states)
- Title: `TEST REPORT CARD`
- Core lines:
1. `STATUS: ...`
2. `LAST`, `MOTOR`, `DRIVE`, `RECAL`, `SEEK`, `READID`
3. `OVERALL ... x/5 PASS`

## 2. Effectiveness Review by Card

### Strong cards (keep structure)
1. `MOTOR AND DRIVE STATUS`: clear hardware line-level view, good first diagnostic.
2. `RECALIBRATE AND SEEK TRACK 2`: concise command-path validation, clear fail points.
3. `READ TRACK DATA LOOP`: practical stress loop with pass/fail counters and recent context.
4. `DISK RPM CHECK LOOP`: useful timing-health estimator with direct out-of-range reporting.

### Cards that are effective but could be clearer
1. `DRIVE READ ID PROBE`:
- Good breadth, but line6 changes meaning (`RECAL` then `ID`), which can be cognitively jumpy.
2. `READ ID ON TRACK 0`:
- Good diagnostics, but mixes `REASON` and `INFO` labels in same line slot.
3. `INTERACTIVE STEP SEEK`:
- Functional, but lacks an explicit `READY`/`DRIVE` context line unlike other hardware cards.

## 3. Consistency Findings

### What is already consistent
1. All test cards use a title + controls + structured lines + explicit `RESULT:`.
2. Labels mostly align to `TOKEN : VALUE` format.
3. Failure reason text is generally specific and useful.

### What is inconsistent now
1. Controls wording differs for equivalent return behavior.
2. Value semantics in some fixed line slots change over time (`RECAL` line becoming `ID`).
3. Terminology varies (`INFO`, `REASON`, `LAST`) beyond strict semantic boundaries.
4. Result-state vocabulary differs across loops vs single-shot cards (partly justified, partly stylistic).

## 4. Proposed Unified Layout Standard (Plan Only)

For single-shot cards:
1. Line 1: `STEP  : <phase>`
2. Line 2: `READY : <YES/NO/...>`
3. Line 3: `STS   : <controller summary>`
4. Line 4: `DATA  : <main payload>`
5. Line 5: `INFO  : <extra detail>`
6. Line 6: `ERROR : <none/specific reason>`
- Result line remains `RESULT: ...`

For loop cards:
1. Line 1: primary metric (`TRACK` or `RPM`)
2. Line 2: `PASS`
3. Line 3: `FAIL`
4. Line 4: `LAST`
5. Line 5: `INFO`
- Result line remains `RESULT: ...`

For controls lines:
1. Single-shot: `KEYS  : ENTER/ESC MENU`
2. Loop: `KEYS  : ENTER/X EXIT`
3. Interactive seek: `KEYS  : K UP  J DOWN  Q EXIT`

## 5. Should Any Test Card Be Changed?

Recommended yes/no outcome (plan only):
1. Keep current test set and card purposes: YES.
2. Change card wording/line semantics for consistency: YES.
3. Change loop cards substantially: NO.
4. Change report card structure substantially: NO.

Priority changes if implemented later:
1. Standardize control hints to match actual accepted keys.
2. Keep stable meaning per line index within each card.
3. Normalize auxiliary labels (`INFO` vs `REASON`) with a simple rule.

## 6. Validation Checklist for Future Implementation

1. Every card has one unambiguous primary metric.
2. Every card has consistent key hint formatting.
3. Every card uses stable per-line semantics across READY/RUNNING/RESULT states.
4. Every failure path shows a specific reason line.
5. Run-all view transitions remain CI-stable while staying human-readable.

## 7. Confirmation Summary

1. Current cards are technically effective and already good for diagnostics.
2. Main improvement opportunity is consistency, not capability.
3. No layout changes applied in this document; this is a planning artifact only.
