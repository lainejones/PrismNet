# Settings files

PrismNet keeps its settings in plain text files in `ENVARC:PrismNet`,
copied to `ENV:PrismNet` at boot; PrismNet reads the `ENV:` copy. PrismNet
Prefs writes all of them, but you can edit them with any text editor too.
Lines starting with `#` or `;` are comments. Commented examples are in
`ENVARC:PrismNet/examples`.

After changing a file by hand, copy it to `ENV:PrismNet` as well (or
reboot), then restart PrismNet: `PrismNet STOP` and `PrismNet START`.

## interfaces

One file per interface (up to four) in the `interfaces` drawer; the file
name is the interface's name.

```
DEVICE=cnet.device UNIT=0
CONFIG=DHCP
```

or with a fixed address:

```
DEVICE=x-surf.device UNIT=0
CONFIG=STATIC ADDRESS=192.168.1.40 NETMASK=255.255.255.0
GATEWAY=192.168.1.1
```

| Key | Meaning |
|---|---|
| `DEVICE=` | the SANA-II driver; a bare name is looked up in `DEVS:Networks` |
| `UNIT=` | the unit (default 0) |
| `CONFIG=` | `DHCP` or `STATIC` |
| `ADDRESS=`, `NETMASK=`, `GATEWAY=` | the fixed address, for `STATIC` |
| `MTU=` | the largest packet, if the driver's own value is wrong |
| `STALL=` | milliseconds a packet may sit with the driver before PrismNet resets the card (500 to 60000, default 2000). Raise it for a card that is legitimately slow at times: a DaynaPORT on a SCSI bus shared with a ZuluSCSI's disks wants 10000 |
| `SLIP=YES`, `BAUD=`, `RTSCTS=YES` | SLIP straight over a serial port; see [Serial links](12-serial.md) |

## routes

```
DEFAULT 192.168.1.1
ROUTE 192.168.2.0/24 192.168.1.254
ROUTE 10.8.0.1 192.168.1.254
```

`DEFAULT` is the default gateway for static setups (DHCP brings its own).
`ROUTE destination[/bits] gateway` adds a static route, up to 16; without
`/bits` it is a route to one address. The gateway must be on one of the
interfaces' networks.

## dns

```
NAMESERVER 192.168.1.1
DOMAIN home.lan
```

Up to three `NAMESERVER` lines; they replace the servers DHCP offers.
`DOMAIN` (or `SEARCH`) is added to names without a dot.

## hostname, hosts

`hostname` holds the computer name, one word. `hosts` lists names for
addresses, checked before DNS and for reverse lookups:

```
192.168.1.10  nas  nas.home.lan
```

## startup

```
AUTOSTART=YES
COMMAND=C:PrismNet
MEMORY=384
INETD=C:inetd
```

| Key | Meaning |
|---|---|
| `AUTOSTART=` | `YES`: a program opening `bsdsocket.library` starts PrismNet |
| `COMMAND=` | where the PrismNet command is, for that start |
| `MEMORY=` | the memory budget in KB; see [Small machines](14-memory.md) |
| `INETD=` | where the server dispatcher is (default `C:inetd`) |
| `UPDATES=` | where `PrismNetUpdate` looks: `OFF` (default), `GITHUB`, `MIRROR` or `BOTH` (GitHub, then the mirror); see [Checking for updates](02-installing.md#checking-for-updates) |
| `MIRROR=` | the mirror: a Forgejo/Gitea repository's address, or an HTTPS directory with `latest.txt` |

## time

```
SERVER=DHCP
```

Sets the Amiga's clock from the network when PrismNet starts. `SERVER=DHCP`
uses the time server DHCP offers, else `pool.ntp.org`; or name a server.
`SETCLOCK=NO` only measures the difference (shown by `PrismNet STATUS`).
The time zone comes from the Locale prefs, read each time the clock is set.
Delete the file to switch it off.

## realname, workgroup

One line each: the user's real name (for `usergroup.library`) and the
Windows workgroup. The user name itself is the standard `ENV:USER`
variable.

## passwd, group

The accounts `usergroup.library` reports, used by the FTP and telnet
servers. One per line, fields separated by `|`:

```
name|password hash|uid|gid|real name|home|shell
name|password|gid|member,member
```

Set passwords with PrismNet Prefs (*Users > Accounts... > Password...*); the
hash is PrismNet's own, not Unix `crypt`. Without a `passwd` file there is
one user, named after `ENV:USER` (else `amiga`), with home `SYS:`.

## services

The servers to run: see [Servers](10-servers.md).

## smb

Passwords the SMB client saved for shares (one file per server and user).
Delete a file there to be asked again.

## printer, printer1 ... printer3

The network printer port's settings, one file per unit: `HOST=`, `PORT=`,
`PROTOCOL=` (`RAW` or `LPD`), `QUEUE=`, `IDLE=`, `TIMEOUT=`. `PrismPrint`
writes them; see [Printing](11-printing.md).
