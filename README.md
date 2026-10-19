# Schwab Low-Latency C++ Execution Engine (`schwab-executor`)

High-performance, ultra-low-latency execution engine engineered in C++20 for Charles Schwab Trader API (`/trader/v1`) trading accounts.

---

## Key Highlights

- **Sub-Millisecond Low Latency:** Zero dynamic heap allocation on execution hot paths, stack-allocated string views, and pre-computed authorization headers.
- **L1 CPU Cache Line Aligned:** 64-byte cache-line aligned `Order` structs (`sizeof(Order) == 64`) to prevent cache splits and false sharing.
- **Native WinHTTP TLS Connection Pooling:** Pre-warms TCP/TLS handshakes to eliminate connection negotiation overhead during critical execution windows.
- **Decoupled Strategy Architecture:** Clean abstract interface (`IStrategy`) cleanly separating quantitative decision logic from execution infrastructure.
- **Deterministic Order Sequencing:** Automatic liquidation-and-sell-first sequencing to release buying power prior to staging buy allocations.
- **Multi-Gate Pre-Trade Risk Controls:** Enforces notional caps, price collars, position exposure limits, and Market-on-Close (MOC) cutoff gates via `RiskGuard`.
- **Real-Time Structured Telemetry:** Terminal ASCII execution reports with per-order latency benchmarking and persistent audit logging.

---

## Architecture Overview

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
│  - Zero-Alloc Headers          - Socket Pooling        │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
            Charles Schwab Trader API Endpoint
```

---

## Quick Start (3 Minutes)

### 1. Prerequisites
- **Operating System:** Windows 10 / 11
- **C++ Compiler:** Microsoft Visual Studio Build Tools 2022 (MSVC) with C++20 support
- **Build Tool:** CMake 3.20+
- **Python:** Python 3.10+ (Standard library only; zero pip dependencies)

### 2. Build & Test Instructions

The engine provides automated build scripts alongside direct CMake command-line support.

#### Option A: Automated Build Script (`build.bat`)
Run the one-click Windows MSVC/CMake build utility:
```cmd
# Build optimized Release configuration (default, recommended for production)
build.bat

# Or select specific configurations / actions:
build.bat Release       # Compiles optimized Release binary (AVX2, inlined hot paths)
build.bat Debug         # Compiles Debug binary with debug symbols
build.bat all           # Compiles both Debug and Release configurations
build.bat clean         # Purges build/ directory and compiles completely fresh
build.bat test          # Compiles and runs the full Catch2 unit test suite
```

#### Option B: Unit Test Suite Runner (`run_tests.bat`)
Execute the Catch2 unit test suite across all 108 test cases (1,665 assertions):
```cmd
run_tests.bat           # Compiles and runs tests in Debug mode
run_tests.bat Release   # Compiles and runs tests in Release mode
```
*(All 1,665 Catch2 assertions execute in ~0.5s with zero warnings).*

#### Option C: Direct CMake Commands (Manual Workflow)
For customized developer environments or CI/CD pipelines:
```powershell
# 1. Configure CMake build system
cmake -B build -S .

# 2. Compile target configuration (multi-core parallel build)
cmake --build build --config Release --parallel
# Or for Debug:
cmake --build build --config Debug --parallel

# 3. Run Catch2 test harness
ctest --test-dir build -C Release --output-on-failure
# Or invoke the test executable directly:
.\build\tests\Release\run_unit_tests.exe

