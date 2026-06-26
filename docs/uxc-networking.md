# uxc container networking on OpenWrt

uxc itself does **not** set up networking — it only creates the namespaces. The
container's connectivity is handled by OpenWrt's normal network stack (netifd +
`/etc/config/network`). This is the least-documented part of uxc, so here is what
actually happens and how to configure it.

There are two models. Pick one with `docker2uxc --network host|isolated`.

## 1. Host networking (`--network host`, the default)

The container does **not** get its own network namespace — it shares the host's.
Consequences:

- It sees the host's interfaces, routes and `/etc/resolv.conf` directly.
- It binds ports straight onto the host (no port mapping, no NAT).
- Nothing to configure in `/etc/config/network`.

This is the simplest mode and the right default for most appliances. Frigate in
particular is normally run host-networked (go2rtc/RTSP/mDNS discovery expects it).
The downside is no isolation: a port the container binds is a port on the host.

## 2. Isolated networking (`--network isolated`)

The container gets its **own** network namespace. Now it has no interfaces until
you give it one, and — importantly — uxc tries to set up the container's
`/etc/resolv.conf` as a symlink into `/dev/resolv.conf.d/...` that **netifd only
populates when an interface for the container exists**. If you start an isolated
container with no matching network config you get:

```
jail: symlink() failed to create link to ../dev/resolv.conf.d/resolv.conf.auto
```

and the container fails to come up. So isolated mode *requires* host-side config.

### How OpenWrt wires a netns container

The mechanism is a **veth pair** plus a netifd protocol that hands one end into
the container's namespace:

```
            host network namespace                 container netns
   ┌───────────────────────────────┐        ┌───────────────────────┐
   │  br-lan ── vh-<name> ●━━━━━━━━━╋━━━━━━━━╋━● <name>0             │
   │  (bridge)  (host end)          │  veth  │  (peer, proto 'infra')│
   └───────────────────────────────┘        └───────────────────────┘
```

- A `config device` of `type 'veth'` defines the pair: `name` is the host end,
  `peer_name` is the end that will live inside the container.
- The host end is added to a bridge (e.g. `br-lan`) so the container shares that
  L2 segment, or to a dedicated bridge you firewall separately.
- A `config interface` with **`option proto 'infra'`** on the peer device tells
  netifd/uxc to move that device into the matching container's namespace and
  manage it (address, DNS) from there. uxc matches it to the container by name.

`docker2uxc --network isolated` (or `--emit-netconfig`) writes exactly this stanza
to `network.uci` in the bundle, named after the container, attached to the bridge
from `--net-bridge` (default `br-lan`). It is **not** applied — review it, adjust
the bridge/addressing, then:

```sh
cat network.uci >> /etc/config/network
/etc/init.d/network reload
```

### Addressing

With the container bridged onto `br-lan`, it gets an address the same way other
LAN clients do (your LAN DHCP, or a static one). For a separate subnet, attach
`vh-<name>` to a dedicated bridge and give that bridge interface its own
`proto 'static'` address + DHCP/firewall — standard OpenWrt, nothing uxc-specific.

### DNS

Don't bind-mount `/etc/resolv.conf` in isolated mode — netifd manages the
container's resolver through the `proto 'infra'` interface (that's what the
`/dev/resolv.conf.d` symlink is for). `--resolv-conf` exists for host mode or odd
cases; leave it off here.

### Firewall

A netns container is just another L3 endpoint. Put its interface in a firewall
zone (reuse `lan`, or make a `container` zone) and add forwarding/rules as needed.
uxc adds nothing here either.

## Verifying

```sh
uxc create <name> --bundle <dir>
uxc start  <name>
uxc state  <name>                 # status should be "running"
uxc attach <name>                 # get a shell; then: ip addr / ping / nslookup
```

If start fails on the `resolv.conf.auto` symlink, the interface for `<name>` isn't
up in `/etc/config/network` — fix that first, or switch to `--network host`.

> Note: some uxc builds print a harmless `uxc error: No such file or directory`
> from `uxc start` even when the container already started at `create` time —
> check `uxc state` / `uxc list` rather than trusting that message alone.
