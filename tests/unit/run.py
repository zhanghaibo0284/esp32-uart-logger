# Build (host gcc) and run the app_modbus unit tests.
# Usage: python tests/unit/run.py
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
EXE = os.path.join(HERE, "test_modbus.exe")

cmd = ["gcc", "-std=c11", "-Wall", "-Wextra",
       "-I", os.path.join(ROOT, "main"),
       os.path.join(HERE, "test_modbus.c"),
       os.path.join(ROOT, "main", "app_modbus.c"),
       "-o", EXE]
r = subprocess.run(cmd)
if r.returncode != 0:
    sys.exit(r.returncode)
sys.exit(subprocess.run([EXE]).returncode)
