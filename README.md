# Pixiv Range Scraper

A high-performance, multi-threaded C++17 CLI tool for downloading artworks from [Pixiv](https://www.pixiv.net/) by artwork ID range. It downloads original-resolution images (`img-original`), multi-page manga/galleries, and animated **Ugoira** works (saved as both the original frame `.zip` archive and an in-memory compiled animated `.gif`).

It features smart directory structuring, native Windows file property tag embedding, atomic writes, SQLite-based state tracking with resume support, and round-robin proxy rotation.

---

## Features

- **Original Quality:** Fetches direct `img-original` URLs via Pixiv's internal REST endpoints without requiring headless browsers.
- **Smart Directory Organization:**
  - **Single-file posts:** Downloaded directly into the target output directory as `<illust_id>.<ext>` without creating unnecessary subdirectories.
  - **Multi-file posts (manga / illustration sets):** Automatically organized into a dedicated subfolder named after the artwork ID (`<output_dir>/<illust_id>/`).
- **Native File Metadata & Tag Embedding:**
  - Automatically parses artwork tags (both Japanese tags and English translations) and artwork titles directly from Pixiv metadata.
  - On Windows, tags are embedded natively into the file's system properties (`PKEY_Keywords` / "Tags" and `PKEY_Title` / "Title") via the Windows Shell Property Store without altering image quality.
  - Tags are immediately visible in Windows File Explorer (under the *Tags* column and in *Properties -> Details*) and are indexed for instant search queries (e.g. `tag:original`).
- **Full Ugoira Support:** Automatically detects animated works, downloads the original frame `.zip` archive, and compiles an animated `.gif` in memory with exact per-frame millisecond delays.
- **Multi-threaded Worker Pool:** Uses persistent per-thread `libcurl` handles that reuse TLS sessions, DNS caches, and keep-alive HTTP connections.
- **Resume & State Tracking (SQLite3 WAL):** Tracks processed artwork IDs (`200 OK` and `404 Not Found`) in a local SQLite database in Write-Ahead Logging (WAL) mode. Interrupted or overlapping runs skip already-processed posts instantaneously.
- **Atomic File Writing:** Downloads directly to temporary `.part` files and atomically renames them upon completion to prevent corrupted files from incomplete downloads.
- **Rate Limit & 429 Handling:** Automatically detects HTTP 429 (Too Many Requests), rotates proxy, sleeps for 15 seconds, and leaves the post uncommitted in the database for a future retry.
- **Proxy Rotation:** Supports single proxy URLs or round-robin rotation from a proxy list file (`http://`, `https://`, `socks5://`, `socks5h://`).
- **Flexible Configuration:** Automatically loads settings from `config.cfg` by default, while allowing CLI arguments to override any parameter.
- **Self-Contained Builds:** Generates completely standalone binaries on Windows with zero external DLL dependencies (statically linked C runtime, SQLite3, libzip, and native Windows Schannel TLS).

---

## Pre-built Binaries

Ready-to-use binaries are available in the `dist/` directory:

| Platform | Path | Description |
|---|---|---|
| **Windows (x64)** | `dist/windows-x64/pixiv_scrapper.exe` | Standalone portable `.exe` (~4.0 MB, statically linked, no external DLLs needed) |
| **Linux (x64)** | `dist/linux-x64/pixiv_scrapper` | ELF 64-bit executable (~600 KB) |

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

> **Zero-Config Fallback:** If any of these libraries are not found in your system or package manager, CMake's `FetchContent` **automatically downloads and builds them statically from source**. You do not need to manually install dependencies on Windows!

---

## Building from Source

### Windows

#### Option A: Standalone MinGW / GCC (Recommended)
This produces a 100% portable, statically linked `.exe` using Windows Schannel (native TLS) and Windows Property Store (`propsys`):

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Optional: strip debug symbols to reduce binary size (~4 MB)
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

# Concurrency and Rate Limiting
threads = 4
delay = 500

# Proxy Configuration (optional)
# proxy = socks5h://127.0.0.1:1080
# proxy-file = ./proxies.txt
```

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
./build/pixiv_scrapper 232400 232500 -o ./downloads -t 8 -d 300 -s "YOUR_PHPSESSID" --proxy-file proxies.txt
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
  -t,--threads INT [4]        Number of worker threads
  -d,--delay INT [500]        Delay between requests per thread in ms
  -p,--proxy TEXT             Single proxy URL (e.g. socks5h://127.0.0.1:1080)
  --proxy-file TEXT           Path to text file with proxy list (one per line)
  --db TEXT [pixiv_state.db]  Path to SQLite state database
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

- When restarting or scanning overlapping ranges, completed artworks and deleted/non-existent IDs (`404`) are skipped instantly without network requests.
- Rate-limited artworks (`429`) and restricted R-18 works without a session cookie are intentionally not recorded so they can be retried on subsequent runs.

---

## License

This project is licensed under the [MIT License](LICENSE).
