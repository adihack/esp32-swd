# SPDX-License-Identifier: MIT
"""Finding the board and opening its link: the USB-Serial-JTAG port directly, or the local socket of a running
`esw serve` daemon that owns the port (then several programs share the board, and the TTY bridge runs alongside).

Linux only for the daemon parts (PTYs, sysfs, Unix sockets); the direct serial link works anywhere pyserial does.
"""
import glob
import os
import socket

import serial

USJ_IDS = ("303a", "1001")   # ESP32-S3 USB-Serial-JTAG (also what the ROM downloader enumerates as)


def run_dir():
    """Where daemons put their sockets and TTY links: $XDG_RUNTIME_DIR/esp-swd (else /tmp/esp-swd-<uid>)."""
    base = os.environ.get("XDG_RUNTIME_DIR")
    return os.path.join(base, "esp-swd") if base else "/tmp/esp-swd-%d" % os.getuid()


def _usb_dir(tty):
    """sysfs directory of the USB device behind /dev/ttyACMx, or None."""
    d = os.path.realpath("/sys/class/tty/%s/device" % os.path.basename(tty))
    while d and d != "/":
        if os.path.exists(os.path.join(d, "idVendor")):
            return d
        d = os.path.dirname(d)
    return None


def _attr(d, name):
    try:
        with open(os.path.join(d, name)) as f:
            return f.read().strip()
    except OSError:
        return None


def usb_serial(port):
    """Board id for a port path (/dev/ttyACM0, /dev/serial/by-id/...): the USB serial number (the chip's MAC)
    without colons, e.g. F0F5BD57EF34. None if it is not a USB device."""
    d = _usb_dir(os.path.realpath(port))
    s = _attr(d, "serial") if d else None
    return s.replace(":", "").upper() if s else None


def find_boards():
    """{board id: /dev/ttyACMx} for every attached USB-Serial-JTAG port."""
    out = {}
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        d = _usb_dir(tty)
        if d and (_attr(d, "idVendor"), _attr(d, "idProduct")) == USJ_IDS:
            s = _attr(d, "serial")
            if s:
                out[s.replace(":", "").upper()] = "/dev/" + os.path.basename(tty)
    return out


def board_dir(board):
    return os.path.join(run_dir(), board)


def daemon_socket(board):
    return os.path.join(board_dir(board), "ctl.sock")


def running_daemons():
    """Board ids with a daemon socket that accepts connections."""
    out = []
    for p in sorted(glob.glob(os.path.join(run_dir(), "*", "ctl.sock"))):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(p)
            out.append(os.path.basename(os.path.dirname(p)))
        except OSError:
            pass
        finally:
            s.close()
    return out


def resolve(port=None):
    """-> ("unix", socket path) or ("serial", device). port: None (the only board), a device path, a board id, or
    "unix:<path>". A daemon serving the board is preferred: the port itself is locked by it."""
    if port and port.startswith("unix:"):
        return "unix", port[5:]
    if port is None:
        boards = set(find_boards()) | set(running_daemons())
        if not boards:
            raise FileNotFoundError("no esp-swd board found (USB 303a:1001) and no daemon running")
        if len(boards) > 1:
            raise FileNotFoundError("several boards (%s): pick one with --port" % ", ".join(sorted(boards)))
        board = boards.pop()
    elif os.path.exists(port):
        board = usb_serial(port)
        if not board:
            return "serial", port
    else:
        board = port.replace(":", "").upper()
    sock = daemon_socket(board)
    if os.path.exists(sock):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(sock)
            return "unix", sock
        except OSError:
            pass   # stale socket of a dead daemon: go direct
        finally:
            s.close()
    dev = find_boards().get(board) if not (port and os.path.exists(port)) else port
    if not dev:
        raise FileNotFoundError("board %s not found" % board)
    return "serial", dev


class SocketPort:
    """The subset of pyserial's Serial that Esw uses, over the daemon's Unix socket (same framed byte stream)."""

    def __init__(self, path, timeout=0.05):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(timeout)
        self.port = "unix:" + path

    in_waiting = 0

    def read(self, n=1):
        try:
            d = self.sock.recv(65536)
        except socket.timeout:
            return b""
        if not d:
            raise ConnectionError("daemon closed the connection")
        return d

    def write(self, data):
        self.sock.sendall(data)
        return len(data)

    def reset_input_buffer(self):
        pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def open_link(port=None, timeout=0.05):
    """An open link to the board: pyserial Serial (exclusive) or SocketPort."""
    kind, where = resolve(port)
    if kind == "unix":
        return SocketPort(where, timeout)
    try:
        return serial.Serial(where, 115200, timeout=timeout, exclusive=True)
    except serial.SerialException as e:
        raise serial.SerialException("%s: %s (a daemon or another program has it open?)" % (where, e)) from e
