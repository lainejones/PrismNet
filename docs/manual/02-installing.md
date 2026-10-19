# Installing

Unpack the archive anywhere (`lha x PrismNet-<version>.lha RAM:`), open the
PrismNet drawer and double-click **Install_PrismNet**. It needs the standard
Installer (in `SYS:Utilities` on OS 3.x).

## What the installer does

1. **Libraries.** `bsdsocket.library` and `usergroup.library` go to `LIBS:`.
   If another stack's libraries are there, it asks first, and saves them in
   `C:Backup` (the first copy saved is never overwritten).
2. **The command.** `PrismNet` goes to `C:`. If another stack put its own
   `ping` or `traceroute` in `C:`, you're asked before each is replaced; the
   old ones go to `C:Backup` too. PrismNet's versions are always in
   `Tools/Net` in the archive.
3. **PrismNet Prefs** goes to `SYS:Prefs`, and **PrismMon** wherever you
   choose.
4. **Network shares** (optional): the NFS client, the SMB client, or both.
   See [Network shares](09-shares.md).
5. **Servers** (optional): the FTP server, the telnet server, or both. See
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
| `Prefs/PrismNet` (+ `.info`) | `SYS:Prefs` |
| `Tools/PrismMon` (+ `.info`) | anywhere, e.g. `SYS:Tools` |
| `Storage/PrismNet/*` | edit, then copy to `ENVARC:PrismNet` and `ENV:PrismNet` |

Then see [Getting connected](03-getting-connected.md).
