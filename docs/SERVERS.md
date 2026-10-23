# PrismNet servers: inetd, ftpd, telnetd and httpd

PrismNet ships its own small servers so other computers can reach the Amiga.
Nothing listens unless `ENV:PrismNet/services` exists.

## inetd

`C:inetd` is the service dispatcher. It listens on the ports listed in
`ENV:PrismNet/services` and, for each connection, starts that service's
program as a process of its own, handing it the connected socket
(`ReleaseSocket(fd, UNIQUE_ID)`, then `<program> SOCKET=<id> <args>`; the
program calls `ObtainSocket(id)`).

**PrismNet runs it for you:** when the core starts and a `services` file
exists, it starts `inetd` (at priority 0); `PrismNet STOP` stops it. Another
path can be set with `INETD=` in `ENV:PrismNet/startup` (default `C:inetd`).
By hand: `inetd [FROM=file] [QUIET]`, CTRL-C stops it.

`ENV:PrismNet/services`, one service a line (`#` starts a comment):

```
name  port  tcp  program  [arguments...]
ftp     21  tcp  C:ftpd     ANONYMOUS=Work:Public
telnet  23  tcp  C:telnetd
http    80  tcp  C:httpd    ROOT=SYS:WWW
```

The installer's *Servers* options install `C:inetd` with any of `C:ftpd`,
`C:telnetd` and `C:httpd`, and write an `ENVARC:PrismNet/services` with the chosen servers
switched on (the full commented sample is `ENVARC:PrismNet/examples/services.sample`).

## PrismNet Prefs: the Services tab

*Run the FTP server*, its port, *Read-only for everyone*, *Anonymous, read-only, in* a
folder (with a folder requester), and the passive ports; *Run the telnet server* and its
port. Prefs edits only the `ftp` and `telnet` lines of `services` and keeps every other line
(comments, other services) as it was; a switched-off server stays as `#ftp ...` /
`#telnet ...` with its options. Save/Use then offers to restart PrismNet,
which restarts inetd with the new list. Prefs also keeps `INETD=` in `startup`.

## ftpd

PrismNet's FTP server (RFC 959), started by inetd per connection.

- **The Amiga as a Unix tree:** `/` lists the mounted volumes, and
  `/Work/Music/x.mod` is `Work:Music/x.mod`. FileZilla, WinSCP and
  command-line `ftp` find their way around.
- **Logins** are PrismNet's users (`ENV:PrismNet/passwd`, PrismNet Prefs >
  Users > Accounts..., where *Password...* sets one): the password is
  checked with usergroup.library's `crypt()`. An account without a password
  can't log in over the network (FTP or telnet). Each user starts in their
  home directory.
- **Commands:** USER PASS QUIT SYST FEAT PWD CWD CDUP TYPE MODE STRU PASV
  EPSV PORT EPRT LIST NLST RETR STOR APPE REST SIZE MDTM DELE MKD RMD RNFR
  RNTO ABOR NOOP STAT HELP. ASCII mode converts line ends (CR LF on the
  wire, LF on the Amiga).
- **Options** (after the program in `services`):

| Option | Meaning |
|---|---|
| `ANONYMOUS=dir` | also let `anonymous`/`ftp` in: read-only, and `/` is `dir` |
| `READONLY` | nobody may change anything |
| `PASVPORTS=a-b` | ports for passive transfers (to open in a firewall) |
| `PASVADDRESS=ip` | address to announce for passive transfers (behind NAT) |
| `IDLE=s` | drop an idle connection after s seconds (default 600) |

Size: `ftpd` 26 KB, `inetd` 13 KB.

## telnetd

An AmigaDOS Shell over telnet (RFC 854), started by inetd per connection.

