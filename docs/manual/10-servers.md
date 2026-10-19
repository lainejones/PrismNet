# Servers

PrismNet has its own small servers, so other computers can reach the Amiga:

- **FTP**: copy files to and from the Amiga with any FTP client (FileZilla,
  WinSCP, the `ftp` command).
- **Telnet**: an AmigaDOS Shell on the Amiga, from a terminal on another
  computer.

Nothing listens unless you switch a server on. Install them with the
installer's *Servers* option, then switch them on and off in PrismNet Prefs
(Services tab).

## Accounts

Logins are PrismNet's users: PrismNet Prefs, *Users* tab, **Accounts...**,
*Users*. Each user needs:

- a **Name** to log in with,
- a **Home** drawer, where FTP and telnet start (for example `Work:`),
- a password, set with **Password...**.

An account without a password can't log in to either server.

FTP and telnet send passwords as plain text. Use them on your own network,
and don't forward their ports from the internet to the Amiga.

## How they start

`C:inetd` is the server dispatcher. It listens on the ports in
`ENV:PrismNet/services` and starts the right server for each connection
that comes in. PrismNet starts inetd by itself when it starts, if that file
exists, and stops it with `PrismNet STOP`.

`services` has one line per server:

```
ftp     21  tcp  C:ftpd
telnet  23  tcp  C:telnetd
```

A line starting with `#` is switched off. Options go after the program
name. PrismNet Prefs edits the `ftp` and `telnet` lines and leaves any
other lines alone.

## FTP

The FTP client sees the Amiga as one folder tree:

- `/` lists the mounted volumes,
- `/Work/Music/x.mod` is `Work:Music/x.mod`.

A user starts in their home drawer and can go anywhere from there. Text
mode (`ascii`) converts line ends; binary mode (the default in most
clients) copies files exactly. Passive and active mode both work.

| Option | Meaning |
|---|---|
| `ANONYMOUS=dir` | also let `anonymous` (or `ftp`) in: read-only, and only inside `dir` |
| `READONLY` | nobody may change anything |
| `PASVPORTS=a-b` | the ports for passive transfers, to open in a firewall |
| `PASVADDRESS=ip` | the address to give for passive transfers (behind NAT) |
| `IDLE=s` | close a connection that is idle for s seconds (default 600) |

Example, with a public drawer for anonymous downloads:

```
ftp  21  tcp  C:ftpd ANONYMOUS=Work:Public
```

## Telnet

Connect with any telnet client (`telnet amiga` on Windows, macOS or Linux,
PuTTY, SyncTERM). After the login you get a Shell in your home drawer, as
if you had opened one on the Amiga: every Shell command works, and so do
programs that run in a Shell. Line editing (backspace, CTRL-X), and
CTRL-C/D/E/F work as on the Amiga's own console.

`EndCLI` ends the session. If the connection drops, the Shell gets
end-of-file and ends by itself.

| Option | Meaning |
|---|---|
| `IDLE=s` | close a session that is idle for s seconds (default 1800) |
| `NOLOGIN=user` | no login: everyone is that user. Only on a network you trust completely |

Programs that open their own windows or screens show them on the Amiga,
not in the telnet session.

## Port numbers

Ports 21 (FTP) and 23 (telnet) are the standard ones. To use others, change
the number in Prefs or in `services`, and give it to the client:
`ftp amiga 2121`, `telnet amiga 2323`.
