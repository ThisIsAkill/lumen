#!/bin/bash
# Installs lumen-expand-helper as a system service, so Lumen can expand
# snippet keywords typed in any app. Run with sudo from Lumen's source folder
# after ./install.sh has built Lumen:
#
#   sudo helper/install-helper.sh            install or update
#   sudo helper/install-helper.sh --uninstall
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BINARY=/usr/local/libexec/lumen-expand-helper
CONFIG=/etc/lumen-expand.conf
UNIT=/etc/systemd/system/lumen-expand.service

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo: sudo $0 $*" >&2
    exit 1
fi

if [ "${1:-}" = "--uninstall" ]; then
    systemctl disable --now lumen-expand.service 2>/dev/null || true
    rm -f "$UNIT" "$BINARY" "$CONFIG"
    systemctl daemon-reload
    echo "Removed the snippet expansion helper."
    exit 0
fi

USER_NAME=${SUDO_USER:-}
if [ -z "$USER_NAME" ] || [ "$USER_NAME" = root ]; then
    echo "Run this with sudo from your own account (it reports to that user's Lumen)." >&2
    exit 1
fi
BUILT="$HERE/../build/lumen-expand-helper"
if [ ! -x "$BUILT" ]; then
    echo "Build Lumen first (./install.sh); $BUILT is missing." >&2
    exit 1
fi

# The keyboard layout from Plasma's settings, so typed characters come out right.
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
KXKBRC="$USER_HOME/.config/kxkbrc"
LAYOUT=us
VARIANT=
if [ -f "$KXKBRC" ]; then
    LAYOUT=$(sed -n 's/^LayoutList=\([^,]*\).*/\1/p' "$KXKBRC" | head -n1)
    VARIANT=$(sed -n 's/^VariantList=\([^,]*\).*/\1/p' "$KXKBRC" | head -n1)
    LAYOUT=${LAYOUT:-us}
fi

install -D -m 0755 "$BUILT" "$BINARY"
printf '# Written by install-helper.sh\nuser=%s\nlayout=%s\nvariant=%s\n' "$USER_NAME" "$LAYOUT" "$VARIANT" > "$CONFIG"
chmod 0644 "$CONFIG"
install -m 0644 "$HERE/lumen-expand.service" "$UNIT"
systemctl daemon-reload
systemctl enable lumen-expand.service >/dev/null
systemctl restart lumen-expand.service

echo "Installed. Lumen connects within a few seconds; then type ;keyword in any app"
echo "(keywords are set per snippet in Lumen Settings → Snippets)."
echo "Keyboard layout: $LAYOUT${VARIANT:+ ($VARIANT)}. If you change layouts, run this again."
