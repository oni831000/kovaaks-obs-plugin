#include <obs-module.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

#ifndef OBS_VERSION
#define OBS_VERSION(major, minor, patch) ((major << 24) | (minor << 16) | patch)
#endif
#undef LIBOBS_API_VER
#define LIBOBS_API_VER OBS_VERSION(30, 0, 0)

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <filesystem>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("kovaaks_plugin", "en-US")

// Kill stats structures
struct KillStatBot {
    std::string name; // display name (may have duplicates)
    std::string key;  // unique position key: "bot_0", "bot_1"...
    int   shots      = 0;
    int   hits       = 0;
    float accuracy   = 0.0f;
    float ttk        = 0.0f;
    float efficiency = 0.0f;
};

struct kovaaks_context {
    std::string kovaaks_save_folder, kovaaks_stats_folder, export_folder, ui_theme, custom_css;
    std::string anim_type = "slide-up";
    std::string alignment = "bottom-left";
    std::string title_alignment = "center";
    std::string label_layout = "horizontal";
    std::string content_align = "left";
    int bg_opacity = 100;
    int rotation_delay = 6;
    bool auto_rotate = false;
    bool fixed_size = false;
    bool show_titles = true; 
    bool show_row_labels = false;
    
    std::string sec_names[5]; 

    struct CustomInfoEntry {
        std::string label;
        std::string value;
        bool visible = false;
        int sec_assignment = 1;
        std::string pos = "inline";
    };
    static constexpr int CUSTOM_INFO_COUNT = 5;
    CustomInfoEntry custom_info[CUSTOM_INFO_COUNT];
    
    std::map<std::string, std::string> pos;
    std::map<std::string, bool> visible;
    std::map<std::string, int> sec_assignment;
    std::map<std::string, std::string> user_themes;

    float time_elapsed = 0.0f;     
    float rotation_timer = 0.0f;   

    // Section display logic is handled in C++, not JS
    bool manual_mode = false;      
    int  current_section_idx = 0;  
    bool section_dirty = false;    

    obs_hotkey_id hotkey_next = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_prev = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_show_killstats = OBS_INVALID_HOTKEY_ID;

    // JSON/INI cache: re-read only when the settings watcher detects a file change
    json cached_json = json::object();
    std::map<std::string, std::string> cached_ini;
    bool cache_valid = false;
    bool kovaaks_running_last = false; // for logging start/stop transitions only

    // KillStats widget settings
    std::string killstats_theme       = "dark";
    int         killstats_opacity     = 90;
    int         killstats_display_sec = 10;
    bool        ks_show_accuracy      = true;
    bool        ks_show_ttk           = true;
    bool        ks_show_shots_hits    = true;
    bool        ks_show_efficiency    = false;
    // Avg comparison (last 10 runs)
    bool        ks_show_avgs          = true;

    // KillStats runtime state
    std::string last_csv_processed;
    float       killstats_timer = -1.0f; // countdown, -1 = hidden
    std::vector<KillStatBot> last_bots;  // Last run cached for manual recall
    bool        showing_ks_preview = false; // Tracks whether fake preview data is being shown
};

// Kill stats averages and gauntlet detection

// Used for the "vs your average over the last 10 runs" comparison in the kill stats widget
struct BotAvg {
    float accuracy   = 0.0f;
    float ttk        = 0.0f;
    float efficiency = 0.0f;
    int   shots      = 0;
    int   hits       = 0;
    int   count      = 0;
};
static std::map<std::string, BotAvg> ComputeAvgs(const std::string& folder, const std::string& exclude_path, int maxRuns);

// A real gauntlet run has at least one bot killed more than once (multiple kills on the same name).
// Target-switching scenarios have exactly 1 kill per bot, so they're excluded.
static bool IsGauntletRun(const std::vector<KillStatBot>& bots) {
    if (bots.size() <= 1) return false;
    std::map<std::string, int> counts;
    for (auto& b : bots) counts[b.name]++;
    for (auto& kv : counts) if (kv.second > 8) return false;
    return true;
}

void kovaaks_update(void* data, obs_data_t* settings);
void ExportToJson(kovaaks_context* ctx, bool force_reload);

// Guards g_watcher_ctx (set by the watcher threads further below) and the runtime
// fields they share with the OBS UI thread (last_bots, last_csv_processed,
// showing_ks_preview, killstats_timer, cache_valid).
static kovaaks_context* g_watcher_ctx = nullptr;
static std::mutex       g_watcher_ctx_mutex;

static void hotkey_next_section(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    auto* ctx = static_cast<kovaaks_context*>(data);
    ctx->manual_mode = true;
    ctx->current_section_idx++;
    ctx->section_dirty = true;
}

static void hotkey_prev_section(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    auto* ctx = static_cast<kovaaks_context*>(data);
    ctx->manual_mode = true;
    ctx->current_section_idx--; 
    ctx->section_dirty = true;
}

static void ExportKillStats(const kovaaks_context* ctx, const std::vector<KillStatBot>& bots,
                             const std::map<std::string, BotAvg>* avgs = nullptr);

static void hotkey_show_killstats(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    auto* ctx = static_cast<kovaaks_context*>(data);
    std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex); // last_bots/last_csv_processed are also touched by the CSV watcher thread
    ctx->showing_ks_preview = false;
    if (!ctx->last_bots.empty()) {
        const std::string& csv_folder = ctx->kovaaks_stats_folder.empty()
            ? ctx->kovaaks_save_folder : ctx->kovaaks_stats_folder;
        auto avgs = ComputeAvgs(csv_folder, ctx->last_csv_processed, 10);
        ExportKillStats(ctx, ctx->last_bots, &avgs);
        ctx->killstats_timer = (ctx->killstats_display_sec > 0) ? (float)ctx->killstats_display_sec : 10.0f;
    }
}

std::string clean_sound(std::string input) {
    if (input.empty()) return "None";
    std::stringstream ss(input);
    std::string s;
    std::vector<std::string> parts;
    while (std::getline(ss, s, ';')) {
        size_t f_pos = s.find(" (FILENAME)");
        if (f_pos != std::string::npos) s.erase(f_pos);
        s.erase(0, s.find_first_not_of(" \r"));
        s.erase(s.find_last_not_of(" \r") + 1);
        if (!s.empty() && s != "FILENAME") {
            if (std::find(parts.begin(), parts.end(), s) == parts.end()) parts.push_back(s);
        }
    }
    return parts.empty() ? "Default" : nlohmann::json(parts).dump();
}

std::map<std::string, std::string> parse_ini(const std::string& path) {
    std::map<std::string, std::string> data;
    std::ifstream file(path);
    if (!file.is_open()) return data;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t sep = line.find('=');
        if (sep != std::string::npos) data[line.substr(0, sep)] = line.substr(sep + 1);
    }
    return data;
}

int LinearToSRGB(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    double srgb;
    if (linear <= 0.0031308) srgb = linear * 12.92;
    else srgb = 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(srgb * 255.999);
}

std::string get_vector_color_hex(const json& j, const std::string& key) {
    if (j.contains("vectorSettings") && j["vectorSettings"].contains(key)) {
        auto& vec = j["vectorSettings"][key];
        int r = LinearToSRGB(vec.value("x", 0.0));
        int g = LinearToSRGB(vec.value("y", 0.0));
        int b = LinearToSRGB(vec.value("z", 0.0));
        std::stringstream ss;
        ss << "#" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << r 
           << std::setw(2) << std::setfill('0') << g << std::setw(2) << std::setfill('0') << b;
        return ss.str();
    }
    return "None";
}

std::string get_rgb_color_hex(const json& j, const std::string& key) {
    if (j.contains("colorSettings") && j["colorSettings"].contains(key)) {
        auto& col = j["colorSettings"][key];
        int r = col.value("r", 0);
        int g = col.value("g", 0);
        int b = col.value("b", 0);
        std::stringstream ss;
        ss << "#" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << r 
           << std::setw(2) << std::setfill('0') << g << std::setw(2) << std::setfill('0') << b;
        return ss.str();
    }
    return "None";
}

// Kill stats CSV parsing and export

static std::vector<KillStatBot> ParseKillStatsCsv(const std::string& path);

// CSV watcher
// Returns true if any running process's name contains the given substring.
static bool IsProcessRunning(const char* name_substr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::string needle = name_substr;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32First(snap, &pe)) {
        do {
            std::string exe = pe.szExeFile;
            std::transform(exe.begin(), exe.end(), exe.begin(), ::tolower);
            if (exe.find(needle) != std::string::npos) { found = true; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// KovaaK's runs on Unreal Engine, packaged as "FPSAimTrainer-Win64-Shipping.exe".
// Matching on the substring covers that and any Steam/standalone naming variants.
static bool IsKovaaksRunning() {
    return IsProcessRunning("fpsaimtrainer");
}

// Event-driven folder watch via ReadDirectoryChangesW instead of re-scanning the whole
// stats folder every second. The thread sleeps at ~0% CPU and Windows wakes it the
// instant a matching file is written, instead of polling for it.
struct FolderWatcher {
    std::thread       thread;
    std::atomic<bool> running{false};
    HANDLE            stop_event = nullptr;
    std::string       path; // folder currently being watched, to detect path changes
};

// Blocks watching `folder` for file changes, calling on_change(filename) for each
// add/modify/rename event, until `watcher.running` is cleared.
static void RunFolderWatcher(const std::string& folder, FolderWatcher& watcher,
                              const std::function<void(const std::string&)>& on_change) {
    HANDLE hDir = CreateFileA(folder.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (hDir == INVALID_HANDLE_VALUE) {
        blog(LOG_WARNING, "[KovaaksPlugin] FolderWatcher: cannot open folder '%s'", folder.c_str());
        return;
    }
    blog(LOG_INFO, "[KovaaksPlugin] FolderWatcher: watching '%s'", folder.c_str());

    char buf[32768];
    OVERLAPPED ov = {};
    HANDLE hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    ov.hEvent = hEvent;
    HANDLE waitHandles[2] = { hEvent, watcher.stop_event };

    while (watcher.running.load()) {
        ResetEvent(hEvent);
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(hDir, buf, sizeof(buf), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytes, &ov, nullptr)) {
            Sleep(500);
            continue;
        }

        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1 || !watcher.running.load()) break; // stop signaled
        if (!GetOverlappedResult(hDir, &ov, &bytes, FALSE) || bytes == 0) continue;

        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf);
        while (true) {
            if (info->Action == FILE_ACTION_ADDED ||
                info->Action == FILE_ACTION_MODIFIED ||
                info->Action == FILE_ACTION_RENAMED_NEW_NAME) {

                int len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                    info->FileNameLength / sizeof(WCHAR), nullptr, 0, nullptr, nullptr);
                std::string fname(len, 0);
                WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                    info->FileNameLength / sizeof(WCHAR), &fname[0], len, nullptr, nullptr);
                on_change(fname);
            }
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<char*>(info) + info->NextEntryOffset);
        }
    }

    CancelIo(hDir);
    CloseHandle(hEvent);
    CloseHandle(hDir);
    blog(LOG_INFO, "[KovaaksPlugin] FolderWatcher: stopped ('%s').", folder.c_str());
}

