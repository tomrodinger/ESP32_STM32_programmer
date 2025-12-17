import serial
import time
import sys

port = "/dev/cu.usbmodem2101"
baud = 115200

try:
    ser = serial.Serial(port, baud, timeout=1)
    print(f"Opened {port}")
    
    # Don't toggle DTR/RTS, just ensure they are set to run mode
    ser.dtr = False
    ser.rts = False
    
    print("Listening... (Press Reset on board if nothing happens)")
    
    # Send 'p' after 2 seconds
    sent_p = False
    start = time.time()
    while time.time() - start < 20:
        if not sent_p and time.time() - start > 2:
            print("Sending 'p'...")
            ser.write(b'r')
            sent_p = True
            
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            print(f"RX: {line}")
            
        if "SUCCESS! Target programmed." in line:
            print("SUCCESS CONFIRMED!")
            sys.exit(0)
        if "Failed!" in line:
            print("FAILURE DETECTED!")
            
except Exception as e:
    print(f"Error: {e}")
