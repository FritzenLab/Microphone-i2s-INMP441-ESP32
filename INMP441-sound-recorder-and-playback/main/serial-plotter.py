"""
Live line plot of an ESP-IDF ESP_LOGI numeric value, similar to the Arduino
IDE Serial Plotter.

Parses lines like:
    I (12345) inmp441: Nivel 7
(ESP-IDF's idf.py monitor/esptool also wraps this in ANSI color codes, which
are stripped below.)

Docs used:
- pyserial API: https://pyserial.readthedocs.io/en/latest/pyserial_api.html
- matplotlib animation: https://matplotlib.org/stable/api/animation_api.html

Install:
    pip install pyserial matplotlib

Usage:
    python serial_plotter.py --port COM19 --baud 115200
    python serial_plotter.py --port /dev/ttyUSB0
"""

import argparse
import re
from collections import deque

import serial
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

# Matches "... Nivel <number>" (also accepts "Nível" if your source encodes
# the accented ESP_LOGI tag) after stripping ANSI escape sequences.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
VALUE_RE = re.compile(r"N[ií]vel\s+(-?\d+(?:\.\d+)?)", re.IGNORECASE)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="Serial port, e.g. COM19 or /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--window", type=int, default=200, help="Number of points kept on screen")
    p.add_argument("--interval-ms", type=int, default=50, help="Plot refresh interval")
    return p.parse_args()


def main():
    args = parse_args()

    # Serial.Serial(): https://pyserial.readthedocs.io/en/latest/pyserial_api.html#serial.Serial
    # timeout=0 -> non-blocking read, so a quiet line doesn't stall the plot loop
    ser = serial.Serial(args.port, args.baud, timeout=0)

    xs = deque(maxlen=args.window)
    ys = deque(maxlen=args.window)
    sample_count = 0

    fig, ax = plt.subplots()
    (line,) = ax.plot([], [], lw=1.5)
    ax.set_ylim(-0.5, 10.5)          # same fixed range as your Arduino plotter screenshot
    ax.set_xlabel("sample")
    ax.set_ylabel("nivel")
    ax.set_title(f"{args.port} @ {args.baud} baud")
    ax.grid(True, alpha=0.3)

    def read_pending_lines():
        """Drain everything currently buffered on the serial port."""
        # readline() with timeout=0 returns immediately, empty bytes if nothing yet
        while True:
            raw = ser.readline()
            if not raw:
                break
            yield raw.decode(errors="ignore")

    def update(_frame):
        nonlocal sample_count
        updated = False
        for text in read_pending_lines():
            clean = ANSI_RE.sub("", text).strip()
            match = VALUE_RE.search(clean)
            if not match:
                continue  # ignore boot logs / unrelated ESP_LOGI lines
            value = float(match.group(1))
            xs.append(sample_count)
            ys.append(value)
            sample_count += 1
            updated = True

        if updated:
            line.set_data(xs, ys)
            ax.set_xlim(max(0, sample_count - args.window), max(args.window, sample_count))
        return (line,)

    # FuncAnimation: https://matplotlib.org/stable/api/_as_gen/matplotlib.animation.FuncAnimation.html
    _anim = FuncAnimation(fig, update, interval=args.interval_ms, blit=False, cache_frame_data=False)
    plt.tight_layout()
    plt.show()

    ser.close()


if __name__ == "__main__":
    main()