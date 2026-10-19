# For programmers

PrismNet's `bsdsocket.library` is a drop-in replacement for AmiTCP's: write
and build against the usual AmiTCP or Roadshow SDK headers, and open the
library as always.

## bsdsocket.library

- **The AmiTCP 4 API**: sockets, `WaitSelect`, the resolver
  (`gethostbyname`, `gethostbyaddr`, `getaddrinfo`, `getnameinfo`),
  `SocketBaseTagList` (errno pointer and size, signal masks, the
  `SBTC_*` tags), `ReleaseSocket`/`ObtainSocket`, `sendmsg`/`recvmsg`,
  socket events (`SO_EVENTMASK` and `GetSocketEvents`).
- **Every opener gets its own library base**, with its own sockets, errno
  and signals, as on AmiTCP.
- **Roadshow's queries**: `ObtainInterfaceList`, `QueryInterfaceTagList`,
  `GetNetworkStatistics` (TCP and UDP socket lists),
  `ObtainDomainNameServerList`, `GetDefaultDomainName`,
  `GetRouteInfo`, and the matching `SBTC_HAVE_*` tags.
- **Roadshow's configuration calls**: `AddRouteTagList`,
  `DeleteRouteTagList`, `ChangeRouteTagList`, `AddInterfaceTagList`,
  `ConfigureInterfaceTagList`, `RemoveInterface`, `AddDomainNameServer`,
  `RemoveDomainNameServer`, `SetDefaultDomainName`. Changes last until
  PrismNet stops; the settings files are not touched.

**Not there yet**: the bpf, mbuf and network monitor hooks and DHCP address
messages (they exist and return 0), alias addresses and IPv6.

## usergroup.library

The AmiTCP user and group API: `getpwnam`, `getpwuid`, `getgrnam`,
`getgrgid`, the `getpwent`/`getgrent` walks, `getuid` and friends, `crypt`,
`getpass`, `umask`. The data comes from `ENV:PrismNet/passwd` and `group`,
or a single user named after `ENV:USER`. `crypt()` is PrismNet's own hash,
not Unix DES.

## Servers under inetd

A server started by `C:inetd` gets the connection as
`SOCKET=<id>` on its command line: call `ObtainSocket(id, AF_INET,
SOCK_STREAM, 0)` and talk on the socket it returns. Any program that does
this can be added to `ENV:PrismNet/services`.

## The source

PrismNet is written in C for Bebbo's amiga-gcc and builds on Linux or WSL;
the networking core is lwIP 2.2.1, unmodified. See `README.md` and
`docs/DEVELOPING.md` in the source for the layout, the build and the tests.
