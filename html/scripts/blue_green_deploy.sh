#!/usr/bin/env bash
# Blue/green slot swap for the IoT server.
# Runs ON the server after new code has been extracted to ~/html/.
#
# One-time setup (run once before first blue/green deploy):
#   sudo cp /home/akp/html/systemd/iot-server-blue.service /etc/systemd/system/
#   sudo cp /home/akp/html/systemd/iot-server-green.service /etc/systemd/system/
#   sudo systemctl daemon-reload
#   sudo systemctl enable iot-server-blue iot-server-green
#   # Remove the inline upstream block from /etc/nginx/conf.d/iot_server.conf
#   # (delete the 'upstream fastapi_backend { ... }' stanza)
#   sudo cp /home/akp/html/nginx/iot_upstream_blue.conf /etc/nginx/conf.d/iot_upstream.conf
#   sudo nginx -s reload
#   sudo systemctl start iot-server-blue
#   echo "blue" > /home/akp/html/etc/active_slot
#   # Stop the old single-slot service if still running:
#   sudo systemctl stop iot-server && sudo systemctl disable iot-server || true
#
# Usage: bash ~/html/scripts/blue_green_deploy.sh

set -euo pipefail

HTML_DIR=/home/akp/html
SLOT_FILE="$HTML_DIR/etc/active_slot"
NGINX_CONF_DIR=/etc/nginx/conf.d
HEALTH_TIMEOUT=30

# Determine active and inactive slots.
if [[ -f "$SLOT_FILE" ]] && [[ "$(cat "$SLOT_FILE")" == "green" ]]; then
    ACTIVE_SLOT=green
    INACTIVE_SLOT=blue
    INACTIVE_PORT=8000
else
    ACTIVE_SLOT=blue
    INACTIVE_SLOT=green
    INACTIVE_PORT=8001
fi

echo "==> Active: $ACTIVE_SLOT  →  deploying to: $INACTIVE_SLOT (port $INACTIVE_PORT)"

# Install/update Python dependencies.
"$HTML_DIR/.venv/bin/pip" install -r "$HTML_DIR/requirements.txt" -q

# Start the inactive slot.
sudo systemctl start "iot-server-$INACTIVE_SLOT"

# Health-check until the new slot is serving or timeout.
echo "==> Health-checking http://127.0.0.1:$INACTIVE_PORT/health"
DEADLINE=$((SECONDS + HEALTH_TIMEOUT))
until curl -sf "http://127.0.0.1:$INACTIVE_PORT/health" > /dev/null 2>&1; do
    if [[ $SECONDS -ge $DEADLINE ]]; then
        echo "ERROR: health check timed out after ${HEALTH_TIMEOUT}s — rolling back"
        sudo systemctl stop "iot-server-$INACTIVE_SLOT"
        exit 1
    fi
    sleep 1
done
echo "==> $INACTIVE_SLOT healthy"

# Atomically swap the Nginx upstream and reload.
sudo cp "$HTML_DIR/nginx/iot_upstream_$INACTIVE_SLOT.conf" "$NGINX_CONF_DIR/iot_upstream.conf"
sudo nginx -s reload
echo "==> Nginx upstream swapped to $INACTIVE_SLOT"

# Stop the now-idle slot.
sudo systemctl stop "iot-server-$ACTIVE_SLOT"

# Record the new active slot.
echo "$INACTIVE_SLOT" > "$SLOT_FILE"
echo "==> Deploy complete. Active slot: $INACTIVE_SLOT"
