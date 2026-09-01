# docker2uxc profiles

A profile is a small JSON file that says everything about running one particular
application that the image itself cannot: which devices it needs, where its data
lives, how much shared memory it wants, how to tell whether it is healthy. Apply
one at pull time:

```sh
uxc pull --profile frigate ghcr.io/blakeblackshear/frigate:0.17.2 frigate
docker2uxc --profile frigate --out /srv/uxc/frigate ghcr.io/blakeblackshear/frigate:0.17.2
```

See what an install has, and what each one does:

```sh
uxc profiles          # or: docker2uxc --profiles
```

A profile writes to **two** places:

| Section | Goes into | Holds |
|---------|-----------|-------|
| the top level | the bundle's `config.json` (OCI) | mounts, env, rlimits, capabilities |
| `_registry`   | `/etc/uxc/<name>.json` (uxcd) | volumes, devices, `shm_size`, healthcheck, notes, … |

The `_registry` half is the important one for most applications: device
pass-through, health checks and data volumes are uxcd concepts, they are visible
and editable in LuCI, and they survive an upgrade. Keys the entry **already has
are never overwritten**, so a re-pull or a version jump keeps your edits.

## Merge rules

The top-level section is deep-merged onto the generated `config.json`:

| Base value | Overlay value | Result |
|------------|---------------|--------|
| object     | object        | merged key by key (recursive) |
| `mounts`   | `mounts`      | merged **by destination** (the profile's entry replaces the base mount at the same path) |
| other array| array         | appended |
| anything   | scalar        | overlay replaces base |

Mounts merge by destination on purpose: two mounts on one destination make ujail
reject the *entire* spec (`parsing of OCI JSON spec has failed`), which is how a
profile that re-binds `/config` or `/dev/shm` used to break a container.

Keys whose name starts with `_` never reach `config.json`. Some of them are
directives:

- **`_description`** — one line, shown by `uxc profiles` and in LuCI.
- **`_caps_add: [...]`** — capabilities **added** to whatever `--caps` produced.
  This is what an application profile normally wants. Writing
  `process.capabilities` instead **replaces** the whole set — useful to scope a
  container down, but leave out `CAP_CHOWN` and the container dies on the first
  `chown` its init does. (That is a real bug this profile format has already
  caused: Frigate's s6 init failed on `/dev/shm/logs`.)
- **`_optional: true`** on a mount — the mount is dropped when its host source
  does not exist, instead of failing the container. One profile can then offer
  `/dev/dri` on boxes that have a GPU without breaking those that do not.
- **`_registry: { ... }`** — the uxcd-side fields (below).
- everything else starting with `_` is a comment.

## What belongs in `_registry`

| Key | Why here rather than in `config.json` |
|-----|----------------------------------------|
| `volumes` | `host:container[:ro]`; visible and editable in LuCI, deduped against bundle mounts |
| `devices` | adds the device node **and** the cgroup rule that permits it — a bind mount alone does not. Directories (`/dev/dri`, `/dev/bus/usb`) are bind-mounted live and allowed per major number, so a USB Coral that re-enumerates keeps working. Devices absent from the host are skipped |
| `shm_size` | a sized `/dev/shm`, deduped against the bundle's own mount |
| `healthcheck` | lets uxcd verify an upgrade and roll a bad one back |
| `web_ports` | the click-through icon in the LuCI overview |
| `respawn`, `urls`, `notes` | supervision and documentation |

Not allowed there: `name`, `path`, `image`, `digest`, `created` — a shared
profile must not rewrite a container's identity or provenance.

## Writing a profile

1. Copy `_template.json` to `<name>.json`.
2. Write `_description`.
3. Put data binds in `_registry.volumes` and devices in `_registry.devices`.
   Edit every host path to a real one — uxcd creates a missing bind source
   directory, but it cannot guess which partition you meant.
4. Add `_caps_add` only for what the application actually needs, and say why in
   a `_comment_caps`.
5. Add a `healthcheck` — it is what makes `uxc upgrade` safe for that app.
6. Test: `uxc pull --profile <name> <image> <container>` prints what the profile
   set, which capabilities it added, and which host paths are still missing.

## Bundled profiles

- **frigate.json** — Frigate NVR: `CAP_SYS_ADMIN` + `CAP_PERFMON`, 1280 MB
  `/dev/shm`, `/dev/dri` + Coral TPU pass-through, `/config` + `/media`, and an
  HTTP health check on port 5000. See [docs/frigate.md](../../docs/frigate.md).
- **mosquitto.json** — MQTT broker, config/data binds, TCP check on 1883.
- **postgres.json** — data bind, sized `/dev/shm`, TCP check on 5432.
- **mariadb.json** — data bind, sized `/dev/shm`, TCP check on 3306.
- **icecc.json** — icecream compile node; needs host networking.
- **_template.json** — annotated boilerplate.

There is no port mapping in uxc — `ExposedPorts` from the image is
informational. Containers either share the host network (the default) or get
their own namespace wired up with netifd; see the top-level README.
