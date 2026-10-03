#!/bin/sh
# Builds Zen Notepad and installs it for the current user (~/.local).
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" >/dev/null
cmake --build build -j"$(nproc)" --target zen-notepad
cmake --install build >/dev/null
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
echo "Installed: $HOME/.local/bin/zen-notepad"
echo "To make it the default for text files:  xdg-mime default io.github.jirimilicka.zen_notepad.desktop text/plain"
