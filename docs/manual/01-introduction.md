# Introduction

PrismNet is a TCP/IP stack: the part of the system that lets programs talk
to other computers. On the Amiga that means a `bsdsocket.library` in `LIBS:`,
which every network program opens, and something behind it that speaks to
the network hardware through its SANA-II driver.

## What you get

- **bsdsocket.library**, compatible with AmiTCP 4 (the API almost every Amiga
  network program is written for), plus the Roadshow extensions that newer
  programs use: interface and route queries, statistics, socket events.
- **usergroup.library**, the AmiTCP user and group API, for programs that
  ask who the user is.
- **The PrismNet command**, which starts, stops and reports on the stack.
- **PrismNet Prefs**, a settings editor: network cards, DHCP or fixed
  addresses, DNS, routes, users, shares and servers. It can import a
  Roadshow setup.
- **PrismMon**, a monitor window: addresses, traffic, open connections,
  memory and the log.
- **Network tools**: `ping`, `traceroute`, `nslookup` and the `httpbench`
  benchmark.
- **Network shares** (optional): mount an NFS export or an SMB share
  (Windows, macOS, a NAS, Samba) as an ordinary Amiga volume.
- **Servers** (optional): an FTP server, so a PC can copy files to and from
  the Amiga, and a telnet server that gives a Shell over the network.
- **An update check**: PrismNet Prefs (or `PrismNetUpdate` in a Shell) tells
  you when a newer PrismNet is out and can install it. See
  [Checking for updates](02-installing.md#checking-for-updates).

The networking core is [lwIP](https://savannah.nongnu.org/projects/lwip/), a
widely used TCP/IP implementation, running as an Amiga task.

## What it does by itself

- **Starts when it is needed.** The first program that opens
  `bsdsocket.library` starts PrismNet and waits for an address. You can also
  start it at boot, or by hand.
- **DHCP**, or fixed addresses, on up to four interfaces at once.
- **Finds other machines by name**: DNS and a `hosts` file.
- **Can be found by name**: Windows PCs reach the Amiga by its NetBIOS name
  (`ping AMIGA`), Macs and Linux by mDNS (`ping amiga.local`).
- **Sets the clock** from a time server, if you want it to.
- **Uses only the memory it needs**: buffers are taken as traffic needs them
  and given back, inside a budget you can set.

## Requirements

- AmigaOS 3.1 or newer, any 68000 or better.
- A network interface with a SANA-II driver in `DEVS:Networks`: a PCMCIA
  card (`cnet.device` and similar), a Zorro card (X-Surf, ZZ9000), a SCSI
  network adapter (DaynaPORT), an A314, or a serial link (SLIP or PPP; see
  [Serial links](11-serial.md)).
- Memory: about 290 KB when idle with the standard core, about 175 KB with
  the small core for 1 to 2 MB machines (see [Small machines](12-memory.md)).

## Tested on

| Machine | Network |
|---|---|
| Amiga 4000, 68060 | ZZ9000 Ethernet (`ZZ9000Net.device`) |
| Amiga 2000, 68030 | X-Surf (`xsurfnet.device`, `x-surf.device`) |
| Amiga 1200, 68030 | A314 (`a314eth.device`); ZuluSCSI DaynaPORT (`scsidayna.device`) |
| Amiga 500, 68030 | BlueSCSI DaynaPORT (`scsidayna.device`) |
| CD32, 68030 | serial SLIP (built-in, and `slip.device`) |
| WinUAE, A1200 OS 3.2 | emulated NE2000 PCMCIA (`cnet.device`) and X-Surf |

Programs used with it include AWeb, IBrowse, Amelinium, wget, AmiSSL-based
HTTPS, FTP clients and the NilBBS telnet BBS.
