"""
set_baud.py — nastavi LD2410C baud rate na 115200
Zahteva: pyserial
Uporaba: "C:\Program Files\Python313\python.exe" set_baud.py
"""

import sys
import time

PORT = "COM9"

try:
    import serial
except ImportError:
    print("ERROR: pyserial ni nameščen.")
    print('Poženi: "C:\\Program Files\\Python313\\python.exe" -m pip install pyserial')
    sys.exit(1)

CMD_ENABLE_CONFIG = bytes([
    0xFD, 0xFC, 0xFB, 0xFA,
    0x04, 0x00,
    0xFF, 0x00,
    0x01, 0x00,
    0x04, 0x03, 0x02, 0x01
])

CMD_SET_BAUD_115200 = bytes([
    0xFD, 0xFC, 0xFB, 0xFA,
    0x04, 0x00,
    0xA1, 0x00,
    0x05, 0x00,
    0x04, 0x03, 0x02, 0x01
])

def send_cmd(ser, cmd, desc):
    ser.reset_input_buffer()
    ser.write(cmd)
    ser.flush()
    time.sleep(0.15)
    resp = ser.read(ser.in_waiting or 1)
    ok = resp and len(resp) >= 6 and resp[4:6] == bytes([0x00, 0x00])
    status = "OK" if ok else "?"
    print(f"  [{status}] {desc}")
    print(f"        TX: {cmd.hex(' ').upper()}")
    print(f"        RX: {resp.hex(' ').upper() if resp else '(ni odgovora)'}")
    return resp

def set_baud(port, radar_num):
    print(f"\n{'='*50}")
    print(f"  LD2410C #{radar_num} na {port} @ 256000 baud")
    print(f"{'='*50}")
    try:
        with serial.Serial(port, 256000, bytesize=8, parity='N',
                           stopbits=1, timeout=1) as ser:
            time.sleep(0.5)
            ser.reset_input_buffer()

            print("\nKorak 1: Vklop config nacina...")
            r1 = send_cmd(ser, CMD_ENABLE_CONFIG, "Enable config")

            if not r1:
                print("\n  NAPAKA: Ni odgovora. Preveri:")
                print("  - TX/RX zice (morda zamenjani?)")
                print("  - Napajanje 5V na LD2410C")
                print("  - COM port (trenutno COM9)")
                return False

            print("\nKorak 2: Nastavljam baud rate na 115200...")
            send_cmd(ser, CMD_SET_BAUD_115200, "Set baud 115200")
            print("\n  Radar se restarta...")
            time.sleep(1.0)

        print("\nKorak 3: Preverjam @ 115200...")
        with serial.Serial(port, 115200, bytesize=8, parity='N',
                           stopbits=1, timeout=2) as ser:
            time.sleep(1.0)
            resp = ser.read(20)
            if resp:
                print(f"  Data @ 115200: {resp.hex(' ').upper()}")
                if b'\xF4\xF3\xF2\xF1' in resp or b'\xFD\xFC\xFB\xFA' in resp:
                    print(f"\n  USPEH! LD2410C #{radar_num} je zdaj na 115200 baud.")
                else:
                    print(f"\n  Podatki prisli @ 115200 — verjetno uspeh.")
                return True
            else:
                print("  Ni podatkov @ 115200.")
                print("  Mozno da je bil radar ze na 115200 — v redu.")
                return True

    except serial.SerialException as e:
        print(f"\n  NAPAKA: {e}")
        print(f"  Zapri vse terminale ki uporabljajo COM9.")
        return False

if __name__ == "__main__":
    print("\nLD2410C Baud Rate Konfiguracija")
    print("================================")
    print(f"Port: {PORT}")
    print("Cilj: 256000 -> 115200 baud")
    print("\nPRIKLJUCITEV CP2102 -> LD2410C:")
    print("  CP2102 TX  ->  LD2410C RX")
    print("  CP2102 RX  ->  LD2410C TX")
    print("  CP2102 5V  ->  LD2410C VCC")
    print("  CP2102 GND ->  LD2410C GND")

    input("\nPriključi PRVI LD2410C in pritisni Enter...")
    r1 = set_baud(PORT, 1)

    if r1:
        input("\nPriključi DRUGI LD2410C in pritisni Enter...")
        set_baud(PORT, 2)

    print("\nKonec.")