package tests

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var (
	suiteClient  *EmulatorClient
	suiteSkip    bool
	tapPath      string
	dskPath      string
	repoRootPath string
)

func TestMain(m *testing.M) {
	cwd, err := os.Getwd()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to resolve cwd: %v\n", err)
		os.Exit(1)
	}
	repoRoot := filepath.Clean(filepath.Join(cwd, ".."))
	repoRootPath = repoRoot

	tapPath, err = absPathFromRepo(repoRoot, "out/disk_tester.tap")
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to resolve TAP path: %v\n", err)
		os.Exit(1)
	}
	dskPath, err = absPathFromRepo(repoRoot, "out/disk_tester_plus3.dsk")
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to resolve DSK path: %v\n", err)
		os.Exit(1)
	}
	if _, err := os.Stat(tapPath); err != nil {
		fmt.Fprintf(os.Stderr, "expected TAP output missing: %s (build artifacts first via CI build step or ./build.sh)\n", tapPath)
		os.Exit(1)
	}
	if _, err := os.Stat(dskPath); err != nil {
		fmt.Fprintf(os.Stderr, "expected DSK output missing: %s (build artifacts first via CI build step or ./build.sh)\n", dskPath)
		os.Exit(1)
	}

	suiteClient, err = startClient()
	if err != nil {
		if os.Getenv("ZX3_REQUIRE_EMU_SMOKE") == "1" {
			fmt.Fprintf(os.Stderr, "emulator smoke required but unavailable: %v\n", err)
			os.Exit(1)
		}
		fmt.Fprintf(os.Stdout, "note: emulator smoke skipped (%v)\n", err)
		suiteSkip = true
	}

	code := m.Run()
	if suiteClient != nil {
		suiteClient.Stop()
	}
	os.Exit(code)
}

func requireSuiteClient(t *testing.T) *EmulatorClient {
	t.Helper()
	if suiteSkip || suiteClient == nil {
		t.Skip("emulator not available")
	}
	return suiteClient
}

func resetAndLoadTap(t *testing.T, c *EmulatorClient) {
	t.Helper()
	if err := c.CloseAllMenus(); err != nil {
		t.Fatalf("close-all-menus failed: %v", err)
	}
	if err := c.HardReset(); err != nil {
		t.Fatalf("hard-reset-cpu failed: %v", err)
	}
	time.Sleep(200 * time.Millisecond)
	if err := c.Smartload(tapPath); err != nil {
		t.Fatalf("smartload TAP failed: %v", err)
	}
}

func waitForMenu(t *testing.T, c *EmulatorClient, timeout time.Duration) {
	t.Helper()
	if _, err := c.WaitForOCR(timeout, "ZX +3 DISK TESTER", "ENTER: SELECT"); err != nil {
		t.Fatalf("timed out waiting for menu: %v", err)
	}
}

// This polls OCR directly because the assertion needs the label and '~' on the
// same rendered line, which WaitForOCR cannot express.
func waitForMenuSelection(t *testing.T, c *EmulatorClient, label string, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	upperLabel := strings.ToUpper(label)
	lastOCR := ""

	for time.Now().Before(deadline) {
		ocr, err := c.OCR()
		if err == nil {
			lastOCR = ocr
			for _, line := range strings.Split(strings.ToUpper(ocr), "\n") {
				if strings.Contains(line, upperLabel) && strings.Contains(line, "~") {
					return
				}
			}
		}
		time.Sleep(120 * time.Millisecond)
	}

	t.Fatalf("menu selection did not move to expected item %q\nlast OCR:\n%s", label, lastOCR)
}

func TestMenuAppearsAfterTapLoad(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	ocr, err := c.OCR()
	if err != nil {
		t.Fatalf("failed to read OCR on menu: %v", err)
	}
	if !containsAll(ocr, "W/F", "S/V", "Q: QUIT") {
		t.Fatalf("menu helper hints missing expected key aliases\nOCR:\n%s", ocr)
	}
}

func TestMenuSelectionMovesAcrossAllItems(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)

	labels := []string{
		"M MOTOR AND DRIVE STATUS",
		"P DRIVE READ ID PROBE",
		"K RECALIBRATE AND SEEK TRACK 2",
		"I INTERACTIVE STEP SEEK",
		"T READ ID ON TRACK 0",
		"D READ TRACK DATA LOOP",
		"H DISK RPM CHECK LOOP",
		"A RUN ALL CORE TESTS",
		"R SHOW REPORT CARD",
		"C CLEAR STORED RESULTS",
		"Q QUIT",
	}

	waitForMenuSelection(t, c, labels[0], 6*time.Second)

	for i := 1; i < len(labels); i++ {
		time.Sleep(300 * time.Millisecond)
		if err := c.SendKey('S'); err != nil {
			t.Fatalf("failed to send S key for index %d: %v", i, err)
		}
		waitForMenuSelection(t, c, labels[i], 6*time.Second)
	}

	for i := len(labels) - 2; i >= 0; i-- {
		time.Sleep(300 * time.Millisecond)
		if err := c.SendKey('W'); err != nil {
			t.Fatalf("failed to send W key for index %d: %v", i, err)
		}
		waitForMenuSelection(t, c, labels[i], 6*time.Second)
	}
}

