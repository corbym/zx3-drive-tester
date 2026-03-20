import os
import socket
import subprocess
import time

EMU = "/Applications/zesarux.app/Contents/MacOS/zesarux"
PORT = 10000
HOME_DIR = "/tmp/zesarux-smoketest-home-go"


def zrcp(cmd: str) -> str:
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
    sock.settimeout(2)
    sock.sendall((cmd + "\n").encode("ascii"))
    out = bytearray()
    while True:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out.extend(chunk)
        except socket.timeout:
            break
    sock.close()
    return out.decode("utf-8", errors="ignore")


def wait_port(timeout_s: float = 15.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.25)
            s.close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("ZRCP port did not come up")


os.makedirs(HOME_DIR, exist_ok=True)
proc = subprocess.Popen(
    [
        EMU,
        "--machine",
        "P340",
        "--emulatorspeed",
        "100",
        "--fastautoload",
        "--enable-remoteprotocol",
        "--remoteprotocol-port",
        str(PORT),
        "--noconfigfile",
        "--vo",
        "null",
        "--ao",
        "null",
    ],
    env={**os.environ, "HOME": HOME_DIR},
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)

try:
    wait_port()
    zrcp("close-all-menus")
    zrcp("hard-reset-cpu")
    time.sleep(0.2)
    zrcp("smartload /Users/mattcorby-eaglen/repos/zx3-disk-test/out/disk_tester.tap")

    for i in range(35):
        time.sleep(1)
        ocr = zrcp("get-ocr")
        if i in (0, 5, 10, 20, 30, 34):
            print(f"--- t={i}s ---")
            print(ocr[:800])
        if "ZX +3 DISK TESTER" in ocr:
            print(f"FOUND MENU at t={i}s")
            break
finally:
    try:
        zrcp("exit-emulator")
    except OSError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

