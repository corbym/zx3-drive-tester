package tests

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

const (
	// Keep this ceiling conservative; if this grows, re-check real memory headroom.
	maxCodeBinBytes = 32000
	// Heap/stack headroom floor derived from current known-good map with buffer.
	minHeapToStackGapBytes = 0x0900
)

func TestTapCodeSizeBudget(t *testing.T) {
	repoRoot := repoRootFromTests(t)
	codePath := filepath.Join(repoRoot, "out", "disk_tester_CODE.bin")

	info, err := os.Stat(codePath)
	if err != nil {
		t.Fatalf("missing %s: build artifacts first via ./build.sh", codePath)
	}

	sz := info.Size()
	if sz > maxCodeBinBytes {
		t.Fatalf("disk_tester_CODE.bin too large: %d bytes (limit %d). This often precedes menu-missing/'Drive not ready' smoke failures due to memory pressure", sz, maxCodeBinBytes)
	}
}

func TestMapHeapStackHeadroom(t *testing.T) {
	repoRoot := repoRootFromTests(t)
	mapPath := filepath.Join(repoRoot, "out", "disk_tester.map")

	data, err := os.ReadFile(mapPath)
	if err != nil {
		t.Fatalf("missing %s: build artifacts first via ./build.sh", mapPath)
	}
	text := string(data)

	heap, err := mapAddress(text, "__malloc_heap")
	if err != nil {
		t.Fatal(err)
	}
	stack, err := mapAddress(text, "__sp_or_ret")
	if err != nil {
		t.Fatal(err)
	}

	if stack <= heap {
		t.Fatalf("invalid memory layout: __sp_or_ret (0x%X) <= __malloc_heap (0x%X)", stack, heap)
	}

	gap := stack - heap
	if gap < minHeapToStackGapBytes {
		t.Fatalf("heap->stack headroom too small: %d bytes (0x%X), need >= %d (0x%X). Likely memory/layout regression causing menu-missing/'Drive not ready' smoke failures", gap, gap, minHeapToStackGapBytes, minHeapToStackGapBytes)
	}
}

func repoRootFromTests(t *testing.T) string {
	t.Helper()
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatalf("failed to resolve cwd: %v", err)
	}
	return filepath.Clean(filepath.Join(cwd, ".."))
}

func mapAddress(mapText string, symbol string) (int, error) {
	pattern := fmt.Sprintf(`(?m)^%s\s+=\s+\$([0-9A-Fa-f]+)\s+;\s+addr`, regexp.QuoteMeta(symbol))
	re := regexp.MustCompile(pattern)
	m := re.FindStringSubmatch(mapText)
	if len(m) != 2 {
		return 0, fmt.Errorf("symbol %s not found in map", symbol)
	}
	v, err := strconv.ParseInt(strings.TrimSpace(m[1]), 16, 32)
	if err != nil {
		return 0, fmt.Errorf("failed parsing %s address %q: %w", symbol, m[1], err)
	}
	return int(v), nil
}
