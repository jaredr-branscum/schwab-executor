#!/usr/bin/env python3
"""
Charles Schwab OAuth 2.0 Standalone Authentication Helper for schwab-executor.
==============================================================================
Token management:
  * Generates browser login URL for user approval and 2FA.
  * Exchanges callback code for access and refresh tokens.
  * Automatically refreshes expired access tokens.
  * Caches tokens locally in .schwab_token.json.

Usage:
  python python/auth_cli.py --login             # Interactive browser login & token generation
  python python/auth_cli.py --check             # Check if active token is valid (exit 0: valid, 1: invalid)
  python python/auth_cli.py --refresh           # Refresh access token using cached refresh token
  python python/auth_cli.py --get-access-token  # Output active Bearer access token string
"""

import argparse
import base64
import gzip
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# Ensure local common module is importable
sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (
    DEFAULT_BASE_URL,
    DEFAULT_CALLBACK_URL,
    DEFAULT_TOKEN_PATH,
    load_env,
    load_token as common_load_token,
    save_token as common_save_token,
)

OAUTH_AUTHORIZE_PATH = "/v1/oauth/authorize"
OAUTH_TOKEN_PATH = "/v1/oauth/token"


def _read_and_decompress(resp_or_fp, headers=None) -> bytes:
    """Read response payload and decompress if gzip encoded or starts with gzip magic bytes."""
    if not resp_or_fp:
        return b""
    try:
        raw = resp_or_fp.read()
    except Exception:
        return b""
    is_gzip = False
    if headers and headers.get("Content-Encoding", "").lower() == "gzip":
        is_gzip = True
    elif len(raw) >= 2 and raw[0] == 0x1F and raw[1] == 0x8B:
        is_gzip = True
    if is_gzip:
        try:
            return gzip.decompress(raw)
        except Exception:
            return raw
    return raw


