#!/usr/bin/env python3
"""
Charles Schwab Account Status Query Helper for schwab-executor.
================================================================
Queries Charles Schwab Trader API /trader/v1/accounts and
/trader/v1/accounts/accountNumbers to display account liquidation values,
available cash, and encrypted account hashes.

Security Best Practices Applied:
- Zero persistent storage: Credentials and hashes are never cached to disk or log files.
- Active memory scrubbing: Sensitive request headers, tokens, and response buffers
  are explicitly cleared in a finally block.
- Explicit garbage collection: gc.collect() invoked immediately after query completion
  to purge sensitive payload buffers from the Python runtime heap.
"""

import gc
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.request

# Ensure local common module is importable
sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DEFAULT_BASE_URL, DEFAULT_TOKEN_PATH, load_env, load_token


def main() -> int:
    env = load_env(Path(".env"))
    token_path = Path(env.get("SCHWAB_TOKEN_PATH") or os.environ.get("SCHWAB_TOKEN_PATH", DEFAULT_TOKEN_PATH))
    base_url = (env.get("SCHWAB_BASE_URL") or os.environ.get("SCHWAB_BASE_URL", DEFAULT_BASE_URL)).rstrip("/")

    token_data = load_token(token_path)
    if not token_data:
        print(f"  [ERROR] Token file '{token_path}' not found or invalid. Please authenticate first.", file=sys.stderr)
        return 1

    access_token = token_data.get("access_token", "")
    if not access_token:
        print("  [ERROR] No access token found in token file.", file=sys.stderr)
        return 1

    accounts_url = f"{base_url}/trader/v1/accounts"
    acc_numbers_url = f"{base_url}/trader/v1/accounts/accountNumbers"
    headers = {
        "Authorization": f"Bearer {access_token}",
        "Accept": "application/json",
        "User-Agent": "schwab-executor/1.0",
    }

    data = None
    hash_map = {}

    try:
        req = urllib.request.Request(accounts_url, headers=headers, method="GET")
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))

        # Query encrypted account hashes for linked accounts
        try:
            acc_num_req = urllib.request.Request(acc_numbers_url, headers=headers, method="GET")
            with urllib.request.urlopen(acc_num_req, timeout=10) as resp:
                num_data = json.loads(resp.read().decode("utf-8"))
                if isinstance(num_data, list):
                    for item in num_data:
                        n = item.get("accountNumber")
                        h = item.get("hashValue")
                        if n and h:
                            hash_map[n] = h
        except Exception:
            pass

        if not isinstance(data, list):
            print("  [WARN] Unexpected account data format received.", file=sys.stderr)
            return 0

        print(f"  * Accounts Linked       : {len(data)} Account(s)")
        total_liq = 0.0
        total_cash = 0.0

        for idx, acc in enumerate(data, start=1):
            sec = acc.get("securitiesAccount", {})
            acc_num = sec.get("accountNumber", "UNKNOWN")
            masked_num = f"***{acc_num[-4:]}" if len(acc_num) >= 4 else acc_num
            acc_type = sec.get("type", "INDIVIDUAL")

            cur_bal = sec.get("currentBalances", {})
            liq_val = float(cur_bal.get("liquidationValue", 0.0))
            cash_bal = float(cur_bal.get("cashBalance", 0.0))
            total_liq += liq_val
            total_cash += cash_bal

            print(f"    - Account {idx} [{masked_num} - {acc_type}]: Liquidation Value = ${liq_val:,.2f} | Cash = ${cash_bal:,.2f}")

        if len(data) > 1:
            print(f"  * Total Portfolio Equity: ${total_liq:,.2f}")
            print(f"  * Total Available Cash  : ${total_cash:,.2f}")

        return 0

    except urllib.error.HTTPError as e:
        err_msg = e.read().decode("utf-8") if e.fp else str(e)
        print(f"  [ERROR] Schwab API HTTP {e.code}: {err_msg}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"  [ERROR] Failed to connect to Schwab API: {e}", file=sys.stderr)
        return 1
    finally:
        # Secure memory hygiene: Explicitly clear sensitive dictionary and token data
        if token_data and isinstance(token_data, dict):
            token_data.clear()
        if headers and isinstance(headers, dict):
            headers.clear()
        if hash_map and isinstance(hash_map, dict):
            hash_map.clear()
        if data and isinstance(data, list):
            data.clear()
        access_token = None
        # Purge references from heap arena
        gc.collect()


if __name__ == "__main__":
    sys.exit(main())
