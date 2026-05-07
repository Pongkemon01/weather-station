"""Prometheus custom metrics for the IoT weather station server."""
from prometheus_client import Counter, Gauge, Histogram

ingest_chunks_total = Counter(
    "ingest_chunks_total",
    "Number of weather data records ingested",
)
ingest_duplicates_total = Counter(
    "ingest_duplicates_total",
    "Number of duplicate upload batches rejected",
)
ota_chunks_served_total = Counter(
    "ota_chunks_served_total",
    "Number of OTA firmware chunks served to devices",
)
cert_verify_failures_total = Counter(
    "cert_verify_failures_total",
    "mTLS client certificate verification failures",
    ["reason"],
)
ingest_lag_seconds = Histogram(
    "ingest_lag_seconds",
    "Seconds between oldest sample timestamp in batch and server ingest time",
    buckets=[60, 120, 300, 600, 900, 1800, 3600, float("inf")],
)
ota_campaign_success_rate = Gauge(
    "ota_campaign_success_rate",
    "Success rate (0.0–1.0) of a completed OTA campaign",
    ["campaign_id"],
)
