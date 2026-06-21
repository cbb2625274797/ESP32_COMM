#!/usr/bin/env python3
"""
serial_monitor.py - ESP32 串口日志采集工具

用法: python serial_monitor.py
依赖: pip install pyserial
功能: 监听 COM15 串口 (115200 波特率)，将输出记录到 serial_log.txt，
      最多运行 5 分钟。
"""
import serial, sys, time
ser = serial.Serial('COM15', 115200, timeout=1)  # COM 口和波特率根据实际修改
outfile = open('G:\\Robot_v2\\DCU\\ESP32_COMM\\serial_log.txt', 'w', encoding='utf-8')  # 输出文件路径
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
