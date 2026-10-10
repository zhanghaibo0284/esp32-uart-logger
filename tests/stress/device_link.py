# -*- coding: utf-8 -*-
"""Shared helpers for black-box tests over the USB Serial JTAG console (COM3)
and the recorder Web API (192.168.4.1)."""
import json
import re
import time
import urllib.parse
import urllib.request

import serial

CONSOLE = "COM3"
BASE = "http://192.168.4.1"

_RX = re.compile(r"PORT2 open=(\d) err=(\d) \S+ \S+ rx=(\d+) drop=(\d+) file=(\S+)")
_TM = re.compile(r"TIME trusted=\d (\S+ \S+)")


def open_console():
    sp = serial.Serial(CONSOLE, 115200, timeout=0.2)
    time.sleep(0.3)
    return sp


def status(sp):
    sp.reset_input_buffer()
    sp.write(b"status\n")
    time.sleep(0.6)
    d = sp.read(16384).decode("utf-8", "replace")
    m = _RX.search(d)
    t = _TM.search(d)
    if not m:
        return None
    return {
        "dev_time": t.group(1) if t else "?",
        "open": int(m.group(1)),
        "err": int(m.group(2)),
        "rx": int(m.group(3)),
        "drop": int(m.group(4)),
        "file": m.group(5),
    }


def http_json(suffix):
    with urllib.request.urlopen(BASE + suffix, timeout=15) as r:
        return json.load(r)


def download(path):
    url = BASE + "/dl?path=" + urllib.parse.quote(path)
    with urllib.request.urlopen(url, timeout=90) as r:
        return r.read()


def sample_window(sp, seconds, interval=2.0, show=True):
    """Sample status points for `seconds`; returns the point list."""
    points = []
    end = time.time() + seconds
    while time.time() < end:
        s = status(sp)
        if s:
            points.append(s)
            if show:
                print("  %s rx=%d drop=%d" % (s["dev_time"], s["rx"], s["drop"]))
        time.sleep(interval)
    return points