func TestReportCardOpens(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	if err := c.SendKey('R'); err != nil {
		t.Fatalf("failed to send R key: %v", err)
	}
	if _, err := c.WaitForOCR(15*time.Second, "TEST REPORT CARD", "OVERALL ["); err != nil {
		t.Fatalf("timed out waiting for report card: %v", err)
	}
}

func TestReportCardRendersPromptly(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	if err := c.SendKey('R'); err != nil {
		t.Fatalf("failed to send R key: %v", err)
	}
	// Tight 3-second window.  The optimised render path (memset-based attr_fill
	// and scanline memset in fill_row) paints the report card in well under a
	// second.  The slow loop path (~768 ui_attr_set_cell + ~1664 per-row calls)
	// would add ~500 ms at Z80 speed and reliably exceed this budget.
	if _, err := c.WaitForOCR(3*time.Second, "TEST REPORT CARD", "OVERALL ["); err != nil {
		t.Fatalf("report card did not appear within 3 s — rendering regression likely (slow attr_fill/fill_row loop reintroduced?): %v", err)
	}
}

func TestReturnToMenuFromReport(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	if err := c.SendKey('R'); err != nil {
		t.Fatalf("failed to open report card: %v", err)
	}
	if _, err := c.WaitForOCR(15*time.Second, "TEST REPORT CARD", "OVERALL ["); err != nil {
		t.Fatalf("report card did not open: %v", err)
	}
	if err := c.SendKey(13); err != nil {
		t.Fatalf("failed to send Enter key: %v", err)
	}
	waitForMenu(t, c, 15*time.Second)
}

func TestReturnToMenuFromReportWithEscape(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	if err := c.SendKey('R'); err != nil {
		t.Fatalf("failed to open report card: %v", err)
	}
	if _, err := c.WaitForOCR(15*time.Second, "TEST REPORT CARD", "OVERALL ["); err != nil {
		t.Fatalf("report card did not open: %v", err)
	}
	if err := c.SendKey('X'); err != nil {
		t.Fatalf("failed to send X exit key: %v", err)
	}
	waitForMenu(t, c, 15*time.Second)
}

