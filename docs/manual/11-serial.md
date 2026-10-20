# Serial links

An Amiga without a network card can still join the network through its
serial port, with SLIP or PPP to another machine (a PC, a Raspberry Pi, an
ESP8266/ESP32 Wi-Fi modem) that passes the packets on.

There are two ways to run SLIP:

| | Built-in SLIP | A SANA-II driver |
|---|---|---|
| Interface file | `DEVICE=serial.device` (or `SLIP=YES`) | `DEVICE=slip.device` (or another driver) |
| Needs | nothing extra | the driver and its own settings file |
| Protocols | SLIP | whatever the driver does: SLIP, CSLIP, PPP, PLIP |
| `PrismNet STOP` / `START` | always works | depends on the driver |

Use the built-in SLIP unless you need CSLIP, PPP or PLIP.

## Built-in SLIP

Make an interface whose device is the serial port:

```
DEVICE=serial.device UNIT=0 BAUD=115200
CONFIG=STATIC ADDRESS=192.168.240.2 NETMASK=255.255.255.0
```

In PrismNet Prefs: add an interface, set *Device* to `serial.device`, and
fill in *Speed* and, if the cable has the lines, *RTS/CTS*.

### Options

| Key | Meaning | Default |
|---|---|---|
| `DEVICE=` | `serial.device`, or another serial driver with `SLIP=YES` | - |
| `UNIT=` | the serial unit (a multi-port card's port number) | 0 |
| `BAUD=` | the speed; the other end must use exactly the same | 19200 |
| `RTSCTS=YES` | hardware handshaking over RTS and CTS | off |
| `SLIP=YES` | run SLIP over a different serial driver, for example a serial card's own device (`DEVICE=8n1.device SLIP=YES`) | - |
| `MTU=` | the largest packet; only lower than 1006 is accepted | 1006 |
| `ADDRESS=`, `NETMASK=` | the Amiga's address on the link | - |
| `CONFIG=STATIC` | SLIP has no way to hand out addresses | - |

### Choosing a speed

- **115200** works on any Amiga with a 68020 or better, and on a 68000 for
  light use. It is about 11 KB/s.
- On a 68000, or when the far end has no RTS/CTS, start at **38400** and go up
  while transfers stay clean.
- **RTS/CTS** lets either end pause the other when its buffer fills. Use it
  whenever the cable carries the lines (a full null-modem cable does; the
  CD32's AUX port and many ESP modules don't). Without it, a busy Amiga can
  lose bytes, and TCP has to resend.

PrismNet keeps up to 8 writes queued to `serial.device` and lets up to 64 more
packets wait behind them (16 and 4 in the small core), so a burst of TCP data
never has to be refused.

## Through a SANA-II driver

Any SANA-II point-to-point driver works: SLIP, CSLIP, PPP or PLIP, for example
`slip.device` or Roadshow's `ppp-serial.device`:

```
DEVICE=slip.device UNIT=0
ADDRESS=192.168.240.2 NETMASK=255.255.255.0
```

The driver's own settings (serial port, speed) stay in its own file, for
`slip.device` `ENVARC:SANA2/slip0.config`. `CONFIG=DHCP` takes the address the
driver reports (SLIP's configured address, or the one PPP negotiated).

## The other end

### A Linux PC or Raspberry Pi

```
slattach -p slip -s 115200 /dev/ttyUSB0 &
ip addr add 192.168.240.1 peer 192.168.240.2 dev sl0
ip link set sl0 up
sysctl -w net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s 192.168.240.0/24 -o eth0 -j MASQUERADE
```

Add `-L` to `slattach` for a 3-wire cable without modem lines. The last two
lines give the Amiga the internet through the PC.

### An ESP8266/ESP32 SLIP router

A Wi-Fi module running SLIP router firmware (for example the "esp_slip_router"
projects) joins your Wi-Fi and routes the serial side. Set it to the same
speed as `BAUD=`. Such modules usually NAT the Amiga's address, so the Amiga
can reach the network but other machines only reach it through ports the
module forwards.

## Good to know

- There is no ARP, broadcast or mDNS on a serial link, so the Amiga can't be
  found by name there; use its address.
- A lone serial link becomes the default route; it needs no gateway.
- `PrismNet STATUS` shows the link's counters. Many "rx dropped" mean bytes
  are being lost on the line: lower the speed or use RTS/CTS.
- Moving from `slip.device` to the built-in SLIP: put the speed from
  `slip0.config` into `BAUD=`, change `DEVICE=` to `serial.device`, and
  restart PrismNet (or reboot).
