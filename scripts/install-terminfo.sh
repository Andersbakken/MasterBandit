#!/bin/sh
# Install MasterBandit's terminfo entry under $HOME/.terminfo.
#
# Usage: scripts/install-terminfo.sh [path/to/mb]
#
# After install, set TERM=xterm-mb in your shell. Defaults to the build
# output if no mb path is given.

set -eu

mb=${1:-build/bin/mb.app/Contents/MacOS/mb}

if [ ! -x "$mb" ]; then
    echo "error: mb not found or not executable: $mb" >&2
    echo "       pass an explicit path: $0 /path/to/mb" >&2
    exit 1
fi

if ! command -v tic >/dev/null 2>&1; then
    echo "error: tic not found in PATH (install ncurses)" >&2
    exit 1
fi

dest="$HOME/.terminfo"
mkdir -p "$dest"

# -x is mandatory: without it tic silently drops every extension capability
# (Tc, Su, Smulx, Setulc, BE/BD, fe/fd, the modifier-encoded kRIT/kf13+ set,
# etc.). The mb-query-* extensions live in XTGETTCAP only and are filtered
# out of the terminfo source by emit-terminfo.
"$mb" --emit-terminfo | tic -x -o "$dest" -

echo "Installed xterm-mb to $dest" >&2
echo "Activate with: export TERM=xterm-mb" >&2
