import os
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict

# Allow the env file path to be overridden via IOT_ENV_FILE; default to
# html/etc/iot.env relative to this file's location (i.e. ../etc/iot.env).
_ENV_FILE = os.getenv(
    "IOT_ENV_FILE",
    str(Path(__file__).parent.parent / "etc" / "iot.env"),
)


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=_ENV_FILE,
        env_file_encoding="utf-8",
        extra="ignore",
    )

    db_dsn: str = ""
    jwt_secret: str = ""
    firmware_dir: str = ""
    firmware_keep_n: int = 3
    slot_len_sec: int = 43200           # 12 h; must match device upload cadence
    max_firmware_size_bytes: int = 491520  # 480 KB — STM32L476RG app Flash partition


settings = Settings()
