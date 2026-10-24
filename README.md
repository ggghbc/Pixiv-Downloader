# Pixiv Downloader

A high-performance, multi-threaded C++17 CLI tool for downloading artworks from [Pixiv](https://www.pixiv.net/) by artwork ID range. It downloads original-resolution images, multi-page manga/galleries, and animated **Ugoira** works (saved as both the original frame `.zip` archive and an in-memory compiled animated `.gif`).

It features smart directory structuring, native Windows file property tag embedding, atomic writes, SQLite-based state tracking with resume support, and round-robin proxy rotation.

---

## Features

- **Original Quality:** Fetches direct `img-original` URLs via Pixiv's internal REST endpoints without requiring headless browsers.
- **Smart Directory Organization:**
  - **Single-file posts:** Downloaded directly into the target output directory as `<illust_id>.<ext>` without creating unnecessary subdirectories.
  - **Multi-file posts (manga / illustration sets):** Automatically organized into a dedicated subfolder named after the artwork ID (`<output_dir>/<illust_id>/`).
- **Native File Metadata & Tag Embedding:**
  - Automatically parses artwork tags (both Japanese tags and English translations) and artwork titles directly from Pixiv metadata.
  - Tags are immediately visible in Windows File Explorer (under the *Tags* column and in *Properties -> Details*) and are indexed for instant search queries (e.g. `tag:original`).
- **AI-Generated Work Filter:** Built-in filter detecting posts marked by creators as **AI-generated work** (Pixiv's official `aiType == 2` attribute, along with AI tags). Configure `ai = false` to automatically skip AI posts.
- **Full Ugoira Support:** Automatically detects animated works, downloads the original frame `.zip` archive, and compiles an animated `.gif` in memory with exact per-frame millisecond delays.
- **Multi-threaded Worker Pool:** Uses persistent per-thread `libcurl` handles that reuse TLS sessions, DNS caches, and keep-alive HTTP connections.
- **Resume & State Tracking (SQLite3 WAL):** Tracks processed artwork IDs (`200 OK` and `404 Not Found`) in a local SQLite database in Write-Ahead Logging (WAL) mode. Interrupted or overlapping runs skip already-processed posts instantaneously.
- **Atomic File Writing:** Downloads directly to temporary `.part` files and atomically renames them upon completion to prevent corrupted files from incomplete downloads.
- **Intelligent 429 Rate Limit Mitigation:**
  - **Cross-Thread Global Cooldown:** When a rate limit is detected on a single IP, all active threads pause synchronously (exponential backoff: 10s -> 20s -> 35s), eliminating concurrent 429 cascade storms.
  - **Automatic In-Flight Retries:** Automatically retries posts up to 3 times with exponential backoff or instant proxy rotation instead of prematurely abandoning downloads.
  - **Anti-Burst Jitter:** Introduces randomized delay variance (±25%) so worker threads do not fire in lockstep bursts.
  - **Modern Browser Fingerprinting:** Emulates standard desktop Chrome headers (`sec-ch-ua`, `Sec-Fetch-*`, `Accept-Language`, etc.) to minimize WAF/Cloudflare bot flags.
- **Proxy Rotation & `noproxy` Direct Mode:** Supports single proxy URLs (`-p` / `proxy`) or round-robin rotation from a proxy list file (`--proxy-file`). You can include `noproxy` or `direct` entries in `proxies.txt` to seamlessly mix direct connections with proxy pools.
- **Flexible Configuration:** Automatically loads settings from `config.cfg` by default, while allowing CLI arguments to override any parameter.
- **Self-Contained Builds:** Generates completely standalone binaries on Windows with zero external DLL dependencies (statically linked C runtime, SQLite3, libzip, and native Windows Schannel TLS).

---

## Output Structure

Artwork files are structured based on the number of images in each post:

```text
downloads/
├── 114999987.jpg              # Single-image post (saved directly into downloads, named <id>.<ext>)
├── 114999988/                 # Multi-page artwork (>1 images, saved in folder <id>/)
│   ├── 114999988_p0.jpg
│   └── 114999988_p1.jpg
└── 44298467/                  # Ugoira animation (>1 files: ZIP + rendered GIF)
    ├── 44298467_ugoira.zip    # Original frame archive
    └── 44298467.gif           # In-memory rendered animated GIF (with embedded tags)
```

---

## File Metadata & Tags (Windows Explorer)

When downloading on Windows, the scraper automatically writes artwork tags and title into the file's Shell Properties:

- **Tags (`PKEY_Keywords`):** Contains original artwork tags (e.g. `オリジナル`, `女の子`) as well as English translations when available (e.g. `girl`).
- **Title (`PKEY_Title`):** The title of the post on Pixiv.

### How to View in Windows:
1. Open Windows File Explorer and navigate to your download folder.
2. Right-click on any downloaded image -> **Properties** -> **Details**.
3. View the **Tags** and **Title** fields.
4. You can also right-click the File Explorer column headers and enable the **Tags** column to sort or filter images by tags.
5. Search directly in Windows Explorer using `tag:your_tag` or `title:your_title`.

---

## Requirements

- **C++ Compiler:** `GCC` (>= 9), `Clang` (>= 10), or `MSVC` (Visual Studio 2019/2022/2026) with C++17 support
- **Build System:** `CMake` (>= 3.20) and optionally `Ninja` or `Make`

### Dependencies

The project depends on:
- `libcurl`
- `sqlite3`
- `libzip` and `zlib`
- `CLI11` (CLI argument parser)
- `nlohmann/json` (JSON parser)
- `stb_image.h` & `gif.h` (Image decoding and GIF generation)

---

## Building from Source

### Windows

#### Option A: Standalone MinGW / GCC (Recommended)
This produces a 100% portable, statically linked `.exe` using Windows Schannel (native TLS) and Windows Property Store (`propsys`):

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Optional: strip debug symbols to reduce binary size
strip -s build/pixiv_scrapper.exe
```

#### Option B: Microsoft Visual Studio (MSVC)
Using Visual Studio generator or Ninja:

```cmd
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

*(Optional: If using `vcpkg`, pass `-DCMAKE_TOOLCHAIN_FILE=[vcpkg_path]/scripts/buildsystems/vcpkg.cmake` to automatically consume libraries from `vcpkg.json`).*

---

### Linux (Ubuntu / Debian / Mint)

You can install development packages from your package manager for instantaneous compilation:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libcurl4-openssl-dev libsqlite3-dev libzip-dev zipcmp libcli11-dev nlohmann-json3-dev

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

*(If you prefer not to install system libraries, simply run `cmake -B build` and CMake will automatically fetch and compile all dependencies).*

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf curl sqlite libzip git
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

### macOS (Intel & Apple Silicon M1/M2/M3/M4)

Using [Homebrew](https://brew.sh/):

```bash
brew install cmake ninja curl sqlite libzip cli11 nlohmann-json

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build --config Release
```

---

## Configuration (`config.cfg`)

By default, `pixiv_scrapper` reads configuration options from `config.cfg` in the working directory:

```ini
# Target ID range (inclusive)
start = 232400
end = 233400

# File Paths
output = ./downloads
db = ./pixiv_state.db

# Pixiv PHPSESSID cookie (optional, required for R-18 works and higher rate limits)
session = YOUR_PHPSESSID_HERE

# AI-generated artwork filter (true = download AI works, false = skip AI works)
ai = true

# Concurrency and Rate Limiting
# Recommended for direct connection (no proxy): threads = 3, delay = 600
# Recommended with multi-proxy list: threads = 8..16, delay = 200..300
threads = 3
delay = 600

# Proxy Configuration (optional)
# Supports http://, https://, socks5://, socks5h://
# Set 'noproxy' or 'direct' to bypass proxies for that slot
# proxy = socks5h://127.0.0.1:1080
# proxy-file = ./proxies.txt
```

### Proxy Configuration & `noproxy` Support

The scraper supports single proxy routing or rotating through a list of proxies:

- **Single Proxy:** Specify in `config.cfg` (`proxy = ...`) or via CLI (`-p socks5h://127.0.0.1:1080`).
- **Proxy List:** Provide a text file with one proxy per line via `proxy-file = proxies.txt` or `--proxy-file proxies.txt`.
- **`noproxy` / Direct Mode in Proxy Lists:** You can specify `noproxy`, `direct`, or `none` as an entry in `proxies.txt` to alternate between proxies and direct connections:

```text
# proxies.txt example
http://proxy1.example.com:8080
noproxy
socks5://proxy2.example.com:1080
direct
```

In this mode, the worker pool alternates requests round-robin between `proxy1`, your direct local connection, `proxy2`, and your direct local connection.

---

## Mitigating HTTP 429 (Too Many Requests)

Pixiv limits single-IP requests to approximately **2–4 requests per second**. If you exceed this rate, Pixiv responds with `HTTP 429 Too Many Requests`.

Pixiv Downloader implements four layers of rate-limit protection:

1. **Synchronized Global Cooldown:** If a 429 occurs on a direct connection or single proxy, all worker threads pause synchronously (exponential backoff: 10s -> 20s -> 35s), preventing simultaneous thread collisions.
2. **In-Flight Retries:** Instead of discarding the artwork on 429, the worker pauses (or rotates proxy) and retries up to 3 times automatically.
3. **Randomized Delay Jitter:** Adds ±25% random variance to `delay_ms` per thread to avoid lockstep request bursts.
4. **Modern Browser Fingerprint:** Sends realistic desktop Chrome headers and Client Hints (`sec-ch-ua`, `Sec-Fetch-*`, `Accept-Language`).

### Recommended Settings:
- **Direct Connection (Default):** `threads = 3`, `delay = 600` (safe ~2-3 req/s without triggering 429).
- **With a Rotating Proxy Pool:** `threads = 8..16`, `delay = 200..300` (load is distributed across distinct IPs).

---

### How to Obtain `PHPSESSID`

1. Log in to [pixiv.net](https://www.pixiv.net/) in your web browser.  
   *(If you wish to download R-18/R-18G works, make sure to enable them first in [Viewing Restrictions](https://www.pixiv.net/settings/viewing)).*
2. Open Developer Tools (`F12` or `Ctrl+Shift+I` / `Cmd+Option+I`).
3. Navigate to **Application** (Chrome/Edge) or **Storage** (Firefox) -> **Cookies** -> `https://www.pixiv.net`.
4. Locate the `PHPSESSID` cookie, copy its value, and paste it into `session = ...` in `config.cfg` or pass via `-s`.

---

## Usage

### 1. Run using `config.cfg`

If `start`, `end`, and `output` are defined in `config.cfg`:

```bash
# Windows
.\build\pixiv_scrapper.exe

# Linux / macOS
./build/pixiv_scrapper
```

### 2. Override ID range via CLI

Keep default settings (output directory, session, threads) in `config.cfg`, but pass a custom range:

```bash
./build/pixiv_scrapper 1000000 1005000
```

### 3. Full CLI usage (without `config.cfg`)

```bash
./build/pixiv_scrapper 232400 232500 -o ./downloads -t 3 -d 600 -s "YOUR_PHPSESSID" --proxy-file proxies.txt --ai=false
```

---

## CLI Reference

```text
Multi-threaded Pixiv artwork scraper (supports illustrations, manga, and Ugoira ZIP+GIF)
Usage: pixiv_scrapper [OPTIONS] start end

Positionals:
  start INT:POSITIVE REQUIRED Start artwork ID (inclusive)
  end INT:POSITIVE REQUIRED   End artwork ID (inclusive)

Options:
  -h,--help                   Print this help message and exit
  -c,--config [config.cfg]    Path to configuration file (INI/TOML)
  --start INT:POSITIVE        Start artwork ID (inclusive)
  --end INT:POSITIVE          End artwork ID (inclusive)
  -o,--output TEXT REQUIRED   Output directory path
  -s,--session TEXT           Pixiv PHPSESSID cookie (for R-18 access and higher rate limits)
  -t,--threads INT [3]        Number of worker threads
  -d,--delay INT [600]        Delay between requests per thread in ms
  -p,--proxy TEXT             Single proxy URL (e.g. socks5h://127.0.0.1:1080 or noproxy)
  --proxy-file TEXT           Path to text file with proxy list (supports noproxy)
  --db TEXT [pixiv_state.db]  Path to SQLite state database
  --ai BOOL [true]            Allow downloading AI-generated works (true/false)
```

---

## State Database (`pixiv_state.db`)

Progress is tracked in an SQLite3 database with WAL mode:

```sql
CREATE TABLE processed_posts (
    illust_id INTEGER PRIMARY KEY,
    status INTEGER NOT NULL,    -- 200 (Downloaded) or 404 (Not Found / Deleted)
    updated_at INTEGER NOT NULL
);
```

- When restarting or scanning overlapping ranges, completed artworks, skipped AI works, and deleted/non-existent IDs (`404`) are skipped instantly without network requests.
- Rate-limited artworks (`429`) and restricted R-18 works without a session cookie are intentionally not recorded so they can be retried on subsequent runs.

---

## License

This project is licensed under the [MIT License](LICENSE).
