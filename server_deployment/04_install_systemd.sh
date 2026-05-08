#!/usr/bin/env bash
# Install all systemd units on the server.
# Idempotent: re-running updates unit files and reloads the daemon.
#
# Units installed:
#   iot-server-blue.service   — FastAPI app on port 8000
#   iot-server-green.service  — FastAPI app on port 8001 (blue/green standby)
#   refresh-crl.service/.timer — weekly CRL refresh + nginx reload
#   backup-db.service/.timer   — daily PostgreSQL backup
#   restore-test.service/.timer — weekly restore smoke-test
#
# Usage: bash server_deployment/04_install_systemd.sh [ssh_key]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
HTML_DIR_LOCAL="$REPO_ROOT/html"

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HTML_DIR="/home/akp/html"
TMP="/tmp/iot_systemd_$$.tar.gz"

echo "==> Uploading systemd unit files"
tar czf "$TMP" -C "$REPO_ROOT" \
    html/systemd \
    html/monitoring/systemd
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no "$TMP" "$REMOTE":~/iot_systemd.tar.gz
rm -f "$TMP"

ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
cd ~
tar xzf iot_systemd.tar.gz
rm  iot_systemd.tar.gz

echo "  Installing application service units..."
for unit in \
    "$HTML_DIR/systemd/iot-server-blue.service" \
    "$HTML_DIR/systemd/iot-server-green.service"; do
    sudo cp "\$unit" /etc/systemd/system/
done

echo "  Installing CRL refresh units..."
sudo cp "$HTML_DIR/systemd/refresh-crl.service" /etc/systemd/system/
sudo cp "$HTML_DIR/systemd/refresh-crl.timer"   /etc/systemd/system/

echo "  Installing backup units..."
sudo cp "$HTML_DIR/systemd/backup-db.service"    /etc/systemd/system/
sudo cp "$HTML_DIR/systemd/backup-db.timer"      /etc/systemd/system/
sudo cp "$HTML_DIR/systemd/restore-test.service" /etc/systemd/system/
sudo cp "$HTML_DIR/systemd/restore-test.timer"   /etc/systemd/system/

echo "  Installing monitoring units..."
sudo cp "$HTML_DIR/monitoring/systemd/loki.service"     /etc/systemd/system/
sudo cp "$HTML_DIR/monitoring/systemd/promtail.service" /etc/systemd/system/

sudo systemctl daemon-reload

# Enable but don't start standby slot — blue/green setup controls which is active.
sudo systemctl enable iot-server-blue iot-server-green
sudo systemctl enable --now refresh-crl.timer
sudo systemctl enable --now backup-db.timer
sudo systemctl enable --now restore-test.timer
sudo systemctl enable loki promtail

echo "  Checking active slot..."
if [ ! -f "$HTML_DIR/etc/active_slot" ]; then
    echo "blue" > "$HTML_DIR/etc/active_slot"
    echo "  active_slot initialised to blue"
fi

ACTIVE=\$(cat "$HTML_DIR/etc/active_slot")
sudo systemctl start "iot-server-\$ACTIVE"
echo "  started iot-server-\$ACTIVE"

sudo systemctl is-active "iot-server-\$ACTIVE" || { echo "ERROR: service failed to start" >&2; exit 1; }
REMOTE

echo "==> Systemd units installed and active."
echo "    Next: run 05_setup_nginx.sh to deploy the nginx config"
