"""Structured JSON logging — call configure_logging() once at app startup."""
from __future__ import annotations

import logging
import logging.config
import logging.handlers
from pathlib import Path


def configure_logging() -> None:
    log_file = Path(__file__).parent.parent / "logs" / "app.log"
    log_file.parent.mkdir(exist_ok=True)

    # pythonjsonlogger 3.x (installed on Python 3.13 host) uses pythonjsonlogger.json
    json_formatter = {
        "()": "pythonjsonlogger.json.JsonFormatter",
        "format": "%(asctime)s %(name)s %(levelname)s %(message)s",
    }

    logging.config.dictConfig(
        {
            "version": 1,
            "disable_existing_loggers": False,
            "formatters": {"json": json_formatter},
            "handlers": {
                "file": {
                    "class": "logging.handlers.RotatingFileHandler",
                    "filename": str(log_file),
                    "maxBytes": 50 * 1024 * 1024,
                    "backupCount": 5,
                    "formatter": "json",
                },
                "console": {
                    "class": "logging.StreamHandler",
                    "formatter": "json",
                },
            },
            "root": {"level": "INFO", "handlers": ["file", "console"]},
            "loggers": {
                "uvicorn": {"handlers": ["file", "console"], "propagate": False},
                "uvicorn.access": {"handlers": ["file", "console"], "propagate": False},
                "gunicorn.error": {"handlers": ["file", "console"], "propagate": False},
            },
        }
    )
