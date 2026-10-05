import serial, time, sys
ser = serial.Serial("COM3", 115200, timeout=0.5)
time.sleep(0.3); ser.read(4096)
ser.write(b"time 2026-10-02 12:29:01\n")
time.sleep(0.7)
sys.stdout.buffer.write(ser.read(4096))
ser.close()
