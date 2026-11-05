#!/bin/bash
# Builds Lumen, installs it to ~/.local and starts it with every Plasma login.
# Run ./install.sh --uninstall to remove it again.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX="$HOME/.local"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT="$UNIT_DIR/lumen.service"

if [ "${1:-}" = "--uninstall" ]; then
    systemctl --user disable --now lumen.service 2>/dev/null || true
    rm -f "$UNIT" "$PREFIX/bin/lumen" "$PREFIX/share/applications/io.github.lumen.desktop"
    systemctl --user daemon-reload
    echo "Lumen removed. Your snippets, quick links and clipboard history are kept in"
    echo "~/.config/lumen and ~/.local/share/lumen; delete those folders to remove them too."
    exit 0
fi

for tool in fd wl-copy wl-paste plocate; do
    command -v "$tool" >/dev/null || echo "Note: '$tool' is not installed; see README.md for what it is used for."
done

echo "==> Building"
cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$HERE/build"
cmake --install "$HERE/build" >/dev/null

echo "==> Starting Lumen with your Plasma session"
mkdir -p "$UNIT_DIR"
cat > "$UNIT" <<EOF
[Unit]
Description=Lumen launcher (Meta+Space)
PartOf=graphical-session.target
After=graphical-session.target

[Service]
ExecStart=$PREFIX/bin/lumen
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF
systemctl --user daemon-reload
systemctl --user enable lumen.service >/dev/null 2>&1
if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    systemctl --user restart lumen.service
    echo "    running."
else
    echo "    enabled; it starts with your next Plasma login."
fi

cat <<EOF

Done. Press Meta+Space to open Lumen. To use another shortcut: type
"Lumen Settings" in Lumen, or System Settings → Keyboard → Shortcuts → Lumen.

Optional: to expand snippet keywords typed in any app (;addr), also run
  sudo $HERE/helper/install-helper.sh
EOF
