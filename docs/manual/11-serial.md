# Serial links

An Amiga without a network card can still join the network through its
serial port, with SLIP or PPP to another machine (a PC, a Raspberry Pi, an
ESP32 Wi-Fi modem) that passes the packets on.

## SLIP built in

PrismNet can run SLIP straight over the serial port, without a SANA-II SLIP
driver. Make an interface with `DEVICE=serial.device`:

```
DEVICE=serial.device UNIT=0 BAUD=38400 RTSCTS=YES
CONFIG=STATIC ADDRESS=192.168.240.2 NETMASK=255.255.255.0
```

- `BAUD=` is the speed (default 19200). The other end must use the same.
- `RTSCTS=YES` switches on hardware handshaking; use it when the cable
  carries RTS and CTS, above all at higher speeds.
- `SLIP=YES` runs SLIP over another serial driver (for example a serial
  card's own device) instead of `serial.device`.

In PrismNet Prefs, these are the *Speed* and *RTS/CTS* fields of the
interface.

## Through a SANA-II driver

Any SANA-II point-to-point driver works too: SLIP, CSLIP, PPP or PLIP, for
example `slip.device` or Roadshow's `ppp-serial.device`:

```
DEVICE=slip.device UNIT=0
ADDRESS=192.168.240.2 NETMASK=255.255.255.0
```

`CONFIG=DHCP` takes the address the driver reports (SLIP's configured
address, or the one PPP negotiated).

## Good to know

- There is no ARP, broadcast or mDNS on a serial link, so the Amiga can't
  be found by name there.
- A lone serial link becomes the default route; it needs no gateway.
- The far end needs SLIP or PPP and forwarding switched on. On Linux:
  `slattach -p slip -s 38400 /dev/ttyUSB0` and `ifconfig sl0 192.168.240.1
  pointopoint 192.168.240.2 up`, plus IP forwarding and NAT for internet
  access.
- See [Troubleshooting](13-troubleshooting.md) for a known problem with
  restarting PrismNet on one old `slip.device`.
