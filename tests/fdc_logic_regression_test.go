package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func loadDiskTesterSource(t *testing.T) string {
	t.Helper()
	path := filepath.Join("..", "disk_tester.c")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("failed to read %s: %v", path, err)
	}
	return string(data)
}

func loadDiskOperationsSource(t *testing.T) string {
	t.Helper()
	path := filepath.Join("..", "disk_operations.c")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("failed to read %s: %v", path, err)
	}
	return string(data)
}

func loadUiSource(t *testing.T) string {
	t.Helper()
	path := filepath.Join("..", "ui.c")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("failed to read %s: %v", path, err)
	}
	return string(data)
}

func TestReadDataAcceptsTcNotWiredCompletion(t *testing.T) {
	src := loadDiskOperationsSource(t)

	requiredSnippets := []string{
		"(out_result->status.st0 & FDC_ST0_INTERRUPT_CODE_MASK) ==",
		"FDC_ST0_INTERRUPT_ABNORMAL_TERMINATION",
		"(out_result->status.st1 & FDC_ST1_END_OF_CYLINDER) != 0",
		"(out_result->status.st1 & FDC_ST1_OTHER_ERROR_MASK) == 0",
		"out_result->status.st2 == 0",
	}
	for _, snippet := range requiredSnippets {
		if !strings.Contains(src, snippet) {
			t.Fatalf("missing TC-not-wired success condition snippet: %q", snippet)
		}
	}
}

func TestReadDataUsesTightExecutionPhaseReadPath(t *testing.T) {
	src := loadDiskOperationsSource(t)

	helperSig := regexp.MustCompile(`static\s+unsigned\s+char\s+fdc_read_data_byte\s*\(\s*unsigned\s+char\s*\*\s*out\s*\)`)
	if !helperSig.MatchString(src) {
		t.Fatalf("missing dedicated execution-phase read helper")
	}
	if !strings.Contains(src, "if (!fdc_read_data_byte(&data[i])) return 0;") {
		t.Fatalf("cmd_read_data is not using tight execution-phase read helper")
	}

	helperBody := regexp.MustCompile(`(?s)static\s+unsigned\s+char\s+fdc_read_data_byte\s*\(\s*unsigned\s+char\s*\*\s*out\s*\)\s*\{(.*?)\n}`)
	match := helperBody.FindStringSubmatch(src)
	if len(match) < 2 {
		t.Fatalf("could not parse fdc_read_data_byte body")
	}
	if strings.Contains(match[1], "delay_us_approx") {
		t.Fatalf("fdc_read_data_byte must not delay between execution-phase bytes")
	}
}

func TestReadIdFailureReasonDoesNotContainWriteOrScanOnlyBits(t *testing.T) {
	src := loadDiskOperationsSource(t)

	forbidden := []string{
		"return \"Write protect\"",
		"return \"Scan not satisfied\"",
		"return \"Scan equal hit\"",
	}
	for _, f := range forbidden {
		if strings.Contains(src, f) {
			t.Fatalf("found stale/non-read failure string in read_id_failure_reason: %q", f)
		}
	}
}

func TestCommandGapConstantUsesCalibratedUnitsNaming(t *testing.T) {
	src := loadDiskOperationsSource(t)

	if strings.Contains(src, "FDC_CMD_BYTE_GAP_US") {
		t.Fatalf("legacy gap constant name FDC_CMD_BYTE_GAP_US should not be present")
	}
	if !strings.Contains(src, "#define FDC_CMD_BYTE_GAP_UNITS") {
		t.Fatalf("expected calibrated gap constant FDC_CMD_BYTE_GAP_UNITS")
	}
}

func TestSeekInteractiveSeparatesSeekCommandAndCompletionFailures(t *testing.T) {
	src := loadDiskTesterSource(t)
	body := extractRenderFuncBody(t, src, `static\s+void\s+test_seek_interactive\(void`)

	if !strings.Contains(body, "if (!cmd_seek(FDC_DRIVE, 0, target))") {
		t.Fatalf("expected separate cmd_seek failure branch in test_seek_interactive")
	}
	if !strings.Contains(body, "set_last_seek_cmd_fail(&interactive_seek_card);") {
		t.Fatalf("expected explicit seek command failure detail in test_seek_interactive")
	}
	if !strings.Contains(body, "if (!wait_seek_complete(FDC_DRIVE, &seek_result))") {
		t.Fatalf("expected separate wait_seek_complete failure branch in test_seek_interactive")
	}
	if !strings.Contains(body, "set_pcn(&interactive_seek_card, seek_result.pcn);") {
		t.Fatalf("expected seek-complete failure to preserve PCN detail in test_seek_interactive")
	}
}

