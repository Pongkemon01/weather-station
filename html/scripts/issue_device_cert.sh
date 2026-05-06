#!/usr/bin/env bash
# S4-2: Issue a device client certificate signed by the intermediate CA.
#
# Usage:
#   issue_device_cert.sh <CN> [OUTPUT_DIR] [PKI_DIR]
#
#   CN          device common name, e.g. "weather-001"
#   OUTPUT_DIR  where to write key + cert (default: PKI_DIR/devices/<CN>/)
#   PKI_DIR     pki/ root directory (default: html/pki/ next to scripts/)
#
# Outputs (in OUTPUT_DIR):
#   <CN>.key         device private key — provision onto device, never stored on server
#   <CN>.crt         device certificate
#   <CN>-chain.pem   <CN>.crt + intermediate.crt  (full client chain for TLS handshake)
set -euo pipefail

CN="${1:?Usage: issue_device_cert.sh <CN> [OUTPUT_DIR] [PKI_DIR]}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"
PKI_DIR="${3:-$HTML_DIR/pki}"
INT_DIR="$PKI_DIR/intermediate"
OUT_DIR="${2:-$PKI_DIR/devices/$CN}"

if [ ! -f "$INT_DIR/ca.conf" ]; then
    echo "ERROR: CA not initialised. Run provision_ca.sh first." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR"

DEVICE_KEY="$OUT_DIR/$CN.key"
DEVICE_CSR="$OUT_DIR/$CN.csr"
DEVICE_CERT="$OUT_DIR/$CN.crt"
CHAIN="$OUT_DIR/$CN-chain.pem"

echo "==> Generating device key for CN=$CN..."
openssl genrsa -out "$DEVICE_KEY" 2048
chmod 400 "$DEVICE_KEY"

echo "==> Generating CSR..."
openssl req -new \
    -key "$DEVICE_KEY" \
    -out "$DEVICE_CSR" \
    -subj "/CN=$CN/O=RobinLab/C=TH"

echo "==> Signing with intermediate CA..."
openssl ca -batch \
    -config "$INT_DIR/ca.conf" \
    -extensions v3_device \
    -in "$DEVICE_CSR" \
    -out "$DEVICE_CERT"

echo "==> Building client chain (device cert + intermediate)..."
cat "$DEVICE_CERT" "$INT_DIR/intermediate.crt" > "$CHAIN"

echo
echo "  Key:   $DEVICE_KEY"
echo "  Cert:  $DEVICE_CERT"
echo "  Chain: $CHAIN"
echo
echo "Provision $DEVICE_KEY and $CHAIN onto the device."
echo "Do NOT store $DEVICE_KEY on the server."
