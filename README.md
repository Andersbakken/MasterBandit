# MasterBandit
A terminal emulator

## Building

Requires [vcpkg](https://github.com/microsoft/vcpkg) for dependency management.

Set `VCPKG_ROOT` to your vcpkg installation:

```sh
export VCPKG_ROOT=/path/to/vcpkg
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
