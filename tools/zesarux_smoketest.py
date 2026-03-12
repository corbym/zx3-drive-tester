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
MENU_MARKERS = ("== ZX +3 DISK TEST ==", "1 MOTOR+STATUS", "KEY:")
RESULT_MARKERS = ("== RESULTS ==", "TOTAL :", "PRESS ANY KEY")


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
    client.command(f"send-keys-ascii {key_delay_ms} {test_key}")
    test_text = wait_for_ocr(client, wait_markers, run_timeout, interval=ocr_poll_s)
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


def build_project(repo_root: Path, debug_mode: str) -> None:
    env = dict(os.environ)
    env["DEBUG"] = "1" if debug_mode == "on" else "0"
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
    parser.add_argument("--load-timeout", type=float, default=30.0)
    parser.add_argument("--run-timeout", type=float, default=60.0)
    parser.add_argument("--key-delay-ms", type=int, default=20, help="Delay between injected key events in ms")
    parser.add_argument("--ocr-poll-ms", type=int, default=100, help="OCR polling interval in ms")
    parser.add_argument("--reset-wait-ms", type=int, default=120, help="Wait after hard reset before loading in ms")
    parser.add_argument("--loader-wait-ms", type=int, default=250, help="Wait after entering +3 Loader before smartload retry in ms")
    parser.add_argument("--menu-return-timeout", type=float, default=4.0, help="Timeout waiting for menu after final keypress")
    parser.add_argument(
        "--debug-mode",
        choices=("on", "off"),
        default="off",
        help="Build tester with compile-time debug output enabled or disabled",
    )
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Run ZEsarUX without a display window (passes --vo null --ao null)",
    )
    args = parser.parse_args()

    ocr_poll_s = max(0.05, args.ocr_poll_ms / 1000.0)
    reset_wait_s = max(0.05, args.reset_wait_ms / 1000.0)
    loader_wait_s = max(0.1, args.loader_wait_ms / 1000.0)
    key_delay_ms = max(10, args.key_delay_ms)

    repo_root = args.repo_root.resolve()
    tap_path = (repo_root / args.tap).resolve() if not args.tap.is_absolute() else args.tap.resolve()
    if not tap_path.exists():
        print(f"ERROR: TAP file not found: {tap_path}", file=sys.stderr)
        return 2

    dsk_path: Path | None = None
    if not args.no_dsk:
        dsk_path = (repo_root / args.dsk).resolve() if not args.dsk.is_absolute() else args.dsk.resolve()
        if not dsk_path.exists():
            print(f"ERROR: DSK file not found: {dsk_path}", file=sys.stderr)
            return 2

    if not args.no_build:
        build_project(repo_root, args.debug_mode)

    if not args.emulator_binary.exists():
        print(f"ERROR: emulator binary not found: {args.emulator_binary}", file=sys.stderr)
        return 2

    if is_port_open(args.host, args.port):
        print(f"ERROR: port {args.port} already in use. Close existing ZEsarUX or choose --port.", file=sys.stderr)
        return 2

    emulator_speed = max(100, args.emulator_speed)
    started = start_emulator(args.emulator_binary, args.port, args.machine, emulator_speed, args.headless)
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

        load_tap_and_wait_menu(
            client,
            tap_path,
            args.load_timeout,
            key_delay_ms,
            loader_wait_s,
            ocr_poll_s,
        )

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
            ("== MOTOR AND STATUS ==", "PRESS ANY KEY"),
            key_delay_ms,
            args.run_timeout,
            args.menu_return_timeout,
            ocr_poll_s,
        )
        # Confirm motor cycling and drive status register were read.
        assert_ocr_fields(
            clean_response(motor_text),
            ["MOTOR ON", "MOTOR OFF", "ST3 =", "READY :"],
            "in motor+status test",
        )
        run_single_test_and_return(
            client,
            51,  # '3'
            ("== RECAL + SEEK 2 ==", "X OR BREAK=EXIT"),
            key_delay_ms,
            args.run_timeout,
            args.menu_return_timeout,
            ocr_poll_s,
            exit_key=88,  # 'X'
            allow_unknown_option=True,
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
        assert_no_unknown_option(cleaned_results, "in run-all output")
        assert_no_unknown_option(cleaned_menu, "after run-all")

        summary_lines = [
            line
            for line in cleaned_results.splitlines()
            if line.startswith(("MOTOR :", "DRIVE :", "RECAL :", "SEEK", "READID:", "TOTAL :"))
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
