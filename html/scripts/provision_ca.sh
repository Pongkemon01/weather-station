#!/usr/bin/env bash
# S4-1: Generate offline root CA + online intermediate CA for the IoT fleet.
#
# Run ONCE on a secure workstation (not the production server).
# After completion, move pki/offline/root.key to an air-gapped USB drive.
#
# Usage:
#   provision_ca.sh [PKI_DIR]
#   PKI_DIR  absolute path to the target pki/ directory (default: html/pki/ next to scripts/)
#
# Outputs:
#   PKI_DIR/offline/root.key          — MOVE TO USB; never expose
#   PKI_DIR/offline/root.crt          — public; safe to keep
#   PKI_DIR/intermediate/intermediate.key  — online signing key; stays on server
#   PKI_DIR/intermediate/intermediate.crt  — online CA cert
#   PKI_DIR/intermediate/ca.conf           — openssl CA config for signing + CRL
#   PKI_DIR/private_ca_chain.pem      — intermediate || root (for Nginx ssl_client_certificate)
#   PKI_DIR/ca.crl                    — initial empty CRL (renewed weekly by refresh_crl.sh)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"
PKI_DIR="${1:-$HTML_DIR/pki}"
OFFLINE_DIR="$PKI_DIR/offline"
INT_DIR="$PKI_DIR/intermediate"

ROOT_SUBJECT="${ROOT_SUBJECT:-/CN=IoT Root CA/O=RobinLab/C=TH}"
INT_SUBJECT="${INT_SUBJECT:-/CN=IoT Intermediate CA/O=RobinLab/C=TH}"
ROOT_DAYS="${ROOT_DAYS:-3650}"   # 10 years
INT_DAYS="${INT_DAYS:-1825}"     # 5 years
DEVICE_DAYS="${DEVICE_DAYS:-365}"

mkdir -p "$OFFLINE_DIR" "$INT_DIR/issued"
chmod 700 "$PKI_DIR" "$OFFLINE_DIR" "$INT_DIR"

# ── Root CA ──────────────────────────────────────────────────────────────────
echo "==> Generating root CA key (4096-bit RSA)..."
openssl genrsa -out "$OFFLINE_DIR/root.key" 4096
chmod 400 "$OFFLINE_DIR/root.key"

echo "==> Self-signing root CA certificate..."
openssl req -new -x509 \
    -key "$OFFLINE_DIR/root.key" \
    -out "$OFFLINE_DIR/root.crt" \
    -days "$ROOT_DAYS" \
    -subj "$ROOT_SUBJECT" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,cRLSign,keyCertSign" \
    -addext "subjectKeyIdentifier=hash"

# ── Intermediate CA ───────────────────────────────────────────────────────────
echo "==> Generating intermediate CA key (2048-bit RSA)..."
openssl genrsa -out "$INT_DIR/intermediate.key" 2048
chmod 400 "$INT_DIR/intermediate.key"

echo "==> Generating intermediate CA CSR..."
openssl req -new \
    -key "$INT_DIR/intermediate.key" \
    -out "$INT_DIR/intermediate.csr" \
    -subj "$INT_SUBJECT"

echo "==> Signing intermediate CA certificate with root..."
openssl x509 -req \
    -in "$INT_DIR/intermediate.csr" \
    -CA "$OFFLINE_DIR/root.crt" \
    -CAkey "$OFFLINE_DIR/root.key" \
    -CAcreateserial \
    -out "$INT_DIR/intermediate.crt" \
    -days "$INT_DAYS" \
    -extfile <(printf 'basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,cRLSign,keyCertSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')

# ── CA database (openssl ca index + serial + crlnumber) ──────────────────────
echo "==> Initialising CA database..."
touch "$INT_DIR/index.txt"
[ -f "$INT_DIR/serial" ]    || echo "01" > "$INT_DIR/serial"
[ -f "$INT_DIR/crlnumber" ] || echo "01" > "$INT_DIR/crlnumber"

cat > "$INT_DIR/ca.conf" << CONF
# openssl CA configuration for IoT fleet intermediate CA.
# Used by: provision_ca.sh, issue_device_cert.sh, refresh_crl.sh.
[ ca ]
default_ca = CA_default

