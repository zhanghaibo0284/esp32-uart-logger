import serial, time, threading, urllib.request, sys

ser = serial.Serial("COM3", 115200, timeout=0.3)
time.sleep(0.2)
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setRTS(False)

start = time.time()

def fire_ble():
    time.sleep(6.0)
    try:
        url = "http://192.168.4.1/api/bridge_ble?index=1&on=1&name=UART-LOG-BLE"
        r = urllib.request.urlopen(url, timeout=3)
        sys.stderr.write("BLE http ok\n")
    except Exception as e:
        sys.stderr.write("BLE fired(err expected): " + str(e) + "\n")

threading.Thread(target=fire_ble).start()

while time.time() - start < 40:
    chunk = ser.read(2048)
    if chunk:
        sys.stdout.buffer.write(chunk); sys.stdout.buffer.flush()
ser.close()