static void StopWatcher(FolderWatcher& watcher) {
    if (watcher.running.load()) {
        watcher.running = false;
        if (watcher.stop_event) SetEvent(watcher.stop_event);
        if (watcher.thread.joinable()) watcher.thread.join();
    }
    watcher.path.clear();
}

static void StartWatcher(FolderWatcher& watcher, const std::string& folder,
                          std::function<void(std::string)> thread_fn) {
    if (folder.empty() || folder == watcher.path) return;
    StopWatcher(watcher);
    if (!watcher.stop_event) watcher.stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(watcher.stop_event);
    watcher.path = folder;
    watcher.running = true;
    watcher.thread = std::thread(std::move(thread_fn), folder);
}

static FolderWatcher g_csv_watcher;
static FolderWatcher g_settings_watcher;

static void ProcessNewCsv(const std::string& csv_folder, const std::string& full_path) {
    std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
    if (!g_watcher_ctx) return;
    auto* ctx = g_watcher_ctx;

    auto bots = ParseKillStatsCsv(full_path);
    ctx->last_csv_processed = full_path;

    // Always cache the latest run's stats for manual recall via hotkey/button
    ctx->last_bots = bots;

    // Auto-display only triggers when there are multiple targets (multi-bot runs)
    if (IsGauntletRun(bots)) {
        ctx->showing_ks_preview = false;
        auto avgs = ComputeAvgs(csv_folder, full_path, 10);
        ExportKillStats(ctx, bots, &avgs);
        ctx->killstats_timer = (ctx->killstats_display_sec > 0) ? (float)ctx->killstats_display_sec : 10.0f;
    }
}

static void StartCsvWatcher(const std::string& csv_folder) {
    StartWatcher(g_csv_watcher, csv_folder, [csv_folder](std::string folder) {
        RunFolderWatcher(folder, g_csv_watcher, [csv_folder](const std::string& fname) {
            bool is_csv = fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".csv") == 0;
            if (is_csv && fname.find("Challenge") != std::string::npos) {
                Sleep(500); // let KovaaK's finish writing the file before we read it
                ProcessNewCsv(csv_folder, csv_folder + "\\" + fname);
            }
        });
    });
}
static void StopCsvWatcher() { StopWatcher(g_csv_watcher); }

// KovaaK's rewrites these two files whenever the player changes sensitivity, binds,
// or other in-game settings, we react to that instead of re-reading them on a timer.
static void StartSettingsWatcher(const std::string& save_folder) {
    StartWatcher(g_settings_watcher, save_folder, [](std::string folder) {
        RunFolderWatcher(folder, g_settings_watcher, [](const std::string& fname) {
            std::string lower = fname;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "primaryusersettings.json" || lower == "weaponsettings.ini") {
                Sleep(200); // let KovaaK's finish writing the file before we read it
                std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
                if (g_watcher_ctx) {
                    g_watcher_ctx->cache_valid = false;
                    ExportToJson(g_watcher_ctx, true);
                }
            }
        });
    });
}
static void StopSettingsWatcher() { StopWatcher(g_settings_watcher); }

// Returns up to maxRuns CSV paths sorted newest-first, excluding exclude_path
static std::string ExtractScenarioName(const std::string& path) {
    try {
        if (path.empty() || !std::filesystem::exists(path)) return "";
        std::ifstream f(path);
        if (!f.is_open()) return "";
        std::string line;
        int linesRead = 0;
        while (std::getline(f, line) && linesRead < 50) {
            linesRead++;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("Scenario:,", 0) == 0) return line.substr(10);
        }
    } catch (...) {}
    return "";
}

static std::vector<std::string> FindRecentCsvs(const std::string& folder, const std::string& exclude_path, int maxRuns) {
    struct CsvEntry { std::string path; std::filesystem::file_time_type mtime; };
    std::vector<CsvEntry> entries;
    try {
        for (auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            if (fname.find(".csv") == std::string::npos) continue;
            if (fname.find("Challenge") == std::string::npos) continue;
            std::string p = entry.path().string();
            if (p == exclude_path) continue;
            entries.push_back({p, entry.last_write_time()});
        }
    } catch (...) {}
    std::sort(entries.begin(), entries.end(), [](const CsvEntry& a, const CsvEntry& b){ return a.mtime > b.mtime; });
    std::vector<std::string> result;
    int count = (maxRuns < (int)entries.size()) ? maxRuns : (int)entries.size();
    for (int i = 0; i < count; i++) result.push_back(entries[i].path);
    return result;
}

// Scenario-filtered version: keeps only runs from the same scenario as exclude_path
static std::vector<std::string> FindRecentCsvsSameScenario(const std::string& folder,
    const std::string& reference_path, int maxRuns) {
    std::string ref_scenario = ExtractScenarioName(reference_path);

    struct CsvEntry { std::string path; std::filesystem::file_time_type mtime; };
    std::vector<CsvEntry> entries;
    try {
        for (auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            if (fname.find(".csv") == std::string::npos) continue;
            if (fname.find("Challenge") == std::string::npos) continue;
            std::string p = entry.path().string();
            if (p == reference_path) continue;
            if (!ref_scenario.empty() && ExtractScenarioName(p) != ref_scenario) continue;
            entries.push_back({p, entry.last_write_time()});
        }
    } catch (...) {}
    std::sort(entries.begin(), entries.end(), [](const CsvEntry& a, const CsvEntry& b){ return a.mtime > b.mtime; });
    std::vector<std::string> result;
    int count = (maxRuns < (int)entries.size()) ? maxRuns : (int)entries.size();
    for (int i = 0; i < count; i++) result.push_back(entries[i].path);
    return result;
}

static std::vector<KillStatBot> ParseKillStatsCsv(const std::string& path) {
    std::vector<KillStatBot> bots;
    std::ifstream f(path);
    if (!f.is_open()) return bots;

    std::string line;
    bool header_skipped = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;

        if (!header_skipped) { header_skipped = true; continue; }

        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, ',')) cols.push_back(col);
        if (cols.size() < 8) continue;
        if (cols[0].empty() || !std::isdigit((unsigned char)cols[0][0])) continue;

        KillStatBot bot;
        bot.name = cols[2];
        try {
            std::string ttk_str = cols[4];
            if (!ttk_str.empty() && ttk_str.back() == 's') ttk_str.pop_back();
            bot.ttk      = std::stof(ttk_str);
            bot.shots    = std::stoi(cols[5]);
            bot.hits     = std::stoi(cols[6]);
            bot.accuracy = std::stof(cols[7]) * 100.0f;
            if (cols.size() >= 11)
                bot.efficiency = std::stof(cols[10]) * 100.0f;
        } catch (...) { continue; }
        bots.push_back(bot);
    }

    // Assign a unique internal key per position ("bot_0", "bot_1"...)
    // display_name keeps the real name for display
    for (int i = 0; i < (int)bots.size(); i++)
        bots[i].key = "bot_" + std::to_string(i);

    return bots;
}

static std::map<std::string, BotAvg> ComputeAvgs(const std::string& folder, const std::string& exclude_path, int maxRuns) {
    std::map<std::string, BotAvg> avgs;
    // Filter by scenario: only average runs from the same scenario
    auto paths = FindRecentCsvsSameScenario(folder, exclude_path, maxRuns);
    for (auto& p : paths) {
        auto bots = ParseKillStatsCsv(p);
        if (!IsGauntletRun(bots)) continue;
        for (auto& b : bots) {
            auto& a = avgs[b.key];
            a.accuracy   += b.accuracy;
            a.ttk        += b.ttk;
            a.efficiency += b.efficiency;
            a.shots      += b.shots;
            a.hits       += b.hits;
            a.count++;
        }
    }
    for (auto& kv : avgs) {
        if (kv.second.count > 0) {
            kv.second.accuracy   /= kv.second.count;
            kv.second.ttk        /= kv.second.count;
            kv.second.efficiency /= kv.second.count;
            kv.second.shots      /= kv.second.count;
            kv.second.hits       /= kv.second.count;
        }
    }
    return avgs;
}

static void ExportKillStats(const kovaaks_context* ctx, const std::vector<KillStatBot>& bots,
                             const std::map<std::string, BotAvg>* avgs) {
    if (ctx->export_folder.empty()) return;

    json out;
    out["visible"]          = true;
    out["theme"]            = ctx->killstats_theme;
    out["opacity"]          = ctx->killstats_opacity / 100.0f;
    out["show_accuracy"]    = ctx->ks_show_accuracy;
    out["show_ttk"]         = ctx->ks_show_ttk;
    out["show_shots_hits"]  = ctx->ks_show_shots_hits;
    out["show_efficiency"]  = ctx->ks_show_efficiency;
    out["show_avgs"]        = ctx->ks_show_avgs;
    out["bots"]             = json::array();

    for (auto& b : bots) {
        std::stringstream acc; acc << std::fixed << std::setprecision(1) << b.accuracy;
        std::stringstream ttk; ttk << std::fixed << std::setprecision(2) << b.ttk;
        std::stringstream eff; eff << std::fixed << std::setprecision(1) << b.efficiency;
        json bot_j = {
            {"name",       b.name},
            {"accuracy",   acc.str()},
            {"ttk",        ttk.str()},
            {"shots",      b.shots},
            {"hits",       b.hits},
            {"efficiency", eff.str()}
        };
        if (avgs) {
            auto it = avgs->find(b.key);
            if (it != avgs->end() && it->second.count > 0) {
                std::stringstream avAcc; avAcc << std::fixed << std::setprecision(1) << it->second.accuracy;
                std::stringstream avTtk; avTtk << std::fixed << std::setprecision(2) << it->second.ttk;
                std::stringstream avEff; avEff << std::fixed << std::setprecision(1) << it->second.efficiency;
                bot_j["avg_accuracy"]   = avAcc.str();
                bot_j["avg_ttk"]        = avTtk.str();
                bot_j["avg_shots"]      = it->second.shots;
                bot_j["avg_hits"]       = it->second.hits;
                bot_j["avg_efficiency"] = avEff.str();
                bot_j["avg_runs"]       = it->second.count;
            }
        }
        out["bots"].push_back(bot_j);
    }
    try {
        std::ofstream o(ctx->export_folder + "/kovaaks_killstats.json");
        o << std::setw(4) << out << std::endl;
    } catch (...) {}
}

