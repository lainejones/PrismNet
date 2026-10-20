# Network shares

PrismNet can mount a folder of another computer as an ordinary Amiga
volume: it appears on Workbench, and every program can open, save, copy and
delete files on it. Two clients come with PrismNet; the installer asks
which ones you want:

- **NFS** (`L:prismnfs-handler`): for a NAS or a Linux machine. Small and
  fast, and the better choice for a 68000 or a machine with little memory.
- **SMB** (`L:prismsmb-handler`): for Windows, macOS, a NAS or Samba. SMB
  2.0.2 up to 3.1.1, with signing and encryption when the server wants them.

A share connects only when it is first used, and lets go of the connection
after a quiet minute (it reconnects by itself). Mounting at boot therefore
costs nothing until you open the volume.

## Setting one up in Prefs

The easiest way: PrismNet Prefs, **Shares** tab, **Add**.

1. **Device**: a name for the DOS device, e.g. `NAS`.
2. **Type**: NFS or SMB.
3. **Server**: the server's name or address.
4. **Share**: for SMB the share name (**Browse...** lists them); for NFS the
   export path, e.g. `/volume1/amiga`.
5. **Volume**: the name shown on Workbench.
6. For SMB, **User**: your account on the server (empty for guest). The
   first time the share is used, a small window asks for the password and
   remembers it.
7. **Mount at boot** if you want it there every time; **Mount now** to try it.

Prefs writes one mount file per share (see the next section) and also lists
shares made by hand, including AmiNFSv3 and smb2fs mount files.

## Mount files

Each share is a standard DOSDriver mount file: in `DEVS:DOSDrivers` when it
mounts at boot, else in `SYS:Storage/DOSDrivers`. Double-click its icon, or
type `Mount SYS:Storage/DOSDrivers/NAS`, to mount it; `ShareUnmount NAS:`
unmounts it. Samples with every option explained are installed as
`SYS:Storage/DOSDrivers/PNFS0` and `PSMB0`.

The whole setup is the `Startup` line:

```
Handler   = L:prismnfs-handler
StackSize = 8192
Priority  = 5
GlobVec   = -1
Startup   = "nas.lan:/volume1/amiga VOLUME=NAS"
```

```
Handler   = L:prismsmb-handler
StackSize = 8192
Priority  = 5
GlobVec   = -1
Startup   = "nas.lan/public VOLUME=NAS USER=amiga"
```

## NFS options

After `server:/export`:

| Option | Meaning |
|---|---|
| `VOLUME=name` | the volume name (default: the last part of the path) |
| `UID=n GID=n` | the identity sent to the server (default 1000/1000) |
| `TZ=+H[:MM]` | the time zone, hours east of UTC (default: from Locale) |
| `READONLY` | never write |
| `RSIZE=n WSIZE=n` | transfer sizes in bytes (default: fits the free memory) |
| `CHARSET=RAW` | names as bytes (default `UTF8`: Latin-1 on the Amiga, UTF-8 on the server) |
| `IDLE=s` | disconnect after s quiet seconds, 0 = never (default 60) |
| `RETRY=s` | keep retrying a lost server for s seconds (default 30) |
| `TIMEOUT=s` | seconds per request (default 15) |
| `DEPTH=1..4` | requests in flight for big transfers (default 1) |
| `RESVPORT=NO` | don't use a source port below 1024 |

**On the server**: export the folder to your network. On a Synology:
*Control Panel > Shared Folder > Edit > NFS Permissions*. On Linux, a line
in `/etc/exports` such as
`/srv/amiga 192.168.1.0/24(rw,no_subtree_check,all_squash,anonuid=1000,anongid=1000)`.
PrismNet connects from a port below 1024, so the server's default "secure"
rule is satisfied.

## SMB options

After `server/share`:

| Option | Meaning |
|---|---|
| `VOLUME=name` | the volume name (default: the share name) |
| `USER=name` | the account on the server; asks for the password once |
| `DOMAIN=name` | the domain or workgroup (default: none; most servers don't need one) |
| `GUEST` | connect without an account |
| `PASSWORD=secret` | the password here instead of the window |
| `TZ=+H[:MM]` | the time zone, hours east of UTC (default: from Locale) |
| `READONLY` | never write |
| `SIGN=AUTO` or `YES` | sign only when the server asks (default), or always |
| `ENCRYPT=AUTO`, `YES` or `NO` | encrypt when the share demands it (default `AUTO`) |
| `MAXPROTO=2.0.2`, `2.1`, `3.0`, `3.0.2` or `3.1.1` | the highest SMB version to use (default 3.1.1) |
| `IDLE=s`, `RETRY=s`, `TIMEOUT=s` | as for NFS |
| `PORT=n` | the server port (default 445) |

**Passwords** are saved after the first successful mount, in
`ENVARC:PrismNet/smb`, so you are asked only once. Delete the file there to
be asked again.

**On the server**: share the folder as usual. Windows 11 requires signing,
which PrismNet always does when asked; it costs some speed on a slow CPU.

## Speed and memory

Around 600 KB/s both ways on a 68030 with fast RAM over Ethernet. A mounted
NFS share costs about 64 KB; SMB with signing is slower on a 68000 or
68020 (the hashing takes CPU time).
