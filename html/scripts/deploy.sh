#!/usr/bin/env bash
# Deploy: git pull → pip sync → systemctl restart
# Run from any directory; script locates html/ and repo root automatically.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"       # html/
GIT_ROOT="$(git -C "$HTML_DIR" rev-parse --show-toplevel)"

echo "==> git pull ($GIT_ROOT)"
git -C "$GIT_ROOT" pull

echo "==> pip install ($HTML_DIR)"
"$HTML_DIR/.venv/bin/pip" install -r "$HTML_DIR/requirements.txt"

echo "==> systemctl restart iot-server"
sudo systemctl restart iot-server

echo "==> Deploy complete."
