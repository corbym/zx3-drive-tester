#!/usr/bin/env python3
"""Capture before/during/after screenshots to verify screen-clear behaviour."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import zesarux_smoketest as sm


def shot(client: sm.ZrcpClient, path: Path) -> None:
    client.command(f"save-screen {path.resolve()}")
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if path.exists() and path.stat().st_size > 0:
            return
        time.sleep(0.1)


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    out = repo / "out" / "screen-check"
    out.mkdir(parents=True, exist_ok=True)
    tap = (repo / "out" / "disk_tester.tap").resolve()

    proc = sm.start_emulator(
        binary=sm.DEFAULT_EMULATOR,
        port=10120,
        machine="P340",
        emulator_speed=300,
        video_driver="cocoa",
        zoom=2,
        headless=False,
    )
    c = None
    try:
        if not sm.wait_for_port("127.0.0.1", 10120, 20.0):
            raise RuntimeError("port did not open")
        c = sm.ZrcpClient("127.0.0.1", 10120, timeout=5.0)
        c.command("hard-reset-cpu")
        time.sleep(0.3)
        c.command(f"smartload {tap}")
        time.sleep(7.0)

        shot(c, out / "01_menu.bmp")

        c.command("send-keys-ascii 25 50")
        time.sleep(3.0)
        shot(c, out / "02_test2_running.bmp")

        time.sleep(3.0)
        shot(c, out / "03_after_test2.bmp")

        c.command("send-keys-ascii 25 53")
        time.sleep(3.0)
        shot(c, out / "04_test5_running.bmp")

        time.sleep(1.0)
        shot(c, out / "05_test5_fail_prompt.bmp")
        c.command("send-keys-ascii 25 32")
        time.sleep(2.5)
        shot(c, out / "06_menu_after_fail.bmp")

        c.command("send-keys-ascii 25 54")
        time.sleep(5.0)
        shot(c, out / "07_test6_loop.bmp")
        time.sleep(4.0)
        shot(c, out / "08_test6_loop2.bmp")
        c.command("send-keys-ascii 25 88")
        time.sleep(3.5)
        shot(c, out / "09_menu_after_loop.bmp")

        print("Screenshots written to", out)
        return 0
    finally:
        if c is not None:
            try:
                c.command("exit-emulator")
            except Exception:
                pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
