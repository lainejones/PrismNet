# PrismNet manual

PrismNet is a free, open-source TCP/IP stack for classic Amigas running
AmigaOS 3.1 or newer. It connects the Amiga to your network and the internet
through any SANA-II network driver, and it gives programs the same
`bsdsocket.library` that AmiTCP, Miami and Roadshow give them, so web
browsers, FTP and IRC clients, mail programs and the rest run unchanged.

This manual is for people who use PrismNet. It ships with the package as
`PrismNet.guide` (an AmigaGuide document made from these pages), and the same
pages are on GitHub.

## Contents

1. [Introduction](01-introduction.md): what PrismNet is, what it needs, what it has been tested on
2. [Installing](02-installing.md): the installer, updating, moving over from another stack, removing
3. [Getting connected](03-getting-connected.md): the first start and how to check it works
4. [PrismNet Prefs](04-prefs.md): every tab of the settings editor
5. [PrismMon](05-prismmon.md): the network monitor
6. [The PrismNet command](06-command.md): START, STOP, STATUS and the rest
7. [Settings files](07-settings.md): what is in `ENVARC:PrismNet`
8. [Network tools](08-tools.md): ping, traceroute, nslookup and friends
9. [Network shares](09-shares.md): a NAS or PC folder as an Amiga volume (NFS, SMB)
10. [Servers](10-servers.md): FTP and telnet into the Amiga
11. [Serial links](11-serial.md): SLIP and PPP over the serial port
12. [Small machines](12-memory.md): memory use, and PrismNet on a 68000 with 1 MB
13. [Troubleshooting](13-troubleshooting.md): when something doesn't work
14. [For programmers](14-programmers.md): the API PrismNet provides
15. [About](15-about.md): licence, credits and history
