# -*- coding: utf-8 -*-
"""Scenario D: TCP passthrough passive verification.

Enables the TCP bridge on UART2, connects a passive TCP client (never sends,
so nothing is injected into the live Modbus bus), collects the uplink bytes
and compares them against the frames logged in the same window.
Usage: python tests/stress/tcp_passthrough_test.py [home_ssid]
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.parse
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device_link as dl
from verify import DATA_RE, regression_report

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "out")
TCP_PORT = 8083


# Ordered subsequence match: count how many bytes of `small` appear, in order,
# inside `big`.
def ordered_match(small, big):
    i = 0
    for b in big:
        if i < len(small) and small[i] == b:
            i += 1
    return i


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


def curl(url):
    r = subprocess.run(["curl.exe", "-sS", "--fail", "--max-time", "10", url],
                       capture_output=True, text=True)
    return r.stdout, r.returncode


def ap_fetch(entries, home):
    os.makedirs(WORK, exist_ok=True)
    manifest = os.path.join(WORK, "fetch3.json")
    json.dump(entries, open(manifest, "w", encoding="utf-8"))
    ps = os.path.join(HERE, "ap_fetch.ps1")
    subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", ps,
                    "-Manifest", manifest, "-HomeSsid", home])


def main():
    home = sys.argv[1] if len(sys.argv) > 1 else "qdmetro"
    time.sleep(6)
    sp = dl.open_console()
    pre = dl.status(sp)
    print("pre:", pre)
    target_file = pre["file"]

    wlan("UART-LOG")
    if not wait_link():
        return 1
    # Enable TCP bridge for port 2.
    out, rc = curl(dl.BASE + "/api/bridge_tcp?index=2&on=1&port=%d" % TCP_PORT)
    print("bridge enable rc=%d" % rc)
    time.sleep(1)

    # Passive TCP client.
    sock = socket.create_connection(("192.168.4.1", TCP_PORT), timeout=10)
    sock.settimeout(2)
    print("tcp connected, passively collecting 20s ...")
    tcp_bytes = bytearray()
    points = []
    end = time.time() + 20
    while time.time() < end:
        try:
            d = sock.recv(4096)
            if d:
                tcp_bytes.extend(d)
        except socket.timeout:
            pass
        s = dl.status(sp)
        if s:
            points.append(s)
        time.sleep(2)
    sock.close()

    # Disable bridge, reconnect home.
    curl(dl.BASE + "/api/bridge_tcp?index=2&on=0&port=%d" % TCP_PORT)
    sp.close()
    wlan(home)
    time.sleep(4)

    # Fetch log for the window.
    log_out = os.path.join(WORK, "tcp_" + target_file)
    log_url = dl.BASE + "/dl?path=" + urllib.parse.quote("/sdcard/UART2/" + target_file)
    ap_fetch([{"url": log_url, "out": log_out}], home)

    raw = open(log_out, "rb").read()

    def pt(t):
        return datetime.strptime(t, "%Y-%m-%d %H:%M:%S")

    t0, t1 = pt(points[0]["dev_time"]), pt(points[-1]["dev_time"])
    logged = bytearray()
    for l in raw.decode("utf-8", "replace").split("\n"):
        m = DATA_RE.match(l)
        if m and m.group(2) == "RX":
            t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
            if t0 <= t < t1:
                logged.extend(bytes(int(x, 16) for x in m.group(5).split(" ")))

    print("\nTCP received: %d bytes; logged in window: %d bytes" % (
        len(tcp_bytes), len(logged)))
    # Require that every logged byte appears, in order, in the TCP stream (uplink
    # fidelity). The TCP stream may additionally contain edge frames received
    # before/after the comparison window; allow up to 15% extra.
    cov = ordered_match(logged, tcp_bytes)
    edge_bytes = len(tcp_bytes) - cov
    print("logged bytes found in TCP stream: %d/%d; TCP edge bytes: %d" % (
        cov, len(logged), edge_bytes))
    passthrough_ok = (cov == len(logged) and
                      edge_bytes <= max(800, int(len(logged) * 0.15)))
    passed, _ = regression_report(raw, points, label="TCP 透传 (passive uplink)")
    print("PASSTHROUGH:", "PASS" if (passthrough_ok and passed) else "FAIL")
    return 0 if (passthrough_ok and passed) else 2


if __name__ == "__main__":
    sys.exit(main())
