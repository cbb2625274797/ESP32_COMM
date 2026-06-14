import serial, sys, time
ser = serial.Serial('COM15', 115200, timeout=1)
outfile = open('G:\\Robot_v2\\DCU\\ESP32_COMM\\serial_log.txt', 'w', encoding='utf-8')
start = time.time()
while time.time() - start < 300:  # 最多 5 分钟
    line = ser.readline()
    if line:
        try:
            text = line.decode('utf-8', errors='replace').rstrip()
            outfile.write(text + '\n')
            outfile.flush()
        except:
            pass
outfile.close()
ser.close()
