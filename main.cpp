#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define INITGUID
#include <windows.h>
#include <propsys.h>
#include <propkey.h>
#include <shobjidl.h>
#endif

#include <curl/curl.h>
#include <sqlite3.h>
#include <zip.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "gif.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Thread-safe console logger
std::mutex log_mtx;
void log_msg(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mtx);
    std::cout << msg << std::endl;
}

// ============================================================================
// Application Configuration
// ============================================================================
struct Config {
    int64_t start_id = 0;
    int64_t end_id = 0;
    fs::path output_dir;
    fs::path db_path = "pixiv_state.db";
    std::string session_id;
    int threads = 4;
    int delay_ms = 500;
};

// ============================================================================
// SQLite3 State Database (tracks 200 OK and 404 Not Found for resume support)
// ============================================================================
class StateDB {
public:
    explicit StateDB(const fs::path& db_path) {
        if (db_path.has_parent_path()) {
            fs::create_directories(db_path.parent_path());
        }
        if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("Failed to open SQLite DB: " + std::string(sqlite3_errmsg(db_)));
        }

        // Enable WAL mode for concurrent read/write performance across threads
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE IF NOT EXISTS processed_posts ("
             "illust_id INTEGER PRIMARY KEY, "
             "status INTEGER NOT NULL, "
             "updated_at INTEGER NOT NULL);");

        sqlite3_prepare_v2(db_, "SELECT 1 FROM processed_posts WHERE illust_id = ?;", -1, &stmt_check_, nullptr);
        sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO processed_posts (illust_id, status, updated_at) "
                                "VALUES (?, ?, strftime('%s','now'));", -1, &stmt_insert_, nullptr);
    }

    ~StateDB() {
        if (stmt_check_) sqlite3_finalize(stmt_check_);
        if (stmt_insert_) sqlite3_finalize(stmt_insert_);
        if (db_) sqlite3_close(db_);
    }

    bool is_processed(int64_t illust_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        sqlite3_reset(stmt_check_);
        sqlite3_bind_int64(stmt_check_, 1, illust_id);
        return sqlite3_step(stmt_check_) == SQLITE_ROW;
    }

    void mark_processed(int64_t illust_id, int status) {
        std::lock_guard<std::mutex> lock(mtx_);
        sqlite3_reset(stmt_insert_);
        sqlite3_bind_int64(stmt_insert_, 1, illust_id);
        sqlite3_bind_int(stmt_insert_, 2, status);
        sqlite3_step(stmt_insert_);
    }

private:
    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "Unknown SQLite error";
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_check_ = nullptr;
    sqlite3_stmt* stmt_insert_ = nullptr;
    std::mutex mtx_;
};

// ============================================================================
// Thread-Safe Round-Robin Proxy Pool
// ============================================================================
class ProxyPool {
public:
    ProxyPool(const std::string& single_proxy, const std::string& proxy_file) {
        if (!single_proxy.empty()) {
            proxies_.push_back(single_proxy);
        }
        if (!proxy_file.empty()) {
            std::ifstream in(proxy_file);
            std::string line;
            while (std::getline(in, line)) {
                auto first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos || line[first] == '#') continue;
                line = line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
                if (!line.empty()) proxies_.push_back(line);
            }
        }
    }

    std::string next() {
        if (proxies_.empty()) return "";
        return proxies_[index_.fetch_add(1, std::memory_order_relaxed) % proxies_.size()];
    }

    bool empty() const { return proxies_.empty(); }
    size_t size() const { return proxies_.size(); }

private:
    std::vector<std::string> proxies_;
    std::atomic<size_t> index_{0};
};

// ============================================================================
// Ugoira ZIP to GIF In-Memory Converter
// ============================================================================
struct UgoiraFrame {
    std::string filename;
    int delay_ms;
};

