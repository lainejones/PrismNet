# Installing

Unpack the archive anywhere (`lha x PrismNet-<version>.lha RAM:`), open the
PrismNet drawer and double-click **Install_PrismNet**. It needs the standard
Installer (in `SYS:Utilities` on OS 3.x).

## What the installer does

It asks all its questions first and only then copies anything, so you can
cancel at any question and nothing has changed. In order of what it then
does:

1. **Libraries.** `bsdsocket.library` and `usergroup.library` go to `LIBS:`.
   If another stack's libraries are there, it asks first, and saves them in
   `C:Backup` (the first copy saved is never overwritten).
2. **The command.** `PrismNet` goes to `C:`. If another stack put its own
   `ping`, `traceroute` or `nslookup` in `C:`, you're asked before each is replaced; the
   old ones go to `C:Backup` too. PrismNet's versions are always in
   `Tools/Net` in the archive. On a 68020 or better, `PrismNetUpdate` (the
   [update check](#checking-for-updates)) goes to `C:` as well.
3. **PrismNet Prefs** goes to `SYS:Prefs`, and **PrismMon** wherever you
   choose.
4. **Network shares** (optional): the NFS client, the SMB client, or both.
   See [Network shares](09-shares.md). If `smb2-handler` (and its
   `filesysbox.library`) is installed, choosing the SMB client asks whether to
   move them to `C:Backup`; the library moves only when no other file system
   in `L:` uses it. With both SMB clients installed, FileSysBox can hold the
   system's list of volumes while it waits for another file system, and then
   `List`, `Info` and `Mount` hang until a reboot. "Remove PrismNet" puts them
   back.
5. **Servers** (optional): the FTP, telnet and web servers, any of them. See
   [Servers](10-servers.md).
6. **Settings.** If you have no PrismNet settings yet, it asks for:
   - your network driver (a file requester in `DEVS:Networks`) and its unit,
   - DHCP or a fixed address (then the address, netmask, gateway and DNS
     server),
   - a name for the Amiga on the network,
   - whether PrismNet should start whenever a program needs the network,
   - whether to set the clock from the network.

   It writes them to `ENVARC:PrismNet` and copies them to `ENV:PrismNet`.
   If you already have settings, it offers to keep them.
7. **Starting.** It offers to start PrismNet at every boot (a marked block in
   `S:User-Startup`) and to start it right away.

Commented examples of every settings file are copied to
`ENVARC:PrismNet/examples`.

The PrismNet drawer comes with a plain drawer icon; the installer gives it,
and any drawer it makes (such as `SYS:Storage/OpenVPN` or `SYS:WWW`), your
Workbench's own drawer icon.

## Coming from another stack

PrismNet replaces AmiTCP, Miami, Genesis or Roadshow; only one stack can own
`bsdsocket.library` at a time.

1. Stop the old stack and take it out of your startup files (for example
   remove the `AddNetInterface` or `AmiTCP:bin/startnet` lines from
   `S:User-Startup`).
2. Run the installer. It saves the old libraries in `C:Backup`.
3. Coming from Roadshow, open PrismNet Prefs and use **Project > Import
   Roadshow settings...**: it reads `DEVS:NetInterfaces` and
   `DEVS:Internet` (name servers, routes, hosts) and fills in the tabs.
4. Reboot, so no program still has the old library open.

The installer tells you to reboot at the end if another stack was there.

## Updating

Run the installer from the new archive and choose **Install or update**. It
replaces the programs and keeps your settings.

The installer asks all its questions first, while PrismNet keeps running, so
you can still look things up or cancel. Cancelling at any question leaves the
installed PrismNet as it was. If PrismNet is running, the last question asks
whether to start it again after the update (the default). Then it stops
PrismNet, copies the new files and starts it again: the network is gone for
only a few seconds.

To find out whether there is a newer release, use **Check now** on the
Startup page of PrismNet Prefs, or `C:PrismNetUpdate` in a Shell (see
[Checking for updates](02-installing.md#checking-for-updates)).

## Checking for updates

`C:PrismNetUpdate` asks whether a newer PrismNet has been released, and can
download and install it. The installer puts it in `C:` on a 68020 or better;
it needs PrismNet running and **AmiSSL 5**, because everything is fetched
over HTTPS. It checks the server's certificate against the ones in
`AmiSSL:Certs`; without them it says the connection is not verified and
carries on.

It never runs by itself. Check from PrismNet Prefs (**Startup** page):

- **Updates** says where to look: **Off**, **GitHub** (PrismNet's releases
  on GitHub), **Mirror**, or **GitHub, then mirror** (the mirror only when
  GitHub can't be reached).
- **Mirror** is the mirror's address (see below).
- **Check now** checks with the settings in the window and tells you the
  result. When a newer release exists, **Download and install** fetches it to
  `T:`, unpacks it and starts its installer in a window; the installer's
  usual questions follow.

Or from a Shell:

```
PrismNetUpdate [GET] [FETCH] [SOURCE=GITHUB|MIRROR|BOTH]
               [MIRROR=https://...] [QUIET]
```

Without arguments it checks with the settings saved by Prefs (`UPDATES=` and
`MIRROR=` in `ENV:PrismNet/startup`). `SOURCE=` and `MIRROR=` override them.
`GET` downloads, unpacks and runs the installer; `FETCH` only downloads and
unpacks it to `T:`. The return code says what it found: 0 up to date, 5 a
newer release exists, 10 it could not check (no stack, no AmiSSL, no
connection), 20 the download or install failed. So a script can check
quietly (`FailAt 21` keeps the script going when it could not check):

```
FailAt 21
PrismNetUpdate QUIET
If ERROR
  Echo "Could not check for a PrismNet update."
Else
  If WARN
    Echo "A new PrismNet is out."
  EndIf
EndIf
```

GitHub answers about 60 checks an hour from one internet address (all the
computers behind your router count together). When that is used up the check
says so; try again later, or set **GitHub, then mirror** so a mirror answers
instead.

### A mirror

A mirror lets machines check somewhere other than GitHub, for example your
own server, or a network without access to GitHub. The address is either:

- a **Forgejo or Gitea repository** that carries PrismNet's releases, e.g.
  `https://git.example.org/someone/PrismNet`: its release list is read like
  GitHub's; or
- a **directory** on any HTTPS server holding a file `latest.txt`, whose first
  line is the version and the archive:

  ```
  0.6.5 PrismNet-0.6.5.lha
  ```

  The archive is a name in the same directory or a full `https://` address.
  Put the release archive next to it and change the line for each release.

## Removing

Run the installer and choose **Remove PrismNet**. It deletes PrismNet's
files (only its own: anything another program put in the same place is
left alone), takes the boot entry out of `S:User-Startup`, puts back what it
saved in `C:Backup`, and asks whether to delete your settings in
`ENVARC:PrismNet` too.

## Installing by hand

Everything is plain files, so you can also copy them yourself:

| From the archive | To |
|---|---|
| `Libs/bsdsocket.library`, `Libs/usergroup.library` | `LIBS:` |
| `C/PrismNet`, `C/ping`, `C/traceroute`, `C/nslookup` | `C:` |
| `C/PrismNetUpdate` (the update check; 68020 or better) | `C:` |
| `Prefs/PrismNet` (+ `.info`) | `SYS:Prefs` |
| `Tools/PrismMon` (+ `.info`), `PrismNet.guide` (+ `.info`) | anywhere, e.g. `SYS:Tools` |
| `Storage/PrismNet/*` | edit, then copy to `ENVARC:PrismNet` and `ENV:PrismNet` |
| shares (optional): `L/prismnfs-handler`, `L/prismsmb-handler` | `L:` |
| shares: `C/ShareUnmount`, `C/SMBShares` | `C:` |
| shares: `Storage/DOSDrivers/PNFS0`, `PSMB0` (+ `.info`) | edit, then `DEVS:DOSDrivers` or `SYS:Storage/DOSDrivers` |
| servers (optional): `C/inetd`, `C/ftpd`, `C/telnetd` | `C:`; see [Servers](10-servers.md) |

Then see [Getting connected](03-getting-connected.md).
