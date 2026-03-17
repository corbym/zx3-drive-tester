#!/usr/bin/env python3
import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 10000
DEFAULT_EMULATOR = Path("/Applications/zesarux.app/Contents/MacOS/zesarux")
DEFAULT_ZESARUX_HOME = Path("/tmp/zesarux-smoketest-home")
DEFAULT_GUI_DRIVER = "cocoa"
MENU_MARKERS = ("ZX +3 DISK TESTER", "ENTER: SELECT")
RESULT_MARKERS = ("TEST REPORT CARD", "OVERALL [")
DEFAULT_MAX_BSS_UNINITIALIZED_TAIL = 0xFEFF


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


def parse_map_symbols(map_path: Path) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for raw_line in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if "=" not in line:
            continue
        left, right = line.split("=", 1)
        symbol = left.strip()
        value_text = right.strip().split()[0]
        if not value_text.startswith("$"):
            continue
        try:
            symbols[symbol] = int(value_text[1:], 16)
        except ValueError:
            continue
    return symbols


def fail_fast_memory_regression(
    repo_root: Path,
    max_bss_uninitialized_tail: int,
    fail_on_backbuffer_symbols: bool,
) -> None:
    map_path = repo_root / "out" / "disk_tester.map"
    if not map_path.exists():
        raise RuntimeError(f"Missing map file for memory check: {map_path}")

    map_text = map_path.read_text(encoding="utf-8", errors="replace")
    symbols = parse_map_symbols(map_path)

    if fail_on_backbuffer_symbols:
        if "_ui_back_pixels" in map_text or "_ui_back_attrs" in map_text:
            raise RuntimeError(
                "Memory regression: full-screen backbuffer symbols found "
                "(_ui_back_pixels/_ui_back_attrs)"
            )

    tail = symbols.get("__BSS_UNINITIALIZED_tail")
    if tail is None:
        raise RuntimeError("Memory regression check failed: __BSS_UNINITIALIZED_tail not found")
    if tail > max_bss_uninitialized_tail:
        raise RuntimeError(
            "Memory regression: __BSS_UNINITIALIZED_tail too high "
            f"(${tail:04X} > ${max_bss_uninitialized_tail:04X})"
        )


def fail_fast_ui_source_regressions(repo_root: Path) -> None:
    source_path = repo_root / "disk_tester.c"
    if not source_path.exists():
        raise RuntimeError(f"Missing source file for UI regression check: {source_path}")

    text = source_path.read_text(encoding="utf-8", errors="replace")
    required_snippets = (
        "wait_after_test_run(1);",
        "#define RUN_ALL_RESULT_DELAY_MS 1800U",
        "ui_render_cached_text_row",
        "ui_style_screen_text_row",
        # Drive read ID probe must carry an explicit interactive parameter, be called
        # with 1 from the menu (solo user selection) and 0 from run_all.  This
        # prevents regression to "AUTO RETURN MENU" / no-wait when the test is
        # chosen directly from the menu.
        "static void test_read_id_probe(int interactive)",
        "test_read_id_probe(1);",
        "test_read_id_probe(0);",
        # RPM loop should not auto-exit on transient not-ready and should arm
        # ENTER/X exit only after a short debounce window.
        "rpm_exit_armed(loop_start_tick) && loop_exit_requested()",
        "LAST  : DRIVE NOT READY",
    )
    missing = [snippet for snippet in required_snippets if snippet not in text]
    if missing:
        raise RuntimeError(
            "UI regression source check failed; expected smoke-protected snippets missing: "
            f"{missing!r}"
        )


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
    markerless_mode: bool = False,
) -> str:
    if markerless_mode:
        # Compact-font mode can defeat OCR menu markers. Use a conservative
        # load path and return best-effort OCR without strict marker matching.
        client.command(f"smartload {tap_path}")
        time.sleep(min(timeout, 4.0))
        text = client.ocr()
        clean = clean_response(text)
        if "Drive not ready" in clean or "Bytes:" in clean:
            client.command(f"send-keys-ascii {key_delay_ms} 13")
            time.sleep(loader_wait_s)
            client.command(f"smartload {tap_path}")
            time.sleep(min(timeout, 4.0))
            text = client.ocr()
        return text

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