// — Rendering fast-path regression tests —
// These prevent the slow per-cell loop paths from being reintroduced.
// The optimised paths replace O(768) ui_attr_set_cell calls and O(1664)
// ui_screen_put_char calls with a handful of memset calls, which is critical
// for first-render latency on a 3.5 MHz Z80.

func extractRenderFuncBody(t *testing.T, src, sig string) string {
	t.Helper()
	// Use [^)]* so the match stops at the first ')' in the parameter list,
	// then require ') {' to select the definition and not a forward declaration.
	re := regexp.MustCompile(`(?s)` + sig + `[^)]*\) \{(.*?)\n\}`)
	match := re.FindStringSubmatch(src)
	if len(match) < 2 {
		t.Fatalf("could not find or parse body of function matching %q", sig)
	}
	return match[1]
}

func TestAttrFillUsesMemset(t *testing.T) {
	src := loadUiSource(t)
	body := extractRenderFuncBody(t, src, `(?:static\s+)?void ui_attr_fill\(unsigned char ink`)

	if !strings.Contains(body, "memset") {
		t.Fatalf("ui_attr_fill does not use memset; slow per-cell loop may have been reintroduced")
	}
	if strings.Contains(body, "ui_attr_set_cell") {
		t.Fatalf("ui_attr_fill must not call ui_attr_set_cell; bulk fill must use memset")
	}
}

func TestFillRowUsesMemset(t *testing.T) {
	// ui_screen_fill_row was merged into ui_screen_write_row; it must not be
	// reintroduced as a separate function (double pixel-write regression).
	// The memset property is now enforced by TestWriteRowDoesNotCallFillRow.
	src := loadUiSource(t)
	re := regexp.MustCompile(`(?s)(?:static\s+)?void ui_screen_fill_row\(unsigned char row[^)]*\) \{`)
	if re.MatchString(src) {
		t.Fatalf("ui_screen_fill_row must not exist as a separate function; its pixel-fill logic belongs in ui_screen_write_row to avoid double pixel-write")
	}
}

func TestWriteRowDoesNotCallFillRow(t *testing.T) {
	src := loadUiSource(t)
	body := extractRenderFuncBody(t, src, `(?:static\s+)?void ui_screen_write_row\(unsigned char row`)

	if strings.Contains(body, "ui_screen_fill_row") {
		t.Fatalf("ui_screen_write_row must not call ui_screen_fill_row; that causes double pixel-write for text columns")
	}
	if !strings.Contains(body, "memset") {
		t.Fatalf("ui_screen_write_row must use memset for attr row and tail-clear; fast render path may have been removed")
	}
}

func TestRpmRevolutionCounterResetsSeenOther(t *testing.T) {
	src := loadDiskOperationsSource(t)

	if !strings.Contains(src, "result.chrn.r != first_r") {
		t.Fatal("fdc_measure_revolutions_ticks must include R mismatch in non-reference-sector detection")
	}
	if !strings.Contains(src, "step_count >= min_steps_per_rev") {
		t.Fatal("fdc_measure_revolutions_ticks must require a minimum transition count before accepting a revolution")
	}
	if !strings.Contains(src, "seen_other = 0;") {
		t.Fatal("fdc_measure_revolutions_ticks must reset seen_other after each counted revolution to prevent double-counting")
	}
}

func TestRpmMeasurementFunctionUsesMinimalPacingDelay(t *testing.T) {
	src := loadDiskOperationsSource(t)

	// Locate the measurement function body.
	start := strings.Index(src, "fdc_measure_revolutions_ticks(")
	if start < 0 {
		t.Fatal("fdc_measure_revolutions_ticks not found in disk_operations.c")
	}
	// Find the opening brace of the function definition (not the declaration).
	defStart := strings.Index(src[start:], "{\n")
	if defStart < 0 {
		t.Fatal("could not locate fdc_measure_revolutions_ticks function body")
	}
	body := src[start+defStart:]
	// Find matching closing brace (simple: stop at next top-level function).
	nextFunc := regexp.MustCompile(`\nunsigned |\nstatic |\nvoid `)
	loc := nextFunc.FindStringIndex(body[2:])
	if loc != nil {
		body = body[:loc[0]+2]
	}

	if !strings.Contains(body, "delay_ms(1)") {
		t.Fatal("fdc_measure_revolutions_ticks should use a minimal delay_ms(1) pacing gap to avoid emulator/controller overrun artifacts")
	}
	if strings.Contains(body, "delay_ms(2)") || strings.Contains(body, "delay_ms(5)") {
		t.Fatal("fdc_measure_revolutions_ticks must not use coarse delays (>1 ms), which bias RPM downward")
	}
}

func TestRpmCheckerUsesMeasurementFunction(t *testing.T) {
	src := loadDiskTesterSource(t)
	if !strings.Contains(src, "fdc_measure_revolutions_ticks(") {
		t.Fatal("test_rpm_checker must use fdc_measure_revolutions_ticks for revolution timing")
	}
}
