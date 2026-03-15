#!/usr/bin/env python3
"""Capture before/during/after screenshots to verify screen-clear behaviour."""
import argparse
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
    parser = argparse.ArgumentParser(description="Capture staged UI screenshots through ZEsarUX")
    parser.add_argument("--port", type=int, default=10120)
    parser.add_argument("--machine", default="P340")
    parser.add_argument("--emulator-speed", type=int, default=300)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--gui-driver", default="cocoa")
    parser.add_argument("--gui-zoom", type=int, default=2)
    parser.add_argument("--emulator-binary", type=Path, default=sm.DEFAULT_EMULATOR)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    out = repo / "out" / "screen-check"
    out.mkdir(parents=True, exist_ok=True)
    tap = (repo / "out" / "disk_tester.tap").resolve()

    if not args.emulator_binary.exists():
        raise RuntimeError(f"emulator binary not found: {args.emulator_binary}")

    video_driver = None if args.headless else args.gui_driver
    zoom = None if args.headless else max(1, args.gui_zoom)

    proc = sm.start_emulator(
        binary=args.emulator_binary,
        port=args.port,
        machine=args.machine,
        emulator_speed=args.emulator_speed,
        video_driver=video_driver,
        zoom=zoom,
        headless=args.headless,
    )
    c = None
    try:
        if not sm.wait_for_port("127.0.0.1", args.port, 20.0):
            raise RuntimeError("port did not open")
        c = sm.ZrcpClient("127.0.0.1", args.port, timeout=5.0)
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