bool convert_ugoira_to_gif(const fs::path& zip_path, const fs::path& gif_path, const std::vector<UgoiraFrame>& frames) {
    int err = 0;
    zip_t* za = zip_open(zip_path.string().c_str(), ZIP_RDONLY, &err);
    if (!za) return false;

    GifWriter gif_writer{};
    bool gif_initialized = false;
    int width = 0, height = 0;
    fs::path temp_gif = gif_path;
    temp_gif += ".part";

    for (const auto& frame : frames) {
        zip_stat_t sb;
        if (zip_stat(za, frame.filename.c_str(), 0, &sb) != 0) continue;

        zip_file_t* zf = zip_fopen(za, frame.filename.c_str(), 0);
        if (!zf) continue;

        // Read compressed frame from ZIP into memory
        std::vector<uint8_t> buf(sb.size);
        zip_int64_t bytes_read = zip_fread(zf, buf.data(), sb.size);
        zip_fclose(zf);

        if (bytes_read != static_cast<zip_int64_t>(sb.size)) continue;

        // Decode image frame to RGBA
        int w = 0, h = 0, channels = 0;
        unsigned char* rgba = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h, &channels, 4);
        if (!rgba) continue;

        // GIF frame delay is measured in centiseconds (1/100th of a second)
        uint32_t delay_cs = std::max(1, (frame.delay_ms + 5) / 10);

        if (!gif_initialized) {
            width = w;
            height = h;
            if (!GifBegin(&gif_writer, temp_gif.string().c_str(), width, height, delay_cs)) {
                stbi_image_free(rgba);
                zip_close(za);
                return false;
            }
            gif_initialized = true;
        }

        if (w == width && h == height) {
            GifWriteFrame(&gif_writer, rgba, width, height, delay_cs);
        }
        stbi_image_free(rgba);
    }

    zip_close(za);

    if (gif_initialized) {
        GifEnd(&gif_writer);
        std::error_code ec;
        fs::rename(temp_gif, gif_path, ec);
        return !ec;
    }
    return false;
}

// ============================================================================
// Per-Thread HTTP Client (reuses persistent CURL connection pool)
// ============================================================================
class WorkerClient {
public:
    WorkerClient(const Config& cfg, ProxyPool& proxies) : cfg_(cfg), proxies_(proxies) {
        curl_ = curl_easy_init();
        // Pixiv requires a valid User-Agent and Referer header to serve image files
        headers_ = curl_slist_append(headers_, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36");
        headers_ = curl_slist_append(headers_, "Referer: https://www.pixiv.net/");
        headers_ = curl_slist_append(headers_, "Accept: application/json");
        current_proxy_ = proxies_.next();
    }

    ~WorkerClient() {
        if (headers_) curl_slist_free_all(headers_);
        if (curl_) curl_easy_cleanup(curl_);
    }

    void rotate_proxy() {
        if (!proxies_.empty()) current_proxy_ = proxies_.next();
    }

    long get_json(const std::string& url, std::string& response_body) {
        response_body.clear();
        setup_request(url);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_mem_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);

        if (curl_easy_perform(curl_) != CURLE_OK) {
            rotate_proxy();
            return -1;
        }
        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        return http_code;
    }

    bool download_file(const std::string& url, const fs::path& dest_path) {
        // Skip download if file already exists and is non-empty
        if (fs::exists(dest_path) && fs::file_size(dest_path) > 0) return true;

        fs::create_directories(dest_path.parent_path());
        fs::path temp_path = dest_path;
        temp_path += ".part";

        FILE* fp = fopen(temp_path.string().c_str(), "wb");
        if (!fp) return false;

        setup_request(url);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_file_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, fp);

        CURLcode res = curl_easy_perform(curl_);
        fclose(fp);

        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK || http_code != 200) {
            std::error_code ec;
            fs::remove(temp_path, ec);
            rotate_proxy();
            return false;
        }

        // Atomic rename from .part to final filename
        std::error_code ec;
        fs::rename(temp_path, dest_path, ec);
        return !ec;
    }

private:
    void setup_request(const std::string& url) {
        // curl_easy_reset resets options but preserves live connections, DNS cache, and SSL sessions
        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, ""); // Enable gzip/brotli decompression
        #ifdef CURLSSLOPT_NATIVE_CA
            curl_easy_setopt(curl_, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA); // Use Windows native certificate store
        #endif

        if (!cfg_.session_id.empty()) {
            std::string cookie = "PHPSESSID=" + cfg_.session_id;
            curl_easy_setopt(curl_, CURLOPT_COOKIE, cookie.c_str());
        }
        if (!current_proxy_.empty()) {
            curl_easy_setopt(curl_, CURLOPT_PROXY, current_proxy_.c_str());
        }
    }

    static size_t write_mem_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    }

    static size_t write_file_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        return fwrite(ptr, size, nmemb, static_cast<FILE*>(userdata));
    }

    const Config& cfg_;
    ProxyPool& proxies_;
    CURL* curl_ = nullptr;
    curl_slist* headers_ = nullptr;
    std::string current_proxy_;
};

