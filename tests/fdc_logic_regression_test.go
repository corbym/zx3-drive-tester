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

func TestReadDataAcceptsTcNotWiredCompletion(t *testing.T) {
	src := loadDiskTesterSource(t)

	requiredSnippets := []string{
		"(*out_st0 & 0xC0) == 0x40",
		"(*out_st1 & 0x80) != 0",
		"(*out_st1 & 0x7F) == 0",
		"(*out_st2 == 0)",
	}
	for _, snippet := range requiredSnippets {
		if !strings.Contains(src, snippet) {
			t.Fatalf("missing TC-not-wired success condition snippet: %q", snippet)
		}
	}
}

func TestReadDataUsesTightExecutionPhaseReadPath(t *testing.T) {
	src := loadDiskTesterSource(t)

	if !strings.Contains(src, "static unsigned char fdc_read_data_byte(unsigned char* out)") {
		t.Fatalf("missing dedicated execution-phase read helper")
	}
	if !strings.Contains(src, "if (!fdc_read_data_byte(&data[i])) return 0;") {
		t.Fatalf("cmd_read_data is not using tight execution-phase read helper")
	}

	helperBody := regexp.MustCompile(`(?s)static unsigned char fdc_read_data_byte\(unsigned char\* out\) \{(.*?)\n\}`)
	match := helperBody.FindStringSubmatch(src)
	if len(match) < 2 {
		t.Fatalf("could not parse fdc_read_data_byte body")
	}
	if strings.Contains(match[1], "delay_us_approx") {
		t.Fatalf("fdc_read_data_byte must not delay between execution-phase bytes")
	}
}

func TestReadIdFailureReasonDoesNotContainWriteOrScanOnlyBits(t *testing.T) {
	src := loadDiskTesterSource(t)

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
	src := loadDiskTesterSource(t)

	if strings.Contains(src, "FDC_CMD_BYTE_GAP_US") {
		t.Fatalf("legacy gap constant name FDC_CMD_BYTE_GAP_US should not be present")
	}
	if !strings.Contains(src, "#define FDC_CMD_BYTE_GAP_UNITS") {
		t.Fatalf("expected calibrated gap constant FDC_CMD_BYTE_GAP_UNITS")
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
	src := loadDiskTesterSource(t)
	body := extractRenderFuncBody(t, src, `static void ui_attr_fill\(unsigned char ink`)

	if !strings.Contains(body, "memset") {
		t.Fatalf("ui_attr_fill does not use memset; slow per-cell loop may have been reintroduced")
	}
	if strings.Contains(body, "ui_attr_set_cell") {
		t.Fatalf("ui_attr_fill must not call ui_attr_set_cell; bulk fill must use memset")
	}
}

func TestFillRowUsesMemset(t *testing.T) {
	src := loadDiskTesterSource(t)
	body := extractRenderFuncBody(t, src, `static void ui_screen_fill_row\(unsigned char row`)

	if !strings.Contains(body, "memset") {
		t.Fatalf("ui_screen_fill_row does not use memset; slow per-cell loop may have been reintroduced")
	}
	if strings.Contains(body, "ui_screen_put_char") {
		t.Fatalf("ui_screen_fill_row must not call ui_screen_put_char; use scanline memset for pixel fill")
	}
	if strings.Contains(body, "ui_attr_set_cell") {
		t.Fatalf("ui_screen_fill_row must not call ui_attr_set_cell; use memset for row attr fill")
	}
}

func TestWriteRowDoesNotCallFillRow(t *testing.T) {
	src := loadDiskTesterSource(t)
	body := extractRenderFuncBody(t, src, `static void ui_screen_write_row\(unsigned char row`)

	if strings.Contains(body, "ui_screen_fill_row") {
		t.Fatalf("ui_screen_write_row must not call ui_screen_fill_row; that causes double pixel-write for text columns")
	}
	if !strings.Contains(body, "memset") {
		t.Fatalf("ui_screen_write_row must use memset for attr row and tail-clear; fast render path may have been removed")
	}
}