class SchwabAuth:
    def __init__(self, env_file: Path = Path(".env"), token_path: Path = None):
        env = load_env(env_file)
        # Fall back to OS environment variables if not in .env
        self.app_key = env.get("SCHWAB_APP_KEY") or os.environ.get("SCHWAB_APP_KEY", "")
        self.app_secret = env.get("SCHWAB_APP_SECRET") or os.environ.get("SCHWAB_APP_SECRET", "")
        self.callback_url = env.get("SCHWAB_CALLBACK_URL") or os.environ.get("SCHWAB_CALLBACK_URL", DEFAULT_CALLBACK_URL)
        if token_path is not None:
            self.token_path = Path(token_path)
        else:
            token_str = env.get("SCHWAB_TOKEN_PATH") or os.environ.get("SCHWAB_TOKEN_PATH", DEFAULT_TOKEN_PATH)
            self.token_path = Path(token_str)
        self.base_url = (env.get("SCHWAB_BASE_URL") or os.environ.get("SCHWAB_BASE_URL", DEFAULT_BASE_URL)).rstrip("/")

    def get_authorization_url(self) -> str:
        if not self.app_key:
            raise ValueError("SCHWAB_APP_KEY is not configured in .env or environment variables.")
        params = {
            "client_id": self.app_key,
            "redirect_uri": self.callback_url,
        }
        return f"{self.base_url}{OAUTH_AUTHORIZE_PATH}?{urllib.parse.urlencode(params)}"

    @staticmethod
    def extract_code(raw_input: str) -> str:
        raw = raw_input.strip()
        if not raw:
            raise ValueError("Authorization code or URL cannot be empty.")
        if "code=" in raw:
            parsed = urllib.parse.urlparse(raw)
            qs = parsed.query or parsed.fragment
            params = urllib.parse.parse_qs(qs)
            codes = params.get("code")
            if codes and codes[0]:
                return urllib.parse.unquote(codes[0])
            raise ValueError(f"Could not parse 'code' parameter from URL: {raw}")
        return raw

    def load_token(self) -> dict:
        return common_load_token(self.token_path)

    def save_token(self, token_data: dict) -> None:
        common_save_token(self.token_path, token_data)

    def is_authenticated(self) -> bool:
        token = self.load_token()
        if not token:
            return False
        # Valid if refresh token expires more than 5 minutes in future
        refresh_expires_at = float(token.get("refresh_token_expires_at", 0.0))
        return time.time() + 300.0 < refresh_expires_at

    def exchange_code(self, code_or_url: str) -> dict:
        clean_code = self.extract_code(code_or_url)
        payload = {
            "grant_type": "authorization_code",
            "code": clean_code,
            "redirect_uri": self.callback_url,
        }
        return self._post_token(payload)

    def refresh_token(self) -> dict:
        current = self.load_token()
        if not current or "refresh_token" not in current:
            raise RuntimeError("No cached refresh token found. Re-login required.")
        payload = {
            "grant_type": "refresh_token",
            "refresh_token": current["refresh_token"],
        }
        return self._post_token(payload, existing=current)

    def get_valid_access_token(self) -> str:
        token = self.load_token()
        if not token or not token.get("access_token"):
            raise RuntimeError("No valid access token available. Please log in.")
        # Auto-refresh if access token expires within 60 seconds
        expires_at = float(token.get("expires_at", 0.0))
        if time.time() + 60.0 >= expires_at:
            token = self.refresh_token()
        return token["access_token"]

    def _post_token(self, payload: dict, existing: dict = None) -> dict:
        token_url = f"{self.base_url}{OAUTH_TOKEN_PATH}"
        auth_bytes = f"{self.app_key}:{self.app_secret}".encode("utf-8")
        headers = {
            "Authorization": f"Basic {base64.b64encode(auth_bytes).decode('utf-8')}",
            "Content-Type": "application/x-www-form-urlencoded",
        }
        encoded_data = urllib.parse.urlencode(payload).encode("utf-8")
        req = urllib.request.Request(token_url, data=encoded_data, headers=headers, method="POST")

        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                resp_bytes = _read_and_decompress(resp, resp.headers)
                try:
                    resp_data = json.loads(resp_bytes.decode("utf-8"))
                except json.JSONDecodeError as je:
                    raise RuntimeError(f"Malformed JSON token response from Schwab: {je}") from je
        except urllib.error.HTTPError as e:
            raw_err = _read_and_decompress(e.fp, e.headers)
            err_msg = raw_err.decode("utf-8", errors="replace") if raw_err else str(e)

            # Invalidate cached token if refresh token is rejected, expired, or revoked
            if payload.get("grant_type") == "refresh_token" and (
                "invalid_grant" in err_msg
                or "invalid, expired or revoked" in err_msg.lower()
                or e.code in (400, 401)
            ):
                target_token = existing if existing else self.load_token()
                if target_token:
                    target_token["refresh_token_expires_at"] = 0.0
                    target_token["expires_at"] = 0.0
                    try:
                        self.save_token(target_token)
                    except Exception:
                        pass

            raise RuntimeError(f"HTTP {e.code} Token Error: {err_msg}") from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"Network error connecting to Schwab: {e.reason}") from e

        now = time.time()
        # Default 30 min (1800s) for access token
        expires_in = float(resp_data.get("expires_in", 1800))

        new_refresh = resp_data.get("refresh_token")
        if new_refresh:
            refresh_token = str(new_refresh)
            refresh_expires_at = now + 7 * 24 * 3600
        elif existing:
            refresh_token = existing.get("refresh_token", "")
            refresh_expires_at = float(existing.get("refresh_token_expires_at", now + 7 * 24 * 3600))
        else:
            refresh_token = ""
            refresh_expires_at = now + 7 * 24 * 3600

        token_record = {
            "access_token": resp_data["access_token"],
            "refresh_token": refresh_token,
            "token_type": resp_data.get("token_type", "Bearer"),
            "expires_at": now + expires_in,
            "refresh_token_expires_at": refresh_expires_at,
            "scope": resp_data.get("scope", ""),
            "id_token": resp_data.get("id_token"),
        }
        self.save_token(token_record)
        return token_record


