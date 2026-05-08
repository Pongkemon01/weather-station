#!/usr/bin/env bash
# Daily PostgreSQL backup: pg_dump → gzip → ~/html/backups/
# Reads DB_NAME and BACKUP_RETAIN_DAYS from the environment (set via iot.env).
# Called by the backup-db.service systemd unit.
set -euo pipefail

HTML_DIR=/home/akp/html
BACKUP_DIR="$HTML_DIR/backups"
DB_NAME="${DB_NAME:-weather}"
RETAIN_DAYS="${BACKUP_RETAIN_DAYS:-14}"
TIMESTAMP=$(date -u +%Y%m%dT%H%M%SZ)
DEST="$BACKUP_DIR/${DB_NAME}_${TIMESTAMP}.sql.gz"

mkdir -p "$BACKUP_DIR"

# pg_dump via Unix-socket peer auth (same as app connection).
pg_dump -U akp "$DB_NAME" | gzip > "$DEST"
SIZE=$(du -sh "$DEST" | cut -f1)
echo "Backup written: $DEST ($SIZE)"

# Prune backups older than RETAIN_DAYS.
find "$BACKUP_DIR" -name "${DB_NAME}_*.sql.gz" -mtime +"$RETAIN_DAYS" -delete
echo "Pruned backups older than ${RETAIN_DAYS} days"