def selected_menu_line_index(text: str) -> int:
    menu_lines = [
        line
        for line in clean_response(text).splitlines()
        if line.startswith(" ") and len(line) > 3
    ]
    for idx, line in enumerate(menu_lines):
        if "~" in line:
            return idx
    return -1


def run_nav_select_and_return(
    client: ZrcpClient,
    nav_key: int,
    wait_markers: tuple[str, ...],
    key_delay_ms: int,
    run_timeout: float,
    menu_return_timeout: float,
    ocr_poll_s: float,
) -> str:
    # Move the highlighted menu row with a navigation key, then press Enter.
    client.command(f"send-keys-ascii {key_delay_ms} {nav_key}")
    time.sleep(max(0.05, ocr_poll_s))
    return run_single_test_and_return(
        client,
        13,  # Enter
        wait_markers,
        key_delay_ms,
        run_timeout,
        menu_return_timeout,
        ocr_poll_s,
    )


def run_single_test_and_return(
    client: ZrcpClient,
    test_key: int,
    wait_markers: tuple[str, ...],
    key_delay_ms: int,
    run_timeout: float,
    menu_return_timeout: float,
    ocr_poll_s: float,
    exit_key: int | None = None,
    exit_key_repeat: int = 1,
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
        if all(marker in test_text for marker in MENU_MARKERS):
            returned_menu_text = test_text
        else:
            returned_menu_text = leave_press_any_key_prompt(
                client,
                menu_return_timeout,
                key_delay_ms,
                ocr_poll_s,
            )
    else:
        returned_menu_text = ""
        deadline = time.time() + menu_return_timeout
        exit_seq = " ".join([str(exit_key)] * max(1, exit_key_repeat))
        while time.time() < deadline:
            client.command(f"send-keys-ascii {key_delay_ms} {exit_seq}")
            prompt_or_menu_text = client.ocr()
            if "PRESS ANY KEY" in prompt_or_menu_text:
                returned_menu_text = leave_press_any_key_prompt(
                    client,
                    menu_return_timeout,
                    key_delay_ms,
                    ocr_poll_s,
                )
                break
            if all(marker in prompt_or_menu_text for marker in MENU_MARKERS):
                returned_menu_text = prompt_or_menu_text
                break
            time.sleep(ocr_poll_s)
        if not returned_menu_text:
            raise TimeoutError("Timed out returning from looped test")
    clean_menu = clean_response(returned_menu_text)
    if not allow_unknown_option:
        assert_no_unknown_option(clean_menu, "after returning to menu")
    return test_text


def run_track_loop_status_regression_check(
    client: ZrcpClient,
    tap_path: Path,
    key_delay_ms: int,
    open_timeout: float,
    stop_timeout: float,
    load_timeout: float,
    loader_wait_s: float,
    reset_wait_s: float,
    ocr_poll_s: float,
) -> tuple[str, str]:
    # Select one test (track loop via hotkey 'D') and verify:
    # 1) initial status page appears,
    # 2) populated status page appears afterwards.
    # Afterwards, hard-reset and reload the TAP so the main smoke run continues
    # from a deterministic menu state.
    open_deadline = time.time() + open_timeout
    initial_text = ""
    while time.time() < open_deadline:
        client.command(f"send-keys-ascii {key_delay_ms} 68")  # 'D'
        initial_text = client.ocr()
        clean_initial = clean_response(initial_text)
        if "READ TRACK DATA LOOP" in clean_initial:
            # Accept template or first populated frame.
            if (
                "TRACK :" not in clean_initial
                and "PASS  :" not in clean_initial
                and "FAIL  :" not in clean_initial
            ) or (
                "KEYS  : J/K TRACK  ENTER/X EXIT" in clean_initial
                and "TRACK :" in clean_initial
                and "PASS  :" in clean_initial
                and "FAIL  :" in clean_initial
            ):
                break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for initial track-loop status screen\n"
            f"Last OCR:\n{initial_text}"
        )

    data_deadline = time.time() + stop_timeout
    populated_text = ""
    while time.time() < data_deadline:
        populated_text = client.ocr()
        clean_data = clean_response(populated_text)
        if (
            "READ TRACK DATA LOOP" in clean_data
            and "TRACK :" in clean_data
            and "PASS  :" in clean_data
            and "FAIL  :" in clean_data
            and "LAST  :" in clean_data
            and "INFO  :" in clean_data
            and "RESULT:" in clean_data
        ):
            break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for populated track-loop status screen\n"
            f"Last OCR:\n{populated_text}"
        )

    # Return to a deterministic menu state for the remainder of the smoke run.
    client.command("hard-reset-cpu")
    time.sleep(reset_wait_s)
    load_tap_and_wait_menu(
        client,
        tap_path,
        load_timeout,
        key_delay_ms,
        loader_wait_s,
        ocr_poll_s,
    )

    return initial_text, populated_text


