# About

## Licence

PrismNet is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 3 (GPL-3.0-only). It
comes with no warranty. The full text is in `Docs/LICENSE` in the archive.

lwIP, the networking core inside PrismNet, is under the BSD licence: see
`Docs/lwIP-COPYING`.

## Credits

- **PrismNet**: Laine Jones.
- **lwIP**: Adam Dunkels, Swedish Institute of Computer Science, and the
  lwIP contributors.
- The SMB and NFS clients are written from the public specifications
  (Microsoft's MS-SMB2 and MS-NLMP, RFC 1813 for NFS version 3).

## History

- **1.0.2** (2026-10-21): an install floppy, `PrismNet-<version>.adf` (880 KB),
  for Amigas with no other way to get the archive across yet. To fit on one
  disk it leaves out the manual (in the `.lha` and on GitHub), the
  duplicate network tools of `Tools/Net` and the test programs; the
  installer copies the manual only when it is there.
- **1.0.1** (2026-10-20): WHDLoad games. A game that takes the whole Amiga
  over could hang on a black screen while the network card kept signalling
  (The Settlers with an X-Surf). `PrismNet STOP HOLD` stops the network and
  keeps it stopped until `START`, and the installer offers WHDLoad's hooks
  to do that around every game (see Troubleshooting). `STOP` now returns
  only once PrismNet has really let go of the card. The installer opens in
  Intermediate mode and names itself PrismNet. The update check: a mirror
  that is not a Forgejo repository falls back to `latest.txt`, the result
  line names the step that failed, and a beta looks at the newest version
  rather than at the release GitHub lists first. The manual shows a
  complete SMB mount file.
- **1.0** (2026-10-20): the first full release, on five real Amigas (68030 and 68060;
  X-Surf, ZZ9000, BlueSCSI and ZuluSCSI DaynaPORT, A314, built-in SLIP).
  Network shares: a folder of icons on an SMB share loads about 2.8 times
  faster (the client remembers the folders it just listed and reads a small
  file in one request); `Protect` no longer clears a file's hidden, system
  and archive attributes on the server; a signed SMB session no longer
  accepts unsigned replies. The installer offers to move `smb2-handler` and
  its `filesysbox.library` aside when the SMB client goes in. The manual was
  checked against the program and corrected; *Getting connected* now starts
  with Prefs and PrismMon.
- **0.6.5** (2026-10-20): checks for updates. `C:PrismNetUpdate` (68020 and
  AmiSSL 5) asks GitHub or a mirror whether a newer PrismNet is out, and can
  download and install it; PrismNet Prefs has **Updates**, **Mirror** and
  **Check now** on its Startup page. The installer asks all its questions
  before it stops PrismNet, so an update takes the network away for only a
  few seconds. See [Checking for updates](02-installing.md#checking-for-updates).
- **0.6.4** (2026-10-19): serial links queue a burst of packets instead of
  dropping it: no more lost sends on SLIP, sending at full line speed, and the
  built-in SLIP is now as fast as `slip.device`. A rewritten
  [Serial links](11-serial.md) chapter with every option.
- **0.6.3** (2026-10-19): offers network drivers SANA-II's direct buffers
  (`S2_DMACopyToBuff32`/`S2_DMACopyFromBuff32`), so a driver that supports them
  skips a copy of every packet: faster uploads on the A1200's A314, the
  A4000's ZZ9000 and the X-Surf with xsurfnet.device 1.1.
- **0.6.2** (2026-10-19): with two network cards, a NetBIOS name query is
  answered with the address of the card it came in on; a driver that refuses
  the multicast write command gets plain writes instead.
- **0.6.1** (2026-10-19): an audit: 18 fixes, among them an anonymous FTP
  user reaching other volumes, the SMB client on a crafted reply, and long
  paths in the NFS client; a bigger TCP send buffer (faster uploads).
- **0.6** (2026-10-18): FTP and telnet servers with the inetd dispatcher,
  the Services tab, account passwords in Prefs, the small core in the
  package, this manual.
- **0.5** (2026-10-18): first release. bsdsocket.library and
  usergroup.library, Roadshow's query and configuration calls, PrismNet
  Prefs and PrismMon, NFS and SMB shares, ping, traceroute, nslookup,
  network time.
