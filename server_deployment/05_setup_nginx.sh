#!/usr/bin/env bash
# Install Nginx configuration files on the server.
# Idempotent: re-running updates configs and reloads nginx.
#
# Deploys:
#   iot_server.conf          — two-vhost config (device mTLS + admin Let's Encrypt)
#   iot_upstream_blue.conf   — upstream template for blue slot (port 8000)
#   iot_upstream_green.conf  — upstream template for green slot (port 8001)
#   iot_upstream.conf        — active upstream (created from blue on first deploy)
#
# Prerequisite: Let's Encrypt cert for adm.robinlab.cc must exist.
#   Obtain with: sudo certbot certonly --nginx -d adm.robinlab.cc
#   before running this script if it's the first time.
#
# Usage: bash server_deployment/05_setup_nginx.sh [ssh_key]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
HTML_DIR_LOCAL="$REPO_ROOT/html"

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HTML_DIR="/home/akp/html"
TMP="/tmp/iot_nginx_$$.tar.gz"

echo "==> Uploading nginx configs"
tar czf "$TMP" -C "$REPO_ROOT" html/nginx
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no "$TMP" "$REMOTE":~/iot_nginx.tar.gz
rm -f "$TMP"

ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
cd ~
tar xzf iot_nginx.tar.gz
rm  iot_nginx.tar.gz

echo "  Deploying iot_server.conf..."
sudo cp "$HTML_DIR/nginx/iot_server.conf" /etc/nginx/conf.d/iot_server.conf

echo "  Copying upstream templates (not the active conf)..."
sudo cp "$HTML_DIR/nginx/iot_upstream_blue.conf"  /etc/nginx/conf.d/iot_upstream_blue.conf
sudo cp "$HTML_DIR/nginx/iot_upstream_green.conf" /etc/nginx/conf.d/iot_upstream_green.conf

# On first deploy: install the active upstream based on active_slot.
# On re-deploy: leave the current active upstream in place.
if [ ! -f /etc/nginx/conf.d/iot_upstream.conf ]; then
    ACTIVE=\$(cat "$HTML_DIR/etc/active_slot" 2>/dev/null || echo "blue")
    sudo cp "$HTML_DIR/nginx/iot_upstream_\${ACTIVE}.conf" /etc/nginx/conf.d/iot_upstream.conf
    echo "  iot_upstream.conf initialised from \$ACTIVE slot"
else
    echo "  iot_upstream.conf already exists — not overwritten (use blue_green_deploy.sh to swap)"
fi

# Remove any conflicting default nginx config.
if [ -f /etc/nginx/sites-enabled/default ]; then
    sudo rm -f /etc/nginx/sites-enabled/default
    echo "  removed default nginx site"
fi

echo "  Testing nginx configuration..."
sudo nginx -t

echo "  Reloading nginx..."
sudo systemctl reload nginx

echo "  nginx ok"
REMOTE

echo "==> Nginx configuration deployed."
echo "    Next: run 06_setup_monitoring.sh to deploy the observability stack"