def run_rpm_loop_status_regression_check(
    client: ZrcpClient,
    key_delay_ms: int,
    open_timeout: float,
    sample_timeout: float,
    linger_timeout: float,
    menu_return_timeout: float,
    ocr_poll_s: float,
) -> tuple[str, str]:
    title = "DISK RPM CHECK LOOP"

    # Move deterministically to RPM row, then use Enter selection.
    for _ in range(8):
        client.command(f"send-keys-ascii {key_delay_ms} 70")  # 'F' up
        time.sleep(max(0.03, ocr_poll_s / 2.0))
    for _ in range(6):
        client.command(f"send-keys-ascii {key_delay_ms} 86")  # 'V' down
        time.sleep(max(0.03, ocr_poll_s / 2.0))

    # Trigger selection once with Enter; avoid repeated Enter because that can
    # be interpreted as loop-exit input right after the test opens.
    client.command(f"send-keys-ascii {key_delay_ms} 13")

    open_deadline = time.time() + open_timeout
    initial_text = ""
    while time.time() < open_deadline:
        initial_text = client.ocr()
        cleaned = clean_response(initial_text)
        if title in cleaned and "RPM   :" in cleaned and "RESULT:" in cleaned:
            break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for selected RPM loop status screen\n"
            f"Last OCR:\n{initial_text}"
        )

    # Regression: selected RPM test should not bounce straight back to menu.
    linger_deadline = time.time() + max(0.3, linger_timeout)
    while time.time() < linger_deadline:
        current = client.ocr()
        cleaned = clean_response(current)
        if all(marker in cleaned for marker in MENU_MARKERS):
            raise RuntimeError("Selected RPM test returned to menu before explicit exit")
        time.sleep(ocr_poll_s)

    sample_deadline = time.time() + sample_timeout
    sampled_text = ""
    while time.time() < sample_deadline:
        sampled_text = client.ocr()
        cleaned = clean_response(sampled_text)
        if all(marker in cleaned for marker in MENU_MARKERS):
            raise RuntimeError("Selected RPM test returned to menu before RPM sample was captured")
        has_numeric_rpm = bool(re.search(r"RPM\s*:\s*[0-9]{2,3}", cleaned))
        has_populated_status = (
            title in cleaned
            and "PASS  :" in cleaned
            and "FAIL  :" in cleaned
            and "LAST  :" in cleaned
            and "INFO  :" in cleaned
            and "RESULT:" in cleaned
        )
        if has_numeric_rpm or has_populated_status:
            break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for RPM loop status/sample frame\n"
            f"Last OCR:\n{sampled_text}"
        )

    returned_menu_text = ""
    leave_deadline = time.time() + menu_return_timeout
    while time.time() < leave_deadline:
        client.command(f"send-keys-ascii {key_delay_ms} 88")  # 'X'
        current = client.ocr()
        cleaned_current = clean_response(current)
        if "PRESS ANY KEY" in current:
            returned_menu_text = leave_press_any_key_prompt(
                client,
                menu_return_timeout,
                key_delay_ms,
                ocr_poll_s,
            )
            break
        if "PRESS ENTER" in cleaned_current or "FAIL - PRESS ENTER FOR MENU" in cleaned_current:
            client.command(f"send-keys-ascii {key_delay_ms} 13")
            returned_menu_text = wait_for_ocr(
                client,
                MENU_MARKERS,
                menu_return_timeout,
                interval=ocr_poll_s,
            )
            break
        if all(marker in current for marker in MENU_MARKERS):
            returned_menu_text = current
            break
        time.sleep(ocr_poll_s)
    if not returned_menu_text:
        raise TimeoutError("Timed out returning to menu after selected RPM loop test")

    assert_no_unknown_option(clean_response(returned_menu_text), "after selected RPM regression")
    return initial_text, sampled_text


