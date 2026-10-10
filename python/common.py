#!/usr/bin/env python3
"""
Common utilities and constants for schwab-executor Python helpers.
"""

import json
import os
from pathlib import Path
import sys

DEFAULT_BASE_URL = "https://api.schwabapi.com"
DEFAULT_CALLBACK_URL = "https://127.0.0.1"
DEFAULT_TOKEN_PATH = ".schwab_token.json"


def load_env(env_path: Path = Path(".env")) -> dict:
    """.env configuration parser with zero third-party dependencies."""
    config = {}
    if not env_path.exists():
        return config
    try:
        with open(env_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, val = line.split("=", 1)
                    config[key.strip()] = val.strip().strip("'\"")
    except OSError as e:
        print(f"[WARN] Failed to read environment file '{env_path}': {e}", file=sys.stderr)
    return config


def load_token(token_path: Path = Path(DEFAULT_TOKEN_PATH)) -> dict:
    """Load and parse JSON OAuth token cache safely."""
    if not token_path.exists():
        return {}
    try:
        with open(token_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except json.JSONDecodeError as e:
        print(f"[WARN] Token file '{token_path}' is corrupted JSON: {e}", file=sys.stderr)
        return {}
    except OSError as e:
        print(f"[ERROR] Cannot access token file '{token_path}': {e}", file=sys.stderr)
        return {}


def save_token(token_path: Path, token_data: dict) -> None:
    """Atomically write token dictionary to disk using a temporary file."""
    token_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = token_path.with_suffix(".tmp")
    with open(temp_path, "w", encoding="utf-8") as f:
        json.dump(token_data, f, indent=2)
    temp_path.replace(token_path)
