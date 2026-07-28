#!/usr/bin/env bash
# S4-2: Issue a device client certificate signed by the intermediate CA.
#
# Usage:
#   issue_device_cert.sh <CN> [OUTPUT_DIR] [PKI_DIR] [--der [FW_DIR]]
#
#   CN          device common name, e.g. "weather-001"
#   OUTPUT_DIR  where to write key + cert (default: PKI_DIR/devices/<CN>/)
#   PKI_DIR     pki/ root directory (default: html/pki/ next to scripts/)
#   --der       also emit DER + C-array files for baking into firmware:
#                 <CN>.der, <CN>-key.der, intermediate.der   (PEM→DER)
#                 client_der.c, client_key_der.c, server_der.c  (firmware arrays)
#               FW_DIR: target directory for *_der.c files (default: ../lib/A7670)
#               PEM_TO_C: override path to scripts/pem_to_c_array.sh
#
# Outputs (in OUTPUT_DIR):
#   <CN>.key         device private key — provision onto device, never stored on server
#   <CN>.crt         device certificate
#   <CN>-chain.pem   <CN>.crt + intermediate.crt  (full client chain for TLS handshake)
#   <CN>.der         device cert in DER (modem format, when --der)
#   <CN>-key.der     device key in DER (modem format, when --der)
#   intermediate.der CA cert in DER (modem format, when --der)
set -euo pipefail

# Parse --der=DIR option first, then treat remaining positionals as before.
EMIT_DER=false
FW_DIR=""
POSARGS=()
next_is_fw=false
for arg in "$@"; do
    if $next_is_fw; then
        FW_DIR="$arg"
        next_is_fw=false
        continue
    fi
    case "$arg" in
        --der)     EMIT_DER=true ;;
        --der=*)   EMIT_DER=true; FW_DIR="${arg#--der=}" ;;
        --)        POSARGS+=("$arg") ;;
        *)         POSARGS+=("$arg") ;;
    esac
done
set -- "${POSARGS[@]}"

CN="${1:?Usage: issue_device_cert.sh <CN> [OUTPUT_DIR] [PKI_DIR] [--der [FW_DIR]]}"

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
# Revoke any existing Valid cert for the same CN first — unique_subject=yes in
# index.txt.attr blocks re-issuing otherwise. Prefer openssl ca -revoke when the
# issued file is on disk; fall back to stripping the orphaned index entry.
PRUNE_LINE="$(awk -F'\t' -v cn="CN=${CN}" '$1=="V"&&index($6,cn){print NR;exit}' \
    "$INT_DIR/index.txt")"
if [ -n "$PRUNE_LINE" ]; then
    FIELD5="$(awk -F'\t' -v n="$PRUNE_LINE" 'NR==n{print $5}' "$INT_DIR/index.txt")"
    if [ "$FIELD5" != "unknown" ] && [ -f "$INT_DIR/issued/$FIELD5" ]; then
        echo "    Revoking previous cert (file $FIELD5)..."
        openssl ca -config "$INT_DIR/ca.conf" \
            -revoke "$INT_DIR/issued/$FIELD5" -batch
    else
        echo "    Stripping orphaned index entry (line $PRUNE_LINE, no cert file)..."
        sed -i "${PRUNE_LINE}d" "$INT_DIR/index.txt"
    fi
fi

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

if $EMIT_DER; then
    REPO_ROOT="$(cd "$HTML_DIR/.." && pwd)"
    FW_DIR="${FW_DIR:-$REPO_ROOT/lib/A7670}"
    PEM_TO_C="${PEM_TO_C:-$REPO_ROOT/scripts/pem_to_c_array.sh}"

    if [ ! -f "$PEM_TO_C" ]; then
        echo "ERROR: pem_to_c_array.sh not found at $PEM_TO_C" >&2
        exit 1
    fi

    DEVICE_CERT_DER="$OUT_DIR/$CN.der"
    DEVICE_KEY_DER="$OUT_DIR/$CN-key.der"
    INT_CA_DER="$OUT_DIR/intermediate.der"

    echo
    echo "==> Converting PEM → DER..."
    openssl x509 -in "$DEVICE_CERT"                  -outform DER -out "$DEVICE_CERT_DER"
    openssl rsa  -in "$DEVICE_KEY"                   -outform DER -out "$DEVICE_KEY_DER"
    openssl x509 -in "$INT_DIR/intermediate.crt"     -outform DER -out "$INT_CA_DER"

    echo "==> Regenerating firmware *_der.c arrays in $FW_DIR..."
    "$PEM_TO_C" "$DEVICE_CERT_DER" client_der     "$FW_DIR/client_der.c"
    "$PEM_TO_C" "$DEVICE_KEY_DER"  client_key_der "$FW_DIR/client_key_der.c"
    "$PEM_TO_C" "$INT_CA_DER"      server_der     "$FW_DIR/server_der.c"

    echo
    echo "  DER (modem format):"
    echo "    Cert DER: $DEVICE_CERT_DER  → client_der.c"
    echo "    Key DER:  $DEVICE_KEY_DER   → client_key_der.c"
    echo "    CA DER:   $INT_CA_DER       → server_der.c"
    echo
    echo "  Next: scp lib/A7670/{client,client_key,server}_der.c to build host,"
    echo "  rebuild firmware, and reload nginx so private_ca_chain.pem matches."
fi

echo
echo "Provision $DEVICE_KEY and $CHAIN onto the device."
echo "Do NOT store $DEVICE_KEY on the server."
