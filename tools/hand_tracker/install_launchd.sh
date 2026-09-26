#!/bin/sh
# Installs the hand tracker as a launchd agent (starts at login, restarts on crash, no idle sleep).
#   tools/hand_tracker/install_launchd.sh [camera-ip]
# camera-ip defaults to $ENCO_CAM_HOST, else 192.168.86.65.
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
CAM=${1:-${ENCO_CAM_HOST:-192.168.86.65}}
PY="$DIR/.venv/bin/python"
LABEL=com.enco.handtracker
DEST="$HOME/Library/LaunchAgents/$LABEL.plist"

# These values are pasted into a sed script and an XML file; refuse anything that could break either.
case "$CAM" in
  *[!0-9A-Za-z.-]* | "") echo "not a host name or IP address: $CAM" >&2; exit 1 ;;
esac
case "$DIR$HOME" in
  *[\#\&\<\>\\]*) echo "paths containing # & < > or \\ are not supported: $DIR" >&2; exit 1 ;;
esac

if [ ! -x "$PY" ]; then
  echo "missing $PY - create the venv first (see README.md)" >&2
  exit 1
fi
if [ ! -f "$DIR/models/gesture_recognizer.task" ]; then
  echo "missing model - run: $PY $DIR/hand_tracker.py --download-model" >&2
  exit 1
fi
if [ ! -f "$DIR/.env" ]; then
  echo "missing $DIR/.env with ENCO_TRACK_KEY (see .env.example)" >&2
  exit 1
fi
chmod 600 "$DIR/.env"

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
sed -e "s#__PYTHON__#$PY#g" -e "s#__TRACKER_DIR__#$DIR#g" -e "s#__CAM__#$CAM#g" -e "s#__HOME__#$HOME#g" \
  "$DIR/$LABEL.plist" > "$DEST"
chmod 644 "$DEST"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$DEST"
echo "installed $DEST (camera $CAM); log: ~/Library/Logs/enco-hand-tracker.log"