// ============================================================================
// File Metadata / Property Helper
// ============================================================================
void set_file_properties(const fs::path& file_path, const std::vector<std::string>& tags, const std::string& title = "") {
#ifdef _WIN32
    if (tags.empty() && title.empty()) return;

    std::wstring wpath = file_path.wstring();
    IPropertyStore* pStore = nullptr;
    HRESULT hr = SHGetPropertyStoreFromParsingName(wpath.c_str(), NULL, GPS_READWRITE, IID_PPV_ARGS(&pStore));
    if (FAILED(hr) || !pStore) return;

    if (!tags.empty()) {
        std::vector<std::wstring> wtags;
        wtags.reserve(tags.size());
        for (const auto& t : tags) {
            if (!t.empty()) {
                int size = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), static_cast<int>(t.size()), NULL, 0);
                if (size > 0) {
                    std::wstring ws(size, 0);
                    MultiByteToWideChar(CP_UTF8, 0, t.c_str(), static_cast<int>(t.size()), &ws[0], size);
                    wtags.push_back(std::move(ws));
                }
            }
        }

        if (!wtags.empty()) {
            PROPVARIANT propvar;
            PropVariantInit(&propvar);
            propvar.vt = VT_VECTOR | VT_LPWSTR;
            propvar.calpwstr.cElems = static_cast<ULONG>(wtags.size());
            propvar.calpwstr.pElems = static_cast<LPWSTR*>(CoTaskMemAlloc(wtags.size() * sizeof(LPWSTR)));
            if (propvar.calpwstr.pElems) {
                for (size_t i = 0; i < wtags.size(); ++i) {
                    size_t len = wtags[i].size() + 1;
                    propvar.calpwstr.pElems[i] = static_cast<LPWSTR>(CoTaskMemAlloc(len * sizeof(wchar_t)));
                    if (propvar.calpwstr.pElems[i]) {
                        wcscpy(propvar.calpwstr.pElems[i], wtags[i].c_str());
                    }
                }
                pStore->SetValue(PKEY_Keywords, propvar);
                PropVariantClear(&propvar);
            }
        }
    }

    if (!title.empty()) {
        int size = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()), NULL, 0);
        if (size > 0) {
            std::wstring wtitle(size, 0);
            MultiByteToWideChar(CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()), &wtitle[0], size);
            PROPVARIANT pvTitle;
            PropVariantInit(&pvTitle);
            pvTitle.vt = VT_LPWSTR;
            pvTitle.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc((wtitle.size() + 1) * sizeof(wchar_t)));
            if (pvTitle.pwszVal) {
                wcscpy(pvTitle.pwszVal, wtitle.c_str());
                pStore->SetValue(PKEY_Title, pvTitle);
                PropVariantClear(&pvTitle);
            }
        }
    }

    pStore->Commit();
    pStore->Release();
#else
    (void)file_path;
    (void)tags;
    (void)title;
#endif
}

// ============================================================================
// Artwork Processing Logic
// ============================================================================
std::string extract_filename(const std::string& url) {
    auto pos = url.find_last_of('/');
    std::string name = (pos == std::string::npos) ? url : url.substr(pos + 1);
    auto qpos = name.find('?');
    return (qpos == std::string::npos) ? name : name.substr(0, qpos);
}

