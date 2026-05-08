#!/usr/bin/env bash
# Deploy the Prometheus + Loki + Promtail + Grafana observability stack.
# Idempotent: re-running updates configs and restarts affected services.
#
# Installs:
#   Prometheus  — scrapes FastAPI /metrics; alert rules for ingest lag + OTA
#   Loki        — log aggregation (receives from Promtail)
#   Promtail    — scrapes html/logs/app.log and nginx access log
#   Grafana     — dashboards + provisioned datasources
#
# Usage: bash server_deployment/06_setup_monitoring.sh [ssh_key]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

SSH_KEY="${1:-$HOME/.ssh/akrapong.key}"
REMOTE="akp@robin-gpu.cpe.ku.ac.th"
HTML_DIR="/home/akp/html"
TMP="/tmp/iot_monitoring_$$.tar.gz"

echo "==> [1/4] Uploading monitoring configs"
tar czf "$TMP" -C "$REPO_ROOT" html/monitoring
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no "$TMP" "$REMOTE":~/iot_monitoring.tar.gz
rm -f "$TMP"

echo "==> [2/4] Installing Prometheus, Loki, Promtail"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'REMOTE'
set -euo pipefail

# ── Prometheus ──────────────────────────────────────────────────────────────
if ! command -v prometheus &>/dev/null; then
    PROM_VER=2.51.2
    curl -sL "https://github.com/prometheus/prometheus/releases/download/v${PROM_VER}/prometheus-${PROM_VER}.linux-amd64.tar.gz" \
        | sudo tar xz -C /usr/local/bin --strip-components=1 \
            "prometheus-${PROM_VER}.linux-amd64/prometheus" \
            "prometheus-${PROM_VER}.linux-amd64/promtool"
    echo "  prometheus ${PROM_VER} installed"
else
    echo "  prometheus already installed: $(prometheus --version 2>&1 | head -1)"
fi

# ── Loki ────────────────────────────────────────────────────────────────────
if ! command -v loki &>/dev/null; then
    LOKI_VER=3.0.0
    curl -sL "https://github.com/grafana/loki/releases/download/v${LOKI_VER}/loki-linux-amd64.zip" \
        -o /tmp/loki.zip
    sudo unzip -qo /tmp/loki.zip -d /usr/local/bin/
    sudo chmod +x /usr/local/bin/loki-linux-amd64
    sudo ln -sf /usr/local/bin/loki-linux-amd64 /usr/local/bin/loki
    rm -f /tmp/loki.zip
    echo "  loki ${LOKI_VER} installed"
else
    echo "  loki already installed: $(loki --version 2>&1 | head -1)"
fi

# ── Promtail ────────────────────────────────────────────────────────────────
if ! command -v promtail &>/dev/null; then
    LOKI_VER=3.0.0
    curl -sL "https://github.com/grafana/loki/releases/download/v${LOKI_VER}/promtail-linux-amd64.zip" \
        -o /tmp/promtail.zip
    sudo unzip -qo /tmp/promtail.zip -d /usr/local/bin/
    sudo chmod +x /usr/local/bin/promtail-linux-amd64
    sudo ln -sf /usr/local/bin/promtail-linux-amd64 /usr/local/bin/promtail
    rm -f /tmp/promtail.zip
    echo "  promtail ${LOKI_VER} installed"
else
    echo "  promtail already installed"
fi
REMOTE

echo "==> [3/4] Installing Grafana"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<'REMOTE'
set -euo pipefail
if ! command -v grafana-server &>/dev/null; then
    sudo apt-get install -y -q apt-transport-https software-properties-common
    curl -fsSL https://apt.grafana.com/gpg.key | sudo gpg --dearmor -o /etc/apt/keyrings/grafana.gpg
    echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" \
        | sudo tee /etc/apt/sources.list.d/grafana.list
    sudo apt-get update -q
    sudo apt-get install -y -q grafana
    sudo systemctl enable grafana-server
    echo "  grafana installed"
else
    echo "  grafana already installed"
fi
REMOTE

echo "==> [4/4] Deploying config files and starting services"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$REMOTE" bash <<REMOTE
set -euo pipefail
cd ~
tar xzf iot_monitoring.tar.gz
rm  iot_monitoring.tar.gz

MONITORING="$HTML_DIR/monitoring"

# Prometheus config + alert rules.
sudo mkdir -p /etc/prometheus
sudo cp "\$MONITORING/prometheus/prometheus.yml" /etc/prometheus/prometheus.yml
sudo cp "\$MONITORING/prometheus/alerts.yml"     /etc/prometheus/alerts.yml

# Loki config.
sudo mkdir -p /etc/loki
sudo cp "\$MONITORING/loki/loki.yml" /etc/loki/loki.yml

# Promtail config.
sudo mkdir -p /etc/promtail
sudo cp "\$MONITORING/promtail/promtail.yml" /etc/promtail/promtail.yml

# Grafana provisioning + dashboards.
sudo mkdir -p /etc/grafana/provisioning/datasources
sudo mkdir -p /etc/grafana/provisioning/dashboards
sudo mkdir -p /var/lib/grafana/dashboards
sudo cp "\$MONITORING/grafana/provisioning/datasources/datasources.yml" \
    /etc/grafana/provisioning/datasources/datasources.yml
sudo cp "\$MONITORING/grafana/provisioning/dashboards/dashboards.yml" \
    /etc/grafana/provisioning/dashboards/dashboards.yml
sudo cp "\$MONITORING/grafana/dashboards/iot-weather.json" \
    /var/lib/grafana/dashboards/iot-weather.json
sudo chown -R grafana:grafana /var/lib/grafana/dashboards

# Create Prometheus systemd unit if not present.
if [ ! -f /etc/systemd/system/prometheus.service ]; then
    sudo tee /etc/systemd/system/prometheus.service > /dev/null <<'UNIT'
[Unit]
Description=Prometheus
After=network.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/prometheus \
    --config.file=/etc/prometheus/prometheus.yml \
    --storage.tsdb.path=/var/lib/prometheus \
    --web.listen-address=127.0.0.1:9090
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT
    sudo mkdir -p /var/lib/prometheus
    echo "  prometheus.service created"
fi

sudo systemctl daemon-reload
sudo systemctl enable --now prometheus
sudo systemctl enable --now loki
sudo systemctl enable --now promtail
sudo systemctl restart grafana-server

echo "  Services:"
for svc in prometheus loki promtail grafana-server; do
    STATUS=\$(sudo systemctl is-active "\$svc" 2>/dev/null || echo "unknown")
    echo "    \$svc: \$STATUS"
done
REMOTE

echo "==> Monitoring stack deployed."
echo "    Grafana: http://<server-ip>:3000  (default: admin/admin — change immediately)"
echo "    Prometheus: http://127.0.0.1:9090 (loopback only)"