static void HideKillStats(const kovaaks_context* ctx) {
    if (ctx->export_folder.empty()) return;
    try {
        json out; out["visible"] = false; out["bots"] = json::array();
        std::ofstream o(ctx->export_folder + "/kovaaks_killstats.json");
        o << out << std::endl;
    } catch (...) {}
}

void ExportToJson(kovaaks_context* ctx, bool force_reload) {    // Only re-read files when cache is invalid or forced (settings watcher, settings change)
    if (!ctx->cache_valid || force_reload) {
        std::string json_path = ctx->kovaaks_save_folder + "/PrimaryUserSettings.json";
        std::string ini_path  = ctx->kovaaks_save_folder + "/WeaponSettings.ini";
        std::ifstream j_f(json_path);
        ctx->cached_json = json::object();
        if (j_f.is_open()) { try { j_f >> ctx->cached_json; } catch(...) {} }
        if (!ctx->cached_json.is_object()) ctx->cached_json = json::object();
        ctx->cached_ini  = parse_ini(ini_path);
        ctx->cache_valid = true;
    }

    json& j   = ctx->cached_json;
    auto& ini = ctx->cached_ini;
    
    // C++ determines which sections are active
    std::set<int> active_sections_set;
    for (auto& kv : ctx->visible) {
        if (kv.second) active_sections_set.insert(ctx->sec_assignment[kv.first]);
    }
    for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
        if (ctx->custom_info[i].visible && !ctx->custom_info[i].label.empty() && !ctx->custom_info[i].value.empty()) {
            active_sections_set.insert(ctx->custom_info[i].sec_assignment);
        }
    }

    std::vector<int> active_sections(active_sections_set.begin(), active_sections_set.end());
    
    int target_sec = -1;
    bool show_single = ctx->auto_rotate || ctx->manual_mode;

    if (!active_sections.empty()) {
        int n = (int)active_sections.size();
        // Correct modulo for negative values (C++ % can return negative)
        ctx->current_section_idx = ((ctx->current_section_idx % n) + n) % n;
        target_sec = active_sections[ctx->current_section_idx];
    }

    json out;
    out["stats"] = json::array();
    
    auto add = [&](std::string id, std::string label, std::string val) {
        auto it = ctx->visible.find(id);
        if (it != ctx->visible.end() && it->second) {
            int s_idx = (ctx->sec_assignment.count(id)) ? ctx->sec_assignment.at(id) : 1;
            
            // C++ now filters the stats itself (JS no longer needs to)
            if (show_single && target_sec != -1 && s_idx != target_sec) return;

            std::string p = (ctx->pos.count(id)) ? ctx->pos.at(id) : "inline";
            std::string sec_name = ctx->sec_names[s_idx - 1];
            if (sec_name.empty()) sec_name = "SECTION " + std::to_string(s_idx);

            out["stats"].push_back({
                {"id", id}, 
                {"label", label}, 
                {"val", val}, 
                {"pos", p},
                {"section", sec_name}
            });
        }
    };

    try {
        out["ui"] = {
            {"theme", ctx->ui_theme}, 
            {"css", ctx->custom_css}, 
            {"opacity", ctx->bg_opacity / 100.0f},
            {"anim_type", ctx->anim_type},
            {"alignment", ctx->alignment},
            {"title_alignment", ctx->title_alignment},
            {"label_layout", ctx->label_layout},
            {"content_align", ctx->content_align},
            {"fixed_size", ctx->fixed_size},
            {"show_titles", ctx->show_titles},
            {"show_row_labels", ctx->show_row_labels}
        };
        
        auto get_float = [&](const std::string& key, float def) -> float {
            if (j.contains("floatSettings") && j["floatSettings"].contains(key)) return j["floatSettings"][key];
            return def;
        };
        auto get_string = [&](const std::string& key, std::string def) -> std::string {
            if (j.contains("stringSettings") && j["stringSettings"].contains(key)) return j["stringSettings"][key];
            return def;
        };
        auto get_int = [&](const std::string& key, int def) -> int {
            if (j.contains("integerSettings") && j["integerSettings"].contains(key)) return j["integerSettings"][key];
            return def;
        };
        auto get_bool = [&](const std::string& key, bool def) -> bool {
            if (j.contains("booleanSettings") && j["booleanSettings"].contains(key)) return j["booleanSettings"][key];
            return def;
        };
        auto add_float = [&](std::string id, std::string label, std::string key) {
            float val = get_float(key, 0.0f);
            std::stringstream ss; ss << std::fixed << std::setprecision(2) << val;
            add(id, label, ss.str());
        };

        add("dpi", "DPI", std::to_string(get_int("EIntegerSettingId::DPI", 0)));

        std::string sens_str;
        if (ini.count("OverrideSens") && ini["OverrideSens"] == "true") {
            float x = ini.count("HorizontalSens") ? std::stof(ini["HorizontalSens"]) : 0.0f;
            std::string sc = ini.count("SensScale") ? ini["SensScale"] : "cm/360";
            std::stringstream ss; ss << std::fixed << std::setprecision(2) << x;
            sens_str = ss.str() + " " + sc;
        } else {
            float x = get_float("EFloatSettingId::XSens", 0.0f);
            std::string sc = get_string("EStringSettingId::SensScaleString", "cm/360");
            std::stringstream ss; ss << std::fixed << std::setprecision(2) << x;
            sens_str = ss.str() + " " + sc;
        }
        add("sens", "Sensitivity", sens_str);

        std::string fov_str;
        if (ini.count("OverrideFOV") && ini["OverrideFOV"] == "true") {
            int fov = ini.count("FOV") ? (int)std::stof(ini["FOV"]) : 103;
            std::string sc = ini.count("FOVScale") ? ini["FOVScale"] : "Overwatch";
            fov_str = std::to_string(fov) + " " + sc;
        } else {
            int fov = (int)get_float("EFloatSettingId::FOV", 103.0f);
            std::string sc = get_string("EStringSettingId::FOVScaleString", "Overwatch");
            fov_str = std::to_string(fov) + " " + sc;
        }
        add("fov", "FOV", fov_str);

        add("ktheme", "Kovaaks Theme", get_string("EStringSettingId::CurrentThemeName", "Default"));
        add("cross", "Crosshair Name", ini.count("CrosshairFile") ? ini["CrosshairFile"] : "None");
        
        if (ini.count("CrosshairColor")) {
            try {
                std::string c = ini["CrosshairColor"];
                int r = LinearToSRGB(std::stod(c.substr(c.find("X=") + 2)));
                int g = LinearToSRGB(std::stod(c.substr(c.find("Y=") + 2)));
                int b = LinearToSRGB(std::stod(c.substr(c.find("Z=") + 2)));
                std::stringstream hex_ss;
                hex_ss << "#" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << r << std::setw(2) << std::setfill('0') << g << std::setw(2) << std::setfill('0') << b;
                add("cross_rgb", "Crosshair RGB", std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b));
                add("cross_hex", "Crosshair HEX", hex_ss.str());
            } catch (...) {}
        }
        add("cross_size", "Crosshair Size", ini.count("CrosshairScale") ? ini["CrosshairScale"] : "1.0");

        add("enemy_col", "Enemy Body Color", get_vector_color_hex(j, "EVectorSettingId::EnemyBodyColor"));
        add("enemy_head_col", "Enemy Head Color", get_vector_color_hex(j, "EVectorSettingId::EnemyHeadColor"));
        add_float("enemy_bright", "Enemy Brightness", "EFloatSettingId::EnemyFullBright");
        add_float("enemy_metal", "Enemy Metallic", "EFloatSettingId::EnemyMetalic");
        add_float("enemy_rough", "Enemy Roughness", "EFloatSettingId::EnemyRoughness");

        int sky_preset = get_int("EIntegerSettingId::SkyPreset", 0);
        add("sky_preset", "Sky Preset", sky_preset == 0 ? "Default (0)" : "Style " + std::to_string(sky_preset));
        add("sky_col", "Sky Color", get_rgb_color_hex(j, "EColorSettingId::SkyColor"));
        add("sky_clouds", "Cloud Cover", std::to_string(get_int("EIntegerSettingId::CloudCover", 3)));
        add("sky_solid", "Solid Color Sky", get_bool("EBooleanSettingId::SolidSkyColor", false) ? "Yes" : "No");
        add("sky_sun", "Show Sun", get_bool("EBooleanSettingId::ShowSunInSkybox", false) ? "Yes" : "No");

        add("wall_col", "Wall Color", get_vector_color_hex(j, "EVectorSettingId::WallColor"));
        add("wall_mat", "Wall Texture", get_string("EStringSettingId::WallMaterial", "Default"));
        add_float("wall_bright", "Wall FullBright", "EFloatSettingId::WallFullBright");
        add_float("wall_metal", "Wall Metallic", "EFloatSettingId::WallMetallic");
        add_float("wall_rough", "Wall Roughness", "EFloatSettingId::WallRoughness");

        add("floor_col", "Floor Color", get_vector_color_hex(j, "EVectorSettingId::FloorColor"));
        add("floor_mat", "Floor Texture", get_string("EStringSettingId::FloorMaterial", "Default"));
        add_float("floor_bright", "Floor FullBright", "EFloatSettingId::FloorFullBright");
        add_float("floor_metal", "Floor Metallic", "EFloatSettingId::FloorMetallic");
        add_float("floor_rough", "Floor Roughness", "EFloatSettingId::FloorRoughness");

        add("ceil_col", "Ceiling Color", get_vector_color_hex(j, "EVectorSettingId::CeilingColor"));
        add("ceil_mat", "Ceiling Texture", get_string("EStringSettingId::CeilingMaterial", "Default"));
        add_float("ceil_bright", "Ceil FullBright", "EFloatSettingId::CeilingFullBright");
        add_float("ceil_metal", "Ceil Metallic", "EFloatSettingId::CeilingMetallic");
        add_float("ceil_rough", "Ceil Roughness", "EFloatSettingId::CeilingRoughness");

        add("ramp_col", "Ramp Color", get_vector_color_hex(j, "EVectorSettingId::RampColor"));
        add("ramp_mat", "Ramp Texture", get_string("EStringSettingId::RampMaterial", "Default"));
        add_float("ramp_bright", "Ramp FullBright", "EFloatSettingId::RampFullBright");
        add_float("ramp_metal", "Ramp Metallic", "EFloatSettingId::RampMetallic");
        add_float("ramp_rough", "Ramp Roughness", "EFloatSettingId::RampRoughness");

        add("hits", "Hit Sounds", clean_sound(ini.count("BodyHitSound") ? ini["BodyHitSound"] : ""));
        add("head", "Head Sound", clean_sound(ini.count("HeadHitSound") ? ini["HeadHitSound"] : ""));
        add("kill", "Kill Sound", clean_sound(get_string("EStringSettingId::KillConfirmedSound", "")));
        add("shot", "Shot Sound", clean_sound(ini.count("ShootSound") ? ini["ShootSound"] : ""));
        add("spawn", "Spawn Sound", clean_sound(get_string("EStringSettingId::SpawnSound", "None")));
        add("cd", "Sound Cooldown", (ini.count("HitSoundCooldown") ? ini["HitSoundCooldown"] : "0.00s"));

        for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
            const auto& ci = ctx->custom_info[i];
            if (ci.visible && !ci.label.empty() && !ci.value.empty()) {
                if (show_single && target_sec != -1 && ci.sec_assignment != target_sec) continue;

                std::string id = "custom_info_" + std::to_string(i);
                std::string sec_name = ctx->sec_names[ci.sec_assignment - 1];
                if (sec_name.empty()) sec_name = "SECTION " + std::to_string(ci.sec_assignment);
                out["stats"].push_back({
                    {"id", id},
                    {"label", ci.label},
                    {"val", ci.value},
                    {"pos", ci.pos},
                    {"section", sec_name}
                });
            }
        }

        std::ofstream o(ctx->export_folder + "/kovaaks_stats.json");
        o << std::setw(4) << out << std::endl;
    } catch (...) {}
}