[ CA_default ]
dir             = $INT_DIR
database        = \$dir/index.txt
serial          = \$dir/serial
crlnumber       = \$dir/crlnumber
certificate     = \$dir/intermediate.crt
private_key     = \$dir/intermediate.key
new_certs_dir   = \$dir/issued
default_days    = $DEVICE_DAYS
default_crl_days = 7
default_md      = sha256
policy          = policy_anything
copy_extensions = copy

[ policy_anything ]
countryName             = optional
stateOrProvinceName     = optional
localityName            = optional
organizationName        = optional
organizationalUnitName  = optional
commonName              = supplied
emailAddress            = optional

[ v3_device ]
basicConstraints        = critical,CA:FALSE
keyUsage                = critical,digitalSignature,keyAgreement
extendedKeyUsage        = clientAuth
subjectKeyIdentifier    = hash
authorityKeyIdentifier  = keyid:always

[ crl_ext ]
authorityKeyIdentifier  = keyid:always
CONF

# ── Root CA database (needed to generate root CRL) ────────────────────────────
echo "==> Initialising root CA database..."
mkdir -p "$OFFLINE_DIR/db"
touch "$OFFLINE_DIR/db/index.txt"
[ -f "$OFFLINE_DIR/db/crlnumber" ] || echo "01" > "$OFFLINE_DIR/db/crlnumber"
# Use absolute paths — do not use $dir variable (breaks when called from scripts).
python3 -c "
import pathlib
pathlib.Path('$OFFLINE_DIR/root_ca.conf').write_text('''
[ca]
default_ca=CA_default
[CA_default]
dir              = $OFFLINE_DIR
database         = $OFFLINE_DIR/db/index.txt
crlnumber        = $OFFLINE_DIR/db/crlnumber
certificate      = $OFFLINE_DIR/root.crt
private_key      = $OFFLINE_DIR/root.key
default_crl_days = 3650
default_md       = sha256
policy           = policy_anything
[policy_anything]
commonName = supplied
[crl_ext]
authorityKeyIdentifier = keyid:always
''')
"

# ── Initial CRL ───────────────────────────────────────────────────────────────
echo "==> Generating initial (empty) intermediate CRL..."
openssl ca -gencrl \
    -config "$INT_DIR/ca.conf" \
    -out "$PKI_DIR/int.crl.tmp" \
    -batch

echo "==> Generating initial (empty) root CRL..."
openssl ca -gencrl \
    -config "$OFFLINE_DIR/root_ca.conf" \
    -out "$PKI_DIR/root.crl" \
    -batch

# nginx ssl_crl requires CRLs for every CA level in the client-cert chain.
# ca.crl = intermediate CRL + root CRL; refresh_crl.sh regenerates intermediate
# and recombines with the static root.crl.
cat "$PKI_DIR/int.crl.tmp" "$PKI_DIR/root.crl" > "$PKI_DIR/ca.crl"
rm -f "$PKI_DIR/int.crl.tmp"

# ── CA chain for Nginx ssl_client_certificate ─────────────────────────────────
echo "==> Building private CA chain (intermediate + root)..."
cat "$INT_DIR/intermediate.crt" "$OFFLINE_DIR/root.crt" > "$PKI_DIR/private_ca_chain.pem"

echo
echo "========================================================================"
echo "  IMPORTANT: Move the offline root key to an air-gapped USB drive now!"
echo "  Offline root: $OFFLINE_DIR/root.key"
echo "========================================================================"
echo
echo "  CA chain:  $PKI_DIR/private_ca_chain.pem"
echo "  CRL:       $PKI_DIR/ca.crl"
echo "  CA config: $INT_DIR/ca.conf"
echo
echo "  Next steps:"
echo "    1. Copy $PKI_DIR/ to the server (excluding offline/)."
echo "    2. Deploy nginx/iot_server.conf with the correct html_dir path."
echo "    3. Enable refresh-crl.timer: systemctl enable --now refresh-crl.timer"
echo "Done."
