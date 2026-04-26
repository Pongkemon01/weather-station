-- Reference DDL for the IoT Weather Station database.
-- Applied incrementally via app/db/migrations/; do not run this file directly.
-- TimescaleDB extension must already exist (created during host provisioning).

CREATE TABLE devices (
    id          SERIAL       PRIMARY KEY,
    region_id   SMALLINT     NOT NULL,
    station_id  SMALLINT     NOT NULL,
    last_seen   TIMESTAMPTZ,
    created_at  TIMESTAMPTZ  DEFAULT now(),
    UNIQUE (region_id, station_id)
);

-- Per-sample sensor readings; partitioned by time via TimescaleDB.
-- Fixed-point values decoded by app/ota/fixedpt.py before insert.
CREATE TABLE weather_records (
    time        TIMESTAMPTZ  NOT NULL,
    device_id   INT          NOT NULL REFERENCES devices(id),
    temperature REAL,       -- °C
    humidity    REAL,       -- %RH
    pressure    REAL,       -- kPa
    light_par   SMALLINT,   -- µmol/s·m²
    rainfall    REAL,       -- mm/hr
    dew_point   REAL,       -- °C
    bus_value   REAL        -- Blast Unit of Severity
);

SELECT create_hypertable('weather_records', 'time');
CREATE INDEX ON weather_records (device_id, time DESC);
ALTER TABLE weather_records SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'device_id'
);
SELECT add_compression_policy('weather_records', INTERVAL '7 days');

-- Idempotency guard: "{region:03d}{station:03d}:{first_sample_ts.isoformat()}"
CREATE TABLE ingest_log (
    idempotency_key VARCHAR(320) PRIMARY KEY,
    recorded_at     TIMESTAMPTZ  DEFAULT now()
);

-- OTA firmware rollout campaigns (Server_Architecture.md §3.3).
CREATE TABLE ota_campaigns (
    id                  SERIAL       PRIMARY KEY,
    version             INT          NOT NULL UNIQUE,
    description         TEXT,
    firmware_sha256     VARCHAR(64)  NOT NULL,
    firmware_size       INT          NOT NULL,
    firmware_file_path  TEXT         NOT NULL,
    rollout_start       TIMESTAMPTZ,
    rollout_window_days INT          DEFAULT 10,
    slot_len_sec        INT          NOT NULL DEFAULT 43200,
    target_cohort_ids   TEXT[],         -- NULL = entire fleet
    status              VARCHAR(32)  DEFAULT 'draft',
    success_rate        NUMERIC,        -- set at terminal-status transition
    created_at          TIMESTAMPTZ  DEFAULT now(),
    updated_at          TIMESTAMPTZ  DEFAULT now()
);

-- Per-device per-chunk download tracking (Arch §10 Q-S13).
-- Written by /get_firmware on every successfully served chunk.
CREATE TABLE download_completions (
    campaign_id INT          NOT NULL REFERENCES ota_campaigns(id),
    device_id   VARCHAR(6)   NOT NULL,  -- "{region:03d}{station:03d}"
    chunk_index INT          NOT NULL,  -- offset // 512
    recorded_at TIMESTAMPTZ  DEFAULT now(),
    PRIMARY KEY (campaign_id, device_id, chunk_index)
);

-- Human operators; roles: admin, operator, viewer.
CREATE TABLE admin_users (
    id            SERIAL       PRIMARY KEY,
    username      VARCHAR(64)  NOT NULL UNIQUE,
    password_hash VARCHAR(72)  NOT NULL,   -- bcrypt
    role          VARCHAR(16)  NOT NULL DEFAULT 'viewer',
    created_at    TIMESTAMPTZ  DEFAULT now()
);
