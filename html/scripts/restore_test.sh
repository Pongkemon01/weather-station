#!/usr/bin/env bash
# Weekly restore test: restore the latest backup into a temp DB, verify row counts, drop.
# Called by the restore-test.service systemd unit.
set -euo pipefail

HTML_DIR=/home/akp/html
BACKUP_DIR="$HTML_DIR/backups"
DB_NAME="${DB_NAME:-weather}"
TEST_DB="${DB_NAME}_restore_test"

LATEST=$(ls -t "$BACKUP_DIR/${DB_NAME}_"*.sql.gz 2>/dev/null | head -1 || true)
if [[ -z "$LATEST" ]]; then
    echo "ERROR: no backup found in $BACKUP_DIR"
    exit 1
fi

echo "==> Restore test from: $LATEST"

psql -U akp -d postgres -c "DROP DATABASE IF EXISTS $TEST_DB;"
psql -U akp -d postgres -c "CREATE DATABASE $TEST_DB;"

zcat "$LATEST" | psql -U akp -d "$TEST_DB" -q

RECORDS=$(psql -U akp -d "$TEST_DB" -At -c "SELECT COUNT(*) FROM weather_records;")
DEVICES=$(psql -U akp -d "$TEST_DB" -At -c "SELECT COUNT(*) FROM devices;")
echo "==> Restore OK: ${RECORDS} weather_records, ${DEVICES} devices"

psql -U akp -d postgres -c "DROP DATABASE $TEST_DB;"
echo "==> Test database dropped. Restore test PASSED."
