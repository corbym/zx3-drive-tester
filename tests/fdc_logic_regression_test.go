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