std::string get_theme_css_from_html(const std::string& export_folder, const std::string& theme_name) {
    if (export_folder.empty() || theme_name.empty()) return "";
    std::string html_path = export_folder + "/kovaaks_widget.html";
    std::ifstream file(html_path);
    if (!file.is_open()) return "/* ERROR: Cannot open kovaaks_widget.html */";

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    std::string css = "";
    std::string theme_class = ".theme-" + theme_name;
    size_t start_pos = content.find(theme_class);
    
    if (start_pos != std::string::npos) {
        size_t anim_search_start = start_pos;
        if (theme_name == "thx") {
            size_t anim = content.rfind("@keyframes thxStatic", start_pos);
            if (anim != std::string::npos) anim_search_start = anim;
        } else if (theme_name == "blood") {
            size_t anim = content.rfind("@keyframes heartbeat", start_pos);
            if (anim != std::string::npos) anim_search_start = anim;
        }
        size_t end_pos = content.find("</style>", start_pos);
        size_t next_pos = start_pos + theme_class.length();
        while ((next_pos = content.find(".theme-", next_pos)) != std::string::npos) {
            if (content.substr(next_pos, theme_class.length()) != theme_class) {
                end_pos = next_pos;
                size_t prev_comment = content.rfind("/*", end_pos);
                if (prev_comment != std::string::npos && prev_comment > start_pos) end_pos = prev_comment;
                break;
            }
            next_pos += theme_class.length();
        }
        css = content.substr(anim_search_start, end_pos - anim_search_start);
        size_t pos = 0;
        while ((pos = css.find(theme_class, pos)) != std::string::npos) {
             css.replace(pos, theme_class.length(), ".theme-custom");
             pos += 13; 
        }
        css.erase(0, css.find_first_not_of(" \n\r\t"));
        css.erase(css.find_last_not_of(" \n\r\t") + 1);
        return css;
    }
    return "/* ERROR: Theme class not found. */";
}

static bool update_ui_visibility(obs_properties_t* props, obs_data_t* settings) {
    std::string sec = obs_data_get_string(settings, "edit_section");
    if (sec.empty()) sec = "sec0";

    obs_property_set_visible(obs_properties_get(props, "grp_killstats"), sec == "sec0"); // Killstats group visible in General (sec0)
    obs_property_set_visible(obs_properties_get(props, "grp_ui"),        sec == "sec0");
    obs_property_set_visible(obs_properties_get(props, "grp_sec1"),      sec == "sec1");
    obs_property_set_visible(obs_properties_get(props, "grp_sec2"),      sec == "sec2");
    obs_property_set_visible(obs_properties_get(props, "grp_sec3"),      sec == "sec3");
    obs_property_set_visible(obs_properties_get(props, "grp_sec_names"), sec == "sec_names");

    bool auto_rot = obs_data_get_bool(settings, "auto_rotate");
    obs_property_set_visible(obs_properties_get(props, "fixed_size"), auto_rot);
    obs_property_set_visible(obs_properties_get(props, "rotation_delay"), auto_rot);

    std::string theme = obs_data_get_string(settings, "ui_theme");
    bool is_custom = (theme == "custom");

    obs_property_set_visible(obs_properties_get(props, "load_template"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "user_theme_select"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "custom_css"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "theme_manage_name"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "theme_action"), is_custom);

    return true;
}

static bool on_custom_info_changed(obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
    if (ctx) kovaaks_update(ctx, settings);
    return update_ui_visibility(props, settings);
}

static bool on_ui_theme_changed(obs_properties_t* props, obs_property_t* p, obs_data_t* settings) {
    std::string theme = obs_data_get_string(settings, "ui_theme");
    if (theme != "custom") {
        obs_data_set_string(settings, "load_template", "");
        obs_data_set_string(settings, "user_theme_select", "");
    }
    auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
    if (ctx) kovaaks_update(ctx, settings);
    return update_ui_visibility(props, settings);
}

static bool on_template_loaded(obs_properties_t* props, obs_property_t* p, obs_data_t* settings) {
    std::string tmpl = obs_data_get_string(settings, "load_template");
    if (!tmpl.empty()) {
        std::string export_folder = obs_data_get_string(settings, "export_folder");
        std::string css = get_theme_css_from_html(export_folder, tmpl);
        obs_data_set_string(settings, "custom_css", css.c_str());
        obs_data_set_string(settings, "ui_theme", "custom");
        obs_data_set_string(settings, "user_theme_select", "");
        auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
        if (ctx) kovaaks_update(ctx, settings);
    }
    return update_ui_visibility(props, settings); 
}

static bool on_user_theme_loaded(obs_properties_t* props, obs_property_t* p, obs_data_t* settings) {
    std::string user_theme = obs_data_get_string(settings, "user_theme_select");
    if (!user_theme.empty()) {
        std::string json_str = obs_data_get_string(settings, "user_themes_data");
        if (!json_str.empty()) {
            try {
                json j = json::parse(json_str);
                if (j.contains(user_theme)) {
                    obs_data_set_string(settings, "custom_css", j[user_theme].get<std::string>().c_str());
                    obs_data_set_string(settings, "ui_theme", "custom");
                    obs_data_set_string(settings, "load_template", "");
                    auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
                    if (ctx) kovaaks_update(ctx, settings);
                }
            } catch(...) {}
        }
    }
    return update_ui_visibility(props, settings);
}

