package tests

import (
	"fmt"
	"os"
	"os/exec"
	"time"
)

type EmulatorClient struct {
	emuCmd *exec.Cmd
	host   string
	port   int
}

func startClient() (*EmulatorClient, error) {
	emulator, err := resolveEmulator()
	if err != nil {
		return nil, err
	}

	emuCmd, err := spawnEmulator(emulator, defaultPort)
	if err != nil {
		return nil, fmt.Errorf("failed to spawn emulator: %w", err)
	}

	if !waitForPort(defaultHost, defaultPort, 15*time.Second) {
		stopEmulator(emuCmd, defaultHost, defaultPort)
		return nil, fmt.Errorf("emulator did not open ZRCP port")
	}

	return &EmulatorClient{emuCmd: emuCmd, host: defaultHost, port: defaultPort}, nil
}

func (c *EmulatorClient) Stop() {
	if c == nil {
		return
	}
	stopEmulator(c.emuCmd, c.host, c.port)
}

func (c *EmulatorClient) CloseAllMenus() error {
	if c == nil {
		return fmt.Errorf("nil client")
	}
	_, err := zrcpCommand(c.host, c.port, "close-all-menus")
	return err
}

func (c *EmulatorClient) HardReset() error {
	if c == nil {
		return fmt.Errorf("nil client")
	}
	_, err := zrcpCommand(c.host, c.port, "hard-reset-cpu")
	return err
}

func (c *EmulatorClient) Smartload(path string) error {
	if c == nil {
		return fmt.Errorf("nil client")
	}
	_, err := zrcpCommand(c.host, c.port, "smartload "+path)
	return err
}

func (c *EmulatorClient) SendKey(key byte) error {
	if c == nil {
		return fmt.Errorf("nil client")
	}
	_, err := zrcpCommand(c.host, c.port, fmt.Sprintf("send-keys-ascii 25 %d", key))
	return err
}

func (c *EmulatorClient) OCR() (string, error) {
	if c == nil {
		return "", fmt.Errorf("nil client")
	}
	return zrcpCommand(c.host, c.port, "get-ocr")
}

func (c *EmulatorClient) WaitForOCR(timeout time.Duration, markers ...string) (string, error) {
	if c == nil {
		return "", fmt.Errorf("nil client")
	}

	deadline := time.Now().Add(timeout)
	var last string
	for time.Now().Before(deadline) {
		ocr, err := c.OCR()
		if err == nil {
			last = ocr
			if containsAll(ocr, markers...) {
				return ocr, nil
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	return last, errTimeout
}

func (c *EmulatorClient) SaveScreen(path string) error {
	if c == nil {
		return fmt.Errorf("nil client")
	}

	if _, err := zrcpCommand(c.host, c.port, "save-screen "+path); err != nil {
		return err
	}

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if st, err := os.Stat(path); err == nil && st.Size() > 0 {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("screen file did not appear: %s", path)
}
