"""
Requests the 5s recording from the ESP32 voice-recorder firmware and saves
it as a standard .wav file, playable in any browser (drag it into a tab,
or open it with an <audio> element) or media player.

Protocol: writes 'd' to the serial port to trigger a dump, then reads
lines until it sees "---AUDIO-START---", collects base64 text lines
until "---AUDIO-END---", decodes them, and writes a WAV file.

Docs used:
- pyserial API: https://pyserial.readthedocs.io/en/latest/pyserial_api.html
- wave module (writes the WAV header for you):
  https://docs.python.org/3/library/wave.html

Install:
    pip install pyserial

Usage:
    python serial_wav_dump.py --port COM19 --baud 115200
"""

import argparse
import base64
import time

import serial

SAMPLE_RATE = 8000  # must match SAMPLE_RATE in main.c
SAMPLE_WIDTH_BYTES = 2  # int16_t samples
CHANNELS = 1

START_MARKER = "---AUDIO-START---"
END_MARKER = "---AUDIO-END---"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="Serial port, e.g. COM19 or /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200)
    return p.parse_args()


def request_and_save(ser: serial.Serial, out_path: str) -> bool:
    # Serial.write(): https://pyserial.readthedocs.io/en/latest/pyserial_api.html#serial.Serial.write
    ser.write(b"d")

    b64_lines = []
    collecting = False
    deadline = time.time() + 15  # generous timeout for the ~107 KB of base64 text

    while time.time() < deadline:
        raw_line = ser.readline()
        if not raw_line:
            continue
        line = raw_line.decode(errors="ignore").strip()

        if line == START_MARKER:
            collecting = True
            b64_lines = []
            continue
        if line == END_MARKER:
            break
        if collecting and line:
            b64_lines.append(line)

    if not b64_lines:
        print("No audio received (is there a recording on the board yet?)")
        return False

    # wave.open(): https://docs.python.org/3/library/wave.html#wave.open
    import wave

    pcm_bytes = base64.b64decode("".join(b64_lines))
    with wave.open(out_path, "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH_BYTES)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm_bytes)

    print(f"Saved {len(pcm_bytes)} bytes -> {out_path}")
    return True


def main():
    args = parse_args()
    ser = serial.Serial(args.port, args.baud, timeout=1)
    time.sleep(2)  # let the board's USB-serial connection settle
    ser.reset_input_buffer()

    print("Connected. Press Enter to fetch the current recording (Ctrl+C to quit).")
    take = 1
    try:
        while True:
            input()
            out_path = f"recording_{take}.wav"
            if request_and_save(ser, out_path):
                take += 1
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()