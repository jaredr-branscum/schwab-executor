# Schwab Low-Latency C++ Execution Engine (`schwab-executor`)

High-performance, low-latency execution engine engineered in C++20 for Charles Schwab API trading accounts.

---

## Project Structure
```text
schwab-executor/
├── CMakeLists.txt          # Modern C++20 build definition (MSVC / Clang / GCC)
├── run_tests.bat           # 1-click test runner (Windows)
├── .env.example            # Environment configuration template
├── .env                    # Local configuration (secrets, ignored by git)
├── .gitignore              # Ignores build artifacts, tokens, and logs
├── include/                # Public C++ header files (.hpp)
│   └── schwab/
├── src/                    # C++ implementation files (.cpp)
├── tests/                  # Catch2 unit test suites
│   ├── CMakeLists.txt
│   └── test_sanity.cpp
└── python/                 # Self-contained Python helpers
    └── auth_cli.py         # Standalone OAuth 2.0 authentication manager
```

---

## 1. Setup & Configuration

### Prerequisites
- **OS:** Windows 10/11
- **C++ Compiler:** Visual Studio Build Tools / MSVC 2022 with C++20 support
- **Build Tool:** CMake 3.20+
- **Python:** Python 3.10+ (standard library only; zero pip dependencies)

### Configuration (`.env`)
1. Copy the example configuration to `.env`:
   ```powershell
   Copy-Item .env.example .env
   ```
2. Populate your Charles Schwab Developer Portal credentials:
   ```ini
   # App Key (Client ID) and App Secret from developer.schwab.com
   SCHWAB_APP_KEY=YOUR_SCHWAB_APP_KEY_HERE
   SCHWAB_APP_SECRET=YOUR_SCHWAB_APP_SECRET_HERE

   # Registered OAuth Callback URL (must match the developer portal exactly)
   SCHWAB_CALLBACK_URL=https://127.0.0.1

   # Local token cache file path (generated automatically upon login)
   SCHWAB_TOKEN_PATH=.schwab_token.json

   # Schwab API Base URL
   SCHWAB_BASE_URL=https://api.schwabapi.com
   ```

---

## 2. Charles Schwab OAuth 2.0 Authentication Helper (`auth_cli.py`)

Charles Schwab requires a browser-based OAuth 2.0 authorization code flow with Two-Factor Authentication (2FA) once every 7 days.

The standalone script [`python/auth_cli.py`](python/auth_cli.py) manages this lifecycle.

### Available Commands

| Command | Purpose | Expected Output / Exit Code |
| :--- | :--- | :--- |
| `python python/auth_cli.py --login` | Interactive browser login & token generation | Saves `.schwab_token.json` (exit `0`) |
| `python python/auth_cli.py --check` | Non-interactive validity check | `AUTHENTICATED` (exit `0`) or `UNAUTHENTICATED` (exit `1`) |
| `python python/auth_cli.py --refresh` | Renew 30-min access token via refresh token | `REFRESH_SUCCESS` (exit `0`) |
| `python python/auth_cli.py --get-access-token` | Print raw Bearer access token string | `<access_token>` string to stdout |

---

### Step-by-Step Interactive Login Walkthrough

Run the interactive login command:
```powershell
python python/auth_cli.py --login
```

1. **URL Display:** The script outputs a Schwab authorization URL:
   ```text
   ================================================================================
     [CHARLES SCHWAB OAUTH 2.0 AUTHENTICATION - SCHWAB-EXECUTOR]
   ================================================================================
     * Initial authorization required.

     Step 1: Open this URL in your browser:
     https://api.schwabapi.com/v1/oauth/authorize?client_id=...&redirect_uri=https://127.0.0.1
   ```
2. **Authenticate:** Open the link in your web browser. Log in with your Schwab credentials, complete 2FA, and click **Agree**.
3. **Copy Redirect URL:** The browser will navigate to your registered callback URL (e.g. `https://127.0.0.1/?code=C0.xxxx@...`).
   *(Note: The browser will typically show "This site can't be reached" or a blank page — this is completely normal!)*
4. **Paste Code:** Copy the full URL from the browser's address bar and paste it into the terminal prompt:
   ```text
     Paste Redirected URL or Code: https://127.0.0.1/?code=C0.xxxx@...
   ```
5. **Success:** The script exchanges the code for OAuth tokens and creates `.schwab_token.json`:
   ```text
     * SUCCESS: OAuth 2.0 token received and saved to .schwab_token.json
   ```

### Token Lifecycle & Expiration Rules
- **Access Token:** Valid for **30 minutes**. Used in HTTP `Authorization: Bearer <token>` headers.
- **Refresh Token:** Valid for **7 days**. Used to automatically generate fresh access tokens without opening a browser.
- **Auto-Refresh:** The script and C++ engine automatically refresh the access token if fewer than 60 seconds remain before expiration.
- **Re-Login:** Required once every 7 days after the refresh token expires.

---

## 3. Running C++ Unit Tests

From the project root directory, run:
```cmd
run_tests.bat
```

This automated runner performs:
1. **CMake Configuration:** Detects MSVC compiler and configures parallel compilation (`/MP`).
2. **Parallel Build:** Compiles the C++20 targets in `Debug` mode.
3. **Catch2 Test Suite:** Executes all unit tests with full console reporting.