func TestRunAllCompletes(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	if err := c.Smartload(dskPath); err != nil {
		t.Fatalf("failed to smartload DSK: %v", err)
	}
	if err := c.HardReset(); err != nil {
		t.Fatalf("failed hard-reset after DSK smartload: %v", err)
	}
	if _, err := c.WaitForOCR(20*time.Second, "ZX +3 DISK TESTER", "ENTER: SELECT"); err != nil {
		t.Fatalf("timed out waiting for menu after DSK load: %v", err)
	}

	deadline := time.Now().Add(180 * time.Second)
	nextTrigger := time.Now()
	var lastOCR string
	for time.Now().Before(deadline) {
		if time.Now().After(nextTrigger) {
			if err := c.SendKey('A'); err != nil {
				t.Fatalf("failed to send A key: %v", err)
			}
			nextTrigger = time.Now().Add(3 * time.Second)
		}

		ocr, err := c.OCR()
		if err == nil {
			lastOCR = ocr
			if containsAll(ocr, "TEST REPORT CARD", "STATUS: COMPLETE") {
				return
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for run-all completion\nlast OCR:\n%s", lastOCR)
}

func TestMotorStatusMenu(t *testing.T) {
	c := requireSuiteClient(t)
	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)
	deadline := time.Now().Add(20 * time.Second)
	nextTrigger := time.Now()
	lastOCR := ""
	for time.Now().Before(deadline) {
		if time.Now().After(nextTrigger) {
			if err := c.SendKey('M'); err != nil {
				t.Fatalf("failed to send M key: %v", err)
			}
			nextTrigger = time.Now().Add(2 * time.Second)
		}
		if ocr, err := c.OCR(); err == nil {
			lastOCR = ocr
			if containsAll(ocr, "MOTOR AND DRIVE STATUS", "ENTER/ESC MENU", "MOTOR :", "RESULT:") {
				return
			}
		}
		if _, err := c.WaitForOCR(1500*time.Millisecond, "MOTOR AND DRIVE STATUS", "RESULT:"); err == nil {
			return
		}
	}
	t.Fatalf("motor status screen did not appear in time\nlast OCR:\n%s", lastOCR)
}

func TestScreenCaptureStages(t *testing.T) {
	c := requireSuiteClient(t)
	screenDir := filepath.Join(repoRootPath, "out", "screen-check")
	approvedDir := filepath.Join(repoRootPath, "tests", "approved", "screen-check")
	updateApproved := os.Getenv("ZX3_UPDATE_APPROVED") == "1"
	if err := os.MkdirAll(screenDir, 0o755); err != nil {
		t.Fatalf("failed to create out/screen-check directory: %v", err)
	}
	if updateApproved {
		if err := os.MkdirAll(approvedDir, 0o755); err != nil {
			t.Fatalf("failed to create approved screenshot directory: %v", err)
		}
	}

	resetAndLoadTap(t, c)
	waitForMenu(t, c, 30*time.Second)

	captureChecked := func(filename string, requireNonBlank bool) {
		t.Helper()
		actual := filepath.Join(screenDir, filename)
		approved := filepath.Join(approvedDir, filename)
		approvedBytes, err := os.ReadFile(approved)
		if err != nil && !updateApproved {
			t.Fatalf("approved screenshot missing or unreadable: %s (set ZX3_UPDATE_APPROVED=1 to refresh baselines)", approved)
		}

		const maxCaptureAttempts = 5
		for attempt := 1; attempt <= maxCaptureAttempts; attempt++ {
			if err := c.SaveScreen(actual); err != nil {
				t.Fatalf("failed to save %s: %v", filename, err)
			}

			actualBytes, readErr := os.ReadFile(actual)
			if readErr != nil {
				t.Fatalf("failed reading actual screenshot %s: %v", actual, readErr)
			}

			if requireNonBlank && screenshotIsBlank(actualBytes) {
				if attempt < maxCaptureAttempts {
					time.Sleep(250 * time.Millisecond)
					continue
				}
				t.Fatalf("captured screenshot is blank after %d attempts: %s", maxCaptureAttempts, filename)
			}

			if updateApproved {
				if err := os.WriteFile(approved, actualBytes, 0o644); err != nil {
					t.Fatalf("failed updating approved screenshot %s: %v", approved, err)
				}
				return
			}

			if screenshotsEqual(actualBytes, approvedBytes) {
				return
			}

			if attempt < maxCaptureAttempts {
				time.Sleep(250 * time.Millisecond)
			}
		}

		t.Fatalf("screenshot mismatch for %s after %d attempts\napproved: %s\nactual: %s", filename, maxCaptureAttempts, approved, actual)
	}

	pressEnterUntilMenu := func(timeout time.Duration) {
		t.Helper()
		deadline := time.Now().Add(timeout)
		lastOCR := ""
		for time.Now().Before(deadline) {
			if ocr, err := c.OCR(); err == nil {
				lastOCR = ocr
				if containsAll(ocr, "ZX +3 DISK TESTER", "ENTER: SELECT") {
					return
				}
			}
			if err := c.SendKey(13); err != nil {
				t.Fatalf("failed to send Enter key: %v", err)
			}
			if _, err := c.WaitForOCR(1500*time.Millisecond, "ZX +3 DISK TESTER", "ENTER: SELECT"); err == nil {
				return
			}
			time.Sleep(250 * time.Millisecond)
		}
		t.Fatalf("timed out waiting for menu after Enter\nlast OCR:\n%s", lastOCR)
	}

	type screenStage struct {
		name        string
		captureFile string
		key         byte
		exitKey     byte
		exitKey2    byte
		waitFor     []string
		waitTimeout time.Duration
		exitWaitFor []string
		exitTimeout time.Duration
		settleDelay time.Duration
		nonBlank    bool
		enterToMenu bool
		loadDSK     bool
	}

	stages := []screenStage{
		{
			name:        "motor status",
			captureFile: "02_motor_status.bmp",
			key:         'M',
			waitFor:     []string{"MOTOR AND DRIVE STATUS", "RESULT:"},
			waitTimeout: 15 * time.Second,
			nonBlank:    true,
		},
		{
			name:        "menu",
			captureFile: "01_menu.bmp",
			waitFor:     []string{"ZX +3 DISK TESTER", "ENTER: SELECT"},
			waitTimeout: 20 * time.Second,
			enterToMenu: true,
			nonBlank:    true,
		},
		{
			name:        "menu after motor",
			captureFile: "03_menu_after_motor.bmp",
			waitFor:     []string{"ZX +3 DISK TESTER", "ENTER: SELECT"},
			waitTimeout: 10 * time.Second,
			nonBlank:    true,
		},
		{
			name:        "report card",
			captureFile: "04_report_card.bmp",
			key:         'R',
			waitFor:     []string{"TEST REPORT CARD", "OVERALL ["},
			waitTimeout: 15 * time.Second,
			nonBlank:    true,
		},
		{
			name:        "menu after report",
			captureFile: "05_menu_after_report.bmp",
			waitFor:     []string{"ZX +3 DISK TESTER", "ENTER: SELECT"},
			waitTimeout: 20 * time.Second,
			enterToMenu: true,
			nonBlank:    true,
		},
		{
			// Load DSK so the loop reads real sector data and the hex
			// preview panel is populated.  The DSK stays loaded for the
			// subsequent run-all stage.
			name:        "read data loop",
			captureFile: "07_read_data_loop_hex_preview.bmp",
			loadDSK:     true,
			key:         'D',
			waitFor:     []string{"READ TRACK DATA LOOP", "DATA PREVIEW"},
			waitTimeout: 30 * time.Second,
			settleDelay: 3 * time.Second,
			exitKey:     'X',
			exitKey2:    13, // dismiss "press any key" after loop stops
			exitWaitFor: []string{"ZX +3 DISK TESTER", "ENTER: SELECT"},
			exitTimeout: 20 * time.Second,
			nonBlank:    true,
		},
		{
			// DSK already loaded from the previous stage.
			name:        "run-all complete",
			captureFile: "06_run_all_complete.bmp",
			key:         'A',
			waitFor:     []string{"TEST REPORT CARD", "STATUS: COMPLETE"},
			waitTimeout: 180 * time.Second,
			nonBlank:    true,
		},
	}

	for _, stage := range stages {
		if stage.loadDSK {
			if err := c.Smartload(dskPath); err != nil {
				t.Fatalf("failed to smartload DSK for %s: %v", stage.name, err)
			}
			if err := c.HardReset(); err != nil {
				t.Fatalf("failed hard-reset after DSK smartload for %s: %v", stage.name, err)
			}
			if _, err := c.WaitForOCR(20*time.Second, "ZX +3 DISK TESTER", "ENTER: SELECT"); err != nil {
				t.Fatalf("timed out waiting for menu after DSK load for %s: %v", stage.name, err)
			}
		}

		if stage.enterToMenu {
			pressEnterUntilMenu(stage.waitTimeout)
		}

		if stage.key != 0 {
			if err := c.SendKey(stage.key); err != nil {
				t.Fatalf("failed to send key %q for %s: %v", stage.key, stage.name, err)
			}
		}

		if len(stage.waitFor) > 0 {
			if _, err := c.WaitForOCR(stage.waitTimeout, stage.waitFor...); err != nil {
				t.Fatalf("timed out waiting for %s: %v", stage.name, err)
			}
		}

		if stage.settleDelay > 0 {
			time.Sleep(stage.settleDelay)
		}

		captureChecked(stage.captureFile, stage.nonBlank)

		if stage.exitKey != 0 {
			if err := c.SendKey(stage.exitKey); err != nil {
				t.Fatalf("failed to send exit key %q for %s: %v", stage.exitKey, stage.name, err)
			}
			if stage.exitKey2 != 0 {
				time.Sleep(1 * time.Second)
				if err := c.SendKey(stage.exitKey2); err != nil {
					t.Fatalf("failed to send exit key2 %d for %s: %v", stage.exitKey2, stage.name, err)
				}
			}
			if len(stage.exitWaitFor) > 0 {
				if _, err := c.WaitForOCR(stage.exitTimeout, stage.exitWaitFor...); err != nil {
					t.Fatalf("timed out waiting to exit %s: %v", stage.name, err)
				}
			}
		}
	}
}

func screenshotsEqual(actual, approved []byte) bool {
	actualPixels, actualOK := bmpPixelData(actual)
	approvedPixels, approvedOK := bmpPixelData(approved)
	if actualOK && approvedOK {
		return bytes.Equal(actualPixels, approvedPixels)
	}
	return bytes.Equal(actual, approved)
}

func bmpPixelData(data []byte) ([]byte, bool) {
	if len(data) < 14 || data[0] != 'B' || data[1] != 'M' {
		return nil, false
	}
	offset := int(data[10]) |
		(int(data[11]) << 8) |
		(int(data[12]) << 16) |
		(int(data[13]) << 24)
	if offset <= 0 || offset >= len(data) {
		return nil, false
	}
	return data[offset:], true
}

func screenshotIsBlank(data []byte) bool {
	pixels, ok := bmpPixelData(data)
	if !ok || len(pixels) == 0 {
		return false
	}
	for _, b := range pixels {
		if b != 0 {
			return false
		}
	}
	return true
}