def assert_stable_selected_test_frame(
    text: str,
    title: str,
    required_fields: tuple[str, ...],
    where: str,
) -> None:
    cleaned = clean_response(text)
    if title not in cleaned:
        return
    missing = [field for field in required_fields if field not in cleaned]
    if missing:
        raise RuntimeError(
            f"Partial selected-test frame seen {where}: missing {missing!r}\nOCR:\n{cleaned}"
        )


def run_selected_recal_status_regression_check(
    client: ZrcpClient,
    key_delay_ms: int,
    open_timeout: float,
    run_timeout: float,
    menu_return_timeout: float,
    ocr_poll_s: float,
) -> tuple[str, str, str]:
    title = "RECALIBRATE AND SEEK TRACK 2"
    stable_fields = ("KEYS", "RECAL :", "SEEK  :", "RESULT:")

    # Move deterministically to the recal row, then use Enter.
    for _ in range(8):
        client.command(f"send-keys-ascii {key_delay_ms} 70")  # 'F' up
        time.sleep(max(0.03, ocr_poll_s / 2.0))
    for _ in range(2):
        client.command(f"send-keys-ascii {key_delay_ms} 86")  # 'V' down
        time.sleep(max(0.03, ocr_poll_s / 2.0))

    ready_deadline = time.time() + open_timeout
    ready_text = ""
    last_send = 0.0
    while time.time() < ready_deadline:
        now = time.time()
        if now - last_send >= 0.35:
            client.command(f"send-keys-ascii {key_delay_ms} 13")
            client.command(f"send-keys-ascii {key_delay_ms} 75")
            client.command(f"send-keys-ascii {key_delay_ms} 107")
            last_send = now
        ready_text = client.ocr()
        assert_stable_selected_test_frame(ready_text, title, stable_fields, "while waiting for READY")
        cleaned = clean_response(ready_text)
        if title in cleaned and "RESULT: READY" in cleaned and "TRACK :" in cleaned:
            break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for selected recal READY card\n"
            f"Last OCR:\n{ready_text}"
        )

    running_deadline = time.time() + run_timeout
    running_text = ""
    while time.time() < running_deadline:
        running_text = client.ocr()
        assert_stable_selected_test_frame(running_text, title, stable_fields, "while waiting for RUNNING")
        cleaned = clean_response(running_text)
        if title in cleaned and "RESULT: RUNNING" in cleaned:
            break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for selected recal RUNNING card\n"
            f"Last OCR:\n{running_text}"
        )

    final_deadline = time.time() + run_timeout
    final_text = ""
    while time.time() < final_deadline:
        final_text = client.ocr()
        assert_stable_selected_test_frame(final_text, title, stable_fields, "while waiting for final result")
        cleaned = clean_response(final_text)
        if (
            title in cleaned
            and ("RESULT: PASS" in cleaned or "RESULT: FAIL" in cleaned)
            and "TRACK :" in cleaned
        ):
            break
        time.sleep(ocr_poll_s)
    else:
        raise TimeoutError(
            "Timed out waiting for selected recal final card\n"
            f"Last OCR:\n{final_text}"
        )

    # It should stay on the final card until Enter is pressed.
    linger_deadline = time.time() + min(0.8, menu_return_timeout)
    while time.time() < linger_deadline:
        current = client.ocr()
        cleaned = clean_response(current)
        assert_stable_selected_test_frame(current, title, stable_fields, "during final-card linger")
        if all(marker in cleaned for marker in MENU_MARKERS):
            raise RuntimeError("Selected recal test returned to menu before ENTER was pressed")
        time.sleep(ocr_poll_s)

    client.command(f"send-keys-ascii {key_delay_ms} 13")
    returned_menu = wait_for_ocr(client, MENU_MARKERS, menu_return_timeout, interval=ocr_poll_s)
    assert_no_unknown_option(clean_response(returned_menu), "after selected recal regression")
    return ready_text, running_text, final_text


