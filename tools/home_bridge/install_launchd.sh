#!/bin/sh
# Installs the home bridge as a launchd agent (auto-start at login, restart on crash, no idle sleep).
#   tools/home_bridge/install_launchd.sh [host-ip]
# host-ip defaults to this Mac's Wi-Fi address; it must match HOME_BRIDGE_HOST in home_config.h.
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
HOST=${1:-$(ipconfig getifaddr en0 || true)}
PY="$DIR/.venv/bin/python"
[ -x "$PY" ] || PY=$(command -v python3)
LABEL=com.enco.homebridge
DEST="$HOME/Library/LaunchAgents/$LABEL.plist"

if [ -z "$HOST" ]; then
  echo "no Wi-Fi address found; pass the host IP explicitly" >&2
  exit 1
fi
if [ ! -f "$DIR/.env" ]; then
  echo "missing $DIR/.env" >&2
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
sed -e "s#__PYTHON__#$PY#g" -e "s#__BRIDGE_DIR__#$DIR#g" -e "s#__HOST__#$HOST#g" -e "s#__HOME__#$HOME#g" \
  "$DIR/$LABEL.plist" > "$DEST"
chmod 644 "$DEST"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$DEST"
echo "installed $DEST (host $HOST); log: ~/Library/Logs/enco-home-bridge.log"
