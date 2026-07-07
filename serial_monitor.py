import serial, time, sys

port = 'COM15'
baud = 115200
outfile = r'G:\Robot_v2\DCU\ESP32_COMM\build\monitor_log.txt'

print('Monitoring %s at %d baud...' % (port, baud), flush=True)

try:
    s = serial.Serial(port, baud, timeout=1)
    with open(outfile, 'w') as f:
        while True:
            data = s.readline()
            if data:
                line = data.decode('utf-8', errors='replace').rstrip()
                ts = time.strftime('%H:%M:%S')
                msg = '[%s] %s' % (ts, line)
                print(msg, flush=True)
                f.write(msg + '\n')
                f.flush()
except Exception as e:
    print('Error: %s' % e, flush=True)