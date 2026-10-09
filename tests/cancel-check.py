"""Manual check: cancelling a running download kills yt-dlp and reports 'cancelled'.

Run:  python3 tests/cancel-check.py
Kept out of ctest because it needs the network and a slow transfer.
"""
import json
import os
import queue
import shutil
import struct
import subprocess
import threading
import time

HOST = os.path.expanduser("~/.cache/dwn-build/dwn-host")
URL = "https://download.samplelib.com/mp4/sample-30s.mp4"
OUT = "/tmp/dwn-cancel-check"


def send(proc, obj):
    payload = json.dumps(obj).encode()
    proc.stdin.write(struct.pack("<I", len(payload)) + payload)
    proc.stdin.flush()


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    proc = subprocess.Popen([HOST], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    messages = queue.Queue()

    def reader():
        while True:
            head = proc.stdout.read(4)
            if len(head) < 4:
                break
            size = struct.unpack("<I", head)[0]
            messages.put(json.loads(proc.stdout.read(size)))

    threading.Thread(target=reader, daemon=True).start()

    send(proc, {"id": "1", "cmd": "download",
                "params": {"url": URL, "format_id": "best", "out_dir": OUT}})
    time.sleep(2.5)  # let the transfer get going
    send(proc, {"id": "2", "cmd": "cancel", "params": {"download_id": "0"}})

    statuses = []
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            message = messages.get(timeout=1)
        except queue.Empty:
            continue
        if message.get("status"):
            statuses.append(message["status"])

    proc.kill()
    print("statuses after cancel:", statuses)
    ok = statuses and statuses[-1] == "cancelled"
    print("PASS" if ok else "FAIL", "- expected the last status to be 'cancelled'")

    alive = subprocess.run(["pgrep", "-c", "-x", "yt-dlp"],
                           capture_output=True).stdout.decode().strip()
    print(f"yt-dlp processes still running: {alive or 0} (expected 0)")


if __name__ == "__main__":
    main()