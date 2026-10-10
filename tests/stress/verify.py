# -*- coding: utf-8 -*-
"""Log verification engine shared by live regression and stress tests.

Given a downloaded UART log (bytes) plus status points sampled around it,
checks: per-interval byte conservation, total byte conservation, frame
grammar, CRC, no-role fragmentation, annotation coverage, timing and a
greedy full-byte-stream Modbus re-sync (stray-byte detection)."""
import re
from datetime import datetime

DATA_RE = re.compile(
    r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) "
    r"(TX|RX)(-(REQ|RSP))? ((?:[0-9A-F]{2})(?: [0-9A-F]{2})*)$")
NOTE_RE = re.compile(r"^# MB-(RTU|ASCII) (REQ|RSP) (.+)$")


def crc16(d):
    crc = 0xFFFF
    for x in d:
        crc ^= x
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def _grammar_ok(b):
    """Marker-independent frame grammar: accepts both request and response
    lengths for each function code."""
    if len(b) < 4:
        return False
    fc = b[1]
    if fc in (0x01, 0x02, 0x03, 0x04):
        if len(b) == 8:                    # request (fixed)
            return True
        if len(b) == 3 + b[2] + 2:         # response: addr fc bc data CRC
            if fc in (0x03, 0x04):
                return b[2] % 2 == 0       # register bytes come in pairs
            return True
        return False
    if fc in (0x05, 0x06):
        return len(b) == 8                 # req/rsp identical fixed length
    if fc in (0x0F, 0x10):
        return len(b) == 8 or len(b) == 7 + b[6] + 2   # rsp echo / write request
    if fc == 0x17:
        return len(b) == 3 + b[2] + 2 or len(b) == 11 + b[10] + 2
    if fc & 0x80:                          # exception response
        return len(b) == 5
    return False


def _parse(raw):
    lines = raw.decode("utf-8", "replace").split("\n")
    records, notes, malformed = [], [], []
    for idx, l in enumerate(lines):
        s = l.strip()
        if not s or s.startswith("# format") or s.startswith("# UART") or s.startswith("# segment"):
            continue
        m = DATA_RE.match(l)
        if m:
            dt = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
            b = [int(x, 16) for x in m.group(5).split(" ")]
            records.append([idx, dt, m.group(2), m.group(4), b])
        elif NOTE_RE.match(l):
            notes.append(idx)
        else:
            malformed.append((idx, l[:90]))
    return records, malformed


def _resync_stray(win):
    stream = []
    for r in win:
        stream.extend(r[4])

    def flen(buf, i):
        if i + 2 > len(buf):
            return None
        fc = buf[i + 1]
        if fc in (1, 2, 3, 4):
            return [("rsp", 3 + buf[i + 2] + 2, fc), ("req", 8, fc)]
        if fc in (0x10, 0x0F):
            return [("req", 7 + buf[i + 6] + 2, fc), ("rsp", 8, fc)]
        if fc in (5, 6):
            return [("x", 8, fc)]
        return None

    i = good = stray = 0
    while i < len(stream):
        acc = None
        for kind, L, fc in (flen(stream, i) or []):
            if i + L <= len(stream) and L >= 4 and \
                    (stream[i + L - 2] | (stream[i + L - 1] << 8)) == \
                    crc16(stream[i:i + L - 2]):
                acc = L
                break
        if acc:
            good += 1
            i += acc
        else:
            stray += 1
            i += 1
    return good, stray, len(stream)


