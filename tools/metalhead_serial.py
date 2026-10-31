"""Talking to a metalhead device over its USB serial port (macOS): find the port, send console
lines. A copy of the helpers in beagle's rpi5/tools/upload_image.py, so the SDK stands alone.

The web UI must not have the port open at the same time.
"""
import glob
import os
import re
import subprocess
import sys
import termios
import time

LINE = 768          # base64 characters per line, as the web UI's unit upload


def metalhead_port(wait=20):
    """The USB serial port of the device called Metalhead: macOS names it after the
    device's serial number (other boards' gadgets show up as /dev/cu.usbmodem* too).
    Waits a while for it, e.g. right after a reboot."""
    end = time.time() + wait
    while True:
        port = find_port()
        if port or time.time() > end:
            break
        time.sleep(0.5)
    if not port:
        sys.exit(f'no Metalhead USB serial port (USB ports: {glob.glob("/dev/cu.usbmodem*")})')
    return port


def find_port():
    tree = subprocess.run(['ioreg', '-p', 'IOUSB', '-l', '-w0'], capture_output=True, text=True).stdout
    serials = []
    for device in tree.split('+-o ')[1:]:       # one block per USB device
        serial = re.search(r'"USB Serial Number" = "([^"]+)"', device)
        if '"USB Product Name" = "Metalhead"' in device and serial:
            serials.append(serial.group(1))
    for serial in serials:
        ports = glob.glob(f'/dev/cu.usbmodem{serial}*')
        if ports:
            return ports[0]
    return None


def open_port():
    fd = os.open(metalhead_port(), os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0      # raw
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def command(fd, line, timeout=5.0):
    """Send one console line; return what came back up to the next prompt."""
    os.write(fd, line.encode() + b'\r')
    out = b''
    end = time.time() + timeout
    while time.time() < end:
        out += os.read(fd, 65536)
        # the console echoes the line, prints its answer, then a "> " prompt
        if out.rstrip().endswith(b'>'):
            break
    return out.decode(errors='replace')


def answer(text, prefix):
    return next((l.strip() for l in text.splitlines() if l.strip().startswith(prefix)), None)
