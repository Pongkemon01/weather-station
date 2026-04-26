-- Migration 0001: initial schema
-- Creates all tables, TimescaleDB hypertable, compression policy, and indexes.
-- Prerequisite: TimescaleDB extension must already exist (created during host provisioning).

CREATE TABLE devices (
    id          SERIAL       PRIMARY KEY,
    region_id   SMALLINT     NOT NULL,
    station_id  SMALLINT     NOT NULL,
    last_seen   TIMESTAMPTZ,
    created_at  TIMESTAMPTZ  DEFAULT now(),
    UNIQUE (region_id, station_id)
);

CREATE TABLE weather_records (
    time        TIMESTAMPTZ  NOT NULL,
    device_id   INT          NOT NULL REFERENCES devices(id),
    temperature REAL,
    humidity    REAL,
    pressure    REAL,
    light_par   SMALLINT,
    rainfall    REAL,
    dew_point   REAL,
    bus_value   REAL
);

SELECT create_hypertable('weather_records', 'time');

CREATE INDEX ON weather_records (device_id, time DESC);

ALTER TABLE weather_records SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'device_id'
);

SELECT add_compression_policy('weather_records', INTERVAL '7 days');

CREATE TABLE ingest_log (
    idempotency_key VARCHAR(320) PRIMARY KEY,
    recorded_at     TIMESTAMPTZ  DEFAULT now()
);

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
    target_cohort_ids   TEXT[],
    status              VARCHAR(32)  DEFAULT 'draft',
    success_rate        NUMERIC,
    created_at          TIMESTAMPTZ  DEFAULT now(),
    updated_at          TIMESTAMPTZ  DEFAULT now()
);

CREATE TABLE download_completions (
    campaign_id INT          NOT NULL REFERENCES ota_campaigns(id),
    device_id   VARCHAR(6)   NOT NULL,
    chunk_index INT          NOT NULL,
    recorded_at TIMESTAMPTZ  DEFAULT now(),
    PRIMARY KEY (campaign_id, device_id, chunk_index)
);

CREATE TABLE admin_users (
    id            SERIAL       PRIMARY KEY,
    username      VARCHAR(64)  NOT NULL UNIQUE,
    password_hash VARCHAR(72)  NOT NULL,
    role          VARCHAR(16)  NOT NULL DEFAULT 'viewer',
    created_at    TIMESTAMPTZ  DEFAULT now()
);
