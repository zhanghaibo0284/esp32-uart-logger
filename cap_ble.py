import serial, time, threading, urllib.request, sys

ser = serial.Serial("COM3", 115200, timeout=0.3)
time.sleep(0.2)
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setRTS(False)

start = time.time()
fired = [False]

def fire_ble():
    # wait until AP likely up
    time.sleep(6.0)
    try:
        url = "http://192.168.4.1/api/bridge_ble?index=1&on=1&name=UART-LOG-BLE"
        r = urllib.request.urlopen(url, timeout=5)
        sys.stderr.write("BLE http: " + r.read().decode(errors="replace") + "\n")
    except Exception as e:
        sys.stderr.write("BLE http err: " + str(e) + "\n")
    fired[0] = True

t = threading.Thread(target=fire_ble)
t.start()

while time.time() - start < 16:
    chunk = ser.read(2048)
    if chunk:
        sys.stdout.buffer.write(chunk); sys.stdout.buffer.flush()

ser.close()
