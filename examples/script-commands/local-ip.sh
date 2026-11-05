#!/bin/bash
# A Raycast script command, as Lumen runs it: copy into
# ~/.config/lumen/script-commands/ (or add this folder in Settings → Commands).
# Inline mode shows its first line under the title, refreshed every minute.

# @raycast.schemaVersion 1
# @raycast.title Local IP Address
# @raycast.mode inline
# @raycast.refreshTime 1m
# @raycast.packageName Network
# @raycast.icon 🌐
# @raycast.description This computer's address on the local network

ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p'
