# docker2uxc profiles

A profile is a small JSON file that is **overlaid** onto the `config.json`
generated from an image. Apply one with:

```sh
docker2uxc --profile frigate ghcr.io/blakeblackshear/frigate:0.17.1
```

Profiles exist because the image itself can only describe *defaults* (entrypoint,
env, working dir, user, exposed ports, declared volumes). Everything host-specific
— **which devices to pass through and where to bind your data** — cannot come from
the image and must be added. A profile is just a reusable bundle of those additions
for a particular application.

## Merge rules

The overlay is deep-merged onto the base config:

| Base value | Overlay value | Result |
|------------|---------------|--------|
| object     | object        | merged key by key (recursive) |
| array      | array         | **appended** (base items kept, overlay items added) |
| anything   | scalar        | overlay replaces base |

Because arrays append, listing a mount or a capability **adds** it to the
defaults — it never removes one. To drop or change a default, edit the generated
`config.json` directly after conversion.

Keys whose name starts with `_` (e.g. `_comment`) are documentation only and are
stripped from the final config.

## Writing a profile

1. Copy `_template.json` to `<name>.json`.
2. Edit every bind `source` to a real host path. `source` is on the **host**,
   `destination` is inside the container.
3. Add device pass-through as bind mounts (uxc/ujail style), e.g.
   - VAAPI / iGPU: `/dev/dri`
   - Coral USB TPU: `/dev/bus/usb`
   - Coral PCIe TPU: `/dev/apex_0`
4. Add a `tmpfs` for `/dev/shm` if the app needs more than the tiny default.
5. Add extra capabilities only if the app needs them.

There is no port mapping in uxc — `ExposedPorts` from the image is informational.
Containers get their own network namespace; wire it up on the host with netifd
(veth) or run the container on the host network. See the top-level README.

## Bundled profiles

- **frigate.json** — Frigate NVR: 256 MB `/dev/shm`, `/dev/dri` (VAAPI),
  `/config` + `/media` binds, `/etc/localtime`, `CAP_SYS_ADMIN`. Edit the
  `source` paths and the shm `size` (scales with camera count/resolution). For a
  Coral TPU add `/dev/bus/usb` (USB) or `/dev/apex_0` (PCIe).
- **_template.json** — annotated boilerplate to start a new profile.
