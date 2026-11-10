# SPDX-License-Identifier: MIT
"""esp-swd wire protocol: COBS + CRC-16/CCITT-FALSE framing, command numbers (mirrors lib/include/esw/proto.h)."""
import binascii

PROTO_VERSION = 4
CH_QUEUE = 4   # proto >= 4: channel request frames in flight per channel (the board's per-channel queue)

PING, INFO, REBOOT = 0x01, 0x02, 0x03
CH_HOLD, CH_RELEASE, CH_STATE = 0x10, 0x11, 0x12
SWD_ATTACH, SWD_DETACH, TGT_UNLOCK, TGT_RESET = 0x20, 0x21, 0x22, 0x23
MEM_READ, MEM_WRITE, TGT_ERASE, TGT_PROGRAM, TGT_VERIFY, SWD_STATS = 0x24, 0x25, 0x26, 0x27, 0x28, 0x29
SWD_BENCH, TGT_CRC32, DAP_RAW = 0x2A, 0x2B, 0x2C
UART_OPEN, UART_CLOSE, UART_WRITE, UART_STATS = 0x30, 0x31, 0x32, 0x33
EVT_UART_RX = 0xE0                       # events: seq 0, cmd 0xE0..0xEF, status 0
EVT_FIRST, EVT_LAST = 0xE0, 0xEF
UART_PARITY = {"N": 0, "E": 1, "O": 2}   # UART_OPEN flags bits 0-1; bit 2: two stop bits
UART_STOP2 = 1 << 2
UART_RX_NOFILT, UART_TX_STRONG, UART_NO_DMA = 1 << 3, 1 << 4, 1 << 5
UART_BACKEND = {0: "off", 1: "irq", 2: "dma"}
UART_RX_LOST, UART_RX_ERROR, UART_RX_BREAK = 1 << 0, 1 << 1, 1 << 2
CRC_RESET_HALT = 1 << 0

GROUP_VTREF, GROUP_SWD, GROUP_UART = 0, 1, 2
GSTATE = {0: "OPEN", 1: "WAIT_VTREF", 2: "CLOSED"}
MEM_KIND = {0: "flash", 1: "ram", 2: "otp", 3: "ext"}

ERRORS = {
    0: "ok", -1: "invalid argument", -2: "no SWD response", -3: "WAIT retries exhausted",
    -4: "FAULT / sticky error", -5: "read parity error", -6: "invalid ACK", -7: "timeout",
    -8: "debug access locked", -9: "verify mismatch", -10: "invalid state", -11: "unsupported",
    -12: "not found", -13: "loader error", -14: "isolation interlock",
}
ERR_LOCKED, ERR_NOT_FOUND, ERR_INTERLOCK = -8, -12, -14

UNLOCK_ALLOW_ERASE = 1 << 0
RESET_HALT = 1 << 0


def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)   # CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, not reflected


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    for chunk in bytes(data).split(b"\x00"):   # every chunk but the last ends in an (implied) zero
        while len(chunk) >= 254:
            out.append(0xFF)                     # full block: no implied zero
            out += chunk[:254]
            chunk = chunk[254:]
        out.append(len(chunk) + 1)
        out += chunk
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if code == 0:
            raise ValueError("zero in COBS data")
        i += 1
        block = data[i:i + code - 1]
        if len(block) != code - 1:
            raise ValueError("truncated COBS block")
        out += block
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def frame_encode(payload: bytes) -> bytes:
    crc = crc16(payload)
    return cobs_encode(payload + bytes([crc & 0xFF, crc >> 8])) + b"\x00"


def frame_decode(raw: bytes):
    """raw: bytes between delimiters. Returns the payload, or None if malformed or the CRC fails."""
    try:
        d = cobs_decode(raw)
    except ValueError:
        return None
    if len(d) < 3:
        return None
    payload, crc = d[:-2], d[-2] | (d[-1] << 8)
    return payload if crc16(payload) == crc else None
