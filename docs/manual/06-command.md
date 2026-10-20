# The PrismNet command

`C:PrismNet` is the stack itself and the command that controls it. Type it
in a Shell with one of these keywords.

## Starting and stopping

| Command | Does |
|---|---|
| `PrismNet START` | starts the stack in the background with the settings in `ENV:PrismNet`, waits for the addresses and shows the status. `PrismNet` alone does the same |
| `PrismNet STOP` | stops it: every connection is closed and the network card is let go. It returns once PrismNet has really ended |
| `PrismNet STOP HOLD` | stops it and keeps it stopped: no program can start it again (*Start when needed*) until `PrismNet START` or a reboot. For WHDLoad games, see [Troubleshooting](13-troubleshooting.md). `QUIET` prints nothing |
| `PrismNet RUN` | runs the stack in the foreground, in this Shell (what START starts in the background); CTRL-C stops it |

START also takes `TRACE=file`, which logs every request programs make to
that file: useful when a program misbehaves (see
[Troubleshooting](13-troubleshooting.md)).

## Looking at it

| Command | Shows |
|---|---|
| `PrismNet STATUS` | the computer name, how many programs and sockets use the stack, each interface (driver, hardware address, link, address, netmask, gateway, traffic), the DNS servers, when the clock was set, and the memory used and free |
| `PrismNet LOG` | the stack's recent messages: DHCP, link changes, errors |
| `PrismNet NETSTAT` | every open socket: protocol, addresses, state, queued data and the program that owns it |
| `PrismNet ROUTE` | where packets go: the local networks, static routes and the default gateway |
| `PrismNet ARP` | the hardware addresses learnt on the local network |

## The clock

`PrismNet SETCLOCK` asks the time server now and sets the Amiga's clock
(see the `time` file in [Settings files](07-settings.md)).

## One interface from the command line

For a quick test without any settings files, give the driver on the command
line. This runs in the foreground:

```
PrismNet DEVICE=cnet.device UNIT=0
PrismNet DEVICE=cnet.device IP=192.168.1.40 NETMASK=255.255.255.0
```

| Argument | Meaning |
|---|---|
| `DEVICE=` | the SANA-II driver; a bare name is looked up in `DEVS:Networks` |
| `UNIT=` | the driver's unit (default 0) |
| `IP=`, `NETMASK=`, `GATEWAY=` | a fixed address; without `IP=` it uses DHCP |
| `DNS=`, `DNS2=` | name servers |
| `HOSTNAME=` | the computer name |
| `QUIET` | don't print the log in the Shell |
| `TRACE=file` | log every request to a file |

## Return codes

Every command returns 0 when all is well. STATUS, LOG, NETSTAT, ROUTE and
ARP return 5 (WARN) when PrismNet isn't running, so a script can test it:

```
PrismNet STATUS >NIL:
IF WARN
  PrismNet START
ENDIF
```