- **Login** as for ftpd (PrismNet's users, `crypt()`), then the user gets a
  Shell in their home directory. Three wrong passwords end the connection.
- **How:** telnetd adds a DOS device of its own for the session
  (`TNxxxxxx:`) whose handler is telnetd itself, and runs
  `NewShell TNxxxxxx:`. The Shell uses it as its console: line editing
  (backspace, CTRL-U/CTRL-X), CTRL-C/D/E/F breaks, `WaitForChar` and raw
  mode (`SetMode`). Output gets CR LF line ends and the
  Amiga's 0x9B CSI becomes `ESC [` so any terminal follows it.
- **Telnet options:** ECHO, SUPPRESS-GO-AHEAD and BINARY are agreed to;
  everything else is refused. IAC in the data is doubled both ways.
- **The end:** `EndCLI` closes the connection. If the caller just goes away,
  the Shell gets end-of-file and a CTRL-C/D and ends; the device goes when
  the last handle on it is closed.
- **Security:** telnet sends the password as plain text. Use it on your own
  network, never forward the port from the internet.

| Option | Meaning |
|---|---|
| `IDLE=s` | drop a session idle for s seconds (default 1800) |
| `NOLOGIN=user` | no login prompt, everyone is that user (a trusted LAN only) |

## httpd

A small web server (HTTP/1.1, RFC 9110/9112), started by inetd per
connection: the files of one folder, read-only, no login.

- **Requests:** GET and HEAD; anything else is 405. Persistent connections
  (HTTP/1.1 by default, 1.0 with `Connection: keep-alive`) and pipelined
  requests, up to 200 on one connection; one process per connection.
- **Paths:** the URL is %-decoded and split at `/`; an empty part (AmigaDOS
  "parent" in `a//b`), `.`, `..`, `:` and `\` are refused (403), so nothing
  outside `ROOT` can be reached. `dir` redirects to `dir/` (301).
- **Folders:** `INDEX` (default `index.html`) is sent if it exists, else a
  sorted HTML list (folders first, names escaped and %-encoded in the links,
  sizes, dates); `NOLIST` refuses instead.
- **Files:** `Content-Type` by extension, `Content-Length`, single byte
  ranges (`bytes=a-b`, `a-`, `-n` -> 206, past the end -> 416).
- **Limits:** 8 KB of request headers (431 beyond), `IDLE` seconds between
  requests (default 15). No `Date`/`Last-Modified` headers: an Amiga's clock
  has no time zone.

| Option | Meaning |
|---|---|
| `ROOT=dir` | the folder served as `/` (default `SYS:WWW`) |
| `INDEX=name` | a folder's own page (default `index.html`) |
| `NOLIST` | no listings |
| `IDLE=s` | close a connection idle for s seconds (default 15) |
| `LOG=file` | append `address "METHOD target" status bytes` per request |

Size: `httpd` 19 KB.

## Accept queue

A listening socket queues up to 16 connections (`listen()`'s backlog,
inetd asks for 16). Past that a new connection's SYN is dropped, as on BSD,
and the client sends it again; until 1.0.6 it was reset, and a browser
opening 8 connections at once got "connection reset" for some of them.

## Tested (2026-10-23)

`test/host/httptest.py <amiga> [port] [bigfile]` (27 checks: the index page,
the redirect, a listing with escaped names, a name with a space and `&`,
300 KB byte-identical at ~960 KB/s, HEAD, three kinds of range and 416, 404,
eight ways out of `ROOT` refused, POST 405, HTTP/2.0 505, six requests on
one connection, pipelining, HTTP/1.0 closing, 431) passes against the A4000
(ZZ9000Net); an idle connection closes after 15 s.

## Tested (2026-10-18)

`test/host/telnettest.py <amiga> [user] [password]` (11 checks: the option
offer, an account without a password refused, login, the prompt, echo, `version`, `cd`, `list`, CR LF line ends,
EndCLI closing the connection) passes against the A4000; an abrupt
disconnect leaves no Shell, process or `TN` device behind.

`test/host/ftptest.py <amiga> [password]` (30 checks: logins, wrong and empty passwords refused, the volume list, 1 MB
STOR/RETR byte-identical, REST, APPE, ASCII mode, SIZE/MDTM, MKD/RMD/DELE,
RNFR/RNTO, active mode, anonymous read-only and its root) passes against the
A4000 (ZZ9000Net): 1.27 MB/s up, 0.9 MB/s down. With inetd started by the
core: STOP ends it, START brings it back.