def wait_for_menu_after_runall_with_progress(
    client: ZrcpClient,
    timeout: float,
    interval: float,
    min_report_frames: int,
    key_delay_ms: int,
) -> tuple[str, list[str]]:
    deadline = time.time() + timeout
    last_text = ""
    report_frames: list[str] = []
    seen_report_frames: set[str] = set()
    sent_enter_on_complete = False

    while time.time() < deadline:
        last_text = client.ocr()
        clean = clean_response(last_text)

        if "TEST REPORT CARD" in clean and "STATUS:" in clean:
            if clean not in seen_report_frames:
                seen_report_frames.add(clean)
                report_frames.append(clean)

            if "STATUS: COMPLETE" in clean and not sent_enter_on_complete:
                client.command(f"send-keys-ascii {key_delay_ms} 13")
                sent_enter_on_complete = True

        if all(marker in clean for marker in MENU_MARKERS):
            if len(report_frames) < min_report_frames:
                raise RuntimeError(
                    "Run-all pacing regression: too few intermediate report-card states observed "
                    f"({len(report_frames)} < {min_report_frames})"
                )
            return last_text, report_frames

        time.sleep(interval)

    raise TimeoutError(
        f"Timed out waiting for menu after run-all\nLast OCR:\n{last_text}"
    )


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