def run_login(auth: SchwabAuth, force: bool = False) -> int:
    print("\n" + "=" * 80)
    print("  [CHARLES SCHWAB OAUTH 2.0 AUTHENTICATION - SCHWAB-EXECUTOR]")
    print("=" * 80)

    if not force:
        token = auth.load_token()
        if token and auth.is_authenticated():
            expires_at = float(token.get("expires_at", 0.0))
            if time.time() + 60.0 < expires_at:
                print("  * Status: [AUTHENTICATED] Active token cache is valid and ready.")
                print("  * (Use '--force' or 'schwab_executor auth --force' to force re-authentication.)")
                return 0
            # If access token is expired, test if refresh token works
            try:
                auth.refresh_token()
                print("  * Status: [AUTHENTICATED] Active session auto-refreshed successfully.")
                return 0
            except Exception as e:
                print(f"  * Cached session expired or cannot be refreshed ({e}). Launching login flow...")

    try:
        url = auth.get_authorization_url()
    except Exception as e:
        print(f"  [CONFIGURATION ERROR] {e}")
        return 1

    print("  * Initial authorization required.")
    print("\n  Step 1: Open this URL in your browser:")
    print(f"  {url}\n")
    print("  Step 2: Log in with your Schwab credentials and click 'Agree'.")
    print("  Step 3: Copy the resulting callback URL from the browser address bar.")

    try:
        redirect_input = input("\n  Paste Redirected URL or Code: ").strip()
    except (EOFError, KeyboardInterrupt):
        print("\n  [ABORTED] Authentication cancelled.")
        return 1

    if not redirect_input:
        print("  [ERROR] Input cannot be empty.")
        return 1

    try:
        auth.exchange_code(redirect_input)
        print("\n  * SUCCESS: OAuth 2.0 token received and saved to", auth.token_path)
        return 0
    except Exception as e:
        print(f"\n  * [ERROR] Token exchange failed: {e}")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Schwab-Executor Standalone Auth Helper")
    parser.add_argument("--login", action="store_true", help="Run interactive browser OAuth login flow")
    parser.add_argument("--force", action="store_true", help="Force interactive login even if active token exists")
    parser.add_argument("--check", action="store_true", help="Check if token is valid (exits 0 if valid, 1 if not)")
    parser.add_argument("--refresh", action="store_true", help="Refresh access token")
    parser.add_argument("--get-access-token", action="store_true", help="Print valid Bearer access token string")
    parser.add_argument("--env", type=str, default=".env", help="Path to .env configuration file")
    parser.add_argument("--token-path", type=str, default=None, help="Path to token cache file")

    args = parser.parse_args()
    token_arg = Path(args.token_path) if args.token_path else None
    auth = SchwabAuth(env_file=Path(args.env), token_path=token_arg)

    if args.check:
        if auth.is_authenticated():
            try:
                auth.get_valid_access_token()
                print("AUTHENTICATED")
                return 0
            except Exception:
                print("UNAUTHENTICATED")
                return 1
        else:
            print("UNAUTHENTICATED")
            return 1

    if getattr(args, "get_access_token", False):
        try:
            token = auth.get_valid_access_token()
            print(token)
            return 0
        except Exception as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 1

    if args.refresh:
        try:
            auth.refresh_token()
            print("REFRESH_SUCCESS")
            return 0
        except Exception as e:
            print(f"REFRESH_FAILED: {e}", file=sys.stderr)
            return 1

    # Default to login flow if --login, --force, or no arguments passed
    return run_login(auth, force=args.force)


if __name__ == "__main__":
    sys.exit(main())
