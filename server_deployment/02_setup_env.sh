#!/usr/bin/env bash
# Create html/etc/iot.env on the server with all required runtime settings.
# Run locally after 01_setup_server.sh.
#
# Usage: bash server_deployment/02_setup_env.sh [ssh_key]
#
# Prompts for secrets interactively. Existing iot.env is preserved unless --force is passed.
# Pass --force to overwrite an existing iot.env.
set -euo pipefail

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HTML_DIR="/home/akp/html"
FORCE="${2:-}"

# Check if iot.env already exists on the server.
EXISTS=$(ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" \
    "[ -f '$HTML_DIR/etc/iot.env' ] && echo yes || echo no")

if [[ "$EXISTS" == "yes" && "$FORCE" != "--force" ]]; then
    echo "  iot.env already exists on server. Pass --force to overwrite."
    echo "  Current contents:"
    ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" \
        "grep -v 'SECRET\|PASSWORD\|KEY\|DSN' '$HTML_DIR/etc/iot.env' || true"
    exit 0
fi

echo "==> Collecting configuration values"

# Generate a cryptographically random JWT secret.
JWT_SECRET=$(openssl rand -hex 32)
echo "  JWT_SECRET: (auto-generated, 64 hex chars)"

read -rp "  PostgreSQL DB name [weather]: " DB_NAME
DB_NAME="${DB_NAME:-weather}"

read -rp "  PostgreSQL user [akp]: " DB_USER
DB_USER="${DB_USER:-akp}"

# Unix-socket DSN is the default (peer auth, no password needed for local app).
DB_DSN="postgresql:///$DB_NAME?host=/var/run/postgresql&user=$DB_USER"
echo "  DATABASE_URL: $DB_DSN"

read -rp "  Admin username [admin]: " ADMIN_USERNAME
ADMIN_USERNAME="${ADMIN_USERNAME:-admin}"

read -rsp "  Admin password: " ADMIN_PASSWORD
echo
if [[ -z "$ADMIN_PASSWORD" ]]; then
    echo "ERROR: admin password cannot be empty" >&2
    exit 1
fi

read -rp "  Firmware signing key path (leave blank to disable Ed25519 signing): " SIGNING_KEY
SIGNING_KEY="${SIGNING_KEY:-}"

read -rp "  Slot length in seconds [43200 = 12h]: " SLOT_LEN
SLOT_LEN="${SLOT_LEN:-43200}"

read -rp "  Firmware keep count [3]: " KEEP_N
KEEP_N="${KEEP_N:-3}"

FIRMWARE_DIR="$HTML_DIR/firmware"

echo
echo "==> Writing iot.env to $REMOTE:$HTML_DIR/etc/iot.env"

ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
mkdir -p "$HTML_DIR/etc"
cat > "$HTML_DIR/etc/iot.env" <<'EOF'
# Runtime environment for the IoT server (FastAPI + gunicorn).
# Loaded by systemd EnvironmentFile= directive.
# Never commit this file — it contains secrets.

# PostgreSQL connection (Unix-socket peer auth).
DATABASE_URL=$DB_DSN

# JWT signing secret (HS256). Keep secret. Rotate by restarting the service.
JWT_SECRET=$JWT_SECRET

# Firmware binary storage directory. Must be absolute and writable.
FIRMWARE_DIR=$FIRMWARE_DIR

# Number of terminal (completed/cancelled) campaign binaries to retain.
FIRMWARE_KEEP_N=$KEEP_N

# Firmware size ceiling — STM32L476RG app Flash is 480 KB.
MAX_FIRMWARE_SIZE_BYTES=491520

# OTA rollout slot length (seconds). 43200 = 12 h = twice-daily upload cadence.
SLOT_LEN_SEC=$SLOT_LEN

# PostgreSQL database name (used by backup_db.sh).
DB_NAME=$DB_NAME

# Number of days to retain DB backups.
BACKUP_RETAIN_DAYS=14

# Ed25519 firmware signing private key path. Leave blank to disable signing.
SIGNING_PRIVATE_KEY_PATH=$SIGNING_KEY

# Seed admin credentials — read once at first migration; not used at runtime.
ADMIN_USERNAME=$ADMIN_USERNAME
ADMIN_PASSWORD=$ADMIN_PASSWORD
EOF
chmod 600 "$HTML_DIR/etc/iot.env"
echo "  iot.env written (mode 600)"
REMOTE

echo "==> iot.env deployed."
echo "    Next: run 03_run_migrations.sh to initialise the database"
