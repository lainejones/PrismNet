# Getting connected

Everything here can be done from Workbench with PrismNet Prefs and PrismMon;
the Shell commands are there for scripts and for those who prefer them.

## Setting up the network card

The installer asks for the card's driver and whether the router gives the
address (DHCP) or you type it in. To change that later, open **PrismNet
Prefs** (`SYS:Prefs/PrismNet`), **Interfaces** tab:

1. **Device**: the card's SANA-II driver; `...` picks it from
   `DEVS:Networks`. **Unit** is almost always 0.
2. **Mode**: **DHCP** for most home networks. For **Static**, fill in
   *Address*, *Netmask* and *Gateway* (the router), and give the name
   servers on the **Network** tab.
3. **Save**. If PrismNet is running, Prefs offers to restart it with the new
   settings.

See [PrismNet Prefs](04-prefs.md) for every field.

## Starting PrismNet

Usually there is nothing to do: with **Start when needed** on (the
default), the first program that opens `bsdsocket.library` starts PrismNet
and waits until it has an address.

To start it yourself, open **PrismMon** (where the installer put it,
usually `SYS:Tools`) and click
**Start**. Its Status tab shows the address once DHCP has given one (that
can take a few seconds). Or, in a Shell:

```
PrismNet START
```

which starts the stack in the background, waits for the addresses and
prints the status. If it can't start, PrismMon's Log tab (or the Shell)
says why: a driver it can't open, not enough memory, no settings.

To have it running from boot, tick **Start at boot** in PrismNet Prefs
(Startup tab), or answer yes in the installer.

## Checking it works

In **PrismMon**, the **Status** tab shows whether PrismNet runs, each
interface's address and link, and the DNS servers; **Interfaces** adds the
netmask, gateway and the packets going in and out. An address and a link
that is up, with packets counting in both directions, mean the card works.
Then open a web page in your browser: if it loads, DNS and the internet
work too.

In a Shell, `PrismNet STATUS` shows the same, and `ping` tests each step,
in this order:

```
ping 192.168.1.1          your router: the network card and cabling work
ping 1.1.1.1              the internet: the gateway works
ping aminet.net           DNS works
```

If the first one fails, see [Troubleshooting](13-troubleshooting.md). If
all three answer, every network program should work.

## Stopping it

Click **Stop** in PrismMon, or in a Shell:

```
PrismNet STOP
```

Either closes every connection and lets go of the network card. Programs
that still have `bsdsocket.library` open get errors until it is started
again.

## How other computers find the Amiga

The name you gave the Amiga (PrismNet Prefs, Network tab, *Computer name*)
works three ways:

- **Windows** finds it by NetBIOS: `ping AMIGA`, or `ftp AMIGA` when the
  FTP server runs. NetBIOS uses the first 15 characters, before any dot.
- **macOS, Linux and Windows 10/11** find it by mDNS: `ping amiga.local`,
  if the network driver accepts multicast (see
  [Known problems](13-troubleshooting.md#known-problems)).
- **Your router** often learns the name from DHCP and answers it in its own
  DNS.
