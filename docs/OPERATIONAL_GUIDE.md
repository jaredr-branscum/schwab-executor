# Charles Schwab Execution Engine (`schwab-executor`)
## Production Operational Guide & Runbook

This runbook outlines operational procedures, deployment patterns, and incident recovery workflows for operating the `schwab-executor`.

---

## 1. System Overview & Architecture

`schwab-executor` is designed to interface with the Charles Schwab Trader API (`/trader/v1`). It translates quantitative strategy allocations into sequenced broker orders, applies pre-trade risk controls, dispatches orders over WinHTTP TLS connections, and generates execution telemetry.

### Core Architectural Components

```text
┌────────────────────────────────────────────────────────┐
│             IStrategy / CompositeStrategy              │
│       (Evaluates positions & generates targets)        │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│                      RiskGuard                         │
│  - Max Order Notional Check    - Position Exposure Cap │
│  - Max Order Quantity Bound    - Price Collar Bounds   │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│                   ExecutionDriver                      │
│  - Sell-First Sequencing       - Sub-millisecond Queue │
│  - Rollback & Idempotency      - Latency Benchmarking  │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│             HttpClient (WinHTTP C++20)                 │
│  - TLS Pre-warming             - Keep-Alive Reuse      │
│  - Sub-millisecond Execution   - Zero-Alloc Headers    │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
           Charles Schwab Trader API Endpoint
```

---

## 2. Operating Mode & Cadence

The execution engine provides:

### Immediate Execution (`schwab_executor execute`)
* **Purpose:** Performs a synchronous, single-pass portfolio rebalance and order execution run.
* **Mechanism:** Queries account cash and positions from Charles Schwab, evaluates the registered quantitative strategy (`IStrategy::generate_orders`), passes proposed orders through `RiskGuard` safety gates, sequences orders (liquidations and trims first, then acquisitions), submits orders to Schwab API, prints formatted execution telemetry, and exits immediately with code `0` (or `1` on error).
* **Recommended Usage Pattern:** Best suited for automated scheduling via external job managers (Windows Task Scheduler, Linux cron, Apache Airflow) where deterministic execution and audit logging per run are required.

## 3. Pre-Flight Checklist

Before launching execution jobs, complete the following pre-flight verification:

### Step 1: Configuration File (`.env`)
Verify that `.env` exists and contains valid credentials:
```ini
SCHWAB_APP_KEY=<your_app_key>
SCHWAB_APP_SECRET=<your_app_secret>
SCHWAB_CALLBACK_URL=https://127.0.0.1
SCHWAB_TOKEN_PATH=.schwab_token.json
SCHWAB_BASE_URL=https://api.schwabapi.com
SCHWAB_ACCOUNTS=all
SCHWAB_DRY_RUN=true
SCHWAB_MAX_ORDER_NOTIONAL=100000.0
SCHWAB_REQUEST_TIMEOUT_MS=5000
```

> [!IMPORTANT]
> Never commit `.env` or `.schwab_token.json` to version control. Ensure `.gitignore` includes both files.

### Step 2: Verify OAuth Session Status
Run the status check to inspect token expiration and pre-computed authorization headers:
```powershell
.\build\Debug\schwab_executor.exe status
```

Expected output:
```text
================================================================================
  [SCHWAB-EXECUTOR] LIVE STATUS & DIAGNOSTICS
================================================================================
  * OAuth Session Status : [AUTHENTICATED]
  * Access Token State   : [VALID]
  * Access Token TTL     : 1785.4s remaining
  * Refresh Token TTL    : 6.94 days remaining
  * Pre-baked Auth Header: Bearer ey...xxxx

  --- Charles Schwab Account Diagnostics ---
  * Accounts Linked       : 1 Account(s)
    - Account 1 [***1234 - MARGIN]: Liquidation Value = $100,000.00 | Cash = $10,000.00
  * Total Portfolio Equity: $100,000.00
  * Total Available Cash  : $10,000.00
================================================================================
```