static bool on_user_theme_action(obs_properties_t* props, obs_property_t* p, obs_data_t* settings) {
    std::string action = obs_data_get_string(settings, "theme_action");
    if (action.empty()) return update_ui_visibility(props, settings);

    std::string json_str = obs_data_get_string(settings, "user_themes_data");
    json j = json::object();
    if (!json_str.empty()) { try { j = json::parse(json_str); } catch(...) {} }

    bool modified = false;
    std::string css = obs_data_get_string(settings, "custom_css");
    std::string popup_msg = "";

    if (action == "save") {
        std::string name = obs_data_get_string(settings, "theme_manage_name");
        if (!name.empty()) {
            j[name] = css;
            obs_data_set_string(settings, "theme_manage_name", "");
            obs_data_set_string(settings, "user_theme_select", name.c_str());
            obs_data_set_string(settings, "load_template", "");
            obs_data_set_string(settings, "ui_theme", "custom");
            modified = true;
            popup_msg = "Theme '" + name + "' successfully saved!";
        } else { popup_msg = "Error: Please enter a name for the new theme."; }
    } else if (action == "update") {
        std::string name = obs_data_get_string(settings, "user_theme_select");
        if (!name.empty() && j.contains(name)) {
            j[name] = css; modified = true; popup_msg = "Theme '" + name + "' successfully updated!";
        } else { popup_msg = "Error: Please select a valid User Theme to update."; }
    } else if (action == "delete") {
        std::string name = obs_data_get_string(settings, "user_theme_select");
        if (!name.empty() && j.contains(name)) {
            j.erase(name); obs_data_set_string(settings, "user_theme_select", ""); modified = true;
            popup_msg = "Theme '" + name + "' successfully deleted!";
        } else { popup_msg = "Error: Please select a valid User Theme to delete."; }
    }

    if (modified) {
        obs_data_set_string(settings, "user_themes_data", j.dump().c_str());
        obs_property_t* p_sel = obs_properties_get(props, "user_theme_select");
        if (p_sel) {
            obs_property_list_clear(p_sel);
            obs_property_list_add_string(p_sel, "Select Saved User Theme", "");
            for (auto& el : j.items()) obs_property_list_add_string(p_sel, el.key().c_str(), el.key().c_str());
        }
        auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
        if (ctx) kovaaks_update(ctx, settings);
    }
    obs_data_set_string(settings, "theme_action", "");
#ifdef _WIN32
    if (!popup_msg.empty()) MessageBoxA(NULL, popup_msg.c_str(), "KovaaK's Theme Manager", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
#endif
    return update_ui_visibility(props, settings); 
}

static bool on_section_changed(obs_properties_t* props, obs_property_t* p, obs_data_t* settings) { return update_ui_visibility(props, settings); }
static bool on_oni_clicked(obs_properties_t* props, obs_property_t* p, void* data) { system("start https://x.com/Oni6666"); return false; }
static bool on_shiku_clicked(obs_properties_t* props, obs_property_t* p, void* data) { system("start https://x.com/ShikuAims"); return false; }

void kovaaks_update(void* data, obs_data_t* settings) {
    obs_data_erase(settings, "_ctx_ptr");
    auto* ctx = static_cast<kovaaks_context*>(data);
    std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex); // ctx fields below are also read by the watcher threads

    ctx->kovaaks_save_folder  = obs_data_get_string(settings, "kovaaks_save_folder");
    ctx->kovaaks_stats_folder = obs_data_get_string(settings, "kovaaks_stats_folder");
    ctx->export_folder        = obs_data_get_string(settings, "export_folder");
    ctx->cache_valid = false; // Invalidate cache when paths or settings change

    ctx->ui_theme = obs_data_get_string(settings, "ui_theme");
    ctx->custom_css = obs_data_get_string(settings, "custom_css");
    ctx->bg_opacity = (int)obs_data_get_int(settings, "bg_opacity");
    
    bool old_auto = ctx->auto_rotate;
    ctx->auto_rotate = obs_data_get_bool(settings, "auto_rotate");
    if (old_auto != ctx->auto_rotate) {
        ctx->manual_mode = false;
        ctx->current_section_idx = 0;
        ctx->rotation_timer = 0.0f;
    }

    ctx->show_titles = obs_data_get_bool(settings, "show_titles");
    ctx->show_row_labels = obs_data_get_bool(settings, "show_row_labels");
    
    ctx->title_alignment = obs_data_get_string(settings, "title_alignment");
    if (ctx->title_alignment.empty()) ctx->title_alignment = "center";

    ctx->label_layout = obs_data_get_string(settings, "label_layout");
    if (ctx->label_layout.empty()) ctx->label_layout = "horizontal";

    ctx->content_align = obs_data_get_string(settings, "content_align");
    if (ctx->content_align.empty()) ctx->content_align = "left";

    ctx->rotation_delay = (int)obs_data_get_int(settings, "rotation_delay");
    if (ctx->rotation_delay < 1) ctx->rotation_delay = 6;

    ctx->fixed_size = obs_data_get_bool(settings, "fixed_size");
    
    ctx->anim_type = obs_data_get_string(settings, "anim_type");
    if (ctx->anim_type.empty()) ctx->anim_type = "slide-up";
    
    ctx->alignment = obs_data_get_string(settings, "alignment");
    if (ctx->alignment.empty()) ctx->alignment = "bottom-left";
    
    std::string u_themes = obs_data_get_string(settings, "user_themes_data");
    ctx->user_themes.clear();
    if (!u_themes.empty()) {
        try {
            json j = json::parse(u_themes);
            for (auto& el : j.items()) ctx->user_themes[el.key()] = el.value().get<std::string>();
        } catch(...) {}
    }

    for(int i=0; i<5; i++) ctx->sec_names[i] = obs_data_get_string(settings, ("sec_name_" + std::to_string(i+1)).c_str());

    for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
        std::string prefix = "ci_" + std::to_string(i) + "_";
        ctx->custom_info[i].visible       = obs_data_get_bool(settings,   (prefix + "visible").c_str());
        ctx->custom_info[i].label         = obs_data_get_string(settings, (prefix + "label").c_str());
        ctx->custom_info[i].value         = obs_data_get_string(settings, (prefix + "value").c_str());
        ctx->custom_info[i].sec_assignment = (int)obs_data_get_int(settings, (prefix + "sec").c_str());
        if (ctx->custom_info[i].sec_assignment < 1 || ctx->custom_info[i].sec_assignment > 5) ctx->custom_info[i].sec_assignment = 1;
        ctx->custom_info[i].pos           = obs_data_get_string(settings, (prefix + "pos").c_str());
        if (ctx->custom_info[i].pos.empty()) ctx->custom_info[i].pos = "inline";
    }
    
    std::vector<std::string> ids = {
        "dpi", "sens", "fov", "ktheme", "cross", "cross_rgb", "cross_hex", "cross_size", 
        "enemy_col", "enemy_head_col", "enemy_bright", "enemy_metal", "enemy_rough",
        "sky_preset", "sky_col", "sky_clouds", "sky_solid", "sky_sun",
        "wall_col", "wall_mat", "wall_bright", "wall_metal", "wall_rough",
        "floor_col", "floor_mat", "floor_bright", "floor_metal", "floor_rough",
        "ceil_col", "ceil_mat", "ceil_bright", "ceil_metal", "ceil_rough",
        "ramp_col", "ramp_mat", "ramp_bright", "ramp_metal", "ramp_rough",
        "hits", "head", "kill", "shot", "spawn", "cd"
    };
    
    for (auto& id : ids) {
        ctx->visible[id] = obs_data_get_bool(settings, ("v_" + id).c_str());
        ctx->pos[id] = obs_data_get_string(settings, ("p_" + id).c_str());
        ctx->sec_assignment[id] = obs_data_get_int(settings, ("s_" + id).c_str());
        if(ctx->sec_assignment[id] == 0) ctx->sec_assignment[id] = 1; 
    }

    if (!ctx->kovaaks_save_folder.empty() && !ctx->export_folder.empty()) ExportToJson(ctx, true);

    // KillStats settings
    ctx->killstats_theme       = obs_data_get_string(settings, "ks_theme");
    if (ctx->killstats_theme.empty()) ctx->killstats_theme = "dark";
    ctx->killstats_opacity     = (int)obs_data_get_int(settings, "ks_opacity");
    if (ctx->killstats_opacity <= 0) ctx->killstats_opacity = 90;
    ctx->killstats_display_sec = (int)obs_data_get_int(settings, "ks_display_sec");
    if (ctx->killstats_display_sec <= 0) ctx->killstats_display_sec = 10;
    ctx->ks_show_accuracy      = obs_data_get_bool(settings, "ks_show_accuracy");
    ctx->ks_show_ttk           = obs_data_get_bool(settings, "ks_show_ttk");
    ctx->ks_show_shots_hits    = obs_data_get_bool(settings, "ks_show_shots_hits");
    ctx->ks_show_efficiency    = obs_data_get_bool(settings, "ks_show_efficiency");
    ctx->ks_show_avgs          = obs_data_get_bool(settings, "ks_show_avgs");
}

