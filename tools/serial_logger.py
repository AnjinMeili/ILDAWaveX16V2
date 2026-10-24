#!/usr/bin/env python3
"""Log a serial port to a file with host timestamps; reconnect if the port goes away.

Works for both chips on the shared USB-C port: the ESP32-S3 console and the
RP2354 [RP] diagnostic log. The port is opened without touching DTR/RTS, so
attaching does not reset the ESP32-S3 (opening it with pyserial's defaults
does).

  python3 tools/serial_logger.py /dev/cu.usbmodem101 logs/esp32-serial.log

Stop it before flashing that chip; it reconnects on its own after a cable
swap or reset.
"""
import datetime
import os
import select
import sys
import termios
import time

port_path, log_path = sys.argv[1], sys.argv[2]


def stamp():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def open_port():
    fd = os.open(port_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~(termios.ICANON | termios.ECHO)
    attrs[2] |= termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


with open(log_path, "a", buffering=1) as log:
    log.write(f"{stamp()} === logger started on {port_path} ===\n")
    pending = b""
    while True:
        try:
            fd = open_port()
        except OSError:
            time.sleep(1)
            continue
        log.write(f"{stamp()} === port open ===\n")
        try:
            while True:
                ready, _, _ = select.select([fd], [], [], 1.0)
                if not ready:
                    if not os.path.exists(port_path):
                        raise OSError("port removed")
                    continue
                data = os.read(fd, 4096)
                if not data:
                    raise OSError("port closed")
                pending += data
                *lines, pending = pending.split(b"\n")
                for line in lines:
                    text = line.decode(errors="replace").rstrip("\r")
                    log.write(f"{stamp()} {text}\n")
        except OSError as exc:
            log.write(f"{stamp()} === port lost ({exc}), waiting to reconnect ===\n")
            try:
                os.close(fd)
            except OSError:
                pass
            time.sleep(1)
