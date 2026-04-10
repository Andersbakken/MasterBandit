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
    libxcb-xkb-dev libxkbcommon-dev libxkbcommon-x11-dev libx11-dev \
    libx11-xcb-dev libfontconfig-dev
```

### Debug build

```sh
cmake --preset default
cmake --build --preset default
```

### Release build

```sh
cmake --preset release
cmake --build --preset release
```
