#!/usr/bin/env bash
# Initial Ubuntu server provisioning — run ONCE on a fresh server.
# Idempotent: safe to re-run; existing state is preserved.
#
# Usage: bash server_deployment/01_setup_server.sh [ssh_key]
# Default key: ~/.ssh/akrapong.key
set -euo pipefail

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HTML_DIR="/home/akp/html"

echo "==> [1/6] Installing system packages"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'REMOTE'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -q
sudo apt-get install -y -q \
    nginx \
    postgresql-17 \
    python3 \
    python3-venv \
    python3-pip \
    certbot \
    python3-certbot-nginx \
    git \
    ufw \
    build-essential \
    libpq-dev \
    curl \
    openssl \
    xxd
echo "  packages ok"
REMOTE

echo "==> [2/6] Creating runtime directories"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
HTML_DIR="$HTML_DIR"
mkdir -p  "\$HTML_DIR/firmware"
chmod 750 "\$HTML_DIR/firmware"
mkdir -p  "\$HTML_DIR/logs"
mkdir -p  "\$HTML_DIR/pki"
chmod 700 "\$HTML_DIR/pki"
mkdir -p  "\$HTML_DIR/etc"
mkdir -p  "\$HTML_DIR/backups"
echo "  dirs ok"
REMOTE

echo "==> [3/6] Creating Python virtual environment"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
HTML_DIR="$HTML_DIR"
if [ ! -d "\$HTML_DIR/.venv" ]; then
    python3 -m venv "\$HTML_DIR/.venv"
    echo "  venv created"
else
    echo "  venv already exists"
fi
REMOTE

echo "==> [4/6] Configuring UFW firewall"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'REMOTE'
set -euo pipefail
sudo ufw --force enable
sudo ufw allow 22/tcp   comment 'SSH'
sudo ufw allow 443/tcp  comment 'HTTPS'
sudo ufw deny  8000/tcp comment 'block FastAPI direct access'
sudo ufw status verbose
REMOTE

echo "==> [5/6] Enabling and starting PostgreSQL"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'REMOTE'
set -euo pipefail
sudo systemctl enable --now postgresql
echo "  postgresql ok"
REMOTE

echo "==> [6/6] Ensuring Nginx is enabled"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'REMOTE'
set -euo pipefail
sudo systemctl enable nginx
echo "  nginx enabled"
REMOTE

echo "==> Server base provisioning complete."
echo "    Next: run 02_setup_env.sh to create iot.env"
