# -*- coding: utf-8 -*-
"""Live-traffic regression with the external Modbus master (e.g. ModScan on
COM18).

Flow (keeps internet up except for one ~15s window):
  1. sample status points for 45s over the COM3 USB console (no WiFi needed)
  2. one AP quick-fetch: connect UART-LOG, download file list + active log,
     reconnect home SSID automatically (ap_fetch.ps1)
  3. run the full verification report locally

Usage: python tests/stress/live_regression.py [home_ssid]
"""
import json
import os
import subprocess
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device_link as dl
from verify import regression_report

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "out")


def ap_fetch(entries, home_ssid):
    os.makedirs(WORK, exist_ok=True)
    manifest = os.path.join(WORK, "fetch.json")
    with open(manifest, "w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False)
    ps = os.path.join(HERE, "ap_fetch.ps1")
    r = subprocess.run(
        ["powershell", "-ExecutionPolicy", "Bypass", "-File", ps,
         "-Manifest", manifest, "-HomeSsid", home_ssid])
    return r.returncode == 0


def main():
    home_ssid = sys.argv[1] if len(sys.argv) > 1 else "qdmetro"
    sp = dl.open_console()
    s0 = dl.status(sp)
    print("start:", s0)
    if not s0 or not s0["open"]:
        print("UART2 not open, abort")
        return 1
    points = dl.sample_window(sp, 45, interval=2.0)
    sp.close()
    if len(points) < 3:
        print("not enough samples")
        return 1

    active = points[-1]["file"]
    listing_out = os.path.join(WORK, "uart2_listing.json")
    log_out = os.path.join(WORK, "live_" + active)
    list_url = dl.BASE + "/api/files?path=" + urllib.parse.quote("/sdcard/UART2")
    log_url = dl.BASE + "/dl?path=" + urllib.parse.quote("/sdcard/UART2/" + active)
    print("AP quick-fetch (internet briefly down ~15s) ...")
    ok = ap_fetch(
        [{"url": list_url, "out": listing_out},
         {"url": log_url, "out": log_out}],
        home_ssid)
    if not ok:
        print("AP fetch failed")
        return 1
    raw = open(log_out, "rb").read()
    passed, details = regression_report(raw, points, label="实时 Modbus 流量")

    # If window crossed a segment boundary, fetch the previous file too and
    # prepend it, then re-verify.
    first_dev = points[0]["file"]
    if first_dev != active:
        listing = json.load(open(listing_out, encoding="utf-8"))
        names = sorted(f["name"] for f in listing["items"] if not f["dir"])
        if active in names:
            prev = names[names.index(active) - 1]
            prev_out = os.path.join(WORK, "live_" + prev)
            prev_url = dl.BASE + "/dl?path=" + urllib.parse.quote(
                "/sdcard/UART2/" + prev)
            print("boundary crossed; fetching previous segment ...")
            ap_fetch([{"url": prev_url, "out": prev_out}], home_ssid)
            raw = open(prev_out, "rb").read() + raw
            passed, details = regression_report(raw, points, label="实时 Modbus 流量(跨段)")
    return 0 if passed else 2


if __name__ == "__main__":
    sys.exit(main())
