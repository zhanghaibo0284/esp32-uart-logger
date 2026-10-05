import serial, time, socket, sys

ser = serial.Serial("COM3", 115200, timeout=0.2)
time.sleep(0.2)
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setRTS(False)

def probe(port):
    s = socket.socket(); s.settimeout(1.5)
    try:
        s.connect(("192.168.4.1", port)); return "OPEN"
    except Exception as e:
        return type(e).__name__
    finally:
        s.close()

start = time.time()
while time.time()-start < 20:
    t = time.time()-start
    sys.stdout.write("%.1fs  :80=%s :53=%s\n" % (t, probe(80), probe(53)))
    sys.stdout.flush()
    time.sleep(1.0)
ser.close()
