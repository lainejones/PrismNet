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

## NetCtl

```
NetCtl IFUP name DEVICE=driver [UNIT=n] [ADDRESS=a] [NETMASK=m] [PEER=p] [MTU=n]
NetCtl IFDOWN name
NetCtl ADDROUTE destination[/bits]|DEFAULT gateway
NetCtl DELROUTE destination[/bits]|DEFAULT [gateway]
NetCtl GATEWAY
NetCtl ROUTES
NetCtl DNS address [address] [address]
NetCtl DNS PUSHED
NetCtl DNSRESTORE
```

Changes the running network, for scripts such as the [VPN](13-vpn.md) ones.
The changes last until PrismNet stops; the settings files aren't touched.
`QUIET` leaves out the confirmation line.

| Action | Does |
|---|---|
| `IFUP` | adds an interface for a SANA-II driver (with `DEVICE=`), or changes one that is there, and switches it on. `PEER=` is the far end of a point-to-point link |
| `IFDOWN` | removes an interface; only the one added last can go |
| `ADDROUTE` | a route to a network (`10.8.0.0/24`) or to one address, through `gateway`. `DEFAULT` sets the default gateway |
| `DELROUTE` | removes a route |
| `GATEWAY` | prints the default gateway, for a script: ``Set gw `NetCtl GATEWAY` ``. Returns WARN when there is none |
| `ROUTES` | lists the routes |
| `DNS` | uses these name servers instead of the current ones. The first `DNS` notes the old ones for `DNSRESTORE`. `DNS PUSHED` takes the ones an OpenVPN server pushed, from the variables OpenVPN gives its scripts |
| `DNSRESTORE` | puts back the name servers `DNS` replaced |

NetCtl uses Roadshow's configuration calls, so it should also work with
Roadshow (not yet tried there). In a script, a value that is still an unset
`${variable}` counts as not given.

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

## PrismNetUpdate

In `C:` on a 68020 or better: checks for a newer PrismNet and can install
it. See [Checking for updates](02-installing.md#checking-for-updates).

## bsdtest, ugtest

In `Tools`: self-tests of `bsdsocket.library` (needs internet access) and
`usergroup.library`. Each should end with `PASS`. Send their output with a
problem report.
