# Getting connected

## Starting PrismNet

Usually there is nothing to do: with **Start when needed** on (the
default), the first program that opens `bsdsocket.library` starts PrismNet
and waits until it has an address. To start it yourself, open a Shell:

```
PrismNet START
```

It starts the stack in the background, waits for the addresses (DHCP can
take a few seconds) and prints the status. If it can't start, it says why:
a driver it can't open, not enough memory, no settings.

To have it running from boot, tick **Start at boot** in PrismNet Prefs
(Startup tab), or answer yes in the installer.

## Checking it works

```
PrismNet STATUS
```

shows each interface with its address, netmask, gateway and link state,
the DNS servers, how much memory the stack uses, and how many programs are
using it. Then try, in this
order:

```
ping 192.168.1.1          your router: the network card and cabling work
ping 1.1.1.1              the internet: the gateway works
ping aminet.net           DNS works
```

If the first one fails, see [Troubleshooting](13-troubleshooting.md). If
all three answer, every network program should work.

## Stopping it

```
PrismNet STOP
```

closes every connection and lets go of the network card. Programs that
still have `bsdsocket.library` open get errors until it is started again.
PrismMon has Start and Stop buttons too.

## How other computers find the Amiga

The name you gave the Amiga (PrismNet Prefs, Network tab, *Computer name*)
works three ways:

- **Windows** finds it by NetBIOS: `ping AMIGA`, or `ftp AMIGA` when the
  FTP server runs. NetBIOS uses the first 15 characters, before any dot.
- **macOS, Linux and Windows 10/11** find it by mDNS: `ping amiga.local`.
- **Your router** often learns the name from DHCP and answers it in its own
  DNS.
