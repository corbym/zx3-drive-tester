#!/usr/bin/env python3
import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 10000
DEFAULT_EMULATOR = Path("/Applications/zesarux.app/Contents/MacOS/zesarux")
MENU_MARKERS = ("SELECT KEY:",)
RESULT_MARKERS = ("TEST REPORT CARD", "OVERALL [", "PRESS ANY KEY")


class ZrcpClient:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def command(self, command: str) -> str:
        last_error = None
        for _ in range(2):
            try:
                with socket.create_connection((self.host, self.port), timeout=self.timeout) as sock:
                    sock.settimeout(self.timeout)
                    payload = (command.rstrip("\n") + "\n").encode("ascii", "replace")
                    sock.sendall(payload)
                    chunks = []
                    while True:
                        try:
                            data = sock.recv(4096)
                        except socket.timeout:
                            break
                        if not data:
                            break
                        chunks.append(data)
                    return b"".join(chunks).decode("utf-8", "replace")
            except (ConnectionResetError, BrokenPipeError, OSError) as exc:
                last_error = exc
                time.sleep(0.1)
        raise RuntimeError(f"ZRCP command failed after retry: {last_error}")

    def ocr(self) -> str:
        return self.command("get-ocr")


def wait_for_port(host: str, port: int, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def wait_for_ocr(client: ZrcpClient, markers, timeout: float, interval: float = 0.1) -> str:
    deadline = time.time() + timeout
    last_text = ""
    while time.time() < deadline:
        last_text = client.ocr()
        if all(marker in last_text for marker in markers):
            return last_text
        time.sleep(interval)
    raise TimeoutError(f"Timed out waiting for OCR markers: {markers}\nLast OCR:\n{last_text}")


def clean_response(text: str) -> str:
    lines = []
    for line in text.splitlines():
        if line.startswith("Welcome to ZEsarUX"):
            continue
        if line.startswith("Write help for available commands"):
            continue
        if line.startswith("command>"):
            line = line[len("command>"):].rstrip()
            if not line:
                continue
        lines.append(line.rstrip())
    return "\n".join(lines).strip()


def snapshot_state(client: ZrcpClient, label: str) -> str:
    parts = [f"--- {label} ---"]
    for cmd in ("get-registers", "get-paging-state", "hexdump 5B67 8"):
        try:
            parts.append(f"$ {cmd}")
            parts.append(clean_response(client.command(cmd)))
        except Exception as exc:
            parts.append(f"$ {cmd}")
            parts.append(f"ERROR: {exc}")
    return "\n".join(parts)


def log_ocr_block(enabled: bool, label: str, text: str, max_lines: int = 20) -> None:
    if not enabled:
        return
    print(f"OCR {label}:")
    cleaned = clean_response(text)
    lines = cleaned.splitlines()
    for line in lines[:max_lines]:
        print(line)
    if len(lines) > max_lines:
        print(f"... ({len(lines) - max_lines} more lines)")


def capture_menu_screenshot(client: ZrcpClient, screenshot_path: Path) -> None:
    screenshot_path.parent.mkdir(parents=True, exist_ok=True)
    if screenshot_path.exists():
        screenshot_path.unlink()

    client.command(f"save-screen {screenshot_path}")

    deadline = time.time() + 3.0
    while time.time() < deadline:
        if screenshot_path.exists() and screenshot_path.stat().st_size > 0:
            return
        time.sleep(0.1)

    raise RuntimeError(f"save-screen did not create screenshot: {screenshot_path}")


def wait_for_results_with_snapshots(
    client: ZrcpClient,
    timeout: float,
    interval: float,
) -> tuple[str, list[str]]:
    deadline = time.time() + timeout
    last_text = ""
    snapshots: list[str] = []
    seek_seen = False

    while time.time() < deadline:
        last_text = client.ocr()
        clean = clean_response(last_text)

        if ("*** Seek track" in clean) and not seek_seen:
            snapshots.append(snapshot_state(client, "seek-header"))
            seek_seen = True

        if all(marker in clean for marker in RESULT_MARKERS):
            snapshots.append(snapshot_state(client, "results-screen"))
            return last_text, snapshots

        time.sleep(interval)

    snapshots.append(snapshot_state(client, "timeout-state"))
    raise TimeoutError(
        f"Timed out waiting for OCR markers: {RESULT_MARKERS}\n"
        f"Last OCR:\n{last_text}\n\n"
        + "\n\n".join(snapshots)
    )


def load_tap_and_wait_menu(
    client: ZrcpClient,
    tap_path: Path,
    timeout: float,
    key_delay_ms: int,
    loader_wait_s: float,
    ocr_poll_s: float,
) -> str:
    # First try: direct smartload.
    client.command(f"smartload {tap_path}")
    try:
        return wait_for_ocr(client, MENU_MARKERS, timeout, interval=ocr_poll_s)
    except TimeoutError:
        pass

    # Fallback for +3 boot menu: press Return to enter Loader, then retry.
    client.command(f"send-keys-ascii {key_delay_ms} 13")
    time.sleep(loader_wait_s)
    client.command(f"smartload {tap_path}")
    return wait_for_ocr(client, MENU_MARKERS, timeout, interval=ocr_poll_s)


def leave_press_any_key_prompt(
    client: ZrcpClient,
    timeout: float,
    key_delay_ms: int,
    ocr_poll_s: float,
) -> str:
    try:
        current = wait_for_ocr(client, MENU_MARKERS, 0.5, interval=ocr_poll_s)
        return current
    except TimeoutError:
        pass

    for seq in ("32", "88", "13"):
        client.command(f"send-keys-ascii {key_delay_ms} {seq}")
        try:
            return wait_for_ocr(client, MENU_MARKERS, timeout, interval=ocr_poll_s)
        except TimeoutError:
            continue
    raise TimeoutError("Timed out leaving 'Press any key' prompt")


def assert_no_unknown_option(text: str, where: str) -> None:
    if "Unknown option" in text or "BAD KEY" in text:
        raise RuntimeError(f"Unexpected bad menu input seen {where}")


def assert_ocr_fields(text: str, fields: list[str], where: str) -> None:
    missing = [f for f in fields if f not in text]
    if missing:
        raise RuntimeError(f"Expected OCR fields missing {where}: {missing!r}")


def assert_no_ocr_fields(text: str, fields: list[str], where: str) -> None:
    present = [f for f in fields if f in text]
    if present:
        raise RuntimeError(f"Unexpected OCR fields present {where}: {present!r}")


def run_single_test_and_return(
    client: ZrcpClient,
    test_key: int,
    wait_markers: tuple[str, ...],
    key_delay_ms: int,
    run_timeout: float,
    menu_return_timeout: float,
    ocr_poll_s: float,
    exit_key: int | None = None,
    allow_unknown_option: bool = False,
) -> str:
    deadline = time.time() + run_timeout
    test_text = ""
    last_send = 0.0

    while time.time() < deadline:
        now = time.time()
        if now - last_send >= 0.35:
            client.command(f"send-keys-ascii {key_delay_ms} {test_key}")
            last_send = now

        test_text = client.ocr()
        if all(marker in test_text for marker in wait_markers):
            break

        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            f"Timed out waiting for test markers after key {test_key}: {wait_markers}\n"
            f"Last OCR:\n{test_text}"
        )

    if exit_key is None:
        returned_menu_text = leave_press_any_key_prompt(
            client,
            menu_return_timeout,
            key_delay_ms,
            ocr_poll_s,
        )
    else:
        returned_menu_text = ""
        deadline = time.time() + menu_return_timeout
        exit_seq = " ".join([str(exit_key)] * 12)
        while time.time() < deadline:
            client.command(f"send-keys-ascii {key_delay_ms} {exit_seq}")
            try:
                returned_menu_text = wait_for_ocr(
                    client,
                    MENU_MARKERS,
                    0.8,
                    interval=ocr_poll_s,
                )
                break
            except TimeoutError:
                continue
        if not returned_menu_text:
            raise TimeoutError("Timed out returning from looped test")
    clean_menu = clean_response(returned_menu_text)
    if not allow_unknown_option:
        assert_no_unknown_option(clean_menu, "after returning to menu")
    return test_text


