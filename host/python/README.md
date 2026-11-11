# esp-swd host tools

The `esw` package is the host side of the r5 board:
- a Python library (`esw.Esw`);
- a command-line tool, `esw`;
- a daemon, `esw serve`, that turns the two DUT UARTs into terminal devices and lets several programs share the
  board.

Linux is required for the daemon (pseudo-terminals, Unix sockets). The library and the CLI without the daemon run
wherever pyserial does.

## Install
```
python3 -m venv .venv && .venv/bin/pip install -e host/python      # pyserial; adds the `esw` command
.venv/bin/pip install esptool                                       # only for `esw update`
sudo cp host/udev/60-esp-swd.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```
The board enumerates as USB `303a:1001` (USB-Serial-JTAG), e.g. `/dev/ttyACM0`. Each board is identified by its
USB serial number (the chip's MAC without colons, e.g. `F0F5BD57EF34`). `--port` takes a device path or that id.
It is needed only when more than one board is attached.

## Everyday use
```
esw serve &                                   # daemon: TTYs + shared access (or run it as a systemd user service)
esw info                                      # firmware, both channels: VTref, switches, modes; TTY paths
picocom -b 1000000 $XDG_RUNTIME_DIR/esp-swd/<board>/ttyA     # DUT A console, any terminal program
esw flash A=app.hex B=stm32.hex               # both channels at once, CRC-verified, then pin reset
esw flash A=app.hex A1=net.hex                # nRF5340: application core, then network core (same channel: in turn)
esw flash B=fw.bin@0x08000000                 # raw binary at an address
esw reset A                                   # pin reset
esw mode A uart off                           # isolation switch modes, see below
esw update app/build                          # new firmware for the board itself
esw stop                                      # stop the daemon
```
Exit status: 0 success, 1 failure, 2 usage error.

Scripts use the same library either way. `Esw()` goes through the daemon when one serves the board, otherwise it
opens the port itself:
```python
from esw import Esw
with Esw() as d:
    r = d.flash_file(0, "app.hex")            # connect, erase touched pages, program, CRC verify, disconnect
    print(r["part"], r["seconds"])
```

## Isolation switches: modes
Each channel has two switch groups toward the DUT: **SWD** (SWDIO, SWCLK, nRESET) and **UART** (TX, RX). A group
closes only when all three of these hold:
- its mode allows it;
- something needs it, or the mode is `on`;
- the DUT is powered: VTref between 2.8 V and 3.6 V, measured through the VTref switch first.

| Mode | The group is closed | Typical use |
|---|---|---|
| `auto` (default after every boot) | while something uses it: an SWD session or job; a UART stream, i.e. an open TTY | normal operation; DUT sleep-current measurements between uses |
| `on` | whenever the DUT is powered, used or not | keep the DUT's RX line driven idle so it never floats |
| `off` | never: attach and UART writes fail with the interlock error | a person is probing the DUT; guarantee the tool cannot touch it |

- `esw mode <A|B|AB> <swd|uart|all> <auto|on|off>` sets a mode; `esw isolate [AB]` sets both groups to `off`.
- Library: `Esw.mode(ch, group, mode)`, `Esw.isolate(ch)`, and `Esw.state(ch)` to read the switches back.
- `off` on SWD first ends an attached session cleanly (debug off, pin reset). `off` on UART first lets TX drain.
- **The board itself decides, not the host.** VTref loss opens a group at once. Host loss (no frame for 3 s plus a
  2 s grace period) releases everything the host held, `on` included. A firmware crash or reboot opens everything.
  `off` survives host loss (it can only keep a switch open) and lasts until changed or until the board reboots.
- **`on` needs a host that stays:** with `esw serve` running, the daemon is the host, so `on` lasts until it is
  changed or the daemon stops. Without the daemon, `esw mode A uart on` ends about 5 s after the command exits
  (host loss).
- The interlock cannot be bypassed: no mode closes a switch toward an unpowered DUT (that would back-power it).

## The daemon (`esw serve`)
The daemon owns the USB port. Run it once per board, in a terminal or as a user service
(`host/systemd/esw.service`).

**TTYs.** Each DUT UART appears as a pseudo-terminal, linked as `$XDG_RUNTIME_DIR/esp-swd/<board>/ttyA` and `ttyB`
(`esw tty` prints them). Standard programs use them like a USB serial adapter: picocom, minicom, screen, pyserial,
`stty` with `cat`.
- **Baud rate:** whatever the program sets, standard or custom, 300 baud to 5 Mbaud.
- **Parity and stop bits:** Linux PTYs cannot carry parity, so they come from `esw serve --format-a 8E1` (default
  8N1). Two stop bits set by the program are honoured.
- **Not available:** modem lines (DTR/RTS) and break; the r5 UART header has neither.
- **Bridge policy per channel** (`--uart-a/--uart-b`, or `esw tty A <policy>` at run time):
  - `open` (default): the UART runs, and its switches may close, while some program has the TTY open, and until
    what it wrote has gone out. `echo reset > ttyA` works like on a serial adapter.
  - `always`: the UART runs from the start. Output that arrives while no program has the TTY open is kept (last
    64 KB) and delivered on open. With switch mode `on`, this catches a DUT's boot log even before the terminal
    opens.
  - `off`: no bridge; the channel's UART is free for scripts (`Esw.uart_*`).
- **Right after opening,** the first bytes wait until the switches have closed (VTref settles in about 50 ms).
- **No DUT power, or mode `off`:** writes to the TTY are dropped and counted (`esw info`), like a serial adapter
  whose cable is unplugged. Reading simply gets nothing.
- **The board restarted or closed the stream** (host loss, reboot, crash): the bridge reopens the UART by itself.

**Shared access.** Every other program reaches the board through the daemon's socket
(`$XDG_RUNTIME_DIR/esp-swd/<board>/ctl.sock`, mode 0600), so flashing, the CLI and the TTYs run at the same time.
- **Ownership:** each channel's SWD belongs to one program at a time (the one that holds or attaches it), and so
  does its UART (stream and group hold together). A second program gets a clear refusal, not a corrupted session.
  The TTY bridge cannot take a UART a program is using. Switch modes may always be changed.
- **Cleanup:** when a program exits or is killed, the daemon ends what it left behind (detach, release, UART close),
  as the board does on host loss.
- **Board reboot or firmware update:** the daemon reconnects by itself. It notices a board restart within about 1 s
  even though USB stays enumerated, and reopens the TTYs' UARTs. TTY links and paths stay the same.
- **Daemon killed:** after about 5 s the board's host lease isolates everything the daemon held. Left-over socket
  files do no harm: the client falls back to the port, and a new daemon replaces them.

## Updating the board firmware
```
esw update app/build                  # an ESP-IDF build directory (uses its flash_args)
esw update esp-swd-merged.bin         # or one merged image (idf.py merge-bin), written at 0x0
```
- Uses esptool over USB-Serial-JTAG: the USB peripheral itself resets the chip into the ROM downloader. It works
  while the firmware runs, and even when it has hung. No button, no jumper.
- With a daemon running, `esw update` suspends it, flashes, and resumes it.
- If a write is interrupted, run it again: the ROM downloader stays reachable over USB.
- `esw reboot --rom` leaves the chip in the downloader for manual esptool use.

## Measured (r5 board 2, firmware 0.5.0, 2026-11-11)
| Test | Result |
|---|---|
| Both UARTs, internal loopback, full duplex at once (`examples/uart_loopback.py`) | 1 Mbaud: 97.6 KB/s each way per channel, 10 s, 0 lost; 2 Mbaud: 195 KB/s each way per channel (about 780 KB/s over USB in total), 0 lost |
| Same through the daemon and both TTYs (`examples/tty_loopback.py`, pyserial) | 1 Mbaud: 97 KB/s each way per channel, byte-exact; 115200, 250000 (custom) and 2 Mbaud also byte-exact |
| Real pads, ttyA ↔ ttyB (`tty_loopback.py --cross`; J302.2 → J402.3, J402.2 → J302.3) | through both channels' isolation switches, both directions at once: byte-exact at 115200, 460800, 1 Mbaud (97 KB/s each way) and 2 Mbaud (175 KB/s each way); 8E1 via `--format-a/-b 8E1` at 1 Mbaud also byte-exact |
| Both DUTs at once (`esw flash A=nrf5340.bin B=stm32.hex`) | nRF5340 1 MB + STM32U585 1.2 MB, CRC-verified: 5.6 s at once against 8.6 s one after the other |
| Everything at once through the daemon | real pads: ttyA ↔ ttyB at 1 Mbaud both ways for 20 s (2 MB each way, 0 frame/parity/overflow errors on the board) while both DUTs flashed at once (verified; 11.5 s instead of 5.6 s, the UARTs keep their full rate) |
| STM32U585 1.2 MB image through the daemon | 4.8 s alone; 6.2 s while another program reads the nRF5340 |
| Robustness | a second program on a busy channel is refused; `kill -9` of a program in a session: the daemon detaches and releases; three reboots in a row: TTYs work 2 s later; reboot during TTY streaming: one channel lost 256 bytes, both streams resumed; `esw update` through the daemon; daemon `kill -9` during a session: the board isolated by itself |
