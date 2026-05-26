# MasterBandit
A terminal emulator

## Building

Requires [vcpkg](https://github.com/microsoft/vcpkg) for dependency management.

Set `VCPKG_ROOT` to your vcpkg installation:

```sh
export VCPKG_ROOT=/path/to/vcpkg
```

### Linux (Debian/Ubuntu) prerequisites

```sh
sudo apt install pkg-config libxcb1-dev libxcb-util-dev libxcb-sync-dev \
    libxcb-xkb-dev libxcb-cursor-dev libxkbcommon-dev libxkbcommon-x11-dev \
    libx11-dev libx11-xcb-dev libfontconfig-dev libdbus-1-dev \
    libwayland-dev wayland-protocols
```

`libwayland-dev` provides `libwayland-cursor.so` and the
`wayland-cursor` pkg-config module. `wayland-protocols` must be `>= 1.32`
(Ubuntu 24.04 LTS has 1.34, Debian trixie has 1.36; older distros need a
backport).

### Build presets

Use the per-OS preset that matches your host. The macOS variants pin a custom vcpkg triplet (`arm64-osx-12`) so dependencies are built against the same `MACOSX_DEPLOYMENT_TARGET=12.0` floor as our code and the prebuilt Dawn tarball; the Linux variants use vcpkg's default triplet.

| Build type | macOS                       | Linux                       |
|------------|-----------------------------|-----------------------------|
| Debug      | `cmake --preset macos`         | `cmake --preset linux`         |
| Release    | `cmake --preset macos-release` | `cmake --preset linux-release` |
| Profile    | `cmake --preset macos-profile` | `cmake --preset linux-profile` |

Build with the matching `--build --preset <name>`:

```sh
cmake --preset macos
cmake --build --preset macos
```

The base presets (`default`, `release`, `profile`) still exist as cross-platform fallbacks — they skip the macOS triplet pin, so vcpkg picks the host default. Use them only if you have a reason to bypass the per-OS variant.

## macOS

The build produces a `.app` bundle at `build/bin/mb.app` and ad-hoc signs it automatically as a post-build step (required for `UNUserNotificationCenter` and other privacy-gated APIs to accept the bundle). Launch with `open ./build/bin/mb.app`.

For a release pipeline that signs with a real Developer ID identity, disable the auto-sign so the proper signature isn't immediately overwritten:

```sh
cmake --preset macos-release -DMB_ADHOC_SIGN=OFF
```

## Protocol extensions

- [Selective Mouse Reporting](docs/specs/selective-mouse-reporting.md) — `CSI = w` / `CSI ? w` lets a TUI declare which mouse buttons and event types it wants forwarded, leaving the rest for the terminal's native selection.