void process_illust(int64_t illust_id, WorkerClient& client, StateDB& db, const Config& cfg) {
    if (db.is_processed(illust_id)) return;

    std::string info_url = "https://www.pixiv.net/ajax/illust/" + std::to_string(illust_id);
    std::string body;
    long code = client.get_json(info_url, body);

    if (code == 404) {
        db.mark_processed(illust_id, 404);
        return;
    }

    if (code == 429) {
        log_msg("[!] 429 Rate Limit on ID " + std::to_string(illust_id) + " - rotating proxy and sleeping 15s...");
        client.rotate_proxy();
        std::this_thread::sleep_for(std::chrono::seconds(15));
        return; // Left unmarked in DB so it can be retried on a future run
    }

    if (code != 200) {
        log_msg("[!] HTTP " + std::to_string(code) + " on ID " + std::to_string(illust_id));
        return;
    }

    try {
        auto j = json::parse(body);
        if (j.value("error", true) || !j.contains("body") || j["body"].empty() || !j["body"].is_object()) {
            db.mark_processed(illust_id, 404);
            return;
        }

        const auto& b = j["body"];
        std::string title = b.value("title", b.value("illustTitle", ""));
        int illust_type = b.value("illustType", 0);
        int page_count = b.value("pageCount", 1);

        // Parse tags
        std::vector<std::string> tags;
        if (b.contains("tags") && b["tags"].is_object() && b["tags"].contains("tags") && b["tags"]["tags"].is_array()) {
            for (const auto& item : b["tags"]["tags"]) {
                if (item.contains("tag") && item["tag"].is_string()) {
                    std::string raw_tag = item["tag"].get<std::string>();
                    if (!raw_tag.empty()) {
                        tags.push_back(raw_tag);
                    }
                }
                if (item.contains("translation") && item["translation"].is_object()) {
                    if (item["translation"].contains("en") && item["translation"]["en"].is_string()) {
                        std::string en_tag = item["translation"]["en"].get<std::string>();
                        if (!en_tag.empty() && std::find(tags.begin(), tags.end(), en_tag) == tags.end()) {
                            tags.push_back(en_tag);
                        }
                    }
                }
            }
        }

        // Detect Ugoira (animated post: illustType == 2)
        if (illust_type == 2) {
            std::string ugoira_url = "https://www.pixiv.net/ajax/illust/" + std::to_string(illust_id) + "/ugoira_meta";
            std::string ugoira_body;
            if (client.get_json(ugoira_url, ugoira_body) != 200) return;

            auto uj = json::parse(ugoira_body);
            if (uj.value("error", true) || !uj.contains("body")) return;

            std::string zip_url = uj["body"]["originalSrc"].get<std::string>();
            std::vector<UgoiraFrame> frames;
            for (const auto& f : uj["body"]["frames"]) {
                frames.push_back({f["file"].get<std::string>(), f["delay"].get<int>()});
            }

            // Ugoira produces 2 files (zip + gif), so save in a directory named after post ID
            fs::path post_dir = cfg.output_dir / std::to_string(illust_id);
            fs::path zip_path = post_dir / (std::to_string(illust_id) + "_ugoira.zip");
            fs::path gif_path = post_dir / (std::to_string(illust_id) + ".gif");

            if (!client.download_file(zip_url, zip_path)) {
                log_msg("[!] Failed to download Ugoira ZIP for ID " + std::to_string(illust_id));
                return;
            }

            if (!fs::exists(gif_path) && !convert_ugoira_to_gif(zip_path, gif_path, frames)) {
                log_msg("[!] Failed to build GIF for ID " + std::to_string(illust_id));
                return;
            }

            set_file_properties(gif_path, tags, title);

            db.mark_processed(illust_id, 200);
            log_msg("[+] [Ugoira] ID " + std::to_string(illust_id) + " saved (ZIP + GIF in " + std::to_string(illust_id) + "/)");
            return;
        }

        // Standard single/multi-page illustration or manga
        std::vector<std::string> img_urls;
        if (page_count == 1 && b.contains("urls") && b["urls"].is_object() &&
            b["urls"].contains("original") && b["urls"]["original"].is_string() &&
            !b["urls"]["original"].get<std::string>().empty()) {
            img_urls.push_back(b["urls"]["original"].get<std::string>());
        } else {
            std::string pages_url = "https://www.pixiv.net/ajax/illust/" + std::to_string(illust_id) + "/pages";
            std::string pages_body;
            long pages_code = client.get_json(pages_url, pages_body);
            if (pages_code == 429) {
                log_msg("[!] 429 Rate Limit on ID " + std::to_string(illust_id) + " - rotating proxy and sleeping 15s...");
                client.rotate_proxy();
                std::this_thread::sleep_for(std::chrono::seconds(15));
                return;
            }
            if (pages_code == 200) {
                auto pj = json::parse(pages_body);
                if (!pj.value("error", true) && pj.contains("body") && pj["body"].is_array()) {
                    for (const auto& page : pj["body"]) {
                        if (page.contains("urls") && page["urls"].contains("original")) {
                            img_urls.push_back(page["urls"]["original"].get<std::string>());
                        }
                    }
                }
            }
        }

        if (img_urls.empty()) {
            int x_restrict = b.value("xRestrict", 0);
            int restrict_val = b.value("restrict", 0);
            if ((x_restrict > 0 || restrict_val > 0) && cfg.session_id.empty()) {
                log_msg("[!] ID " + std::to_string(illust_id) + " is R-18 / restricted. Set session = PHPSESSID in config.cfg to download.");
                return; // Leave unmarked so user can retry with session cookie
            }
            db.mark_processed(illust_id, 404);
            return;
        }

        if (img_urls.size() == 1) {
            // Exactly 1 file: do NOT create subfolder. Name file <illust_id>.<ext>
            const std::string& url = img_urls[0];
            std::string filename = extract_filename(url);
            std::string ext = fs::path(filename).extension().string();
            if (ext.empty()) ext = ".jpg";

            fs::path dest_file = cfg.output_dir / (std::to_string(illust_id) + ext);
            if (client.download_file(url, dest_file)) {
                set_file_properties(dest_file, tags, title);
                db.mark_processed(illust_id, 200);
                log_msg("[+] ID " + std::to_string(illust_id) + " downloaded (" + dest_file.filename().string() + ")");
            } else {
                log_msg("[!] Failed to download file for ID " + std::to_string(illust_id));
            }
        } else {
            // Multiple files: create folder named with illust_id
            fs::path post_dir = cfg.output_dir / std::to_string(illust_id);
            bool all_ok = true;
            for (const auto& url : img_urls) {
                fs::path dest_file = post_dir / extract_filename(url);
                if (!client.download_file(url, dest_file)) {
                    all_ok = false;
                    break;
                }
                set_file_properties(dest_file, tags, title);
            }

            if (all_ok) {
                db.mark_processed(illust_id, 200);
                log_msg("[+] ID " + std::to_string(illust_id) + " downloaded (" + std::to_string(img_urls.size()) + " files in " + std::to_string(illust_id) + "/)");
            } else {
                log_msg("[!] Failed to download all pages for ID " + std::to_string(illust_id));
            }
        }
    } catch (const std::exception& e) {
        log_msg("[!] Parse error on ID " + std::to_string(illust_id) + ": " + e.what());
    }
}

