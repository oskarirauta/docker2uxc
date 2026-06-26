# docker2uxc

Turn a Docker/OCI **registry image** into an [OpenWrt **uxc**](https://openwrt.org/)
(procd/ujail) OCI runtime bundle — no Docker, Podman, containerd or skopeo
required on the host.

It pulls the image straight from the registry with nothing but `wget` + `jq`,
flattens the layers into a `rootfs/` (correctly handling AUFS `.wh.` whiteouts),
and writes an OCI `config.json` derived from the image's own config. The result
is a directory you can hand to `uxc create`.

```sh
docker2uxc ghcr.io/blakeblackshear/frigate:0.17.1
uxc create frigate --bundle ./frigate
uxc start  frigate
```

## Why

`uxc` runs OCI bundles but has no way to *fetch* or *flatten* an image — that job
normally belongs to a container engine. `skopeo` can copy an image out of a
registry but still won't flatten it or produce a uxc `config.json`, and it drags
in a chunk of the Podman stack. The whole point here is to need **none** of that:
a single POSIX shell script using tools already present on a stock OpenWrt box.

## Requirements

On the OpenWrt host: `sh`, `wget` (BusyBox/uclient-fetch or GNU), `jq`, `tar`,
`gzip`, `sha256sum`. Optional: `xz`/`unxz` and `zstd`, only if an image ships
layers in those formats (most use gzip).

```sh
opkg update && opkg install jq            # the only one not usually preinstalled
```

## Usage

```
docker2uxc [options] <image-ref>

  <image-ref>   ghcr.io/blakeblackshear/frigate:0.17.1
                alpine:3.20                 (-> docker.io/library/alpine)
                quay.io/prometheus/busybox:latest

Options:
  -o, --out DIR        output bundle directory (default: ./<name>)
  -n, --name NAME      container id / hostname (default: repo basename)
  -a, --arch ARCH      amd64 | arm64 | arm/v7 ...   (default: host arch)
      --profile NAME   apply profiles/<NAME>.json overlay (e.g. frigate)
      --network MODE   host | isolated               (default: host)
      --resolv-conf    bind-mount the host /etc/resolv.conf
      --caps SET       permissive | minimal          (default: permissive)
      --rw-overlay     tune config for a writable overlay
      --cache DIR      blob cache dir (default: /tmp/docker2uxc-cache)
      --no-verify      skip sha256 digest verification
  -f, --force          overwrite an existing output directory
  -v, --verbose
```

The bundle it writes:

```
<out>/
  rootfs/             flattened root filesystem
  config.json         OCI runtime spec for uxc (generated)
  image-config.json   the original image config, for reference
  manifest.json       the per-arch manifest, for reference
  README.notes        entrypoint, exposed ports, declared volumes, install cmds
```

## What is taken from the image, and what you must add

The image config gives the **defaults** and they are filled in automatically:
entrypoint + cmd → `process.args`, environment → `process.env`, working dir,
and the user (numeric, or resolved against the rootfs `/etc/passwd`).

What an image **cannot** tell you is host-specific, so you add it yourself:

- **Data**: bind-mount your storage (e.g. recordings) and config directories.
- **Devices**: pass through `/dev/dri`, a Coral TPU, USB, etc. as bind mounts.
- **Ports**: uxc does no port mapping (see Networking below). The image's
  `ExposedPorts` are listed in `README.notes` for reference.

[Profiles](profiles/README.md) bundle these host-specific additions per
application. `--profile frigate` adds a sized `/dev/shm`, `/dev/dri`,
`/config` + `/media` binds and `CAP_SYS_ADMIN` — you just edit the `source`
paths. Copy `profiles/_template.json` to make your own.

## Networking

This is the part of running a uxc container that the image cannot describe, and
it is the most common reason a freshly converted container won't start.

**`--network host` (default)** — the container shares the host network namespace.
Nothing to configure: it reaches the network exactly as the host does, DNS
included, and binds ports directly on the host. This is the simplest mode and is
the usual way to run Frigate (go2rtc/RTSP discovery wants host networking anyway).

**`--network isolated`** — the container gets its own network namespace. uxc then
expects a matching interface in the host's `/etc/config/network`; without it the
jail can't set up `/etc/resolv.conf` and the container fails to start. In this mode
docker2uxc **emits** a ready veth/`proto infra` snippet to `network.uci` in the
bundle (host bridge from `--net-bridge`, default `br-lan`) but does **not** apply
it — review, edit addressing, then:

```sh
cat network.uci >> /etc/config/network && /etc/init.d/network reload
```

The full mechanism (veth pairs, `proto infra`, DNS, firewall) is documented in
**[docs/uxc-networking.md](docs/uxc-networking.md)** — uxc's own docs barely cover
it. Start with `--network host` unless you specifically need isolation.

## Frigate, end to end

```sh
# 1. build the bundle (edit profiles/frigate.json source paths first)
docker2uxc --profile frigate -o /srv/uxc/frigate \
           ghcr.io/blakeblackshear/frigate:0.17.1

# 2. create your host data dirs to match the profile
mkdir -p /srv/frigate/config /srv/frigate/media
cp my-frigate-config.yml /srv/frigate/config/config.yml

# 3. register and start
uxc create frigate --bundle /srv/uxc/frigate
uxc start  frigate
uxc enable frigate          # autostart on boot
```

Frigate runs **s6-overlay** as PID 1 (entrypoint `/init`); the generated config
keeps that and provides the writable `/run` and `/tmp` it expects. Size `/dev/shm`
to your cameras (the profile defaults to 256 MB). For hardware the converter
cannot know about — a Coral TPU (`/dev/apex_0` PCIe or `/dev/bus/usb` USB) or a
specific VAAPI device — add the bind mount in `config.json` or your profile.

## Limitations

- **Public images only** (anonymous pull). No registry login yet.
- Layer compression: gzip and plain tar work out of the box; `xz` and `zstd`
  need the matching tool installed.
- `--network isolated` needs you to provision the container interface in
  `/etc/config/network` yourself (see Networking); the default `host` mode does not.
- Device pass-through is a profile/host concern; this tool produces a sane
  baseline `config.json`, not a turnkey runtime for every workload.

## Auto-restart (Docker's `restart: unless-stopped`)

uxc has **no restart policy** — `uxc enable` only starts a container on boot, it
does not respawn one that exits. That matters for apps which exit on purpose to
reload config: Frigate's web UI **"Save & Restart"** terminates the process
(exit 143 / SIGTERM) expecting the runtime to bring it back; under plain uxc it
just stays stopped.

With `--emit-keeper`, docker2uxc writes a procd **keeper** service, `<name>.init`,
that supplies that policy **without bypassing uxc** — the container stays a normal
uxc container (visible in `uxc list`, controllable with `uxc state|kill|attach`);
the keeper re-creates and starts it whenever it isn't running (it polls
`uxc start`, since how long a container takes to become start-ready scales with
image size). It is **opt-in** — most containers don't need it, and you don't want
`/etc/init.d` filling up with keepers.

