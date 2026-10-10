# -*- coding: utf-8 -*-
"""Scenario C: heavy Web download contending with live logging.

A large file download repeatedly takes the recursive fs lock per 2KB chunk;
the writer task can only write between chunks while RX keeps running. Verifies
zero loss and that all parked data flushes after the download ends.
Usage: python tests/stress/contention_test.py [home_ssid]
"""
import json
import os
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device_link as dl
from verify import regression_report

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "out")


def wlan(ssid):
    subprocess.run(["netsh", "wlan", "connect", "name=" + ssid, "ssid=" + ssid],
                   capture_output=True)


def wait_link():
    for _ in range(30):
        time.sleep(0.8)
        r = subprocess.run(["ping", "-n", "1", "-w", "600", "192.168.4.1"],
                           capture_output=True, text=True)
        if "TTL=64" in r.stdout:
            return True
    return False


def ap_fetch(entries, home):
    os.makedirs(WORK, exist_ok=True)
    manifest = os.path.join(WORK, "fetch2.json")
    json.dump(entries, open(manifest, "w", encoding="utf-8"))
    ps = os.path.join(HERE, "ap_fetch.ps1")
    subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", ps,
                    "-Manifest", manifest, "-HomeSsid", home])


def main():
    home = sys.argv[1] if len(sys.argv) > 1 else "qdmetro"
    time.sleep(8)
    sp = dl.open_console()
    s0 = dl.status(sp)
    print("pre:", s0)
    target_file = s0["file"]

    # Connect AP; start the contention download in the background.
    wlan("UART-LOG")
    if not wait_link():
        print("AP link failed")
        return 1
    dl_size = 800000
    dl_path = os.path.join(WORK, "contention_download.bin")
    url = dl.BASE + "/dl?path=" + urllib.parse.quote("/sdcard/UART2/" + target_file)
    print("starting contention download:", url)
    dl_proc = subprocess.Popen(["curl.exe", "-sS", "--fail", "--max-time", "120",
                                "--output", dl_path, url])

    # Sample live status over COM3 while download churns the fs lock.
    points = []
    end = time.time() + 30
    while time.time() < end:
        s = dl.status(sp)
        if s:
            points.append(s)
            print("  %s rx=%d drop=%d" % (s["dev_time"], s["rx"], s["drop"]))
        time.sleep(2)
    rc = dl_proc.wait()
    print("download rc=%d, bytes=%s" % (rc, os.path.getsize(dl_path) if os.path.exists(dl_path) else 0))
    sp.close()

    # Reconnect home, then fetch the post-contention log.
    wlan(home)
    time.sleep(4)
    log_out = os.path.join(WORK, "contention_" + target_file)
    log_url = dl.BASE + "/dl?path=" + urllib.parse.quote("/sdcard/UART2/" + target_file)
    ap_fetch([{"url": log_url, "out": log_out}], home)

    raw = open(log_out, "rb").read()
    passed, _ = regression_report(raw, points, label="下载争用 (Web download vs live log)")
    return 0 if passed else 2


if __name__ == "__main__":
    sys.exit(main())
