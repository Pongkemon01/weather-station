#!/usr/bin/env bash
# Full server setup orchestrator — runs all deployment steps in order.
# Safe to re-run; each step is idempotent.
#
# Steps:
#   01 — System package installation, runtime dirs, UFW, Python venv
#   02 — iot.env secrets file (interactive)
#   03 — Application code upload + database migrations
#   04 — Systemd unit installation and activation
#   05 — Nginx config deployment and reload
#   06 — Monitoring stack (Prometheus, Loki, Promtail, Grafana)
#
# Usage:
#   bash server_deployment/full_setup.sh [ssh_key] [--skip-monitoring]
#
# Options:
#   --skip-monitoring  Skip step 06 (useful on resource-constrained VPS)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
SKIP_MONITORING=false

for arg in "$@"; do
    [[ "$arg" == "--skip-monitoring" ]] && SKIP_MONITORING=true
done

announce() {
    echo
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  $1"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

START=$(date +%s)

announce "Step 1/6 — Server provisioning"
bash "$SCRIPT_DIR/01_setup_server.sh" "$SSH_KEY"

announce "Step 2/6 — Environment file (iot.env)"
bash "$SCRIPT_DIR/02_setup_env.sh" "$SSH_KEY"

announce "Step 3/6 — Application code + database migrations"
bash "$SCRIPT_DIR/03_run_migrations.sh" "$SSH_KEY"

announce "Step 4/6 — Systemd units"
bash "$SCRIPT_DIR/04_install_systemd.sh" "$SSH_KEY"

announce "Step 5/6 — Nginx configuration"
# Prompt operator to obtain the Let's Encrypt cert before nginx config is deployed.
echo "  Checking Let's Encrypt cert for adm.robinlab.cc..."
SSH_KEY_ARG="$SSH_KEY"
CERT_OK=$(ssh -i "$SSH_KEY_ARG" -o StrictHostKeyChecking=no \
    akp@robin-gpu.cpe.ku.ac.th \
    "[ -f /etc/letsencrypt/live/adm.robinlab.cc/fullchain.pem ] && echo yes || echo no")

if [[ "$CERT_OK" == "no" ]]; then
    echo
    echo "  Let's Encrypt cert for adm.robinlab.cc not found."
    echo "  Obtain it on the server with:"
    echo "    sudo certbot certonly --nginx -d adm.robinlab.cc --agree-tos --non-interactive"
    echo
    read -rp "  Press Enter after the cert is issued to continue, or Ctrl-C to abort: "
fi
bash "$SCRIPT_DIR/05_setup_nginx.sh" "$SSH_KEY"

if [[ "$SKIP_MONITORING" == "false" ]]; then
    announce "Step 6/6 — Monitoring stack"
    bash "$SCRIPT_DIR/06_setup_monitoring.sh" "$SSH_KEY"
else
    announce "Step 6/6 — Monitoring stack (SKIPPED)"
fi

END=$(date +%s)
ELAPSED=$((END - START))

echo
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Full setup complete in ${ELAPSED}s"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo
echo "  Post-setup checklist:"
echo "  [ ] Run html/scripts/provision_ca.sh (workstation) to generate PKI"
echo "  [ ] Copy pki/ to server (excluding pki/offline/root.key)"
echo "  [ ] Run html/scripts/issue_device_cert.sh to issue the fleet cert"
echo "  [ ] Convert fleet cert to DER and update lib/A7670/ C arrays"
echo "  [ ] Run html/scripts/generate_signing_key.sh for Ed25519 firmware signing"
echo "  [ ] Rebuild and flash firmware (platformio run -t upload)"
echo "  [ ] Verify mTLS: openssl s_client -cert ... -connect robin-gpu.cpe.ku.ac.th:443"
echo "  [ ] Run server_test/ integration tests: pytest server_test/"
echo "  [ ] Change Grafana default admin password at http://<ip>:3000"
echo
