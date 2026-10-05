import serial, time
ser = serial.Serial("COM3", 115200, timeout=1)
time.sleep(0.3)
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setRTS(False)
end = time.time()+7
buf = b""
while time.time() < end:
    chunk = ser.read(2048)
    if chunk: buf += chunk
ser.close()
import sys
sys.stdout.buffer.write(buf)