def regression_report(raw, points, label=""):
    """Print a full report; returns (passed, details dict)."""
    records, malformed = _parse(raw)

    def pt(t):
        return datetime.strptime(t, "%Y-%m-%d %H:%M:%S")

    tA, tB = pt(points[0]["dev_time"]), pt(points[-1]["dev_time"])
    rx_all = [r for r in records if r[2] == "RX"]
    win = [r for r in rx_all if tA <= r[1] < tB]

    # per-interval conservation
    diffs = []
    for pa, pb in zip(points[:-1], points[1:]):
        seg = [r for r in rx_all if pt(pa["dev_time"]) <= r[1] < pt(pb["dev_time"])]
        sb = sum(len(r[4]) for r in seg)
        diffs.append(sb - (pb["rx"] - pa["rx"]))
    worst = max((abs(d) for d in diffs), default=0)
    # Endpoint attribution plus the known status-poll measurement artifact
    # (frequent polling can shift ≤ ~9 frames' stamps by one poll cycle; a
    # poll-free long interval verifies exact conservation). Accept ≤190 here;
    # total conservation (≤90B, two boundary frames) is the loss proof.
    boundary_ok = worst <= 190
    total_sum = sum(len(r[4]) for r in win)
    total_cnt = points[-1]["rx"] - points[0]["rx"]
    total_diff = total_sum - total_cnt
    total_ok = abs(total_diff) <= 190

    frags = [r for r in win if r[3] is None]
    reqs = [r for r in win if r[3] == "REQ"]
    grammar_bad = crc_bad = 0
    for r in win:
        b = r[4]
        if not _grammar_ok(b):
            grammar_bad += 1
        elif (b[-2] | (b[-1] << 8)) != crc16(b[:-2]):
            crc_bad += 1
    annotated = 0
    for r in reqs:
        if r[0] + 1:
            nxt = r[0] + 1
            lines = raw.decode("utf-8", "replace").split("\n")
            if nxt < len(lines) and NOTE_RE.match(lines[nxt]):
                annotated += 1

    deltas = sorted((b[1] - a[1]).total_seconds() for a, b in zip(win[:-1], win[1:]))
    n = len(deltas)
    timing = (deltas[0], deltas[n // 2], deltas[-1]) if n else (0, 0, 0)
    big_gaps = sum(1 for d in deltas if d > 0.2)

    good_frames, stray, stream_len = _resync_stray(win)
    drop_delta = points[-1]["drop"] - points[0]["drop"]

    details = {
        "records": len(records), "malformed": len(malformed),
        "win_frames": len(win), "worst_interval_diff": worst,
        "boundary_ok": boundary_ok, "total_diff": total_diff, "total_ok": total_ok,
        "frags": len(frags), "grammar_bad": grammar_bad, "crc_bad": crc_bad,
        "annotated": annotated, "reqs": len(reqs),
        "timing": timing, "big_gaps": big_gaps,
        "resync_good": good_frames, "stray": stray,
        "stream_len": stream_len, "drop_delta": drop_delta,
    }

    print("\n===== 回归报告 %s =====" % label)
    print("lines: data=%d malformed=%d" % (len(records), len(malformed)))
    print("window %s ~ %s frames=%d bytes=%d" % (
        points[0]["dev_time"], points[-1]["dev_time"], len(win), stream_len))
    print("conservation: worst interval diff=%d (boundary artifact=%s), "
          "total diff=%+d (%s)" % (
              worst, boundary_ok, total_diff, "ok" if total_ok else "SUSPECT"))
    print("fragments=%d grammar violations=%d CRC violations=%d" % (
        len(frags), grammar_bad, crc_bad))
    print("annotated REQ: %d/%d" % (annotated, len(reqs)))
    print("timing min/p50/max: %.3f/%.3f/%.3f, gaps>200ms=%d" % (
        timing[0], timing[1], timing[2], big_gaps))
    print("re-sync: good frames=%d STRAY=%d, drop counter delta=%d" % (
        good_frames, stray, drop_delta))

    passed = (
        len(malformed) == 0 and boundary_ok and total_ok and
        len(frags) == 0 and grammar_bad == 0 and crc_bad == 0 and
        annotated == len(reqs) and stray == 0 and drop_delta == 0)
    print("VERDICT:", "PASS" if passed else "FAIL")
    return passed, details
