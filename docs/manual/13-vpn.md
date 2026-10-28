# VPN

[AmiOpenVPN](https://github.com/Telefonorosso/openvpn) is an OpenVPN 2.6
client for AmigaOS: it connects the Amiga to an OpenVPN server and encrypts
the traffic with AmiSSL. Its packets reach the TCP/IP stack through its own
`vpn.device`, a SANA-II driver that PrismNet uses like any other network
card. AmiOpenVPN is a separate project; it isn't part of PrismNet.

PrismNet comes with the scripts OpenVPN runs when the tunnel comes up and
goes down. They are in `SYS:Storage/OpenVPN`, along with `StartVPN`, which
starts a connection.

## What you need

- AmiOpenVPN (`openvpn`) and its `vpn.device`, from its release.
- AmiSSL 5.
- A 68020 or better; the encryption is a lot of work for an Amiga.
- An OpenVPN server for a *tun* (routed IPv4) tunnel, and the files it gives
  you: the CA certificate, your certificate and key, and often a
  `tls-crypt` key.

## Setting it up

1. Copy `vpn.device` to `DEVS:Networks`, and `openvpn` and your keys and
   certificates to a drawer of your choice. Assign `OPENVPN:` to it in
   `S:User-Startup`:

   ```
   Assign OPENVPN: Work:OpenVPN
   ```

2. Copy the three scripts from `SYS:Storage/OpenVPN` into the same drawer,
   and `StartVPN` too.

3. Make the configuration file, for example `OPENVPN:client.ovpn`. Start
   from what your server gives you, and add these lines:

   ```
   ifconfig-noexec
   route-noexec
   tun-mtu 1400
   script-security 2
   up OPENVPN:PrismNetUp.script
   route-up OPENVPN:PrismNetRouteUp.script
   route-pre-down OPENVPN:PrismNetDown.script
   ```

   `ifconfig-noexec` and `route-noexec` leave the network settings to the
   scripts. Point `ca`, `cert`, `key` and `tls-crypt` at the files in
   `OPENVPN:`.

4. Connect:

   ```
   Execute OPENVPN:StartVPN OPENVPN:client.ovpn
   ```

   CTRL-C in that Shell (or `Break` on the `openvpn` process) ends the
   connection.

## What the scripts do

| Script | When | Does |
|---|---|---|
| `StartVPN` | before OpenVPN starts | adds `vpn.device` unit 0 as interface `vpn0`, so OpenVPN finds the driver, then runs OpenVPN |
| `PrismNetUp.script` | the tunnel is up | gives `vpn0` the address the server assigned |
| `PrismNetRouteUp.script` | routes may be set | keeps the way to the VPN server on your normal gateway, sends everything else through the tunnel (`0.0.0.0/1` and `128.0.0.0/1`), and uses the name servers the server pushed |
| `PrismNetDown.script` | the tunnel goes down | removes the routes, puts the old name servers back and removes `vpn0` |

They do all of it with [NetCtl](08-tools.md#netctl). To keep your own
traffic off the tunnel and send only the server's network through it, change
the two `ADDROUTE` lines for `0.0.0.0/1` and `128.0.0.0/1` in
`PrismNetRouteUp.script` to the network you want, for example
`NetCtl ADDROUTE 10.8.0.0/24 ${route_vpn_gateway}`, and the matching
`DELROUTE` lines in `PrismNetDown.script`.

`PrismNet STATUS` shows `vpn0` while the tunnel is up, and `PrismNet ROUTE`
shows the routes.

## When it doesn't connect

- **"certificate is not yet valid"**: the Amiga's clock is behind, or keeps
  local time while certificates use UTC. Set the clock (PrismNet can set it
  from the network: `PrismNet SETCLOCK`); a server certificate made minutes
  ago can still be "in the future" for an Amiga a few time zones west.
- **"cannot open vpn.device unit 1"**: OpenVPN was started without
  `StartVPN`, so `vpn.device` wasn't loaded yet. Run
  `NetCtl IFUP vpn0 DEVICE=vpn.device` first.
- **The tunnel comes up but nothing gets through**: check with
  `PrismNet ROUTE` that there is a route to the VPN server's address via your
  normal gateway. Without it OpenVPN's own packets go into the tunnel.
- AmiOpenVPN prints "cannot open MiamiDX routing socket": harmless here; the
  scripts ask PrismNet for the gateway instead.