def mount_dsk_after_tap_load(
    client: ZrcpClient,
    dsk_path: Path,
    tap_path: Path,
    key_delay_ms: int,
    load_timeout: float,
    loader_wait_s: float,
    menu_timeout: float,
    ocr_poll_s: float,
) -> None:
    # ZRCP has no explicit floppy insert command; smartload supports DSK and
    # may switch UI context, so we re-sync back to the tester menu after load.
    client.command(f"smartload {dsk_path}")
    try:
        wait_for_ocr(client, MENU_MARKERS, menu_timeout, interval=ocr_poll_s)
        return
    except TimeoutError:
        pass

    load_tap_and_wait_menu(
        client,
        tap_path,
        load_timeout,
        key_delay_ms,
        loader_wait_s,
        ocr_poll_s,
    )


def build_project(repo_root: Path, debug_mode: str, compact_ui: str) -> None:
    env = dict(os.environ)
    env["DEBUG"] = "1" if debug_mode == "on" else "0"
    env["COMPACT_UI"] = "1" if compact_ui == "on" else "0"
    subprocess.run(["sh", str(repo_root / "build.sh")], cwd=repo_root, check=True, env=env)


def is_port_open(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


def start_emulator(
    binary: Path,
    port: int,
    machine: str,
    emulator_speed: int,
    video_driver: str | None,
    zoom: int | None,
    headless: bool = False,
) -> subprocess.Popen:
    cmd = [
        str(binary),
        "--machine",
        machine,
        "--emulatorspeed",
        str(emulator_speed),
        "--fastautoload",
        "--enable-remoteprotocol",
        "--remoteprotocol-port",
        str(port),
        "--noconfigfile",
    ]
    if headless:
        cmd += ["--vo", "null", "--ao", "null"]
    else:
        if video_driver:
            cmd += ["--vo", video_driver]
        if zoom is not None and zoom > 0:
            cmd += ["--zoom", str(zoom)]

    return subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def stop_emulator(client: ZrcpClient, proc: subprocess.Popen | None) -> None:
    try:
        client.command("exit-emulator")
    except Exception:
        pass

    if proc is None:
        return

    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass

    proc.terminate()
    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def write_ocr_artifact(artifacts_dir: Path | None, name: str, text: str) -> None:
    if artifacts_dir is None:
        return
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    target = artifacts_dir / name
    target.write_text(clean_response(text), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a full ZEsarUX +3 smoke test against disk_tester.tap")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--tap", type=Path, default=Path("out/disk_tester.tap"))
    parser.add_argument("--dsk", type=Path, default=Path("out/disk_tester_plus3.dsk"), help="DSK image to mount in drive A for disk read tests")
    parser.add_argument("--no-dsk", action="store_true", help="Do not mount any DSK image")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--no-build", action="store_true", help="Skip build before running the smoke test")
    parser.add_argument("--emulator-binary", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--machine", default="P340", help="ZEsarUX machine id (e.g. P340, P341, P3S)")
    parser.add_argument("--emulator-speed", type=int, default=600, help="ZEsarUX emulation speed percentage")
    parser.add_argument("--boot-timeout", type=float, default=20.0)
    parser.add_argument("--load-timeout", type=float, default=20.0)
    parser.add_argument("--run-timeout", type=float, default=60.0)
    parser.add_argument("--key-delay-ms", type=int, default=20, help="Delay between injected key events in ms")
    parser.add_argument("--ocr-poll-ms", type=int, default=100, help="OCR polling interval in ms")
    parser.add_argument("--reset-wait-ms", type=int, default=120, help="Wait after hard reset before loading in ms")
    parser.add_argument("--loader-wait-ms", type=int, default=120, help="Wait after entering +3 Loader before smartload retry in ms")
    parser.add_argument("--menu-return-timeout", type=float, default=4.0, help="Timeout waiting for menu after final keypress")
    parser.add_argument(
        "--debug-mode",
        choices=("on", "off"),
        default="off",
        help="Build tester with compile-time debug output enabled or disabled",
    )
    parser.add_argument(
        "--compact-ui",
        choices=("on", "off"),
        default="off",
        help="Build tester with compact display font (off keeps OCR-safe glyphs)",
    )
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Run ZEsarUX without a display window (passes --vo null --ao null)",
    )
    parser.add_argument(
        "--gui-driver",
        default="cocoa",
        help="Video output driver for non-headless runs (default: cocoa)",
    )
    parser.add_argument(
        "--gui-zoom",
        type=int,
        default=2,
        help="Window zoom factor for non-headless runs (default: 2)",
    )
    parser.add_argument(
        "--ocr-artifacts-dir",
        type=Path,
        default=None,
        help="Optional directory to write OCR captures and snapshots",
    )
    parser.add_argument(
        "--menu-screenshot",
        type=Path,
        default=None,
        help="Optional BMP/SCR/PBM screenshot path captured when tester menu is visible",
    )
    parser.add_argument(
        "--log-ocr",
        action="store_true",
        help="Print key OCR captures to stdout for CI build logs",
    )
    parser.add_argument(
        "--menu-only",
        action="store_true",
        help="Load tester and capture menu artifacts only, then exit",
    )
    args = parser.parse_args()

    ocr_poll_s = max(0.05, args.ocr_poll_ms / 1000.0)
    reset_wait_s = max(0.05, args.reset_wait_ms / 1000.0)
    loader_wait_s = max(0.1, args.loader_wait_ms / 1000.0)
    key_delay_ms = max(10, args.key_delay_ms)

    repo_root = args.repo_root.resolve()
    tap_path = (repo_root / args.tap).resolve() if not args.tap.is_absolute() else args.tap.resolve()
    dsk_path: Path | None = None
    if not args.no_dsk:
        dsk_path = (repo_root / args.dsk).resolve() if not args.dsk.is_absolute() else args.dsk.resolve()

    if not args.no_build:
        build_project(repo_root, args.debug_mode, args.compact_ui)

    if args.compact_ui == "on":
        print("WARNING: --compact-ui on is intended for human display and may reduce OCR reliability", file=sys.stderr)

    if not tap_path.exists():
        print(f"ERROR: TAP file not found: {tap_path}", file=sys.stderr)
        return 2

    if dsk_path is not None and not dsk_path.exists():
        print(f"ERROR: DSK file not found: {dsk_path}", file=sys.stderr)
        return 2

    if not args.emulator_binary.exists():
        print(f"ERROR: emulator binary not found: {args.emulator_binary}", file=sys.stderr)
        return 2

    if is_port_open(args.host, args.port):
        print(f"ERROR: port {args.port} already in use. Close existing ZEsarUX or choose --port.", file=sys.stderr)
        return 2

    if args.headless:
        emulator_speed = max(100, args.emulator_speed)
        video_driver = None
        zoom = None
    else:
        # Keep GUI runs readable and stable by default.
        emulator_speed = 100
        video_driver = args.gui_driver
        zoom = max(1, args.gui_zoom)

    artifacts_dir: Path | None = None
    if args.ocr_artifacts_dir is not None:
        artifacts_dir = (
            (repo_root / args.ocr_artifacts_dir).resolve()
            if not args.ocr_artifacts_dir.is_absolute()
            else args.ocr_artifacts_dir.resolve()
        )

    menu_screenshot_path: Path | None = None
    if args.menu_screenshot is not None:
        menu_screenshot_path = (
            (repo_root / args.menu_screenshot).resolve()
            if not args.menu_screenshot.is_absolute()
            else args.menu_screenshot.resolve()
        )

    started = start_emulator(
        args.emulator_binary,
        args.port,
        args.machine,
        emulator_speed,
        video_driver,
        zoom,
        args.headless,
    )
    if not wait_for_port(args.host, args.port, args.boot_timeout):
        print("ERROR: timed out waiting for ZEsarUX to start", file=sys.stderr)
        return 2

    client = ZrcpClient(args.host, args.port)

    try:
        client.command("close-all-menus")
        client.command("hard-reset-cpu")
        time.sleep(reset_wait_s)

        machine = clean_response(client.command("get-current-machine"))
        if "+3" not in machine:
            raise RuntimeError(f"Emulator not in +3 mode: {machine}")

        try:
            menu_text = load_tap_and_wait_menu(
                client,
                tap_path,
                args.load_timeout,
                key_delay_ms,
                loader_wait_s,
                ocr_poll_s,
            )
        except TimeoutError:
            if not args.menu_only:
                raise
            # In compact-font human mode OCR may fail to match menu markers.
            menu_text = client.ocr()
        write_ocr_artifact(artifacts_dir, "menu_loaded.txt", menu_text)
        log_ocr_block(args.log_ocr, "menu-loaded", menu_text)
        if menu_screenshot_path is not None:
            capture_menu_screenshot(client, menu_screenshot_path)
            print(f"Menu screenshot saved: {menu_screenshot_path}")

        if args.menu_only:
            print("Menu-only capture complete")
            return 0

        if dsk_path is not None:
            mount_dsk_after_tap_load(
                client,
                dsk_path,
                tap_path,
                key_delay_ms,
                args.load_timeout,
                loader_wait_s,
                args.menu_return_timeout,
                ocr_poll_s,
            )

        # Verify single-key menu interactions and menu return path.
        motor_text = run_single_test_and_return(
            client,
            49,  # '1'
            ("MOTOR ON", "PRESS ANY KEY"),
            key_delay_ms,
            args.run_timeout,
            args.menu_return_timeout,
            ocr_poll_s,
        )
        write_ocr_artifact(artifacts_dir, "test_motor_status.txt", motor_text)
        log_ocr_block(args.log_ocr, "test-motor", motor_text)
        # Confirm motor cycling and drive status register were read.
        assert_ocr_fields(
            clean_response(motor_text),
            ["MOTOR ON", "MOTOR OFF", "ST3 =", "READY :"],
            "in motor+status test",
        )
        recal_seek_text = run_single_test_and_return(
            client,
            51,  # '3'
            ("X OR BREAK=EXIT",),
            key_delay_ms,
            args.run_timeout,
            args.menu_return_timeout,
            ocr_poll_s,
            exit_key=88,  # 'X'
            allow_unknown_option=True,
        )
        cleaned_recal_seek = clean_response(recal_seek_text)
        assert_ocr_fields(
            cleaned_recal_seek,
            ["SenseInt: ST0=0x", "PCN=0", "PCN=2", "Recal: PASS", "Seek : PASS"],
            "in recal+seek flow",
        )
        assert_no_ocr_fields(
            cleaned_recal_seek,
            ["DRIVE NOT READY", "FAIL: recal cmd", "FAIL: seek cmd", "FAIL: wait timeout"],
            "in recal+seek flow",
        )
        track_text = run_single_test_and_return(
            client,
            54,  # '6'
            ("PASS #",),
            key_delay_ms,
            min(args.run_timeout, 20.0),
            max(args.menu_return_timeout, 12.0),
            ocr_poll_s,
            exit_key=88,  # 'X'
            allow_unknown_option=True,
        )
        write_ocr_artifact(artifacts_dir, "test_track_loop.txt", track_text)
        log_ocr_block(args.log_ocr, "test-track-loop", track_text)
        # Confirm at least one full sector was read with checksum output.
        assert_ocr_fields(
            clean_response(track_text),
            ["PASS #", "BYTES=", "SUM=0x"],
            "in read-track-loop test",
        )
        rpm_text = run_single_test_and_return(
            client,
            55,  # '7'
            ("RPM",),
            key_delay_ms,
            min(args.run_timeout, 20.0),
            max(args.menu_return_timeout, 12.0),
            ocr_poll_s,
            exit_key=88,  # 'X'
            allow_unknown_option=True,
        )
        write_ocr_artifact(artifacts_dir, "test_rpm.txt", rpm_text)
        log_ocr_block(args.log_ocr, "test-rpm", rpm_text)
        # Accept a real measurement (VALUE=) or known emulator N/A reasons.
        # Emulators that return the same sector ID on every Read ID command will
        # produce SAME SEC; real hardware with no index signal produces NO REV MARK.
        cleaned_rpm = clean_response(rpm_text)
        if not any(s in cleaned_rpm for s in ("VALUE=", "SAME SEC", "NO REV MARK", "ID FAIL")):
            raise RuntimeError(f"RPM test produced no recognisable result\nOCR: {cleaned_rpm!r}")

        # Full run-all from menu key only (no Enter).
        client.command(f"send-keys-ascii {key_delay_ms} 65")
        results_text, snapshots = wait_for_results_with_snapshots(
            client,
            args.run_timeout,
            ocr_poll_s,
        )

        returned_menu_text = leave_press_any_key_prompt(
            client,
            args.menu_return_timeout,
            key_delay_ms,
            ocr_poll_s,
        )

        cleaned_results = clean_response(results_text)
        cleaned_menu = clean_response(returned_menu_text)
        write_ocr_artifact(artifacts_dir, "results_screen.txt", results_text)
        write_ocr_artifact(artifacts_dir, "returned_menu.txt", returned_menu_text)
        log_ocr_block(args.log_ocr, "results", results_text)
        log_ocr_block(args.log_ocr, "returned-menu", returned_menu_text)

        if artifacts_dir is not None:
            for idx, snap in enumerate(snapshots, start=1):
                (artifacts_dir / f"snapshot_{idx:02d}.txt").write_text(snap, encoding="utf-8")
        assert_no_unknown_option(cleaned_results, "in run-all output")
        assert_no_unknown_option(cleaned_menu, "after run-all")

        summary_lines = [
            line
            for line in cleaned_results.splitlines()
            if line.startswith(("MOTOR", "DRIVE", "RECAL", "SEEK", "READID", "OVERALL ["))
        ]

        print("ZEsarUX smoke test passed")
        print(f"Machine: {args.machine}")
        print(f"TAP: {tap_path}")
        print(f"DSK: {dsk_path if dsk_path is not None else 'not mounted'}")
        print("Summary:")
        for line in summary_lines:
            print(line)
        print("Snapshots:")
        for snap in snapshots:
            print(snap)
        print("Returned menu detected:")
        for line in cleaned_menu.splitlines()[:12]:
            print(line)
        return 0
    except Exception as exc:
        print(f"ZEsarUX smoke test failed: {exc}", file=sys.stderr)
        try:
            print("Last OCR:", file=sys.stderr)
            print(clean_response(client.ocr()), file=sys.stderr)
        except Exception:
            pass
        return 1
    finally:
        stop_emulator(client, started)


if __name__ == "__main__":
    raise SystemExit(main())