If the session is expired or unauthenticated, proceed to [OAuth 2.0 Maintenance](#6-oauth-20-maintenance--lifecycle).

---

## 4. Pre-Trade Simulation & Dry Run Workflow

Always validate strategy behavior in simulation mode before executing live broker orders.

### Executing Dry Run
```powershell
.\build\Debug\schwab_executor.exe execute --dry-run
```

During a dry-run execution:
1. Live account portfolio holdings and cash balances are queried via the API.
2. The active strategy evaluates the portfolio and emits target orders.
3. Pre-trade risk validation runs (notional caps, share bounds, price collars, MOC cutoffs).
4. Order payloads are fully formatted and validated against the Schwab Trader API schema.
5. Simulated orders receive synthetic order IDs (`DRY_RUN_SIMULATED`) without submitting live market orders.
6. Execution latency and telemetry tables are output to stdout (telemetry log files are not written during dry runs to prevent audit trail contamination).

### Sample Dry-Run Telemetry Output
```text
================================================================================================================
  [SCHWAB-EXECUTOR] SIMULATION TELEMETRY (DRY RUN)
================================================================================================================
  SIDE  SYMBOL    TAG           QUANTITY    TYPE            STATUS      ORDER ID              LATENCY (ms)    NOTES
  ----------------------------------------------------------------------------------------------------------------
  SELL  MSFT      REBALANCE     15.00       MARKET          SIMULATED   DRY_RUN_SIMULATED     0.02            
  BUY   AAPL      REBALANCE     30.00       MARKET          SIMULATED   DRY_RUN_SIMULATED     0.02            
  ----------------------------------------------------------------------------------------------------------------
  Total Orders Submitted: 2 | Succeeded: 2 | Failed: 0 | Total Latency: 0.04 ms
================================================================================================================
```

---

## 5. Live Production Execution: Immediate Execution Mode (`execute`)

The **Immediate Execution** mode (`schwab_executor execute`) evaluates current portfolio state and dispatches rebalance orders once before terminating with an explicit exit code (`0` for success, `1` for error).

```powershell
# Live order submission with telemetry logging
.\build\Debug\schwab_executor.exe execute --telemetry-log logs/execution_telemetry.log
```

### Immediate Execution Lifecycle
When invoked, the engine completes the following sequence:
1. **Configuration Resolution:** Loads credentials, target account hash, and safety thresholds from `.env` or custom `--config`.
2. **Session Health Verification:** Inspects cached OAuth token in `.schwab_token.json`. If fewer than 60 seconds of TTL remain, the engine transparently refreshes the access token using the refresh token before proceeding.
3. **Portfolio State Ingestion:** Sends an authenticated REST request to `/trader/v1/accounts/{accountNumber}` to retrieve current cash balance, liquidation value, and existing positions.
4. **Strategy Order Generation:** Feeds live positions and available liquid capital into the active strategy (e.g. `SampleRebalanceStrategy` or custom C++ model) to generate target orders.
5. **Pre-Trade Risk Controls (`RiskGuard`):** Validates all orders against notional limits (`SCHWAB_MAX_ORDER_NOTIONAL`), share count limits, price collars, and MOC cutoff deadlines.
6. **Low-Latency Order Routing:** Submits orders sequentially to the Charles Schwab order placement endpoint:
   - Sells are dispatched first to release purchasing power.
   - Buys are dispatched second using realized cash.
   - Per-order round-trip HTTP latency is measured using microsecond-precision monotonic clocks.
7. **Telemetry Persistence:** Prints the ASCII telemetry table to the console and appends to `--telemetry-log` (if configured and in live execution mode).
8. **Exit Code:** Returns `0` if all staged orders completed successfully, or `1` if risk checks rejected orders or broker network errors occurred.

### CLI Flags for Immediate Execution
- `--dry-run`: Force simulation mode regardless of `.env` settings.
- `--account-hash <hash>`: Override the target Schwab encrypted account hash.
- `--telemetry-log <path>` or `--log <path>`: Append execution reports to the specified file (live execution only; skipped during dry runs).
- `--config <path>`: Specify an alternative `.env` configuration file path.
- `--token-path <path>`: Specify an alternative token cache path (default: `.schwab_token.json`).

### Scheduling via Windows Task Scheduler
Immediate execution is ideal for external schedulers. To run rebalancing every weekday at 3:59 PM:
```powershell
# Create a Windows Scheduled Task for 3:59 PM Mon-Fri
$Action = New-ScheduledTaskAction -Execute "build\Release\schwab_executor.exe" `
                                  -Argument "execute --telemetry-log logs/execution_telemetry.log" `
                                  -WorkingDirectory (Get-Location).Path
$Trigger = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday,Tuesday,Wednesday,Thursday,Friday -At 3:59PM
Register-ScheduledTask -TaskName "SchwabExecutorDailyMOC" -Action $Action -Trigger $Trigger -Description "Daily Schwab Rebalance"
```

---

## 6. OAuth 2.0 Maintenance & Lifecycle

Charles Schwab implements a dual-token OAuth 2.0 architecture:

| Token | Validity Period | Renewal Mechanism |
| :--- | :--- | :--- |
| **Access Token** | 30 minutes | Automatically refreshed using refresh token. |
| **Refresh Token** | 7 days | Requires browser login with 2FA once every 7 days. |

### Automatic Refresh Workflow
The engine proactively refreshes the access token:
- Automatically before immediate execution if fewer than 60 seconds remain.
- Manually via `python python/auth_cli.py --refresh`.

### Weekly 7-Day Re-Authentication Procedure
When the 7-day refresh token expires (or if revoked by Schwab), the operator must re-authenticate:

1. Run the interactive authenticator:
   ```powershell
   .\build\Release\schwab_executor.exe auth
   # Alternatively: python python/auth_cli.py --login
   # To explicitly force re-authentication even if a cached token exists:
   .\build\Release\schwab_executor.exe auth --force
   ```
   > [!NOTE]
   > If the refresh token has expired or is revoked on Schwab's servers, the engine automatically detects the `invalid_grant` rejection, invalidates stale local timestamps, and immediately launches the interactive browser authorization flow without manual token deletion.
2. Follow the URL displayed in the console, log into Charles Schwab in your browser, complete 2FA, and click **Agree**.
3. Copy the URL from the browser's address bar (even if the page shows a blank or unreachable screen) and paste it into the terminal.
4. The new credentials will be written atomically to `.schwab_token.json`.

---

## 7. Telemetry & Execution Audit Trails

All trade executions emit ASCII telemetry records. When `--telemetry-log` or `SCHWAB_TELEMETRY_LOG_PATH` is configured, live execution reports are appended with UTC timestamps (dry-run simulations only output to stdout):

```text
[2026-10-11 19:30:00 UTC] Mode: EXECUTE | Strategy: SampleRebalanceStrategy
================================================================================================================
  [SCHWAB-EXECUTOR] ORDER EXECUTION TELEMETRY REPORT
================================================================================================================
  SIDE  SYMBOL    TAG           QUANTITY    TYPE            STATUS      ORDER ID              LATENCY (ms)    NOTES
  ----------------------------------------------------------------------------------------------------------------
  SELL  MSFT      REBALANCE     15.00       MARKET          FILLED      10084928172           18.42           
  BUY   AAPL      REBALANCE     30.00       MARKET          FILLED      10084928173           17.95           
  ----------------------------------------------------------------------------------------------------------------
  Total Orders Submitted: 2 | Succeeded: 2 | Failed: 0 | Total Latency: 36.37 ms
================================================================================================================
```

### Telemetry Field Dictionary
- **SIDE:** `BUY` or `SELL`. Sell orders are automatically sequenced before buy orders to free liquid capital.
- **SYMBOL:** Up to 8-character ticker symbol stored in zero-allocation `Symbol` representation.
- **TAG:** Zero-allocation 16-character strategy attribution identifier (e.g. `REBALANCE`, `LIQUIDATE`).
- **QUANTITY:** Number of shares executed.
- **TYPE:** `MARKET`, `LIMIT`, `STOP`, or `STOP_LIMIT`.
- **STATUS:** `FILLED/STAGED` (live), `SIMULATED` (dry-run), or `FAILED`.
- **ORDER ID:** Unique Schwab broker order ID or dry-run placeholder.
- **LATENCY (ms):** Round-trip HTTP network execution latency in milliseconds (rounded by microseconds).
- **NOTES:** Error descriptions or rejection reasons returned by the broker or risk controls.

---

## 8. Incident Response & Troubleshooting Runbook

### Issue 1: Authentication Failure (`HTTP 401 Unauthorized`)
- **Symptom:** CLI displays `[UNAUTHENTICATED] No active session found` or API returns `401 Unauthorized`.
- **Root Cause:** Refresh token exceeded 7 days, was revoked/invalidated by Schwab, or access token could not be refreshed.
- **Remediation:**
  1. Run `.\build\Release\schwab_executor.exe auth` (or `.\build\Release\schwab_executor.exe auth --force`) to launch the interactive OAuth 2.0 browser handshake.
  2. Complete the login and paste the resulting callback URL into the terminal.
  3. Verify that `.\build\Release\schwab_executor.exe status` reports `* OAuth Session Status : [AUTHENTICATED]`.

### Issue 2: Pre-Trade Risk Rejection (`RiskGuard`)
- **Symptom:** Execution halts before orders reach the broker with an error like `Order notional $150,000 exceeds maximum allowable limit of $100,000`.
- **Root Cause:** A generated order exceeds `SCHWAB_MAX_ORDER_NOTIONAL`.
- **Remediation:**
  1. Inspect portfolio size and strategy allocation percentages.
  2. If the order is intentional, adjust `SCHWAB_MAX_ORDER_NOTIONAL` in `.env` to accommodate higher capacity.
  3. Re-run in simulation mode (`--dry-run`) to verify compliance before live submission.

### Issue 3: Schwab API Rate Limit (`HTTP 429 Too Many Requests`)
- **Symptom:** Order placement returns status `429`.
- **Root Cause:** Schwab API limits request rates per app key (typically 120 requests/minute).
- **Remediation:**
  1. The execution driver implements sequential ordering with latency measurement; avoid firing automated requests faster than 120 calls per minute.
  2. Avoid running multiple simultaneous processes with the same app key.

### Issue 4: Missing Account Specification
- **Symptom:** Execution outputs `Zero accounts matched filter` or account query error.
- **Root Cause:** Neither `SCHWAB_ACCOUNTS` nor `--accounts` nor `--account-hash` was provided or no linked accounts matched.
- **Remediation:**
  1. Run `.\build\Debug\schwab_executor.exe status` to view linked accounts and encrypted hashes.
  2. Set `SCHWAB_ACCOUNTS=all` in `.env` (or pass `--accounts all` on the CLI) to target all linked accounts.
  3. Alternatively, specify account numbers or hashes: `SCHWAB_ACCOUNTS=1234,5678`.
