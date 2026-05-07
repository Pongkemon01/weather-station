"""S9-6 verification: send a duplicate upload, assert ingest_duplicates_total increments.

Run from WSL:
  python3 server_test/s9_6_verify.py

Requires: requests (pip3 install requests)
Certs:    /home/akp/device_test/weather-test.{crt,key}
CA:       /home/akp/iot_pki/private_ca_chain.pem
"""
from __future__ import annotations

import struct
import subprocess
import sys
from datetime import datetime, timezone

import requests

BASE_URL = "https://robin-gpu.cpe.ku.ac.th"
CA_BUNDLE = "/home/akp/iot_pki/private_ca_chain.pem"
CERT = ("/home/akp/device_test/weather-test.crt", "/home/akp/device_test/weather-test.key")
UPLOAD_URL = f"{BASE_URL}/api/v1/weather/upload"
METRICS_SSH = "ssh -i /home/akp/.ssh/akrapong.key -o StrictHostKeyChecking=no akp@robin-gpu.cpe.ku.ac.th"

# Build a minimal 1-record payload: region=1, station=1, count=1
header = struct.pack("<HHB", 1, 1, 1)
ts = int(
    (datetime(2026, 5, 7, 12, 0, 0, tzinfo=timezone.utc) - datetime(2000, 1, 1, tzinfo=timezone.utc)).total_seconds()
)
# temp=25.0 (3200/128), hum=62.5 (8000/128), pres=100.0 (12800/128),
# light=500, rain=0, dew=19.53 (2500/128), bus=0
record = struct.pack("<IhhhHhhh", ts, 3200, 8000, 12800, 500, 0, 2500, 0)
payload = header + record


def scrape_metric(name: str) -> float:
    out = subprocess.check_output(
        f"{METRICS_SSH} 'curl -s http://127.0.0.1:8000/metrics'",
        shell=True,
        text=True,
    )
    for line in out.splitlines():
        if line.startswith(name + " "):
            return float(line.split()[1])
    return 0.0


def main() -> None:
    session = requests.Session()
    session.cert = CERT
    session.verify = CA_BUNDLE
    headers = {"Content-Type": "application/octet-stream"}

    # Baseline
    before = scrape_metric("ingest_duplicates_total")
    print(f"ingest_duplicates_total before: {before}")

    # First upload — should succeed
    print("POST 1 (expect ok)...")
    r1 = session.post(UPLOAD_URL, data=payload, headers=headers)
    print(f"  {r1.status_code} {r1.text.strip()}")
    if r1.json().get("status") not in ("ok", "duplicate"):
        print(f"FAIL: unexpected response: {r1.text}", file=sys.stderr)
        sys.exit(1)

    # Second upload — same payload, same idempotency key → duplicate
    print("POST 2 (expect duplicate)...")
    r2 = session.post(UPLOAD_URL, data=payload, headers=headers)
    print(f"  {r2.status_code} {r2.text.strip()}")
    if r2.json().get("status") != "duplicate":
        print(f"FAIL: second upload not flagged as duplicate: {r2.text}", file=sys.stderr)
        sys.exit(1)

    # Check counter incremented
    after = scrape_metric("ingest_duplicates_total")
    print(f"ingest_duplicates_total after:  {after}")

    if after <= before:
        print("FAIL: ingest_duplicates_total did not increment", file=sys.stderr)
        sys.exit(1)

    # Also print ingest_chunks_total for completeness
    chunks = scrape_metric("ingest_chunks_total")
    print(f"ingest_chunks_total:            {chunks}")

    print("\n✓ S9-6 PASS — duplicate detected and counter incremented")


if __name__ == "__main__":
    main()
