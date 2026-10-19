# Network tools

These are in `C:` (and in `Tools/Net` in the archive). They work with any
AmiTCP-compatible stack, not only PrismNet.

## ping

```
ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=s] [WAIT=s] [TTL=n] [QUIET]
```

Sends ICMP echo requests and shows the replies and their times. By default
4 pings of 56 bytes, one second apart; `COUNT=0` pings until CTRL-C. `QUIET`
shows only the summary. `HOST` is a name or an address.

## traceroute

```
traceroute HOST [MAXHOPS=n] [QUERIES=n] [WAIT=s] [UDP]
```

Shows each router on the way to `HOST`. It sends ICMP echo probes with a
rising time-to-live, like Windows' `tracert`; `UDP` sends classic UDP probes
instead.

## nslookup

```
nslookup NAME [TYPE=A|AAAA|MX|NS|CNAME|PTR|TXT|SOA|ANY]
         [SERVER=address] [PORT=n] [TIMEOUT=s]
```

Asks a DNS server directly and shows the full answer. The server is
`SERVER=`, else PrismNet's first name server. An address as `NAME` does a
reverse lookup; a name ending in `.local` is asked by mDNS.

## SMBShares

```
SMBShares HOST [USER=name] [DOMAIN=name] [ALL]
```

Lists the shares an SMB server offers (installed with the SMB client).
Without `USER=` it asks as a guest; with it, it uses the password the SMB
client saved for that server and user, or asks for one in the Shell. `ALL`
also shows the hidden shares (names ending in `$`). `HOST` may be a Windows
(NetBIOS) name.

## ShareUnmount

```
ShareUnmount DEVICE
```

Unmounts a PrismNet NFS or SMB share, e.g. `ShareUnmount NAS:`.

## httpbench

```
httpbench HOST [PORT=n] [PATH=/file] [COUNT=n] [CONNECTS=n]
          [UPLOAD=kbytes] [SINKPORT=n] [BUFSIZE=bytes] [CALLS=n]
```

A benchmark for the stack: download speed (`COUNT` HTTP GETs of `PATH`),
connections per second (`CONNECTS`), upload speed (`UPLOAD` KB to a discard
server on `SINKPORT`), and the cost of one library call (`CALLS`). In
`Tools/Net` only.

## bsdtest, ugtest

In `Tools`: self-tests of `bsdsocket.library` (needs internet access) and
`usergroup.library`. Each should end with `PASS`. Send their output with a
problem report.
