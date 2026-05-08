#!/usr/bin/env bash
# Deploy latest application code and run database migrations.
# Run locally after 02_setup_env.sh and after every code update.
#
# Usage: bash server_deployment/03_run_migrations.sh [ssh_key]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
HTML_DIR_LOCAL="$REPO_ROOT/html"

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HTML_DIR="/home/akp/html"
TMP="/tmp/iot_app_$$.tar.gz"

echo "==> [1/3] Archiving application"
tar czf "$TMP" \
    --exclude='app/__pycache__' \
    --exclude='app/**/__pycache__' \
    --exclude='*.pyc' \
    -C "$REPO_ROOT" \
    "html/app" \
    "html/requirements.txt" \
    "html/scripts"

echo "==> [2/3] Uploading to $REMOTE"
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no "$TMP" "$REMOTE":~/iot_app.tar.gz
rm -f "$TMP"

echo "==> [3/3] Installing deps and running migrations"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
cd ~
tar xzf iot_app.tar.gz
rm  iot_app.tar.gz

# Ensure TimescaleDB extension is loaded.
psql -U akp -d weather -c "CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE;" 2>/dev/null || true

# Install Python dependencies.
"$HTML_DIR/.venv/bin/pip" install -r "$HTML_DIR/requirements.txt" -q

# Run idempotent migrations.
cd "$HTML_DIR"
"$HTML_DIR/.venv/bin/python" -m scripts.migrate
echo "  migrations ok"
REMOTE

echo "==> Migrations complete."
echo "    Next: run 04_install_systemd.sh to install service units"
