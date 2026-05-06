#!/usr/bin/env bash
# Deploy html/app + requirements.txt to akp@robin-gpu.cpe.ku.ac.th via scp.
# Usage: bash html/scripts/deploy.sh [ssh_key]
# Default key: ~/.ssh/akrapong.key
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"
SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
TMP="/tmp/iot_deploy_$$.tar.gz"

echo "==> Creating archive from $HTML_DIR"
tar czf "$TMP" \
    --exclude='app/__pycache__' \
    --exclude='app/**/__pycache__' \
    --exclude='*.pyc' \
    -C "$(dirname "$HTML_DIR")" \
    "$(basename "$HTML_DIR")/app" \
    "$(basename "$HTML_DIR")/requirements.txt"

echo "==> Uploading to $REMOTE"
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no "$TMP" "$REMOTE":~/iot_deploy.tar.gz
rm -f "$TMP"

echo "==> Extracting and restarting"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'EOF'
cd ~
tar xzf iot_deploy.tar.gz
rm iot_deploy.tar.gz
~/html/.venv/bin/pip install -r ~/html/requirements.txt -q
sudo systemctl restart iot-server
sleep 2
sudo systemctl is-active iot-server
EOF

echo "==> Deploy complete."