# 4. Clean rebuild
cmake --build build --target clean
# Or delete the build tree and reconfigure:
rmdir /s /q build
cmake -B build -S .
cmake --build build --config Release --parallel
```

#### Generated Artifacts & Build Layout
| Artifact Path | Target Type | Description |
| :--- | :--- | :--- |
| `build\Release\schwab_executor.exe` | CLI Executable | Optimized production CLI (sub-millisecond latency, AVX2 enabled) |
| `build\Debug\schwab_executor.exe` | CLI Executable | Development CLI with debug symbols for debugging |
| `build\Release\schwab_core.lib` | Static Library | Execution engine core linked by external strategy modules |
| `build\tests\Release\run_unit_tests.exe` | Test Suite | Catch2 test suite (108 test cases, 1,665 assertions) |

---

### 3. Configure Credentials
Copy the environment template and populate your Schwab Developer portal credentials:
```powershell
Copy-Item .env.example .env
```

Configure `.env`:
```ini
SCHWAB_APP_KEY=YOUR_APP_KEY
SCHWAB_APP_SECRET=YOUR_APP_SECRET
SCHWAB_CALLBACK_URL=https://127.0.0.1
SCHWAB_ACCOUNTS=all
SCHWAB_DRY_RUN=true
```

### 4. Authenticate OAuth 2.0 Session
Launch the interactive authenticator:
```powershell
.\build\Release\schwab_executor.exe auth
```
*(Note: You can also use `.\build\Debug\schwab_executor.exe auth` during development).*
Follow the prompt, log in via your browser, and paste the redirect URL to save tokens atomically to `.schwab_token.json`.

### 5. Run Immediate Simulation
Execute a dry-run pass with full ASCII telemetry:
```powershell
.\build\Release\schwab_executor.exe execute --dry-run --accounts all
```

---

## CLI Reference

```powershell
# Display help and usage banner
schwab_executor --help

# Authenticate or refresh 7-day OAuth session (add --force to override cache and force re-login)
schwab_executor auth [--force]

# Inspect token TTLs, pre-baked auth header, and account balances
schwab_executor status

# Validate orders with live broker margin engines without execution
schwab_executor execute --preview --accounts all

# Evaluate strategy and execute orders immediately across all linked accounts
schwab_executor execute [--dry-run] [--preview] [--accounts <filter>] [--config <path>] [--telemetry-log <path>]
```

### Command Summary

| Command | Description | Typical Use Case |
| :--- | :--- | :--- |
| `auth` | Verifies active session or launches interactive browser 2FA login. | Initial setup and weekly 7-day maintenance. |
| `status` | Checks token TTLs, auth headers, and queries live portfolio equity/cash. | Morning pre-flight checks. |
| `execute` | Evaluates strategy against portfolio and dispatches orders immediately. | Scheduled cron jobs, Windows Task Scheduler, manual runs. |

### Common Options

| Option / Flag | Description |
| :--- | :--- |
| `--force` | Forces interactive browser OAuth 2.0 re-authentication regardless of local cache state. |
| `--dry-run` | Forces simulation mode without sending live broker orders. |
| `--preview` | Validates orders against live Schwab margin/order engine (`/previewOrder`) without submitting. |
| `--accounts <filter>` | Multi-account filter: `"all"`, comma-separated numbers/hashes, or last 4 digits (e.g. `1234,5678`). |
| `--account-hash <hash>` | Overrides the target Schwab encrypted account hash. |
| `--config <path>` | Path to `.env` configuration file (default: `.env`). |
| `--token-path <path>` | Path to OAuth token JSON cache (default: `.schwab_token.json`). |
| `--track-fills` | Polls broker post-dispatch to verify fills and calculate execution slippage. |
| `--telemetry-log <path>` | Appends execution reports to a persistent log file. |
| `--help`, `-h` | Displays usage and command reference. |

---

## Documentation Directory (`docs/`)

Detailed architectural designs, operational playbooks, and strategy developer guides are organized in the [`docs/`](docs/) directory:

- **[Quantitative Strategy Development Guide](docs/STRATEGY_DEVELOPMENT.md):**
  - Option A: Configuration-driven portfolio rebalancing (`SCHWAB_STRATEGY_TARGETS`) with zero C++ code.
  - Option B: Building in-repo custom strategies implementing `IStrategy`.
  - Option C: Integrating proprietary alpha from private external repositories via submodules.
  - Multi-model portfolio aggregation with `CompositeStrategy` and streaming technical indicators.
- **[System Architecture & Technical Design](docs/ARCHITECTURE.md):**
  - Low-latency engineering, zero-allocation hot paths, and 64-byte L1 cache-line layout guarantees.
  - Multi-gate pre-trade risk controls (`RiskGuard`).
  - Native WinHTTP connection pooling and socket pre-warming.
  - Full repository directory tree and component structure.
- **[Production Operational Guide & Runbook](docs/OPERATIONAL_GUIDE.md):**
  - Automated production scheduling with Windows Task Scheduler.
  - Morning operational pre-flight checklist.
  - OAuth 2.0 dual-token lifecycle and weekly maintenance playbooks.
  - Structured telemetry dictionary and incident response runbook.
