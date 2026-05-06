#!/usr/bin/env bash
# S4-5: Regenerate CRL from intermediate CA and reload Nginx.
#
# Run weekly via refresh-crl.timer.
# Reads revoked entries from pki/intermediate/index.txt (updated by openssl ca -revoke).
#
# Usage: refresh_crl.sh [PKI_DIR]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"
PKI_DIR="${1:-$HTML_DIR/pki}"
INT_DIR="$PKI_DIR/intermediate"
CRL_OUT="$PKI_DIR/ca.crl"
CRL_TMP="$CRL_OUT.tmp"
ROOT_CRL="$PKI_DIR/root.crl"     # static; generated once during PKI provisioning

if [ ! -f "$INT_DIR/ca.conf" ]; then
    echo "ERROR: CA not initialised. Run provision_ca.sh first." >&2
    exit 1
fi

echo "==> Generating intermediate CRL from $INT_DIR/index.txt..."
openssl ca -gencrl \
    -config "$INT_DIR/ca.conf" \
    -out "$CRL_TMP" \
    -batch

# Combine intermediate CRL with static root CRL (nginx ssl_crl requires CRLs for
# every CA level in the client-cert chain; root CRL is long-lived and stays offline).
if [ -f "$ROOT_CRL" ]; then
    cat "$CRL_TMP" "$ROOT_CRL" > "$CRL_OUT"
    rm -f "$CRL_TMP"
    echo "==> Combined CRL (intermediate + root) written to $CRL_OUT"
else
    mv "$CRL_TMP" "$CRL_OUT"
    echo "==> WARNING: $ROOT_CRL not found; ca.crl contains intermediate CRL only"
    echo "==>          nginx may reject client certs — copy root.crl to $PKI_DIR/"
    echo "==> CRL written to $CRL_OUT"
fi

echo "==> Testing Nginx config..."
nginx -t

echo "==> Reloading Nginx..."
nginx -s reload

echo "==> CRL refresh complete."
