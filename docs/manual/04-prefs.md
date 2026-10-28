# PrismNet Prefs

PrismNet Prefs (`SYS:Prefs/PrismNet`) edits every setting. It has six tabs
and three buttons at the bottom:

- **Save** writes the settings to `ENV:PrismNet` (in use now) and
  `ENVARC:PrismNet` (kept after a reboot).
- **Use** writes them to `ENV:PrismNet` only, until the next reboot.
- **Cancel** closes the window and changes nothing.

If PrismNet is running when you Save or Use, it offers to restart it so the
new settings take effect. Connections that are open at that moment are
closed.

The **Project** menu has *Import Roadshow settings...* and *Quit*; the
**Edit** menu has *Reset to defaults* and *Last saved* (reload what is in
`ENVARC:`).

## Interfaces

The network interfaces PrismNet uses, up to four. The list on the left has
one line per interface; **Add** and **Remove** change it. For the selected
interface:

| Field | Meaning |
|---|---|
| Name | a short name of your choice (`eth0`, `wifi`, ...) |
| Mode | **DHCP** (the router gives the address) or **Static** (you type it) |
| Device | the SANA-II driver; `...` picks one from `DEVS:Networks` |
| Unit | the driver's unit, almost always 0 |
| Address, Netmask, Gateway | for Static: this Amiga's address, its network's mask, the router |
| MTU | the largest packet; leave empty for the driver's own value |
| Speed, RTS/CTS | for a serial SLIP link: the baud rate and hardware handshaking |

With several interfaces, give only one of them a gateway (or use DHCP on
the one that leads to the internet).

## Network

| Field | Meaning |
|---|---|
| Computer name | the Amiga's name on the network (NetBIOS, mDNS, DHCP) |
| Workgroup | the Windows workgroup, kept in `ENV:PrismNet/workgroup` for programs that ask |
| Domain | added to names without a dot: `nas` is looked up as `nas.home.lan` |
| Default gateway | the router, for Static setups (DHCP brings its own) |
| DNS servers | up to three; with DHCP, these replace the ones the router offers |

**Routes...** opens the list of static routes: a destination (`192.168.2.0/24`,
or one address) and the gateway that leads there. You only need these on a
network with more than one router.

**Edit hosts...** opens `ENV:PrismNet/hosts` in your editor (the `EDITOR`
variable, else `C:Ed`): names for addresses, checked before DNS.

## Users

AmigaOS has one user, but some programs ask for a name: mail and news
clients, FTP clients, `usergroup.library`. **User name** and **Real name**
are what they get.

**Accounts...** edits the users and groups that `usergroup.library`
reports, for the servers and for programs that look up accounts. Choose
*Users* or *Groups*. For a user:

| Field | Meaning |
|---|---|
| Name | the login name |
| UID, GID | the user and group numbers |
| Real name | free text |
| Home | the drawer the user starts in (FTP and telnet), e.g. `Work:` |
| Shell | stored for programs that ask; PrismNet's servers don't use it |

**Password...** sets the selected user's password: type it twice (the
fields show `*`). Leave both empty for no password. An account without a
password can't log in to the FTP or telnet server; the list shows it as
*(no password)*.

## Startup

- **Start when needed**: the first program that opens `bsdsocket.library`
  starts PrismNet. Without it, the library only opens while PrismNet runs.
- **Start at boot**: adds `PrismNet START` to `S:User-Startup` (in a marked
  block that Prefs and the installer manage).
- **Memory budget (KB)**: the most memory PrismNet may use for packets and
  connections. Empty means the default. See [Small machines](14-memory.md).
- **Updates**: where **Check now** looks for a newer PrismNet: Off, GitHub,
  Mirror, or GitHub, then mirror. **Check now** checks and, when there is a
  newer release, offers to download and install it.
- **Mirror**: a mirror's address, for Mirror or GitHub, then mirror.
  See [Checking for updates](02-installing.md#checking-for-updates).

## Shares

Network shares as Amiga volumes: an NFS export of a NAS or Linux machine,
or an SMB share of Windows, macOS, a NAS or Samba. The list shows each share;
**Add** and **Remove** change it. See [Network shares](09-shares.md) for
what to put in the fields and how to set up the server side.

| Field | Meaning |
|---|---|
| Device | the DOS device name, e.g. `NAS` (mounted as `NAS:`) |
| Type | NFS or SMB |
| Server | name or address of the server |
| Browse... | (SMB) lists the server's shares to pick from |
| Share | NFS: the export path (`/volume1/amiga`); SMB: the share name |
| Volume | the volume name shown on Workbench |
| User | (SMB) the account on the server; empty for guest |
| UID, GID | (NFS) the identity sent to the server |
| Mount at boot | mount it at every boot (it connects on first use) |
| Read-only | never write to the share |

**Mount now** mounts the selected share, **Dismount** unmounts it.

Each share is one mount file: in `DEVS:DOSDrivers` when it mounts at boot,
else in `SYS:Storage/DOSDrivers`, so you can also mount it by
double-clicking its icon there.

## Services

The servers that let other computers in (see [Servers](10-servers.md)):

- **Run the FTP server** and its **Port** (21).
- **Read-only for everyone**: nobody may change files.
- **Anonymous, read-only, in**: also let `anonymous` in, read-only, inside
  one drawer (`...` picks it).
- **Passive ports**: a range like `50000-50100`, if you need to open them in
  a firewall.
- **Run the telnet server** and its **Port** (23).
- **Run the web server**, its **Port** (80) and the **Folder** it shows
  (`...` picks it).

Logins are the users from *Users > Accounts...* (the web server has none). After Save or Use, PrismNet
restarts its server dispatcher with the new list.
