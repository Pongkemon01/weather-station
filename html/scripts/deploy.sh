#!/usr/bin/env bash
# Final deployment script — upload code + blue/green zero-downtime swap.
#
# Runs from the workstation. Packages app code, uploads via scp, then
# deploys to the inactive slot on the server, health-checks, and atomically
# swaps Nginx traffic.
#
# Prerequisites (one-time on server):
#   sudo systemctl enable iot-server-blue iot-server-green
#   sudo cp ~/html/nginx/iot_upstream_blue.conf /etc/nginx/conf.d/iot_upstream.conf
#   echo "blue" > /home/akp/html/etc/active_slot
#   sudo systemctl start iot-server-blue
#
# Usage:
#   bash html/scripts/deploy.sh [ssh_key]
#   bash html/scripts/deploy.sh           # uses ~/.ssh/akrapong.key
#
set -euo pipefail

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HEALTH_TIMEOUT=30

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"
TMP="/tmp/iot_deploy_$$.tar.gz"

cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

echo "==> Archiving $HTML_DIR"
tar czf "$TMP" \
    --exclude='__pycache__' \
    --exclude='**/__pycache__' \
    --exclude='*.pyc' \
    -C "$(dirname "$HTML_DIR")" \
    "$(basename "$HTML_DIR")/app" \
    "$(basename "$HTML_DIR")/requirements.txt" \
    "$(basename "$HTML_DIR")/scripts" \
    "$(basename "$HTML_DIR")/nginx" \
    "$(basename "$HTML_DIR")/systemd"

echo "==> Uploading to $REMOTE"
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no "$TMP" "$REMOTE":~/iot_deploy.tar.gz
cleanup
trap - EXIT

echo "==> Extracting and deploying on server"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE_SCRIPT
set -euo pipefail

# Extract fresh code
cd ~
tar xzf iot_deploy.tar.gz
rm -f iot_deploy.tar.gz

# Refresh Python deps
~/html/.venv/bin/pip install -r ~/html/requirements.txt -q

# Refresh Nginx main config only on first deploy — it references PKI files under
# ~/html/pki/ which are managed by provision_ca.sh / issue_device_cert.sh and
# are NOT included in the deploy tarball. Overwriting every deploy would break
# nginx -t if pki/private_ca_chain.pem hasn't been (re-)provisioned.
NGINX_CONF_DIR=/etc/nginx/conf.d
if [ ! -f "\$NGINX_CONF_DIR/iot_server.conf" ] || [ ! -f "\$NGINX_CONF_DIR/iot_upstream.conf" ]; then
    sudo cp ~/html/nginx/iot_server.conf "\$NGINX_CONF_DIR/iot_server.conf"
    echo "   Deployed iot_server.conf"
fi
NGINX_CONF_DIR=/etc/nginx/conf.d

# Determine active / inactive slots
HTML_DIR=/home/akp/html
SLOT_FILE="\$HTML_DIR/etc/active_slot"
NGINX_CONF_DIR=/etc/nginx/conf.d

if [[ "\$(cat "\$SLOT_FILE" 2>/dev/null || echo blue)" == "green" ]]; then
    ACTIVE_SLOT=green; INACTIVE_SLOT=blue; INACTIVE_PORT=8000
else
    ACTIVE_SLOT=blue; INACTIVE_SLOT=green; INACTIVE_PORT=8001
fi

echo "   Active: \$ACTIVE_SLOT  →  deploying to: \$INACTIVE_SLOT (port \$INACTIVE_PORT)"

# Stop inactive slot if a stale instance is lingering
sudo systemctl stop "iot-server-\$INACTIVE_SLOT" 2>/dev/null || true

# Start the new slot
sudo systemctl start "iot-server-\$INACTIVE_SLOT"

# Health-check with deadline
HEALTH_DEADLINE=\$((SECONDS + $HEALTH_TIMEOUT))
echo "   Health-checking http://127.0.0.1:\$INACTIVE_PORT/health"
until curl -sf "http://127.0.0.1:\$INACTIVE_PORT/health" > /dev/null 2>&1; do
    if [[ \$SECONDS -ge \$HEALTH_DEADLINE ]]; then
        echo "   ERROR: health check timed out — rolling back"
        sudo systemctl stop "iot-server-\$INACTIVE_SLOT" 2>/dev/null || true
        exit 1
    fi
    sleep 1
done
echo "   \$INACTIVE_SLOT is healthy"

# Atomically swap Nginx upstream
sudo cp "\$HTML_DIR/nginx/iot_upstream_\$INACTIVE_SLOT.conf" "\$NGINX_CONF_DIR/iot_upstream.conf"
sudo nginx -t
sudo systemctl reload nginx

# Stop the old (now-idle) slot
sudo systemctl stop "iot-server-\$ACTIVE_SLOT" 2>/dev/null || true

# Record new active slot
echo "\$INACTIVE_SLOT" > "\$SLOT_FILE"

echo "   Swap complete. Active: \$INACTIVE_SLOT"
sudo systemctl is-active "iot-server-\$INACTIVE_SLOT" > /dev/null
REMOTE_SCRIPT

echo "==> Deploy complete."
