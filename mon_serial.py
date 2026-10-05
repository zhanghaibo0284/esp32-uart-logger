import serial, time, sys
ser = serial.Serial("COM3", 115200, timeout=0.5)
end = time.time()+25
while time.time() < end:
    chunk = ser.read(2048)
    if chunk:
        sys.stdout.buffer.write(chunk); sys.stdout.buffer.flush()
ser.close()