```sh
docker2uxc --emit-keeper ...                 # generates <name>.init
cp <name>.init /etc/init.d/<name>-keeper && chmod +x /etc/init.d/<name>-keeper
/etc/init.d/<name>-keeper enable && /etc/init.d/<name>-keeper start
```

Stop the container with `/etc/init.d/<name>-keeper stop` — a plain `uxc kill`
would just be restarted by the keeper (like Docker's `restart: always`). Verified:
calling Frigate's `/api/restart` (what the web UI's "Save & Restart" does) brought
the container back automatically in ~25 s, still listed by `uxc`. Use the plain
`uxc` method for simple containers, the keeper for anything that restarts itself.

## Troubleshooting

Hard-won gotchas when starting a converted container with uxc:

- **`jail: mount_all() failed` / `failed to build jail fs`** — a bind mount source
  doesn't exist on the host. ujail stats *every* bind source and fails the whole
  container if one is missing. Create the dirs/files first (docker2uxc prints a
  WARNING listing missing sources). Note OpenWrt usually has **no `/etc/localtime`**
  — the Frigate profile uses a `TZ` env var instead of bind-mounting it.
- **`bind() ... Address already in use`** — with `--network host` the container
  binds ports directly on the host. Free the port or use `--network isolated`.
- **`uxc create <name> --bundle <dir>`** — the first argument is the container
  *name* and `--bundle` points at the bundle **directory** (the one containing
  `config.json` + `rootfs/`), not at `config.json` itself. Passing the file path
  gives `uxc error: No such file or directory`.
- **`uxc error: No such file or directory` from `create`/`start`** — on some
  uxc/procd builds this message is **cosmetic**: the container is brought up
  **asynchronously** and a heavy image (Frigate) takes ~10-30 s to boot. Don't
  trust the message or an immediate status check — wait, then look at
  `uxc state <name>` (expect `"running"`). You can also verify the bundle directly
  with `ujail -n <name> -J <bundle> -i` (exactly what uxc wraps).

This project was validated by booting Frigate 0.17.1 from a converted bundle:
s6-overlay came up, the web UI answered on `:5000` (`/api/version` →
`0.17.1-416a9b7`), and go2rtc/RTSP/WebRTC listened on `:1984/:8554/:8555`.

## Testing

```sh
sh test/test_whiteout.sh     # validates layer flattening + whiteout handling
```

## License

MIT — see [LICENSE](LICENSE).