// ============================================================================
// Entry Point
// ============================================================================
int main(int argc, char** argv) {
    Config cfg;
    std::string single_proxy;
    std::string proxy_file;

    CLI::App app{"Multi-threaded Pixiv artwork scraper (supports illustrations, manga, and Ugoira ZIP+GIF)"};

    // Automatically loads config.cfg if present, or a custom file via -c / --config
    app.set_config("-c,--config", "config.cfg", "Path to configuration file (INI/TOML)", false);

    app.add_option("--start,start", cfg.start_id, "Start artwork ID (inclusive)")
        ->required()->check(CLI::PositiveNumber);
    app.add_option("--end,end", cfg.end_id, "End artwork ID (inclusive)")
        ->required()->check(CLI::PositiveNumber);
    app.add_option("-o,--output", cfg.output_dir, "Output directory path")->required();
    app.add_option("-s,--session", cfg.session_id, "Pixiv PHPSESSID cookie (for R-18 access and higher rate limits)");
    app.add_option("-t,--threads", cfg.threads, "Number of worker threads")->default_val(4);
    app.add_option("-d,--delay", cfg.delay_ms, "Delay between requests per thread in ms")->default_val(500);
    app.add_option("-p,--proxy", single_proxy, "Single proxy URL (e.g. socks5h://127.0.0.1:1080)");
    app.add_option("--proxy-file", proxy_file, "Path to text file with proxy list (one per line)");
    app.add_option("--db", cfg.db_path, "Path to SQLite state database")->default_val("pixiv_state.db");

    CLI11_PARSE(app, argc, argv);

    if (cfg.end_id < cfg.start_id) {
        std::cerr << "Error: end ID cannot be less than start ID.\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    try {
        StateDB db(cfg.db_path);
        ProxyPool proxies(single_proxy, proxy_file);

        std::atomic<int64_t> next_id{cfg.start_id};
        std::vector<std::thread> workers;

        std::cout << "[*] Range:   " << cfg.start_id << ".." << cfg.end_id << "\n"
                  << "[*] Output:  " << cfg.output_dir.string() << "\n"
                  << "[*] Threads: " << cfg.threads << " (delay " << cfg.delay_ms << " ms)\n"
                  << "[*] Session: " << (cfg.session_id.empty() ? "none (public works only)" : "authenticated (PHPSESSID)") << "\n"
                  << "[*] Proxies: " << (proxies.empty() ? "none (direct connection)" : std::to_string(proxies.size()) + " loaded") << "\n\n";

        for (int i = 0; i < cfg.threads; ++i) {
            workers.emplace_back([&]() {
#ifdef _WIN32
                CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
                WorkerClient client(cfg, proxies);
                while (true) {
                    int64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
                    if (id > cfg.end_id) break;

                    process_illust(id, client, db, cfg);

                    if (cfg.delay_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.delay_ms));
                    }
                }
#ifdef _WIN32
                CoUninitialize();
#endif
            });
        }

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        std::cout << "\n[Done] Range processing complete.\n";
    } catch (const std::exception& e) {
        std::cerr << "[Fatal Error] " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}
