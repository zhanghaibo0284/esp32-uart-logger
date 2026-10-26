# Build (host gcc) and run the app unit tests.
# Usage: python tests/unit/run.py
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

CASES = [
    ("test_modbus.exe", ["test_modbus.c", "app_modbus.c"]),
    ("test_codec.exe", ["test_codec.c", "app_modbus_codec.c", "app_parse.c", "app_modbus.c"]),
]

fails = 0
for exe_name, sources in CASES:
    exe = os.path.join(HERE, exe_name)
    cmd = ["gcc", "-std=c11", "-Wall", "-Wextra", "-I", os.path.join(ROOT, "main")]
    cmd += [os.path.join(HERE, sources[0])]
    cmd += [os.path.join(ROOT, "main", s) for s in sources[1:]]
    cmd += ["-o", exe]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        fails += 1
        continue
    if subprocess.run([exe]).returncode != 0:
        fails += 1

sys.exit(1 if fails else 0)
