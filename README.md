<div align="center">
  <img src="https://github.com/mangowm/mango/blob/main/assets/mango-transparency-256.png" alt="Mango Logo" width="120"/>

  <h1>Mango Wayland Compositor</h1>

  <p>A fast, feature-rich Wayland compositor built on <a href="https://codeberg.org/dwl/dwl">dwl</a></p>

<a href="https://github.com/mangowm/mango/stargazers"><img src="https://img.shields.io/github/stars/mangowm/mango?style=flat&color=orange" alt="Stars"/></a>
<a href="https://github.com/mangowm/mango/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat" alt="License"/></a>
<a href="https://repology.org/project/mangowm/versions"><img src="https://repology.org/badge/tiny-repos/mangowm.svg" alt="Packaged in"/></a>
<a href="https://discord.gg/CPjbDxesh5"><img src="https://img.shields.io/discord/1430889676264177687?style=flat&logo=discord&label=discord" alt="Discord"/></a>

</div>

---

https://github.com/user-attachments/assets/bb83004a-0563-4b48-ad89-6461a9b78b1f

https://github.com/user-attachments/assets/be85e13f-7798-456d-957e-f8931687392e



> See all layouts in action at [mangowm.github.io](https://mangowm.github.io/)

## Why Mango?

Mango starts where dwl ends. It keeps the lightweight, fast-build philosophy while adding the features that make a compositor actually usable day-to-day — without the bloat.

- **Lightweight & fast** — as lean as dwl, no functionality compromised
- **Excellent xwayland support** — run X11 apps without friction(Supports scale without blurring)
- **Tags, not workspaces** — each tag maintains its own independent window layout
- **Smooth animations** — window open/move/close, tag transitions, layer surfaces
- **Flexible layouts** — scroller, master-stack, monocle, dwindle, grid, and more
- **Rich window states** — swallow, minimize, maximize, global, overlay, fakefullscreen
- **Window effects** — blur, shadow, corner radius, opacity (via scenefx)
- **Excellent input method support** — text-input v2/v3
- **Sway-like scratchpad** — named scratchpad support included
- **Hycov-style overview** — see all windows at a glance
- **IPC** — send/receive messages from external programs
- **Hot-reload config** — no restart needed for keybinding changes
- **Zero flickering** — every frame is correct

## Vision

**Stability first.** After months of testing, Mango is solid enough for daily use. Breaking changes will be minimal.

**Practicality over novelty.** Features get added when they genuinely improve daily workflows — not for the sake of completeness.

**Focused scope.** Niche requests are evaluated by community interest. Significant upvotes move things forward.

## Installation

[![Packaging status](https://repology.org/badge/vertical-allrepos/mangowm.svg)](https://repology.org/project/mangowm/versions)

### Arch Linux

```bash
yay -S mangowm-git
```
#### use my config
- install dependencies
```
yay -S rofi foot xdg-desktop-portal-wlr swaybg waybar wl-clip-persist cliphist wl-clipboard wlsunset xfce-polkit swaync pamixer wlr-dpms sway-audio-idle-inhibit-git swayidle dimland-git brightnessctl swayosd wlr-randr grim slurp satty swaylock-effects-git wlogout sox
```
- clone config
```
git clone https://github.com/DreamMaoMao/mango-config.git ~/.config/mango
```

### Other distributions

See the [Installation Guide](https://mangowm.github.io/docs/installation) for Fedora, Gentoo, Guix, NixOS, openSUSE, PikaOS, AerynOS, and building from source.

### Anland 5

The native Anland backend requires the matching `anland5` stack installed in dependency order:

| Order | Dependency | Branch | Notes |
|-------|------------|--------|-------|
| 1 | [wlroots](https://github.com/luochen88/wlroots/tree/anland5) | `anland5` | wlroots 0.20 with external swapchain support |
| 2 | [SceneFX](https://github.com/luochen88/scenefx/tree/anland5) | `anland5` | SceneFX 0.5 built against the wlroots branch above |
| 3 | [Anland](https://github.com/luochen88/anland/tree/anland5) | `anland5` | Installs the `display-producer` pkg-config dependency |
| 4 | Mango | `anland5` | Build this repository after the three libraries above |

`mangobar` remains an ordinary layer-shell client and does not need an Anland-specific branch.

Enable the backend explicitly when configuring Mango:

```sh
meson setup build -Danland=enabled -Dxwayland=enabled
meson compile -C build
```

`ANLAND_SOCKET` opts into the backend; when unset, Mango uses its normal backend
selection unchanged. `ANLAND_DRM_DEVICE` selects the render node (falling back to
`WLR_RENDER_DRM_DEVICE`). A typical Anland session with MangoBar is:

```sh
export ANLAND_SOCKET=/run/display.sock
export ANLAND_DRM_DEVICE=/dev/dri/renderD128
export XDG_RUNTIME_DIR=/run/user/$(id -u)
exec mango -s 'mangobar'
```

The `mangobar` process is respawned by `mango -s`, so it inherits the session
environment and shares the compositor's cgroup.

#### Supervised session

Running the compositor under a systemd user unit gives crash supervision and
control-group cleanup. The session unit (`mango-anland.service`) runs the
`mango-anland` command, which sets the Anland environment, waits for the daemon
socket and render node, and then `exec`s the compositor:

```sh
systemctl --user enable --now mango-anland.service
mango-anland {start|stop|restart|kill|status}
```

`Restart=on-failure` plus `RestartSec=2s` brings the compositor back after a crash;
`kill` additionally clears the failed state so the next `start` is not rate-limited.
Because `ExecSearchPath=` replaces the unit's `$PATH` with just that list, the unit
also restates the full `PATH` — otherwise programs the session spawns (`mangobar`,
`foot`, `swaybg`) would not resolve.

#### Runtime volume control

`anland-volume.sh {get|up|down|toggle|set <0-150>}` maintains `$XDG_RUNTIME_DIR/anland-volume-state`,
which the producer polls every 100 ms to apply volume to the audio stream. Updates are
published with write-then-rename and serialized with `flock` so a `get` running
concurrently with `up`/`down` can neither observe a torn file nor lose an update.

## Documentation

- **[mangowm.github.io](https://mangowm.github.io/)** — website docs with configuration reference, keybindings, layouts, IPC, and more
- **[GitHub Wiki](https://github.com/mangowm/mango/wiki/)** — community-maintained wiki

## Community

Join us on **[Discord](https://discord.gg/CPjbDxesh5)**

## Acknowledgements

- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — Wayland protocol implementation
- [dwl](https://codeberg.org/dwl/dwl) — the foundation Mango builds on
- [scenefx](https://github.com/wlrfx/scenefx) — window effects library
- [owl](https://github.com/dqrk0jeste/owl) — animation groundwork
- [sway](https://github.com/swaywm/sway) — protocol reference

## Sponsor

If Mango makes your desktop better, consider supporting its development.

Thanks to everyone who has sponsored this project:

<table>
  <tr>
    <!-- add new sponsors here: copy the <td>...</td> block below -->
    <td align="center">
      <a href="https://github.com/dl09r">
        <img src="https://unavatar.io/github/dl09r" width="48" style="border-radius:50%"/><br/>
        <sub>dl09r</sub>
      </a>
    </td>
    <td align="center">
      <a href="https://github.com/tonybanters">
        <img src="https://unavatar.io/github/tonybanters" width="48" style="border-radius:50%"/><br/>
        <sub>tonybanters</sub>
      </a>
    </td>
    <td align="center">
      <a href="https://github.com/vinthara">
        <img src="https://unavatar.io/github/vinthara" width="48" style="border-radius:50%"/><br/>
        <sub>vinthara</sub>
      </a>
    </td>
    <td align="center">
      <a href="https://github.com/stepbrobd">
        <img src="https://unavatar.io/github/stepbrobd" width="48" style="border-radius:50%"/><br/>
        <sub>stepbrobd</sub>
      </a>
    </td>
  </tr>
</table>

Crypto donations accepted:

<table>
  <tr>
    <td valign="middle">
      <strong>Network:</strong> BEP20 (BSC)<br/>
      <strong>Address:</strong> <code>0xf9cda472f2556671d2504afc4c35340ec5615da1</code>
    </td>
    <td valign="middle">
      <img width="120" alt="sponsor QR" src="assets/crypto_sponserme_qrcode.png" />
    </td>
  </tr>
</table>
