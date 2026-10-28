# Printing

PrismNet's network printer port lets the Amiga print to a printer on the
network, the way it would to one on the parallel port. Every program that
prints through the Amiga's printer support works with it: Workbench, word
processors, paint programs, `Copy file PRT:`.

It suits printers that understand **PostScript** or **PCL** (HP LaserJet
language), as most office laser printers do. The Amiga's own printer
drivers make the job (*PostScript*, or *HP_LaserJet* for PCL), and the port
passes it to the printer as it is. A printer that only takes its own
manufacturer's format (most cheap inkjets) can't be reached this way.

Install it with the installer's *network printer port* question. That puts
`DEVS:prismprint.device` and `C:PrismPrint` on the Amiga.

## Setting it up

In a Shell:

```
PrismPrint HOST=192.168.1.40 SAVE
PrismPrint TEST
```

`HOST` is the printer's address or network name. `TEST` sends a page of
plain text; if it comes out, the network path to the printer works. Then
tell the Amiga's printer support to use the port:

```
PrismPrint PREFS SAVE
```

and pick the printer driver in **Prefs/Printer**: *PostScript*, or an HP
LaserJet driver for a PCL printer. From then on everything printed goes to
the network printer.

`PrismPrint PREFS` sets *Printer Port* in Printer prefs to *Custom* with the
device name `prismprint`. You can also do that by hand in Prefs/Printer:
the name goes in **without** `.device` (the system adds it).

## How the job travels

| Setting | Meaning |
|---|---|
| `PROTOCOL=RAW` | TCP port 9100 (also called JetDirect or "raw" printing). The job goes out as it is made. The default, and what most network printers take. |
| `PROTOCOL=LPD` | LPD (RFC 1179, port 515). The job is kept in memory and sent when it is complete. `QUEUE=` names the printer's queue: Brother printers call theirs `BINARY_P1`, many others take `lp`. |
| `PORT=n` | another TCP port |
| `IDLE=n` | a job also ends after this many seconds without data (default 10), for programs that keep the printer open between pages |
| `TIMEOUT=n` | how long the printer may refuse data before the job fails (default 60) |

`PrismPrint` with no arguments shows the settings; without `SAVE` a change
lasts until the next reboot. The settings are `ENVARC:PrismNet/printer`;
`UNIT=1` to `UNIT=3` give up to three more printers (`printer1` ...), each
chosen in Printer prefs by its unit number.

`PrismPrint FILE=name` sends a file to the printer unchanged - for example a
PostScript file made by another program.

## When it doesn't print

- **"Printer trouble"** when printing: the Amiga couldn't reach the printer.
  Check that it is on, and `HOST` with `ping`. **Retry** tries again.
- **Nothing comes out, no message**: the printer got the job but doesn't
  understand it. Choose the other driver (PostScript or PCL), or check the
  printer's own settings for the language it expects.
- The network starts by itself when something prints, like any other
  network program.

## OS 3.1

OS 3.1's printer support can only print to `parallel.device`. The package
has the same port under that name in `SYS:Storage/Printer-OS3.1` (the
installer puts it there on OS 3.1). To use it, keep a copy of the real one
first, then put it in its place:

```
Copy DEVS:parallel.device DEVS:parallel.device.real
Copy SYS:Storage/Printer-OS3.1/parallel.device DEVS:
```

and reboot. Printer prefs stays on *Parallel*. To go back, copy
`parallel.device.real` over it again. While it is in place the parallel
port itself can't be used for anything else.