def build_project(repo_root: Path, debug_mode: str, compact_ui: str, headless: bool) -> None:
    env = dict(os.environ)
    env["DEBUG"] = "1" if debug_mode == "on" else "0"
    env["COMPACT_UI"] = "1" if compact_ui == "on" else "0"
    env["HEADLESS_ROM_FONT"] = "1" if headless else "0"
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
    env = dict(os.environ)
    # Keep smoke runs isolated from user-level ZEsarUX and SDL config drift.
    DEFAULT_ZESARUX_HOME.mkdir(parents=True, exist_ok=True)
    env["HOME"] = str(DEFAULT_ZESARUX_HOME)

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
        resolved_driver = (video_driver or DEFAULT_GUI_DRIVER).strip().lower()
        if resolved_driver == "null":
            # Never allow accidental null renderer in human mode.
            resolved_driver = DEFAULT_GUI_DRIVER
            print(
                f"WARNING: ignoring --gui-driver null in GUI mode; using {resolved_driver}",
                file=sys.stderr,
            )
        env.pop("SDL_VIDEODRIVER", None)
        env.pop("SDL_AUDIODRIVER", None)
        cmd += ["--vo", resolved_driver]
        cmd += ["--ao", "coreaudio"]
        # With --noconfigfile, force desktop panel so GUI shows floppy/menus.
        cmd += ["--enable-zxdesktop", "--zxdesktop-width", "256"]
        if zoom is not None and zoom > 0:
            cmd += ["--zoom", str(zoom)]

    return subprocess.Popen(
        cmd,
        env=env,
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
        "--log-ocr",
        action="store_true",
        help="Print key OCR captures to stdout for CI build logs",
    )
    parser.add_argument(
        "--menu-only",
        action="store_true",
        help="Load tester and capture menu artifacts only, then exit",
    )
    parser.add_argument(
        "--max-bss-uninitialized-tail",
        type=lambda s: int(s, 0),
        default=DEFAULT_MAX_BSS_UNINITIALIZED_TAIL,
        help=(
            "Fail fast if __BSS_UNINITIALIZED_tail in out/disk_tester.map exceeds this value "
            "(accepts decimal or hex, e.g. 0xFEFF)"
        ),
    )
    parser.add_argument(
        "--allow-screen-backbuffers",
        action="store_true",
        help="Allow _ui_back_pixels/_ui_back_attrs in map file (default: fail if present)",
    )
    parser.add_argument(
        "--check-track-loop-status",
        action="store_true",
        help="Run a regression check that verifies the selected track-loop test shows an initial status page and then a populated status page",
    )
    parser.add_argument(
        "--track-loop-open-timeout",
        type=float,
        default=12.0,
        help="Timeout waiting for initial track loop status page",
    )
    parser.add_argument(
        "--track-loop-stop-timeout",
        type=float,
        default=12.0,
        help="Timeout waiting for stopped track loop status page",
    )
    parser.add_argument(
        "--check-rpm-loop-status",
        action="store_true",
        help="Run a regression check that verifies selected RPM loop stays visible and reaches a populated status/sample frame",
    )
    parser.add_argument(
        "--rpm-loop-open-timeout",
        type=float,
        default=12.0,
        help="Timeout waiting for selected RPM loop status screen",
    )
    parser.add_argument(
        "--rpm-loop-sample-timeout",
        type=float,
        default=12.0,
        help="Timeout waiting for a numeric RPM sample on selected RPM loop",
    )
    parser.add_argument(
        "--rpm-loop-linger-timeout",
        type=float,
        default=1.0,
        help="Minimum time selected RPM loop must stay on-screen before exit",
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
        build_project(repo_root, args.debug_mode, args.compact_ui, args.headless)

    fail_fast_memory_regression(
        repo_root,
        args.max_bss_uninitialized_tail,
        fail_on_backbuffer_symbols=not args.allow_screen_backbuffers,
    )
    fail_fast_ui_source_regressions(repo_root)

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
                markerless_mode=(args.compact_ui == "on"),
            )
        except TimeoutError:
            if not args.menu_only:
                raise
            # In compact-font human mode OCR may fail to match menu markers.
            menu_text = client.ocr()
        write_ocr_artifact(artifacts_dir, "menu_loaded.txt", menu_text)
        log_ocr_block(args.log_ocr, "menu-loaded", menu_text)

        if args.compact_ui == "off":
            # Regression: OCR-only selected-row marker should move with V/F.
            initial_idx = selected_menu_line_index(menu_text)
            if initial_idx != 0:
                raise RuntimeError(f"Expected initial selected row index 0, got {initial_idx}")

            client.command(f"send-keys-ascii {key_delay_ms} 86")  # 'V'
            menu_after_v = wait_for_ocr(client, MENU_MARKERS, args.menu_return_timeout, interval=ocr_poll_s)
            idx_after_v = selected_menu_line_index(menu_after_v)
            if idx_after_v != 1:
                raise RuntimeError(f"Expected selected row index 1 after V, got {idx_after_v}")

            client.command(f"send-keys-ascii {key_delay_ms} 70")  # 'F'
            menu_after_f = wait_for_ocr(client, MENU_MARKERS, args.menu_return_timeout, interval=ocr_poll_s)
            idx_after_f = selected_menu_line_index(menu_after_f)
            if idx_after_f != 0:
                raise RuntimeError(f"Expected selected row index 0 after F, got {idx_after_f}")

        if args.menu_only:
            print("Menu-only capture complete")
            return 0

        if args.check_track_loop_status:
            status_before_text, status_after_text = run_track_loop_status_regression_check(
                client,
                tap_path,
                key_delay_ms,
                args.track_loop_open_timeout,
                args.track_loop_stop_timeout,
                args.load_timeout,
                loader_wait_s,
                reset_wait_s,
                ocr_poll_s,
            )
            write_ocr_artifact(artifacts_dir, "selected_test_initial_status.txt", status_before_text)
            write_ocr_artifact(artifacts_dir, "selected_test_populated_status.txt", status_after_text)
            log_ocr_block(args.log_ocr, "selected-test-initial", status_before_text)
            log_ocr_block(args.log_ocr, "selected-test-populated", status_after_text)

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

        if args.check_rpm_loop_status:
            if dsk_path is None:
                raise RuntimeError("--check-rpm-loop-status requires a mounted DSK (omit --no-dsk)")
            rpm_initial_text, rpm_sampled_text = run_rpm_loop_status_regression_check(
                client,
                key_delay_ms,
                args.rpm_loop_open_timeout,
                args.rpm_loop_sample_timeout,
                args.rpm_loop_linger_timeout,
                args.menu_return_timeout,
                ocr_poll_s,
            )
            write_ocr_artifact(artifacts_dir, "selected_rpm_initial_status.txt", rpm_initial_text)
            write_ocr_artifact(artifacts_dir, "selected_rpm_sampled_status.txt", rpm_sampled_text)
            log_ocr_block(args.log_ocr, "selected-rpm-initial", rpm_initial_text)
            log_ocr_block(args.log_ocr, "selected-rpm-sampled", rpm_sampled_text)

        # Full run-all via menu hotkey 'A'.  Rather than racing the transient
        # results screen (which can disappear before the first OCR poll), we
        # wait for the menu to return while also recording intermediate report
        # card frames. This catches pacing regressions where cards disappear
        # too quickly to be observed reliably.
        client.command(f"send-keys-ascii {key_delay_ms} 65")
        snapshots: list[str] = []

        menu_after_runall, runall_report_frames = wait_for_menu_after_runall_with_progress(
            client,
            args.run_timeout,
            ocr_poll_s,
            min_report_frames=3,
            key_delay_ms=key_delay_ms,
        )
        snapshots.append(snapshot_state(client, "after-run-all"))
        if artifacts_dir is not None:
            for idx, frame in enumerate(runall_report_frames, start=1):
                (artifacts_dir / f"runall_report_frame_{idx:02d}.txt").write_text(frame, encoding="utf-8")

        cleaned_after_runall = clean_response(menu_after_runall)
        status_lines = [l for l in cleaned_after_runall.splitlines() if "STATUS:" in l]
        status_summary = status_lines[0] if status_lines else "STATUS: unknown"
        if "5/5" not in status_summary:
            raise RuntimeError(
                f"Run-all did not complete all tests: {status_summary}\n"
                f"Menu OCR:\n{cleaned_after_runall}"
            )

        client.command(f"send-keys-ascii {key_delay_ms} 82")  # 'R' – open report card
        results_text = wait_for_ocr(
            client,
            ("TEST REPORT CARD", "OVERALL ["),
            args.menu_return_timeout * 2,
            interval=ocr_poll_s,
        )
        snapshots.append(snapshot_state(client, "report-card"))
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
        print(f"Memory guard: __BSS_UNINITIALIZED_tail <= ${args.max_bss_uninitialized_tail:04X}")
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
