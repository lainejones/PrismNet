# Troubleshooting

Start with `PrismNet STATUS` and `PrismNet LOG`: most problems show up
there.

## PrismNet won't start

`PrismNet START` says why. The usual reasons:

- **The driver can't be opened**: wrong name or unit, the driver isn't in
  `DEVS:Networks`, or the card isn't there. Check the Device field in
  Prefs; `...` lists what is installed.
- **Another stack has the card**: Roadshow, AmiTCP or Miami still running.
  Take it out of the startup files and reboot.
- **Not enough memory**: it shows the free memory and the largest block.
  See [Small machines](12-memory.md).
- **No settings**: run PrismNet Prefs or the installer.

## No address from DHCP

- Is the link up? `PrismNet STATUS` shows `link up` or `down` for each
  interface. Check the cable or the Wi-Fi bridge.
- `PrismNet LOG` shows the DHCP exchange. No answer at all usually means the
  packets don't reach the network: the wrong unit, or a driver that needs
  configuring first.
- Try a fixed address to rule DHCP out.

## Pings work, names don't

- `PrismNet STATUS` shows the DNS servers. With none, set one in Prefs
  (Network tab), e.g. your router or `1.1.1.1`.
- `nslookup aminet.net` asks the DNS server directly and shows what comes
  back.
- A name without a dot gets the domain added (Network tab, *Domain*).

## A program says there is no TCP/IP stack

- `bsdsocket.library` in `LIBS:` must be PrismNet's: `Version LIBS:bsdsocket.library FULL`
  shows *PrismNet*. Another stack's installer may have put its own back.
- With *Start when needed* off, PrismNet has to be running before the
  program starts.
- **In WinUAE**: switch off *bsdsocket.library emulation* (`bsdsocket_emu=false`
  in the configuration), or WinUAE's own library answers instead of
  PrismNet's.

## A program misbehaves

Start PrismNet with a trace:

```
PrismNet STOP
PrismNet START TRACE=RAM:trace.txt
```

Run the program, then stop PrismNet again. `RAM:trace.txt` has every
request the program sent to the stack (the operation, socket, arguments,
result and error code) between the log messages. It is meant for the
developers: send it with a problem report.

## A WHDLoad game hangs on a black screen

WHDLoad takes the whole Amiga over for a game. A network card that keeps
signalling new packets then has nobody to answer it, and some games hang
(The Settlers does, with an X-Surf); others don't mind. The cure is to
switch the network off while a game runs. WHDLoad runs a command of your
choice before each game and another after it, so this does it for every
game. The installer offers it when WHDLoad is installed; by hand, add two
lines to `S:WHDLoad.prefs`:

```
ExecuteStartup=C:PrismNet STOP HOLD QUIET
ExecuteCleanup=C:PrismNet START
```

or run `rx WHDLoadHooks.rexx ADD` from the archive's Tools drawer (`REMOVE`
takes them out again). `HOLD` keeps PrismNet off until the game ends, even
if something asks for the network meanwhile. If you already have
ExecuteStartup/ExecuteCleanup commands, put the PrismNet ones in the
scripts they run.

## The FTP, telnet or web server doesn't answer

- Is the server switched on? Prefs, Services tab; `ENV:PrismNet/services`
  must exist and have the line without `#`.
- `PrismNet NETSTAT` should show a socket listening on port 21, 23 or 80.
- **Web: 404 Not Found** for every page: does the folder in Prefs (the
  `ROOT=` in `services`) exist?
- **Login incorrect**: the account needs a password (Prefs, Users,
  *Accounts... > Password...*).

## Known problems

- **CD32 with `slip.device` 37.4 (1993)**: after `PrismNet STOP`, a
  `PrismNet START` can hang while opening the driver; the machine gets
  sluggish and the network stays down until a reboot. A reboot starts
  cleanly. Use PrismNet's built-in SLIP instead (`DEVICE=serial.device`,
  see [Serial links](11-serial.md)): it restarts fine and is as fast.
- **`amiga.local` does not resolve** on some cards: the network driver
  refuses the mDNS group address. `PrismNet LOG` then says "refused
  multicast". `x-surf.device` 1.16 and the A314's `a314eth.device` refuse it; the
  DaynaPORT driver from 1.7 on (scsidayna.device, with `MULTICAST=1`), the
  ZZ9000 and [xsurfnet.device](https://github.com/lainejones/XSurfNet) accept it. Windows still finds the Amiga
  by its NetBIOS name (`ping A2000`).
- **IPv6** isn't supported yet.

## Reporting a problem

Include the output of `PrismNet STATUS` and `PrismNet LOG`, the Amiga
model, CPU, OS version and network card, and what you did. `Tools/bsdtest`
output helps too.