obs_properties_t* kovaaks_get_properties(void* data) {
    obs_properties_t* props = obs_properties_create_param(data, nullptr);
    auto* ctx = static_cast<kovaaks_context*>(data);
    bool is_custom = ctx ? (ctx->ui_theme == "custom") : false;

    obs_properties_add_path(props, "kovaaks_save_folder", "Kovaaks SaveGames Folder", OBS_PATH_DIRECTORY, nullptr, "");
    obs_properties_add_path(props, "kovaaks_stats_folder", "Kovaaks Stats Folder (optional)", OBS_PATH_DIRECTORY, nullptr, "");
    obs_properties_add_path(props, "export_folder", "Widget Export Folder", OBS_PATH_DIRECTORY, nullptr, "");

    obs_property_t* section_list = obs_properties_add_list(props, "edit_section", "SELECT CATEGORY TO EDIT", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(section_list, "General", "sec0");
    obs_property_list_add_string(section_list, "Stats: Core & Audio", "sec1");
    obs_property_list_add_string(section_list, "Stats: Crosshair & Enemies", "sec2");
    obs_property_list_add_string(section_list, "Stats: Environment Visuals", "sec3");
    obs_property_list_add_string(section_list, "Custom Section Names & Info", "sec_names");
    obs_property_set_modified_callback(section_list, on_section_changed);

    // Kill stats widget, placed first for quick access
    // Live callback: re-exports immediately if preview is active
    static auto on_ks_changed = [](obs_properties_t* props, obs_property_t*, obs_data_t* settings) -> bool {
        auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
        if (!ctx) return false;
        ctx->killstats_theme       = obs_data_get_string(settings, "ks_theme");
        if (ctx->killstats_theme.empty()) ctx->killstats_theme = "dark";
        ctx->killstats_opacity     = (int)obs_data_get_int(settings, "ks_opacity");
        if (ctx->killstats_opacity <= 0) ctx->killstats_opacity = 90;
        ctx->ks_show_accuracy      = obs_data_get_bool(settings, "ks_show_accuracy");
        ctx->ks_show_ttk           = obs_data_get_bool(settings, "ks_show_ttk");
        ctx->ks_show_shots_hits    = obs_data_get_bool(settings, "ks_show_shots_hits");
        ctx->ks_show_efficiency    = obs_data_get_bool(settings, "ks_show_efficiency");
        ctx->ks_show_avgs          = obs_data_get_bool(settings, "ks_show_avgs");
        
        // If the widget is currently being shown
        std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex); // last_bots/killstats_timer also touched by the CSV watcher thread
        if (ctx->killstats_timer > 0.0f) {
            // If not showing the preview and real data exists, re-export the real data
            if (!ctx->showing_ks_preview && !ctx->last_bots.empty()) {
                const std::string& csv_folder = ctx->kovaaks_stats_folder.empty()
                    ? ctx->kovaaks_save_folder : ctx->kovaaks_stats_folder;
                auto avgs = ComputeAvgs(csv_folder, ctx->last_csv_processed, 10);
                ExportKillStats(ctx, ctx->last_bots, &avgs);
            } else {
                // Otherwise (preview mode or no data yet), export fake stats for the preview
                std::vector<KillStatBot> fake;
                KillStatBot b1; b1.name="fast strafes blink";   b1.shots=1728; b1.hits=1000; b1.accuracy=57.87f; b1.ttk=17.27f; b1.efficiency=57.87f; fake.push_back(b1);
                KillStatBot b2; b2.name="long strafe ufo blink"; b2.shots=1511; b2.hits=1000; b2.accuracy=66.18f; b2.ttk=16.72f; b2.efficiency=66.18f; fake.push_back(b2);
                KillStatBot b3; b3.name="air1_UFO";              b3.shots=1736; b3.hits=1000; b3.accuracy=57.60f; b3.ttk=18.90f; b3.efficiency=57.60f; fake.push_back(b3);
                KillStatBot b4; b4.name="ufo slow";              b4.shots=1609; b4.hits=1000; b4.accuracy=62.15f; b4.ttk=17.56f; b4.efficiency=62.15f; fake.push_back(b4);
                // Fake avgs for preview (slightly lower than current)
                std::map<std::string, BotAvg> fakeAvgs;
                auto makeFakeAvg = [](float acc, float ttk, float eff, int shots, int hits) {
                    BotAvg a; a.accuracy=acc; a.ttk=ttk; a.efficiency=eff; a.shots=shots; a.hits=hits; a.count=10; return a;
                };
                fakeAvgs["fast strafes blink"]    = makeFakeAvg(54.2f, 17.80f, 54.2f, 1780, 965);
                fakeAvgs["long strafe ufo blink"] = makeFakeAvg(63.1f, 17.10f, 63.1f, 1560, 985);
                fakeAvgs["air1_UFO"]              = makeFakeAvg(55.0f, 19.40f, 55.0f, 1800, 990);
                fakeAvgs["ufo slow"]              = makeFakeAvg(60.0f, 17.90f, 60.0f, 1650, 990);
                ExportKillStats(ctx, fake, &fakeAvgs);
            }
        }
        return false;
    };

    obs_properties_t* g_ks = obs_properties_create_param(ctx, nullptr);
    obs_property_t* ks_theme = obs_properties_add_list(g_ks, "ks_theme", "Theme", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(ks_theme, "Amuse Dark",            "dark");
    obs_property_list_add_string(ks_theme, "Glass White",           "glass");
    obs_property_list_add_string(ks_theme, "Neon Cyan",             "neon");
    obs_property_list_add_string(ks_theme, "Midnight Purple",       "purple");
    obs_property_list_add_string(ks_theme, "Solarized",             "solar");
    obs_property_list_add_string(ks_theme, "Retro CRT",             "crt");
    obs_property_list_add_string(ks_theme, "Blood Red",             "red");
    obs_property_list_add_string(ks_theme, "Forest Green",          "green");
    obs_property_list_add_string(ks_theme, "Ocean Deep",            "ocean");
    obs_property_list_add_string(ks_theme, "Gold Luxury",           "gold");
    obs_property_list_add_string(ks_theme, "Bloodbath",             "blood");
    obs_property_list_add_string(ks_theme, "Myspace Emo",           "emo");
    obs_property_list_add_string(ks_theme, "Gothic Graveyard",      "goth");
    obs_property_list_add_string(ks_theme, "Minimalist",            "minimal");
    obs_property_list_add_string(ks_theme, "LG56 (Sci-Fi)",         "lg56");
    obs_property_list_add_string(ks_theme, "THX (Analog VHS)",      "thx");
    obs_property_list_add_string(ks_theme, "Shikuretto (Huntrix)",  "shikuretto");
    obs_property_list_add_string(ks_theme, "Shikuretto (Sukajan)",  "sukajan");
    obs_property_list_add_string(ks_theme, "Shiku (Huntrix Dark)",  "shikuretto-dark");
    obs_property_list_add_string(ks_theme, "Shiku (Sukajan Dark)",  "sukajan-dark");
    obs_property_set_modified_callback(ks_theme, on_ks_changed);
    obs_properties_add_int_slider(g_ks, "ks_opacity",     "Opacity (%)",            10, 100, 1);
    obs_properties_add_int_slider(g_ks, "ks_display_sec", "Display duration (sec)", 1,  60,  1);
    obs_property_t* ks_acc = obs_properties_add_bool(g_ks, "ks_show_accuracy",   "Show Accuracy");
    obs_property_t* ks_ttk = obs_properties_add_bool(g_ks, "ks_show_ttk",        "Show TTK");
    obs_property_t* ks_sh  = obs_properties_add_bool(g_ks, "ks_show_shots_hits", "Show Shots / Hits");
    obs_property_t* ks_eff = obs_properties_add_bool(g_ks, "ks_show_efficiency", "Show Efficiency");
    obs_property_set_modified_callback(ks_acc, on_ks_changed);
    obs_property_set_modified_callback(ks_ttk, on_ks_changed);
    obs_property_set_modified_callback(ks_sh,  on_ks_changed);
    obs_property_set_modified_callback(ks_eff, on_ks_changed);

    // Single avg toggle: compares against last 10 runs
    obs_property_t* ks_avgs = obs_properties_add_bool(g_ks, "ks_show_avgs", "Show averages (last 10 runs)");
    obs_property_set_modified_callback(ks_avgs, on_ks_changed);

    obs_properties_add_button(g_ks, "ks_preview_btn", "Preview (show fake data for positioning)",
        [](obs_properties_t* p, obs_property_t*, void*) -> bool {
            auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(p));
            if (!ctx) return false;
            std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
            ctx->showing_ks_preview = true;
            std::vector<KillStatBot> fake;
            KillStatBot b1; b1.name="fast strafes blink";   b1.shots=1728; b1.hits=1000; b1.accuracy=57.87f; b1.ttk=17.27f; b1.efficiency=57.87f; fake.push_back(b1);
            KillStatBot b2; b2.name="long strafe ufo blink"; b2.shots=1511; b2.hits=1000; b2.accuracy=66.18f; b2.ttk=16.72f; b2.efficiency=66.18f; fake.push_back(b2);
            KillStatBot b3; b3.name="air1_UFO";              b3.shots=1736; b3.hits=1000; b3.accuracy=57.60f; b3.ttk=18.90f; b3.efficiency=57.60f; fake.push_back(b3);
            KillStatBot b4; b4.name="ufo slow";              b4.shots=1609; b4.hits=1000; b4.accuracy=62.15f; b4.ttk=17.56f; b4.efficiency=62.15f; fake.push_back(b4);
            // Fake avgs slightly lower to demonstrate the comparison indicators
            std::map<std::string, BotAvg> fakeAvgs;
            auto makeFakeAvg = [](float acc, float ttk, float eff, int shots, int hits) {
                BotAvg a; a.accuracy=acc; a.ttk=ttk; a.efficiency=eff; a.shots=shots; a.hits=hits; a.count=10; return a;
            };
            fakeAvgs["fast strafes blink"]    = makeFakeAvg(54.2f, 17.80f, 54.2f, 1780, 965);
            fakeAvgs["long strafe ufo blink"] = makeFakeAvg(63.1f, 17.10f, 63.1f, 1560, 985);
            fakeAvgs["air1_UFO"]              = makeFakeAvg(60.0f, 18.40f, 60.0f, 1700, 970);
            fakeAvgs["ufo slow"]              = makeFakeAvg(63.5f, 17.20f, 63.5f, 1580, 985);
            ExportKillStats(ctx, fake, &fakeAvgs);
            ctx->killstats_timer = 3600.0f;
            return false;
        });
    obs_properties_add_button(g_ks, "ks_hide_btn", "Hide Preview",
        [](obs_properties_t* p, obs_property_t*, void*) -> bool {
            auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(p));
            if (!ctx) return false;
            std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
            ctx->showing_ks_preview = false;
            HideKillStats(ctx);
            ctx->killstats_timer = -1.0f;
            return false;
        });
        
    obs_properties_add_button(g_ks, "ks_show_last_btn", "Show Latest Run Stats",
        [](obs_properties_t* p, obs_property_t*, void*) -> bool {
            auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(p));
            if (!ctx) return false;
            std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
            if (ctx->last_bots.empty()) return false;
            ctx->showing_ks_preview = false;
            const std::string& csv_folder = ctx->kovaaks_stats_folder.empty()
                ? ctx->kovaaks_save_folder : ctx->kovaaks_stats_folder;
            auto avgs = ComputeAvgs(csv_folder, ctx->last_csv_processed, 10);
            ExportKillStats(ctx, ctx->last_bots, &avgs);
            ctx->killstats_timer = (ctx->killstats_display_sec > 0) ? (float)ctx->killstats_display_sec : 10.0f;
            return false;
        });

    obs_properties_add_group(props, "grp_killstats", "STATS WIDGET", OBS_GROUP_NORMAL, g_ks);

    obs_properties_t* g_ui = obs_properties_create();
    
    obs_property_t* t = obs_properties_add_list(g_ui, "ui_theme", "Theme", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(t, "Amuse Dark", "dark");
    obs_property_list_add_string(t, "Glass White", "glass");
    obs_property_list_add_string(t, "Neon Cyan", "neon");
    obs_property_list_add_string(t, "Midnight Purple", "purple");
    obs_property_list_add_string(t, "Solarized", "solar");
    obs_property_list_add_string(t, "Retro CRT", "crt");
    obs_property_list_add_string(t, "Blood Red", "red");
    obs_property_list_add_string(t, "Forest Green", "green");
    obs_property_list_add_string(t, "Ocean Deep", "ocean");
    obs_property_list_add_string(t, "Gold Luxury", "gold");
    obs_property_list_add_string(t, "Bloodbath", "blood");
    obs_property_list_add_string(t, "Myspace Emo", "emo");
    obs_property_list_add_string(t, "Gothic Graveyard", "goth");
    obs_property_list_add_string(t, "Minimalist", "minimal");
    obs_property_list_add_string(t, "LG56 (Sci-Fi)", "lg56");
    obs_property_list_add_string(t, "THX (Analog VHS)", "thx");
    obs_property_list_add_string(t, "Shikuretto (Huntrix)", "shikuretto");
    obs_property_list_add_string(t, "Shikuretto (Sukajan)", "sukajan");
    obs_property_list_add_string(t, "Shiku (Huntrix Dark)", "shikuretto-dark");
    obs_property_list_add_string(t, "Shiku (Sukajan Dark)", "sukajan-dark");
    obs_property_list_add_string(t, "Custom CSS Mode", "custom");
    obs_property_set_modified_callback(t, on_ui_theme_changed);

    obs_property_t* tmpl = obs_properties_add_list(g_ui, "load_template", "Load Template into Custom CSS", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(tmpl, "Select Standard Template", "");
    obs_property_list_add_string(tmpl, "Amuse Dark", "dark");
    obs_property_list_add_string(tmpl, "Glass White", "glass");
    obs_property_list_add_string(tmpl, "Neon Cyan", "neon");
    obs_property_list_add_string(tmpl, "Midnight Purple", "purple");
    obs_property_list_add_string(tmpl, "Solarized", "solar");
    obs_property_list_add_string(tmpl, "Retro CRT", "crt");
    obs_property_list_add_string(tmpl, "Blood Red", "red");
    obs_property_list_add_string(tmpl, "Forest Green", "green");
    obs_property_list_add_string(tmpl, "Ocean Deep", "ocean");
    obs_property_list_add_string(tmpl, "Gold Luxury", "gold");
    obs_property_list_add_string(tmpl, "Bloodbath", "blood");
    obs_property_list_add_string(tmpl, "Myspace Emo", "emo");
    obs_property_list_add_string(tmpl, "Gothic Graveyard", "goth");
    obs_property_list_add_string(tmpl, "Minimalist", "minimal");
    obs_property_list_add_string(tmpl, "LG56 (Sci-Fi)", "lg56");
    obs_property_list_add_string(tmpl, "THX (Analog VHS)", "thx");
    obs_property_list_add_string(tmpl, "Shikuretto (Huntrix)", "shikuretto");
    obs_property_list_add_string(tmpl, "Shikuretto (Sukajan)", "sukajan");
    obs_property_list_add_string(tmpl, "Shiku (Huntrix Dark)", "shikuretto-dark");
    obs_property_list_add_string(tmpl, "Shiku (Sukajan Dark)", "sukajan-dark");
    obs_property_set_modified_callback(tmpl, on_template_loaded);
    obs_property_set_visible(tmpl, is_custom);

    obs_property_t* user_themes_prop = obs_properties_add_list(g_ui, "user_theme_select", "Load Saved User Theme", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(user_themes_prop, "Select Saved User Theme", "");
    if (ctx) for (const auto& pair : ctx->user_themes) obs_property_list_add_string(user_themes_prop, pair.first.c_str(), pair.first.c_str());
    obs_property_set_modified_callback(user_themes_prop, on_user_theme_loaded);
    obs_property_set_visible(user_themes_prop, is_custom);
    
    obs_property_t* p_css = obs_properties_add_text(g_ui, "custom_css", "Custom CSS Editor", OBS_TEXT_MULTILINE);
    obs_property_set_visible(p_css, is_custom);

    obs_property_t* p_mname = obs_properties_add_text(g_ui, "theme_manage_name", "New Theme Name (for saving):", OBS_TEXT_DEFAULT);
    obs_property_set_visible(p_mname, is_custom);
    
    obs_property_t* theme_act = obs_properties_add_list(g_ui, "theme_action", "Theme Action:", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(theme_act, "Select Action", "");
    obs_property_list_add_string(theme_act, "Save Current CSS as New Theme", "save");
    obs_property_list_add_string(theme_act, "Update Selected User Theme", "update");
    obs_property_list_add_string(theme_act, "Delete Selected User Theme", "delete");
    obs_property_set_modified_callback(theme_act, on_user_theme_action);
    obs_property_set_visible(theme_act, is_custom);

    obs_properties_add_int_slider(g_ui, "bg_opacity", "Widget Opacity (%)", 0, 100, 1);
    
    obs_property_t* anim = obs_properties_add_list(g_ui, "anim_type", "Rotation Animation", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(anim, "Smooth Fade", "fade");
    obs_property_list_add_string(anim, "Slide Up", "slide-up");
    obs_property_list_add_string(anim, "Slide Down", "slide-down");
    obs_property_list_add_string(anim, "Slide Left", "slide-left");
    obs_property_list_add_string(anim, "Slide Right", "slide-right");
    obs_property_list_add_string(anim, "Zoom In", "zoom-in");
    obs_property_list_add_string(anim, "Zoom Out", "zoom-out");
    obs_property_list_add_string(anim, "3D Flip X (Vertical)", "flip-x");
    obs_property_list_add_string(anim, "3D Flip Y (Horizontal)", "flip-y");
    obs_property_list_add_string(anim, "Cinematic Blur", "blur");

    obs_property_t* align = obs_properties_add_list(g_ui, "alignment", "Widget Anchor", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(align, "Bottom Left (Grows Up & Right)", "bottom-left");
    obs_property_list_add_string(align, "Bottom Right (Grows Up & Left)", "bottom-right");
    obs_property_list_add_string(align, "Top Left (Grows Down & Right)", "top-left");
    obs_property_list_add_string(align, "Top Right (Grows Down & Left)", "top-right");

    obs_property_t* p_autorot = obs_properties_add_bool(g_ui, "auto_rotate", "Enable Auto-Rotation");
    obs_property_set_modified_callback(p_autorot, [](obs_properties_t* props, obs_property_t*, obs_data_t* settings) -> bool {
        bool ar = obs_data_get_bool(settings, "auto_rotate");
        obs_property_set_visible(obs_properties_get(props, "fixed_size"), ar);
        obs_property_set_visible(obs_properties_get(props, "rotation_delay"), ar);
        return true;
    });
    obs_properties_add_int_slider(g_ui, "rotation_delay", "Rotation Delay (seconds)", 1, 60, 1);
    obs_properties_add_bool(g_ui, "fixed_size", "Lock Widget Size (uses largest section)");
    obs_properties_add_bool(g_ui, "show_titles", "Section Titles");
    
    obs_property_t* title_align = obs_properties_add_list(g_ui, "title_alignment", "Title Alignment", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(title_align, "Center", "center");
    obs_property_list_add_string(title_align, "Left", "left");
    obs_property_list_add_string(title_align, "Right", "right");

    obs_properties_add_bool(g_ui, "show_row_labels", "Row Titles");

    obs_property_t* lbl_layout = obs_properties_add_list(g_ui, "label_layout", "Label/Value Layout", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(lbl_layout, "Horizontal  (Label: Value)", "horizontal");
    obs_property_list_add_string(lbl_layout, "Vertical  (Label / Value)", "vertical");

    obs_property_t* cont_align = obs_properties_add_list(g_ui, "content_align", "Items & Labels Alignment", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(cont_align, "Left (default)", "left");
    obs_property_list_add_string(cont_align, "Center", "center");
    obs_property_list_add_string(cont_align, "Right", "right");

    obs_properties_add_group(props, "grp_ui", "UI & SECTIONS SETUP", OBS_GROUP_NORMAL, g_ui);

    obs_properties_t* g_sec_names = obs_properties_create();
    obs_properties_add_text(g_sec_names, "sec_name_1", "Section 1 Name", OBS_TEXT_DEFAULT);
    obs_properties_add_text(g_sec_names, "sec_name_2", "Section 2 Name", OBS_TEXT_DEFAULT);
    obs_properties_add_text(g_sec_names, "sec_name_3", "Section 3 Name", OBS_TEXT_DEFAULT);
    obs_properties_add_text(g_sec_names, "sec_name_4", "Section 4 Name", OBS_TEXT_DEFAULT);
    obs_properties_add_text(g_sec_names, "sec_name_5", "Section 5 Name", OBS_TEXT_DEFAULT);
    
    obs_properties_add_group(g_sec_names, "grp_sep_custominfo", "Custom Info Rows", OBS_GROUP_NORMAL, obs_properties_create());

    for (int i = 0; i < 5; i++) {
        std::string prefix = "ci_" + std::to_string(i) + "_";
        std::string group_id = "grp_ci_" + std::to_string(i);
        std::string label = "Custom Info " + std::to_string(i + 1);

        obs_properties_t* g_ci = obs_properties_create();

        obs_properties_add_bool(g_ci,  (prefix + "visible").c_str(), "Enable");
        obs_property_t* lbl = obs_properties_add_text(g_ci,  (prefix + "label").c_str(),   "Label (e.g. Mouse)",      OBS_TEXT_DEFAULT);
        obs_property_t* val = obs_properties_add_text(g_ci,  (prefix + "value").c_str(),   "Value (e.g. G Pro X)",    OBS_TEXT_DEFAULT);

        obs_property_t* s = obs_properties_add_list(g_ci, (prefix + "sec").c_str(), "Assign to Section",
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(s, "Section 1", 1);
        obs_property_list_add_int(s, "Section 2", 2);
        obs_property_list_add_int(s, "Section 3", 3);
        obs_property_list_add_int(s, "Section 4", 4);
        obs_property_list_add_int(s, "Section 5", 5);

        obs_property_t* p = obs_properties_add_list(g_ci, (prefix + "pos").c_str(), "Layout",
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(p, "Continue Line", "inline");
        obs_property_list_add_string(p, "Start New Line", "newline");

        obs_properties_set_param(g_ci, ctx, nullptr);
        obs_property_set_modified_callback(lbl, on_custom_info_changed);
        obs_property_set_modified_callback(val, on_custom_info_changed);
        obs_property_set_modified_callback(s,   on_custom_info_changed);
        obs_property_set_modified_callback(p,   on_custom_info_changed);

        obs_properties_add_group(g_sec_names, group_id.c_str(), label.c_str(), OBS_GROUP_CHECKABLE, g_ci);
    }

    obs_properties_add_group(props, "grp_sec_names", "CUSTOM SECTION NAMES & INFO", OBS_GROUP_NORMAL, g_sec_names);

    auto add_stat_prop = [](obs_properties_t* g, const char* id, const char* name) {
        std::string v_id = std::string("v_") + id;
        std::string s_id = std::string("s_") + id;
        std::string p_id = std::string("p_") + id;
        
        obs_properties_add_bool(g, v_id.c_str(), name);
        
        obs_property_t* s = obs_properties_add_list(g, s_id.c_str(), "Assign to:", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(s, "Section 1", 1);
        obs_property_list_add_int(s, "Section 2", 2);
        obs_property_list_add_int(s, "Section 3", 3);
        obs_property_list_add_int(s, "Section 4", 4);
        obs_property_list_add_int(s, "Section 5", 5);

        obs_property_t* p = obs_properties_add_list(g, p_id.c_str(), "Layout:", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(p, "Continue Line", "inline");
        obs_property_list_add_string(p, "Start New Line", "newline");
    };

    obs_properties_t* g_sec1 = obs_properties_create();
    add_stat_prop(g_sec1, "dpi", "DPI"); 
    add_stat_prop(g_sec1, "sens", "Sensitivity"); 
    add_stat_prop(g_sec1, "fov", "FOV");
    add_stat_prop(g_sec1, "ktheme", "Kovaaks Theme"); 
    add_stat_prop(g_sec1, "hits", "Hit Sounds"); 
    add_stat_prop(g_sec1, "head", "Head Sound");
    add_stat_prop(g_sec1, "kill", "Kill Sound"); 
    add_stat_prop(g_sec1, "shot", "Shot Sound"); 
    add_stat_prop(g_sec1, "spawn", "Spawn Sound"); 
    add_stat_prop(g_sec1, "cd", "Sound Cooldown");
    obs_properties_add_group(props, "grp_sec1", "CORE & AUDIO", OBS_GROUP_NORMAL, g_sec1);

    obs_properties_t* g_sec2 = obs_properties_create();
    add_stat_prop(g_sec2, "cross", "Crosshair Name"); 
    add_stat_prop(g_sec2, "cross_rgb", "Crosshair RGB"); 
    add_stat_prop(g_sec2, "cross_hex", "Crosshair HEX");
    add_stat_prop(g_sec2, "cross_size", "Crosshair Size"); 
    add_stat_prop(g_sec2, "enemy_col", "Enemy Body Color"); 
    add_stat_prop(g_sec2, "enemy_head_col", "Enemy Head Color");
    add_stat_prop(g_sec2, "enemy_bright", "Enemy Brightness"); 
    add_stat_prop(g_sec2, "enemy_metal", "Enemy Metallic"); 
    add_stat_prop(g_sec2, "enemy_rough", "Enemy Roughness");
    obs_properties_add_group(props, "grp_sec2", "CROSSHAIR & ENEMIES", OBS_GROUP_NORMAL, g_sec2);

    obs_properties_t* g_sec3 = obs_properties_create();
    add_stat_prop(g_sec3, "sky_preset", "Sky Style/Preset"); 
    add_stat_prop(g_sec3, "sky_col", "Sky Color"); 
    add_stat_prop(g_sec3, "sky_clouds", "Cloud Cover");
    add_stat_prop(g_sec3, "sky_solid", "Solid Color Mode"); 
    add_stat_prop(g_sec3, "sky_sun", "Sun Visible");
    
    add_stat_prop(g_sec3, "wall_col", "Wall Color"); 
    add_stat_prop(g_sec3, "wall_mat", "Wall Texture"); 
    add_stat_prop(g_sec3, "wall_bright", "Wall FullBright");
    add_stat_prop(g_sec3, "wall_metal", "Wall Metallic"); 
    add_stat_prop(g_sec3, "wall_rough", "Wall Roughness");
    
    add_stat_prop(g_sec3, "floor_col", "Floor Color"); 
    add_stat_prop(g_sec3, "floor_mat", "Floor Texture"); 
    add_stat_prop(g_sec3, "floor_bright", "Floor FullBright");
    add_stat_prop(g_sec3, "floor_metal", "Floor Metallic"); 
    add_stat_prop(g_sec3, "floor_rough", "Floor Roughness");
    
    add_stat_prop(g_sec3, "ceil_col", "Ceiling Color"); 
    add_stat_prop(g_sec3, "ceil_mat", "Ceiling Texture"); 
    add_stat_prop(g_sec3, "ceil_bright", "Ceiling FullBright");
    add_stat_prop(g_sec3, "ceil_metal", "Ceiling Metallic"); 
    add_stat_prop(g_sec3, "ceil_rough", "Ceiling Roughness");
    
    add_stat_prop(g_sec3, "ramp_col", "Ramp Color"); 
    add_stat_prop(g_sec3, "ramp_mat", "Ramp Texture"); 
    add_stat_prop(g_sec3, "ramp_bright", "Ramp FullBright");
    add_stat_prop(g_sec3, "ramp_metal", "Ramp Metallic"); 
    add_stat_prop(g_sec3, "ramp_rough", "Ramp Roughness");
    obs_properties_add_group(props, "grp_sec3", "ENVIRONMENT VISUALS", OBS_GROUP_NORMAL, g_sec3);

    obs_properties_t* g_credits = obs_properties_create();
    obs_properties_add_button(g_credits, "btn_oni", "Onì (Twitter)", on_oni_clicked);
    obs_properties_add_button(g_credits, "btn_shiku", "ShikuAims (Twitter)", on_shiku_clicked);
    obs_properties_add_group(props, "grp_credits", "Plugin by :", OBS_GROUP_NORMAL, g_credits);

    return props;
}

void kovaaks_video_tick(void* data, float seconds) {
    auto* ctx = static_cast<kovaaks_context*>(data);

    if (ctx->section_dirty) {
        ctx->section_dirty = false;
        ctx->rotation_timer = 0.0f;
        if (!ctx->kovaaks_save_folder.empty() && !ctx->export_folder.empty()) {
            std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex); // ExportToJson reads fields also touched by the watcher threads
            ExportToJson(ctx, false);
        }
        return;
    }

    // KillStats display countdown
    {
        std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex); // killstats_timer is also touched by the CSV watcher thread
        if (ctx->killstats_timer > 0.0f) {
            ctx->killstats_timer -= seconds;
            if (ctx->killstats_timer <= 0.0f) {
                ctx->killstats_timer = -1.0f;
                HideKillStats(ctx);
            }
        }
    }

    ctx->time_elapsed += seconds;

    if (ctx->auto_rotate) {
        ctx->rotation_timer += seconds;
        if (ctx->rotation_timer >= (float)ctx->rotation_delay) {
            ctx->rotation_timer = 0.0f;
            ctx->current_section_idx++;
            ctx->section_dirty = true;
        }
    }

    if (ctx->time_elapsed < 1.0f) return;
    ctx->time_elapsed = 0.0f;

    // Only watch while the game is actually running. Avoids any background
    // disk activity (folder watchers) while KovaaK's isn't even open.
    bool game_running = IsKovaaksRunning();
    if (game_running != ctx->kovaaks_running_last) {
        ctx->kovaaks_running_last = game_running;
        blog(LOG_INFO, "[KovaaksPlugin] KovaaK's %s",
             game_running ? "detected, watching for runs." : "not running, pausing.");
    }

    if (!game_running) {
        StopCsvWatcher();
        StopSettingsWatcher();
        return;
    }

    if (!ctx->kovaaks_save_folder.empty() && !ctx->export_folder.empty()) {
        const std::string& csv_folder = ctx->kovaaks_stats_folder.empty()
            ? ctx->kovaaks_save_folder : ctx->kovaaks_stats_folder;
        StartCsvWatcher(csv_folder);                    // no-op if already watching this folder
        StartSettingsWatcher(ctx->kovaaks_save_folder); // no-op if already watching this folder
    }
}

void* kovaaks_create(obs_data_t* settings, obs_source_t* source) {
    try {
    blog(LOG_INFO, "[KovaaksPlugin] kovaaks_create called.");
    auto* ctx = new kovaaks_context();
    {
        std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
        g_watcher_ctx = ctx;
    }

    // Read the .ini created by the installer once, then delete it
    char dll_path[MAX_PATH] = {};
    HMODULE hmod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&kovaaks_create, &hmod);
    GetModuleFileNameA(hmod, dll_path, MAX_PATH);
    std::string ini_path = std::filesystem::path(dll_path).parent_path().string()
                           + "\\kovaaks_plugin.ini";

    std::ifstream ini_f(ini_path);
    if (ini_f.is_open()) {
        std::string line;
        while (std::getline(ini_f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto set = [&](const char* key, const char* setting) {
                if (line.rfind(key, 0) == 0) {
                    std::string val = line.substr(strlen(key));
                    if (!val.empty()) obs_data_set_string(settings, setting, val.c_str());
                }
            };
            set("kovaaks_save_folder=",  "kovaaks_save_folder");
            set("kovaaks_stats_folder=", "kovaaks_stats_folder");
            set("export_folder=",        "export_folder");
        }
        ini_f.close();
        // Delete the .ini after reading; this only ever runs once
        try {
            std::filesystem::remove(ini_path);
            blog(LOG_INFO, "[KovaaksPlugin] Loaded and removed installer ini: %s", ini_path.c_str());
        } catch (...) {
            blog(LOG_WARNING, "[KovaaksPlugin] Could not remove installer ini (access denied?): %s", ini_path.c_str());
        }    }

    kovaaks_update(ctx, settings);

    ctx->hotkey_next = obs_hotkey_register_source(source, "kovaaks_next_section", "Kovaaks: Next Section", hotkey_next_section, ctx);
    ctx->hotkey_prev = obs_hotkey_register_source(source, "kovaaks_prev_section", "Kovaaks: Previous Section", hotkey_prev_section, ctx);
    ctx->hotkey_show_killstats = obs_hotkey_register_source(source, "kovaaks_show_killstats", "Kovaaks: Show Latest Kill Stats", hotkey_show_killstats, ctx);

    obs_data_array_t* next_array  = obs_data_get_array(settings, "hotkey_next_section");
    obs_data_array_t* prev_array  = obs_data_get_array(settings, "hotkey_prev_section");
    obs_data_array_t* ks_array    = obs_data_get_array(settings, "hotkey_show_killstats");

    if (next_array)  { obs_hotkey_load(ctx->hotkey_next,  next_array);  obs_data_array_release(next_array); }
    if (prev_array)  { obs_hotkey_load(ctx->hotkey_prev,  prev_array);  obs_data_array_release(prev_array); }
    if (ks_array)    { obs_hotkey_load(ctx->hotkey_show_killstats, ks_array); obs_data_array_release(ks_array); }

    return ctx;
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[KovaaksPlugin] kovaaks_create exception: %s", e.what());
        return nullptr;
    } catch (...) {
        blog(LOG_ERROR, "[KovaaksPlugin] kovaaks_create: unknown exception");
        return nullptr;
    }
}

void kovaaks_destroy(void* data) {
    auto* ctx = static_cast<kovaaks_context*>(data);
    StopCsvWatcher();
    StopSettingsWatcher();
    {
        std::lock_guard<std::mutex> lock(g_watcher_ctx_mutex);
        if (g_watcher_ctx == ctx) g_watcher_ctx = nullptr;
    }
    if (ctx->hotkey_next  != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_next);
    if (ctx->hotkey_prev  != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_prev);
    if (ctx->hotkey_show_killstats != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_show_killstats);
    delete ctx;
}

struct obs_source_info kovaaks_info_source = {};
bool obs_module_load(void) {
    blog(LOG_INFO, "[KovaaksPlugin] obs_module_load starting...");
    try {
    kovaaks_info_source.id = "kovaaks_tracker";
    kovaaks_info_source.type = OBS_SOURCE_TYPE_INPUT;
    kovaaks_info_source.output_flags = 0;
    kovaaks_info_source.get_name = [](void*) -> const char* { return "Kovaaks Plugin Settings"; };
    kovaaks_info_source.create = kovaaks_create;
    kovaaks_info_source.destroy = kovaaks_destroy;
    kovaaks_info_source.update = kovaaks_update;
    kovaaks_info_source.get_properties = kovaaks_get_properties;
    kovaaks_info_source.video_tick = kovaaks_video_tick;
    kovaaks_info_source.save = [](void* data, obs_data_t* settings) {
        auto* ctx = static_cast<kovaaks_context*>(data);
        if (ctx->hotkey_next != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_next);
            obs_data_set_array(settings, "hotkey_next_section", arr);
            obs_data_array_release(arr);
        }
        if (ctx->hotkey_prev != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_prev);
            obs_data_set_array(settings, "hotkey_prev_section", arr);
            obs_data_array_release(arr);
        }
        if (ctx->hotkey_show_killstats != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_show_killstats);
            obs_data_set_array(settings, "hotkey_show_killstats", arr);
            obs_data_array_release(arr);
        }
    };
    obs_register_source(&kovaaks_info_source);

    blog(LOG_INFO, "[KovaaksPlugin] Plugin loaded successfully.");
    return true;
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[KovaaksPlugin] obs_module_load exception: %s", e.what());
        return false;
    } catch (...) {
        blog(LOG_ERROR, "[KovaaksPlugin] obs_module_load: unknown exception");
        return false;
    }
}

void obs_module_unload(void) {
    StopCsvWatcher();
    StopSettingsWatcher();
}