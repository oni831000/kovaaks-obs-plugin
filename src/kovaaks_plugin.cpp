#include <obs-module.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#endif

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

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
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("kovaaks_plugin", "en-US")

// KILL STATS — STRUCTURES
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
    obs_hotkey_id hotkey_imgui = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_show_killstats = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_graph          = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_evolution      = OBS_INVALID_HOTKEY_ID;

    // JSON/INI cache — only re-read files every ~1s, not on every hotkey press
    json cached_json = json::object();
    std::map<std::string, std::string> cached_ini;
    bool cache_valid = false;

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

// Global pointer for the ImGui thread
static kovaaks_context* g_imgui_ctx = nullptr;
// Thread-safe toggle triggered by the OBS hotkey
static std::atomic<bool> g_imgui_running{false};
static std::atomic<bool> g_imgui_toggle{false};
static std::atomic<bool> g_graph_toggle{false};
static std::atomic<bool> g_evolution_toggle{false};
static std::thread g_imgui_thread;

static ID3D11Device*            g_pd3dDevice            = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*          g_pSwapChain            = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView  = nullptr;

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

static void CleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain)           { g_pSwapChain->Release();           g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)    { g_pd3dDeviceContext->Release();    g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)           { g_pd3dDevice->Release();           g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// VKey-to-printable-character table for ImGui AddInputCharacter
static void RawKeyToImGui(USHORT vkey, USHORT scanCode, bool isDown) {
    ImGuiIO& io = ImGui::GetIO();

    // Control keys
    ImGuiKey imkey = ImGuiKey_None;
    switch (vkey) {
        case VK_BACK:      imkey = ImGuiKey_Backspace; break;
        case VK_DELETE:    imkey = ImGuiKey_Delete;    break;
        case VK_LEFT:      imkey = ImGuiKey_LeftArrow; break;
        case VK_RIGHT:     imkey = ImGuiKey_RightArrow;break;
        case VK_HOME:      imkey = ImGuiKey_Home;      break;
        case VK_END:       imkey = ImGuiKey_End;       break;
        case VK_RETURN:    imkey = ImGuiKey_Enter;     break;
        case VK_ESCAPE:    imkey = ImGuiKey_Escape;    break;
        case VK_TAB:       imkey = ImGuiKey_Tab;       break;
        case VK_CONTROL:   imkey = ImGuiKey_LeftCtrl;  break;
        case VK_SHIFT:     imkey = ImGuiKey_LeftShift; break;
        case VK_MENU:      imkey = ImGuiKey_LeftAlt;   break;
        case VK_UP:        imkey = ImGuiKey_UpArrow;   break;
        case VK_DOWN:      imkey = ImGuiKey_DownArrow; break;
        case 'A': imkey = ImGuiKey_A; break;
        case 'C': imkey = ImGuiKey_C; break;
        case 'V': imkey = ImGuiKey_V; break;
        case 'X': imkey = ImGuiKey_X; break;
        case 'Z': imkey = ImGuiKey_Z; break;
    }
    if (imkey != ImGuiKey_None) io.AddKeyEvent(imkey, isDown);

    // Printable characters (key down only)
    if (isDown && vkey != VK_BACK && vkey != VK_DELETE &&
        vkey != VK_RETURN && vkey != VK_ESCAPE && vkey != VK_TAB) {
        BYTE keyState[256] = {};
        GetKeyboardState(keyState);
        WCHAR buf[4] = {};
        int result = ToUnicode(vkey, scanCode, keyState, buf, 4, 0);
        if (result == 1 && buf[0] >= 32) io.AddInputCharacterUTF16(buf[0]);
    }
}

static LRESULT WINAPI ImGuiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_INPUT: {
            // Raw keyboard input — works without window focus
            UINT size = 0;
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size > 0) {
                std::vector<BYTE> buf(size);
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
                    RAWINPUT* raw = (RAWINPUT*)buf.data();
                    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                        USHORT vkey     = raw->data.keyboard.VKey;
                        USHORT scanCode = raw->data.keyboard.MakeCode;
                        bool   isDown   = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                        RawKeyToImGui(vkey, scanCode, isDown);
                    }
                }
            }
            return 0;
        }
        case WM_SIZE:
            if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
                if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
                g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                ID3D11Texture2D* pBB;
                g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBB));
                g_pd3dDevice->CreateRenderTargetView(pBB, nullptr, &g_mainRenderTargetView);
                pBB->Release();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

// KILL STATS VIEWER — data structures

// BotAvg defined here for g_graph_avgs (duplicate identical struct removed further down)
struct BotAvg {
    float accuracy   = 0.0f;
    float ttk        = 0.0f;
    float efficiency = 0.0f;
    int   shots      = 0;
    int   hits       = 0;
    int   count      = 0;
};
static std::map<std::string, BotAvg> ComputeAvgs(const std::string& folder, const std::string& exclude_path, int maxRuns);

struct GraphRunPoint {
    float value = 0.0f;
};

struct GraphBotSeries {
    std::string                bot_name;
    std::vector<GraphRunPoint> accuracy;
    std::vector<GraphRunPoint> ttk;
    std::vector<GraphRunPoint> efficiency;
    std::vector<GraphRunPoint> shots;
    std::vector<GraphRunPoint> hits;
};

static std::vector<GraphBotSeries>   g_graph_data;
static std::map<std::string, BotAvg> g_graph_avgs;
static std::mutex                    g_graph_mutex;
static bool                          g_graph_data_dirty = false; // loaded on demand, not at startup
static std::atomic<bool>             g_graph_loading{false};
static std::atomic<bool>             g_graph_new_csv{false};   // signaled by video_tick to trigger a reload

static std::vector<KillStatBot> ParseKillStatsCsv(const std::string& path);
static std::string ExtractScenarioName(const std::string& path);

// A real gauntlet run has at least one bot killed more than once (multiple kills on the same name).
// Target-switching scenarios have exactly 1 kill per bot, so they're excluded.
static bool IsGauntletRun(const std::vector<KillStatBot>& bots) {
    if (bots.size() <= 1) return false;
    std::map<std::string, int> counts;
    for (auto& b : bots) counts[b.name]++;
    for (auto& kv : counts) if (kv.second > 8) return false;
    return true;
}

static void LoadGraphData(const std::string& csv_folder, int maxRuns) {
    if (csv_folder.empty()) return;
    try {
        if (!std::filesystem::exists(csv_folder)) return;
    } catch (...) { return; }
    struct CsvEntry { std::string path; std::filesystem::file_time_type mtime; };
    std::vector<CsvEntry> entries;
    try {
        for (auto& entry : std::filesystem::directory_iterator(csv_folder)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            if (fname.find(".csv") == std::string::npos) continue;
            if (fname.find("Challenge") == std::string::npos) continue;
            entries.push_back({entry.path().string(), entry.last_write_time()});
        }
    } catch (...) { return; }
    std::sort(entries.begin(), entries.end(), [](const CsvEntry& a, const CsvEntry& b){ return a.mtime < b.mtime; });

    try {
        // Find the most recent gauntlet run and extract its scenario
        std::string ref_scenario;
        std::string ref_path;
        for (int i = (int)entries.size()-1; i >= 0; i--) {
            auto bots = ParseKillStatsCsv(entries[i].path);
            if (IsGauntletRun(bots)) {
                ref_path     = entries[i].path;
                ref_scenario = ExtractScenarioName(ref_path);
                blog(LOG_INFO, "[KovaaksPlugin] LoadGraphData: ref scenario = '%s'", ref_scenario.c_str());
                break;
            }
        }
        if (ref_path.empty()) {
            std::lock_guard<std::mutex> lock(g_graph_mutex);
            g_graph_data.clear();
            g_graph_avgs.clear();
            return;
        }

        // Filter: only gauntlet runs from the same scenario
        std::vector<CsvEntry> filtered;
        for (auto& e : entries) {
            auto bots = ParseKillStatsCsv(e.path);
            if (!IsGauntletRun(bots)) continue;
            if (!ref_scenario.empty() && ExtractScenarioName(e.path) != ref_scenario) continue;
            filtered.push_back(e);
        }
        if ((int)filtered.size() > maxRuns)
            filtered.erase(filtered.begin(), filtered.end() - maxRuns);

        std::map<std::string, GraphBotSeries> series_map;
        blog(LOG_INFO, "[KovaaksPlugin] LoadGraphData: %d filtered runs for scenario '%s'",
             (int)filtered.size(), ref_scenario.c_str());
        for (auto& e : filtered) {
            auto bots = ParseKillStatsCsv(e.path);
            for (auto& b : bots) {
                auto& s    = series_map[b.key];
                s.bot_name = b.name;
                s.accuracy.push_back({b.accuracy});
                s.ttk.push_back({b.ttk});
                s.efficiency.push_back({b.efficiency});
                s.shots.push_back({(float)b.shots});
                s.hits.push_back({(float)b.hits});
            }
        }

        std::vector<GraphBotSeries>   new_data;
        std::map<std::string, BotAvg> new_avgs;

        auto last_bots = ParseKillStatsCsv(ref_path);
        for (auto& b : last_bots) {
            auto it = series_map.find(b.key);
            if (it != series_map.end())
                new_data.push_back(it->second);
        }
        new_avgs = ComputeAvgs(csv_folder, ref_path, maxRuns);

        {
            std::lock_guard<std::mutex> lock(g_graph_mutex);
            g_graph_data = std::move(new_data);
            g_graph_avgs = std::move(new_avgs);
        }
    } catch (...) {
        blog(LOG_WARNING, "[KovaaksPlugin] LoadGraphData: exception caught, skipping.");
    }
}

static void ImGuiMenuThread() {
    try {
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, ImGuiWndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "KovaaksImGuiClass", nullptr };
    ::RegisterClassExA(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = ::CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName, "Kovaaks Custom Info",
        WS_POPUP, 0, 0, screenW, screenH,
        nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    // Register raw keyboard input — RIDEV_INPUTSINK receives input even without focus
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; // Generic Desktop
    rid.usUsage     = 0x06; // Keyboard
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding  = 8.0f;
    style.FrameRounding   = 4.0f;
    style.ItemSpacing     = ImVec2(8, 6);
    style.WindowPadding   = ImVec2(14, 14);
    style.Colors[ImGuiCol_WindowBg]       = ImVec4(0.10f, 0.10f, 0.13f, 0.95f);
    style.Colors[ImGuiCol_FrameBg]        = ImVec4(0.18f, 0.18f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.28f, 0.38f, 1.0f);
    style.Colors[ImGuiCol_Button]         = ImVec4(0.25f, 0.40f, 0.65f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.35f, 0.50f, 0.75f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.15f, 0.25f, 0.45f, 1.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool show_menu      = false;
    bool show_graph     = false;
    bool show_evolution = false;
    bool was_visible    = false;

    // Thread-local input buffers, reloaded from ctx each time the menu opens
    char lbl_buf[5][128] = {};
    char val_buf[5][128] = {};
    int  sec_buf[5]      = { 0, 0, 0, 0, 0 }; // 0-indexed
    bool vis_buf[5]      = { false, false, false, false, false };
    int  pos_buf[5]      = { 0, 0, 0, 0, 0 }; // 0=inline, 1=newline
    bool buffers_loaded  = false;

    while (g_imgui_running) {
        auto frame_start = std::chrono::steady_clock::now();

        // Process Win32 messages FIRST to drain the queue
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_imgui_running = false;
        }
        if (!g_imgui_running) break;

        // Read hotkey toggles (atomic, thread-safe)
        bool expected = true;
        if (g_imgui_toggle.compare_exchange_strong(expected, false)) {
            show_menu = !show_menu;
            if (show_menu) buffers_loaded = false;
        }

        bool expected2 = true;
        if (g_graph_toggle.compare_exchange_strong(expected2, false)) {
            show_graph = !show_graph;
            if (show_graph) {
                show_evolution = false; // close the other panel
                std::lock_guard<std::mutex> lock(g_graph_mutex);
                if (g_graph_data.empty()) g_graph_data_dirty = true;
            }
        }

        bool expected3 = true;
        if (g_evolution_toggle.compare_exchange_strong(expected3, false)) {
            show_evolution = !show_evolution;
            if (show_evolution) {
                show_graph = false; // close the other panel
                std::lock_guard<std::mutex> lock(g_graph_mutex);
                if (g_graph_data.empty()) g_graph_data_dirty = true;
            }
        }

        bool any_visible = show_menu || show_graph || show_evolution;
        if (any_visible && !was_visible) {
            ShowWindow(hwnd, SW_SHOWNA);
            SetWindowLongPtrA(hwnd, GWL_EXSTYLE,
                GetWindowLongPtrA(hwnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
            was_visible = true;
        } else if (!any_visible && was_visible) {
            ShowWindow(hwnd, SW_HIDE);
            SetWindowLongPtrA(hwnd, GWL_EXSTYLE,
                GetWindowLongPtrA(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
            was_visible = false;
        }

        // Preload graph data in the background, even while menus are closed
        {
            bool need_load = g_graph_data_dirty;
            if (!need_load) {
                bool new_csv = true;
                need_load = g_graph_new_csv.compare_exchange_strong(new_csv, false);
            }
            if (need_load && !g_graph_loading && g_imgui_ctx) {
                std::string csv_folder = g_imgui_ctx->kovaaks_stats_folder.empty()
                    ? g_imgui_ctx->kovaaks_save_folder : g_imgui_ctx->kovaaks_stats_folder;
                if (!csv_folder.empty()) {
                    g_graph_data_dirty = false;
                    g_graph_loading    = true;
                    std::thread([csv_folder]() {
                        try {
                            LoadGraphData(csv_folder, 10);
                        } catch (...) {
                            blog(LOG_WARNING, "[KovaaksPlugin] LoadGraphData thread: exception caught.");
                        }
                        g_graph_loading = false;
                    }).detach();
                }
            }
        }

        if (any_visible) {
            // Load values from ctx when the menu opens
            if (show_menu && !buffers_loaded && g_imgui_ctx) {
                for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
                    strncpy_s(lbl_buf[i], sizeof(lbl_buf[i]), g_imgui_ctx->custom_info[i].label.c_str(), _TRUNCATE);
                    strncpy_s(val_buf[i], sizeof(val_buf[i]), g_imgui_ctx->custom_info[i].value.c_str(), _TRUNCATE);
                    sec_buf[i] = g_imgui_ctx->custom_info[i].sec_assignment - 1;
                    vis_buf[i] = g_imgui_ctx->custom_info[i].visible;
                    pos_buf[i] = (g_imgui_ctx->custom_info[i].pos == "newline") ? 1 : 0;
                }
                buffers_loaded = true;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImVec2 win_size(620, 560);

            if (show_menu) {
                ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);
                ImGui::SetNextWindowPos(
                    ImVec2((screenW - win_size.x) * 0.5f, (screenH - win_size.y) * 0.5f),
                    ImGuiCond_Always);

                bool p_open = true;
                ImGui::Begin("Kovaaks Plugin Settings", &p_open,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse);

            if (!g_imgui_ctx) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Waiting for OBS source...");
                ImGui::TextDisabled("Please add a Kovaaks Tracker source in OBS.");
            } else {
                if (ImGui::BeginTabBar("MainTabs")) {

                    // TAB 1: UI SETTINGS
                    if (ImGui::BeginTabItem("UI Settings")) {
                        ImGui::Spacing();

                        // Theme
                        const char* themes[] = {
                            "Amuse Dark","Glass White","Neon Cyan","Midnight Purple",
                            "Solarized","Retro CRT","Blood Red","Forest Green",
                            "Ocean Deep","Gold Luxury","Bloodbath","Myspace Emo",
                            "Gothic Graveyard","Minimalist","LG56 (Sci-Fi)","THX (Analog VHS)",
                            "Shikuretto (Huntrix)","Shikuretto (Sukajan)",
                            "Shiku (Huntrix Dark)","Shiku (Sukajan Dark)","Custom CSS"
                        };
                        const char* theme_ids[] = {
                            "dark","glass","neon","purple","solar","crt","red","green",
                            "ocean","gold","blood","emo","goth","minimal","lg56","thx",
                            "shikuretto","sukajan","shikuretto-dark","sukajan-dark","custom"
                        };
                        static int theme_idx = 0;
                        // Sync from ctx on open
                        if (!buffers_loaded) {
                            for (int t = 0; t < 21; t++)
                                if (g_imgui_ctx->ui_theme == theme_ids[t]) { theme_idx = t; break; }
                        }
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::Combo("Theme", &theme_idx, themes, 21)) {
                            g_imgui_ctx->ui_theme = theme_ids[theme_idx];
                            g_imgui_ctx->section_dirty = true;
                        }

                        // Opacity
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::SliderInt("Opacity (%)", &g_imgui_ctx->bg_opacity, 0, 100))
                            g_imgui_ctx->section_dirty = true;

                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                        // Animation
                        const char* anims[] = {
                            "Smooth Fade","Slide Up","Slide Down","Slide Left","Slide Right",
                            "Zoom In","Zoom Out","3D Flip X","3D Flip Y","Cinematic Blur"
                        };
                        const char* anim_ids[] = {
                            "fade","slide-up","slide-down","slide-left","slide-right",
                            "zoom-in","zoom-out","flip-x","flip-y","blur"
                        };
                        static int anim_idx = 1;
                        if (!buffers_loaded)
                            for (int a = 0; a < 10; a++)
                                if (g_imgui_ctx->anim_type == anim_ids[a]) { anim_idx = a; break; }
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::Combo("Animation", &anim_idx, anims, 10)) {
                            g_imgui_ctx->anim_type = anim_ids[anim_idx];
                            g_imgui_ctx->section_dirty = true;
                        }

                        // Alignment
                        const char* aligns[] = {
                            "Bottom Left","Bottom Right","Top Left","Top Right"
                        };
                        const char* align_ids[] = {
                            "bottom-left","bottom-right","top-left","top-right"
                        };
                        static int align_idx = 0;
                        if (!buffers_loaded)
                            for (int a = 0; a < 4; a++)
                                if (g_imgui_ctx->alignment == align_ids[a]) { align_idx = a; break; }
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::Combo("Anchor", &align_idx, aligns, 4)) {
                            g_imgui_ctx->alignment = align_ids[align_idx];
                            g_imgui_ctx->section_dirty = true;
                        }

                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                        // Checkboxes
                        if (ImGui::Checkbox("Show Section Titles",  &g_imgui_ctx->show_titles))     g_imgui_ctx->section_dirty = true;
                        if (ImGui::Checkbox("Show Row Labels",      &g_imgui_ctx->show_row_labels)) g_imgui_ctx->section_dirty = true;
                        if (ImGui::Checkbox("Lock Widget Size",     &g_imgui_ctx->fixed_size))      g_imgui_ctx->section_dirty = true;

                        ImGui::Spacing();
                        if (ImGui::Checkbox("Auto-Rotation", &g_imgui_ctx->auto_rotate)) {
                            g_imgui_ctx->manual_mode = false;
                            g_imgui_ctx->current_section_idx = 0;
                            g_imgui_ctx->rotation_timer = 0.0f;
                            g_imgui_ctx->section_dirty = true;
                        }
                        if (g_imgui_ctx->auto_rotate) {
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderInt("Delay (s)", &g_imgui_ctx->rotation_delay, 1, 60))
                                g_imgui_ctx->section_dirty = true;
                        }

                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                        // Label layout
                        const char* lbl_layouts[] = { "Horizontal (Label: Value)", "Vertical (Label / Value)" };
                        const char* lbl_ids[]     = { "horizontal", "vertical" };
                        static int lbl_idx = 0;
                        if (!buffers_loaded)
                            for (int l = 0; l < 2; l++)
                                if (g_imgui_ctx->label_layout == lbl_ids[l]) { lbl_idx = l; break; }
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::Combo("Label Layout", &lbl_idx, lbl_layouts, 2)) {
                            g_imgui_ctx->label_layout = lbl_ids[lbl_idx];
                            g_imgui_ctx->section_dirty = true;
                        }

                        // Content align
                        const char* cont_aligns[] = { "Left", "Center", "Right" };
                        const char* cont_ids[]    = { "left", "center", "right" };
                        static int cont_idx = 0;
                        if (!buffers_loaded)
                            for (int c = 0; c < 3; c++)
                                if (g_imgui_ctx->content_align == cont_ids[c]) { cont_idx = c; break; }
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::Combo("Content Align", &cont_idx, cont_aligns, 3)) {
                            g_imgui_ctx->content_align = cont_ids[cont_idx];
                            g_imgui_ctx->section_dirty = true;
                        }

                        // Title align
                        const char* title_aligns[] = { "Center", "Left", "Right" };
                        const char* title_ids[]    = { "center", "left", "right" };
                        static int title_idx = 0;
                        if (!buffers_loaded)
                            for (int c = 0; c < 3; c++)
                                if (g_imgui_ctx->title_alignment == title_ids[c]) { title_idx = c; break; }
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::Combo("Title Align", &title_idx, title_aligns, 3)) {
                            g_imgui_ctx->title_alignment = title_ids[title_idx];
                            g_imgui_ctx->section_dirty = true;
                        }

                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                        ImGui::TextDisabled("Section Names");
                        static char sn_buf[5][64] = {};
                        if (!buffers_loaded)
                            for (int s = 0; s < 5; s++)
                                strncpy_s(sn_buf[s], sizeof(sn_buf[s]), g_imgui_ctx->sec_names[s].c_str(), _TRUNCATE);
                        for (int s = 0; s < 5; s++) {
                            ImGui::PushID(s);
                            char lbl[32]; snprintf(lbl, sizeof(lbl), "Section %d", s+1);
                            ImGui::SetNextItemWidth(220);
                            if (ImGui::InputText(lbl, sn_buf[s], sizeof(sn_buf[s]))) {
                                g_imgui_ctx->sec_names[s] = sn_buf[s];
                                g_imgui_ctx->section_dirty = true;
                            }
                            ImGui::PopID();
                        }

                        ImGui::EndTabItem();
                    }

                    // TAB 2: STATS VISIBILITY
                    if (ImGui::BeginTabItem("Stats Visibility")) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Toggle stats and assign them to a section.");
                        ImGui::Spacing();

                        const char* sec_labels[] = { "S1","S2","S3","S4","S5" };

                        struct StatEntry { const char* id; const char* label; };
                        static const StatEntry stats[] = {
                            {"dpi","DPI"}, {"sens","Sensitivity"}, {"fov","FOV"},
                            {"ktheme","Kovaaks Theme"},
                            {"hits","Hit Sounds"}, {"head","Head Sound"},
                            {"kill","Kill Sound"}, {"shot","Shot Sound"},
                            {"spawn","Spawn Sound"}, {"cd","Sound Cooldown"},
                            {"cross","Crosshair Name"}, {"cross_rgb","Crosshair RGB"},
                            {"cross_hex","Crosshair HEX"}, {"cross_size","Crosshair Size"},
                            {"enemy_col","Enemy Body Color"}, {"enemy_head_col","Enemy Head Color"},
                            {"enemy_bright","Enemy Brightness"}, {"enemy_metal","Enemy Metallic"},
                            {"enemy_rough","Enemy Roughness"},
                            {"sky_preset","Sky Preset"}, {"sky_col","Sky Color"},
                            {"sky_clouds","Cloud Cover"}, {"sky_solid","Solid Color Sky"},
                            {"sky_sun","Show Sun"},
                            {"wall_col","Wall Color"}, {"wall_mat","Wall Texture"},
                            {"wall_bright","Wall FullBright"}, {"wall_metal","Wall Metallic"},
                            {"wall_rough","Wall Roughness"},
                            {"floor_col","Floor Color"}, {"floor_mat","Floor Texture"},
                            {"floor_bright","Floor FullBright"}, {"floor_metal","Floor Metallic"},
                            {"floor_rough","Floor Roughness"},
                            {"ceil_col","Ceiling Color"}, {"ceil_mat","Ceiling Texture"},
                            {"ceil_bright","Ceil FullBright"}, {"ceil_metal","Ceil Metallic"},
                            {"ceil_rough","Ceil Roughness"},
                            {"ramp_col","Ramp Color"}, {"ramp_mat","Ramp Texture"},
                            {"ramp_bright","Ramp FullBright"}, {"ramp_metal","Ramp Metallic"},
                            {"ramp_rough","Ramp Roughness"},
                        };
                        constexpr int STAT_COUNT = sizeof(stats) / sizeof(stats[0]);

                        ImGui::BeginChild("stats_scroll", ImVec2(0, 380), true);
                        for (int i = 0; i < STAT_COUNT; i++) {
                            ImGui::PushID(i);
                            std::string id = stats[i].id;
                            bool vis = g_imgui_ctx->visible.count(id) ? g_imgui_ctx->visible[id] : false;
                            if (ImGui::Checkbox(stats[i].label, &vis)) {
                                g_imgui_ctx->visible[id] = vis;
                                g_imgui_ctx->section_dirty = true;
                            }
                            if (vis) {
                                ImGui::SameLine(200);
                                int sec = g_imgui_ctx->sec_assignment.count(id) ? g_imgui_ctx->sec_assignment[id] - 1 : 0;
                                ImGui::SetNextItemWidth(80);
                                if (ImGui::Combo("##sec", &sec, sec_labels, 5)) {
                                    g_imgui_ctx->sec_assignment[id] = sec + 1;
                                    g_imgui_ctx->section_dirty = true;
                                }
                                ImGui::SameLine();
                                const char* pos_labels[] = { "inline", "newline" };
                                std::string cur_pos = g_imgui_ctx->pos.count(id) ? g_imgui_ctx->pos[id] : "inline";
                                int pos_idx = (cur_pos == "newline") ? 1 : 0;
                                ImGui::SetNextItemWidth(90);
                                if (ImGui::Combo("##pos", &pos_idx, pos_labels, 2)) {
                                    g_imgui_ctx->pos[id] = pos_labels[pos_idx];
                                    g_imgui_ctx->section_dirty = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }

                    // TAB 3: CUSTOM INFO
                    if (ImGui::BeginTabItem("Custom Info")) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Manual entries (e.g. OW sensitivity, monitor, mouse...)");
                        ImGui::Spacing();

                        const char* sec_labels[] = { "Section 1","Section 2","Section 3","Section 4","Section 5" };
                        const char* pos_labels[] = { "Continue line (inline)", "New line (newline)" };

                        for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
                            ImGui::PushID(i);
                            char header[32];
                            snprintf(header, sizeof(header), "Custom Info %d", i + 1);
                            if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
                                ImGui::Indent(10.0f);
                                ImGui::Checkbox("Enabled", &vis_buf[i]);
                                ImGui::SetNextItemWidth(190);
                                ImGui::InputText("Label", lbl_buf[i], sizeof(lbl_buf[i]));
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(190);
                                ImGui::InputText("Value", val_buf[i], sizeof(val_buf[i]));
                                ImGui::SetNextItemWidth(130);
                                ImGui::Combo("Section", &sec_buf[i], sec_labels, 5);
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200);
                                ImGui::Combo("Layout", &pos_buf[i], pos_labels, 2);
                                ImGui::Unindent(10.0f);
                            }
                            ImGui::PopID();
                            ImGui::Spacing();
                        }

                        ImGui::Separator();
                        ImGui::Spacing();
                        float btn_w = 130.0f;
                        ImGui::SetCursorPosX((win_size.x - btn_w) * 0.5f);
                        if (ImGui::Button("Apply Custom Info", ImVec2(btn_w, 28))) {
                            for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
                                g_imgui_ctx->custom_info[i].label          = lbl_buf[i];
                                g_imgui_ctx->custom_info[i].value          = val_buf[i];
                                g_imgui_ctx->custom_info[i].sec_assignment = sec_buf[i] + 1;
                                g_imgui_ctx->custom_info[i].visible        = vis_buf[i];
                                g_imgui_ctx->custom_info[i].pos            = (pos_buf[i] == 1) ? "newline" : "inline";
                            }
                            g_imgui_ctx->section_dirty = true;
                        }
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                // Global close button
                ImGui::Spacing();
                float close_w = 80.0f;
                ImGui::SetCursorPosX((win_size.x - close_w) * 0.5f);
                if (ImGui::Button("Close", ImVec2(close_w, 26))) {
                    // Save custom info before closing
                    for (int i = 0; i < kovaaks_context::CUSTOM_INFO_COUNT; i++) {
                        g_imgui_ctx->custom_info[i].label          = lbl_buf[i];
                        g_imgui_ctx->custom_info[i].value          = val_buf[i];
                        g_imgui_ctx->custom_info[i].sec_assignment = sec_buf[i] + 1;
                        g_imgui_ctx->custom_info[i].visible        = vis_buf[i];
                        g_imgui_ctx->custom_info[i].pos            = (pos_buf[i] == 1) ? "newline" : "inline";
                    }
                    g_imgui_ctx->section_dirty = true;
                    show_menu = false;
                }
            }


                ImGui::End(); // end Kovaaks Plugin Settings
            } // end if (show_menu)

            // KILL STATS VIEWER (mirrors the HTML widget)
            if (show_graph && g_imgui_ctx) {
                bool showAcc = g_imgui_ctx->ks_show_accuracy;
                bool showTTK = g_imgui_ctx->ks_show_ttk;
                bool showSH  = g_imgui_ctx->ks_show_shots_hits;
                bool showEff = g_imgui_ctx->ks_show_efficiency;
                bool showAvg = g_imgui_ctx->ks_show_avgs;

                // Thread-safe snapshot of the data
                std::vector<GraphBotSeries>   snap_data;
                std::map<std::string, BotAvg> snap_avgs;
                bool is_loading = g_graph_loading.load();
                if (!is_loading) {
                    std::lock_guard<std::mutex> lock(g_graph_mutex);
                    snap_data = g_graph_data;
                    snap_avgs = g_graph_avgs;
                }

                // Fixed column widths
                const float cBot = 160.0f;
                const float cAcc = 150.0f;
                const float cTTK = 140.0f;
                const float cSH  = 150.0f;
                const float cEff = 140.0f;
                const float bar_h = 4.0f;

                int  bot_count = (int)snap_data.size();
                float win_w    = cBot
                    + (showAcc ? cAcc : 0.0f)
                    + (showTTK ? cTTK : 0.0f)
                    + (showSH  ? cSH  : 0.0f)
                    + (showEff ? cEff : 0.0f)
                    + 24.0f;
                if (win_w < 300.0f) win_w = 300.0f;

                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(win_w, 60.0f),
                    ImVec2(win_w, screenH * 0.90f));
                ImGui::SetNextWindowPos(
                    ImVec2(screenW * 0.5f, screenH * 0.5f),
                    ImGuiCond_Always,
                    ImVec2(0.5f, 0.5f));

                bool g_open = true;
                ImGui::Begin("Kill Stats", &g_open,
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoMove     |
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_AlwaysAutoResize);

                if (is_loading) {
                    ImGui::TextDisabled("Loading stats...");
                } else if (snap_data.empty()) {
                    ImGui::TextDisabled("No multi-bot run found.");
                    ImGui::TextDisabled("Run a tracking gauntlet scenario first.");
                } else {
                    auto deltaColor = [](float delta, bool higherBetter) -> ImVec4 {
                        bool better = higherBetter ? delta > 0.005f : delta < -0.005f;
                        return better ? ImVec4(0.3f,1.0f,0.44f,1.0f) : ImVec4(1.0f,0.36f,0.36f,1.0f);
                    };
                    auto accColor = [](float acc) -> ImVec4 {
                        float r = (acc < 50.0f) ? 1.0f : (1.0f - (acc - 50.0f) / 50.0f);
                        float g = (acc > 50.0f) ? 1.0f : (acc / 50.0f);
                        return ImVec4(r, g, 0.2f, 1.0f);
                    };
                    auto toU32 = [](ImVec4 c) -> ImU32 {
                        return IM_COL32((int)(c.x*255),(int)(c.y*255),(int)(c.z*255),(int)(c.w*255));
                    };

                    int ncols = 1 + (showAcc?1:0) + (showTTK?1:0) + (showSH?1:0) + (showEff?1:0);

                    ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
                    if (ImGui::BeginTable("ks_table", ncols, tflags)) {
                        ImGui::TableSetupColumn("Bot",        ImGuiTableColumnFlags_WidthFixed, cBot);
                        if (showAcc) ImGui::TableSetupColumn("Acc %",      ImGuiTableColumnFlags_WidthFixed, cAcc);
                        if (showTTK) ImGui::TableSetupColumn("TTK",        ImGuiTableColumnFlags_WidthFixed, cTTK);
                        if (showSH)  ImGui::TableSetupColumn("Hits/Shots", ImGuiTableColumnFlags_WidthFixed, cSH);
                        if (showEff) ImGui::TableSetupColumn("Eff %",      ImGuiTableColumnFlags_WidthFixed, cEff);
                        ImGui::TableHeadersRow();

                        ImDrawList* dl = ImGui::GetWindowDrawList();

                        for (auto& s : snap_data) {
                            float acc   = s.accuracy.empty()   ? 0.0f : s.accuracy.back().value;
                            float ttk   = s.ttk.empty()        ? 0.0f : s.ttk.back().value;
                            float eff   = s.efficiency.empty() ? 0.0f : s.efficiency.back().value;
                            int   shots = s.shots.empty() ? 0 : (int)s.shots.back().value;
                            int   hits  = s.hits.empty()  ? 0 : (int)s.hits.back().value;

                            float avg_acc=0, avg_ttk=0, avg_eff=0, avg_hits=0, avg_shots=0;
                            bool hasAvg = false;
                            int gd_idx = (int)(&s - snap_data.data());
                            std::string bot_key = "bot_" + std::to_string(gd_idx);
                            auto it = snap_avgs.find(bot_key);
                            if (it != snap_avgs.end() && it->second.count > 0) {
                                hasAvg    = true;
                                avg_acc   = it->second.accuracy;
                                avg_ttk   = it->second.ttk;
                                avg_eff   = it->second.efficiency;
                                avg_hits  = (float)it->second.hits;
                                avg_shots = (float)it->second.shots;
                            }

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            // Vertical padding to center against row height
                            ImGui::Dummy(ImVec2(0, 2));
                            ImGui::TextUnformatted(s.bot_name.c_str());

                            int col = 1;

                            // Accuracy
                            if (showAcc) {
                                ImGui::TableSetColumnIndex(col++);
                                ImGui::Dummy(ImVec2(0, 2));
                                ImVec4 ac = accColor(acc);
                                ImGui::TextColored(ac, "%.1f%%", acc);
                                // Accuracy bar
                                ImVec2 bar_pos = ImGui::GetCursorScreenPos();
                                float bar_w = cAcc - 16.0f;
                                dl->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                                    IM_COL32(60,60,60,180), 2.0f);
                                dl->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w * (acc / 100.0f), bar_pos.y + bar_h),
                                    toU32(ac), 2.0f);
                                ImGui::Dummy(ImVec2(bar_w, bar_h + 2.0f));
                                if (showAvg && hasAvg) {
                                    float d = acc - avg_acc;
                                    char buf[48]; snprintf(buf, sizeof(buf), "%+.1f / avg %.1f", d, avg_acc);
                                    ImGui::TextColored(deltaColor(d, true), "%s", buf);
                                }
                            }

                            // TTK
                            if (showTTK) {
                                ImGui::TableSetColumnIndex(col++);
                                ImGui::Dummy(ImVec2(0, 2));
                                ImGui::Text("%.2fs", ttk);
                                ImGui::Dummy(ImVec2(0, bar_h + 2.0f)); // align with acc bar
                                if (showAvg && hasAvg) {
                                    float d = ttk - avg_ttk;
                                    char buf[48]; snprintf(buf, sizeof(buf), "%+.2f / avg %.2f", d, avg_ttk);
                                    ImGui::TextColored(deltaColor(d, false), "%s", buf);
                                }
                            }

                            // Hits/Shots
                            if (showSH) {
                                ImGui::TableSetColumnIndex(col++);
                                ImGui::Dummy(ImVec2(0, 2));
                                ImGui::Text("%d/%d", hits, shots);
                                ImGui::Dummy(ImVec2(0, bar_h + 2.0f));
                                if (showAvg && hasAvg) {
                                    int dh = hits - (int)avg_hits;
                                    char buf[48]; snprintf(buf, sizeof(buf), "%+d / avg %d/%d", dh, (int)avg_hits, (int)avg_shots);
                                    ImGui::TextColored(deltaColor((float)dh, true), "%s", buf);
                                }
                            }

                            // Efficiency
                            if (showEff) {
                                ImGui::TableSetColumnIndex(col++);
                                ImGui::Dummy(ImVec2(0, 2));
                                ImGui::Text("%.1f%%", eff);
                                ImGui::Dummy(ImVec2(0, bar_h + 2.0f));
                                if (showAvg && hasAvg) {
                                    float d = eff - avg_eff;
                                    char buf[48]; snprintf(buf, sizeof(buf), "%+.1f / avg %.1f", d, avg_eff);
                                    ImGui::TextColored(deltaColor(d, true), "%s", buf);
                                }
                            }
                        }
                        ImGui::EndTable();
                    }
                }

                if (!g_open) show_graph = false;
                ImGui::End();
            }

            // EVOLUTION GRAPH — per-bot curves over 10 runs
            if (show_evolution && g_imgui_ctx) {
                bool showAcc = g_imgui_ctx->ks_show_accuracy;
                bool showTTK = g_imgui_ctx->ks_show_ttk;
                bool showEff = g_imgui_ctx->ks_show_efficiency;

                std::vector<GraphBotSeries> snap_data;
                {
                    std::lock_guard<std::mutex> lock(g_graph_mutex);
                    snap_data = g_graph_data;
                }

                // Per-bot colors (up to 8)
                static const ImVec4 BOT_COLORS[] = {
                    {0.2f, 0.7f, 1.0f, 1.0f},  // blue
                    {0.2f, 1.0f, 0.4f, 1.0f},  // green
                    {1.0f, 0.6f, 0.1f, 1.0f},  // orange
                    {1.0f, 0.3f, 0.5f, 1.0f},  // pink
                    {0.8f, 0.3f, 1.0f, 1.0f},  // purple
                    {1.0f, 1.0f, 0.2f, 1.0f},  // yellow
                    {0.3f, 1.0f, 0.9f, 1.0f},  // cyan
                    {1.0f, 0.5f, 0.5f, 1.0f},  // light red
                };

                int nStats = (showAcc?1:0) + (showTTK?1:0) + (showEff?1:0);
                if (nStats == 0) nStats = 1;
                int nBots = (int)snap_data.size();

                const float GRAPH_W   = 700.0f;
                const float GRAPH_H   = 220.0f;
                const float PAD       = 12.0f;
                float win_w = PAD*2 + GRAPH_W;
                float win_h = PAD*2 + nStats * (GRAPH_H + 36.0f) + 20.0f;

                ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
                ImGui::SetNextWindowPos(
                    ImVec2(screenW * 0.5f, screenH * 0.5f),
                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));

                bool ev_open = true;
                ImGui::Begin("Stats Evolution", &ev_open,
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoMove     |
                    ImGuiWindowFlags_NoResize   |
                    ImGuiWindowFlags_NoScrollbar);

                if (snap_data.empty()) {
                    ImGui::TextDisabled("No data — run a tracking gauntlet first.");
                } else {

                    auto DrawStatGraph = [&](const char* label,
                        std::function<const std::vector<GraphRunPoint>*(const GraphBotSeries&)> getter,
                        bool lowerBetter)
                    {
                        const float NAME_W = 200.0f;
                        const float PLOT_W = GRAPH_W - NAME_W;
                        const float PLOT_H = GRAPH_H;
                        int nBots = (int)snap_data.size();

                        int maxPts = 0;
                        for (auto& s : snap_data) { auto* p = getter(s); if (p) maxPts = std::max(maxPts, (int)p->size()); }

                        // Y scale
                        float gmin, gmax;
                        if (!lowerBetter) {
                            // Accuracy/Efficiency: zoom around actual values with margin
                            gmin = FLT_MAX; gmax = -FLT_MAX;
                            for (auto& s : snap_data) {
                                auto* p = getter(s);
                                if (!p) continue;
                                for (auto& pt : *p) { gmin = std::min(gmin, pt.value); gmax = std::max(gmax, pt.value); }
                            }
                            float margin = std::max((gmax - gmin) * 0.4f, 3.0f);
                            gmin = std::max(0.0f, gmin - margin);
                            gmax = std::min(100.0f, gmax + margin);
                        } else {
                            gmin = FLT_MAX; gmax = -FLT_MAX;
                            for (auto& s : snap_data) {
                                auto* p = getter(s);
                                if (!p) continue;
                                for (auto& pt : *p) { gmin = std::min(gmin, pt.value); gmax = std::max(gmax, pt.value); }
                            }
                            float pad = std::max((gmax - gmin) * 0.3f, 0.05f);
                            gmin -= pad; gmax += pad;
                        }
                        float grange = (gmax - gmin > 0) ? gmax - gmin : 1.0f;

                        ImGui::Text("%s", label);
                        ImVec2 origin = ImGui::GetCursorScreenPos();
                        // Names column on the left, graph on the right
                        float totalH = PLOT_H + 18.0f; // +18 for X labels
                        ImGui::Dummy(ImVec2(GRAPH_W, totalH));
                        ImDrawList* dl2 = ImGui::GetWindowDrawList();

                        ImVec2 plotOrigin = ImVec2(origin.x + NAME_W, origin.y);

                        // Graph background
                        dl2->AddRectFilled(plotOrigin,
                            ImVec2(plotOrigin.x + PLOT_W, plotOrigin.y + PLOT_H),
                            IM_COL32(25, 25, 35, 230), 4.0f);

                        // Horizontal grid + Y labels
                        for (int gi = 0; gi <= 4; gi++) {
                            float gy = plotOrigin.y + PLOT_H * gi / 4.0f;
                            dl2->AddLine(ImVec2(plotOrigin.x, gy),
                                         ImVec2(plotOrigin.x + PLOT_W, gy),
                                         IM_COL32(70, 70, 90, 100));
                        }

                        // Vertical grid + X labels — based on the longest series
                        // each bot has its own X values, so we just label 1..maxPts
                        if (maxPts > 1) {
                            for (int i = 0; i < maxPts; i++) {
                                float x = plotOrigin.x + PLOT_W * i / (float)(maxPts-1);
                                dl2->AddLine(ImVec2(x, plotOrigin.y),
                                             ImVec2(x, plotOrigin.y + PLOT_H),
                                             IM_COL32(60, 60, 80, 80));
                                char rb[8]; snprintf(rb, sizeof(rb), "%d", i+1);
                                dl2->AddText(ImVec2(x - 3, plotOrigin.y + PLOT_H + 2),
                                             IM_COL32(130,130,150,180), rb);
                            }
                        }

                        auto valueToY = [&](float v) {
                            float norm = (v - gmin) / grange;
                            norm = std::max(0.0f, std::min(1.0f, norm));
                            return plotOrigin.y + PLOT_H * (1.0f - norm);
                        };

                        // Names column on the left
                        dl2->AddRectFilled(origin,
                            ImVec2(origin.x + NAME_W - 4, origin.y + PLOT_H),
                            IM_COL32(20, 20, 30, 200));

                        // Curves + names (label anti-collision)
                        // First compute label Y positions
                        std::vector<float> labelYs(nBots, 0.0f);
                        for (int bi = 0; bi < nBots; bi++) {
                            auto* pts = getter(snap_data[bi]);
                            if (!pts || pts->empty()) { labelYs[bi] = origin.y + bi * 14.0f; continue; }
                            labelYs[bi] = std::max(origin.y + 2, std::min(origin.y + PLOT_H - 14, valueToY(pts->back().value) - 6));
                        }
                        // Push-apart: iterate to avoid overlaps
                        const float LABEL_H = 16.0f;
                        for (int iter = 0; iter < 20; iter++) {
                            for (int bi = 1; bi < nBots; bi++) {
                                float overlap = (labelYs[bi-1] + LABEL_H) - labelYs[bi];
                                if (overlap > 0) {
                                    labelYs[bi-1] -= overlap * 0.5f;
                                    labelYs[bi]   += overlap * 0.5f;
                                }
                            }
                            // Clamp within the zone
                            for (int bi = 0; bi < nBots; bi++)
                                labelYs[bi] = std::max(origin.y + 2, std::min(origin.y + PLOT_H - 14, labelYs[bi]));
                        }

                        // Clip labels within the names column
                        dl2->PushClipRect(origin, ImVec2(origin.x + NAME_W - 2, origin.y + PLOT_H), false);
                        for (int bi = 0; bi < nBots; bi++) {
                            auto* pts = getter(snap_data[bi]);
                            ImU32 col = ImGui::ColorConvertFloat4ToU32(BOT_COLORS[bi % 8]);
                            if (!pts || pts->empty()) continue;

                            // Value on the right (fixed)
                            char valBuf[12]; snprintf(valBuf, sizeof(valBuf), "%.1f", pts->back().value);
                            float valW = ImGui::CalcTextSize(valBuf).x;
                            dl2->AddText(ImVec2(origin.x + NAME_W - valW - 4, labelYs[bi]), col, valBuf);

                            // Name truncated to fit the remaining space
                            std::string name = snap_data[bi].bot_name;
                            float maxNameW = NAME_W - valW - 10;
                            while (name.size() > 2 && ImGui::CalcTextSize(name.c_str()).x > maxNameW)
                                name.pop_back();
                            dl2->AddText(ImVec2(origin.x + 2, labelYs[bi]), col, name.c_str());
                        }
                        dl2->PopClipRect();

                        // Curves within the plot area
                        dl2->PushClipRect(plotOrigin, ImVec2(plotOrigin.x + PLOT_W, plotOrigin.y + PLOT_H), true);
                        for (int bi = 0; bi < nBots; bi++) {
                            auto* pts = getter(snap_data[bi]);
                            ImU32 col = ImGui::ColorConvertFloat4ToU32(BOT_COLORS[bi % 8]);
                            if (!pts || pts->empty()) continue;
                            int n = (int)pts->size();

                            auto xPosN = [&](int i) {
                                return plotOrigin.x + PLOT_W * i / (float)std::max(n-1, 1);
                            };

                            if (n == 1) {
                                float y = valueToY((*pts)[0].value);
                                dl2->AddLine(ImVec2(plotOrigin.x, y),
                                             ImVec2(plotOrigin.x + PLOT_W, y), col, 1.5f);
                                dl2->AddCircleFilled(ImVec2(plotOrigin.x + PLOT_W * 0.5f, y), 3.5f, col);
                            } else {
                                for (int i = 1; i < n; i++) {
                                    float x0 = xPosN(i-1), x1 = xPosN(i);
                                    float y0 = valueToY((*pts)[i-1].value);
                                    float y1 = valueToY((*pts)[i].value);
                                    dl2->AddLine(ImVec2(x0,y0), ImVec2(x1,y1), col, 2.0f);
                                    dl2->AddCircleFilled(ImVec2(x1,y1), 3.0f, col);
                                }
                                dl2->AddCircleFilled(ImVec2(xPosN(0), valueToY((*pts)[0].value)), 3.0f, col);
                            }
                        }
                        dl2->PopClipRect();
                    };

                    if (showAcc) DrawStatGraph("Accuracy (%)",
                        [](const GraphBotSeries& s) { return &s.accuracy; }, false);
                    if (showTTK) DrawStatGraph("TTK (s)",
                        [](const GraphBotSeries& s) { return &s.ttk; }, true);
                    if (showEff) DrawStatGraph("Efficiency (%)",
                        [](const GraphBotSeries& s) { return &s.efficiency; }, false);
                    if (!showAcc && !showTTK && !showEff)
                        ImGui::TextDisabled("No stats enabled — check plugin settings.");
                }

                if (!ev_open) show_evolution = false;
                ImGui::End();
            }

            ImGui::Render();

            const float clear_color[4] = { 0.f, 0.f, 0.f, 0.f };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_pSwapChain->Present(0, 0);
        }

        // Frame limiter ~30 FPS
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - frame_start).count();
        if (elapsed < 33) std::this_thread::sleep_for(std::chrono::milliseconds(33 - elapsed));
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();

    // Unregister raw input
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x06;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    ::DestroyWindow(hwnd);
    ::UnregisterClassA(wc.lpszClassName, wc.hInstance);
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[KovaaksPlugin] ImGuiMenuThread exception: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "[KovaaksPlugin] ImGuiMenuThread: unknown exception");
    }
}

void kovaaks_update(void* data, obs_data_t* settings);

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

static void hotkey_open_imgui(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    g_imgui_toggle = true;
}

static void hotkey_open_graph(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    g_graph_toggle = true;
}

static void hotkey_open_evolution(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    g_evolution_toggle = true;
}

static void ExportKillStats(const kovaaks_context* ctx, const std::vector<KillStatBot>& bots,
                             const std::map<std::string, BotAvg>* avgs = nullptr);

// Shortcut to show the latest stats
static void hotkey_show_killstats(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    if (!pressed) return;
    auto* ctx = static_cast<kovaaks_context*>(data);
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

// KILL STATS — CSV PARSING & EXPORT

static std::string FindLatestCsv(const std::string& folder) {
    std::string latest_path;
    std::filesystem::file_time_type latest_time;
    try {
        for (auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            if (fname.find(".csv") == std::string::npos) continue;
            if (fname.find("Challenge") == std::string::npos) continue;
            auto mtime = entry.last_write_time();
            if (latest_path.empty() || mtime > latest_time) {
                latest_time = mtime;
                latest_path = entry.path().string();
            }
        }
    } catch (...) {}
    return latest_path;
}

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

// Scenario-filtered version — keeps only runs from the same scenario as exclude_path
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
            if (p == reference_path) continue; // exclude the current run
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

// Per-bot averages from history
static std::map<std::string, BotAvg> ComputeAvgs(const std::string& folder, const std::string& exclude_path, int maxRuns) {
    std::map<std::string, BotAvg> avgs;
    // Filter by scenario — only average runs from the same scenario
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

void ExportToJson(kovaaks_context* ctx, bool force_reload) {    // Only re-read files when cache is invalid (~1s tick) or forced (settings change)
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

    // KILL STATS WIDGET — placed first for quick access
    // Live callback: re-exports immediately if preview is active
    static auto on_ks_changed = [](obs_properties_t* props, obs_property_t*, obs_data_t* settings) -> bool {
        auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(props));
        if (!ctx) return false;
        // Update killstats settings from the properties
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

    // Single avg toggle — compares against last 10 runs
    obs_property_t* ks_avgs = obs_properties_add_bool(g_ks, "ks_show_avgs", "Show averages (last 10 runs)");
    obs_property_set_modified_callback(ks_avgs, on_ks_changed);

    obs_properties_add_button(g_ks, "ks_preview_btn", "Preview (show fake data for positioning)",
        [](obs_properties_t* p, obs_property_t*, void*) -> bool {
            auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(p));
            if (!ctx) return false;
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
            ctx->showing_ks_preview = false;
            HideKillStats(ctx);
            ctx->killstats_timer = -1.0f;
            return false;
        });
        
    obs_properties_add_button(g_ks, "ks_show_last_btn", "Show Latest Run Stats",
        [](obs_properties_t* p, obs_property_t*, void*) -> bool {
            auto* ctx = static_cast<kovaaks_context*>(obs_properties_get_param(p));
            if (!ctx || ctx->last_bots.empty()) return false;
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
        if (!ctx->kovaaks_save_folder.empty() && !ctx->export_folder.empty()) ExportToJson(ctx, false);
        return;
    }

    // KillStats display countdown
    if (ctx->killstats_timer > 0.0f) {
        ctx->killstats_timer -= seconds;
        if (ctx->killstats_timer <= 0.0f) {
            ctx->killstats_timer = -1.0f;
            HideKillStats(ctx);
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
    ctx->cache_valid = false;
    if (!ctx->kovaaks_save_folder.empty() && !ctx->export_folder.empty()) {
        ExportToJson(ctx, true);

        // Check for new CSV
        if (ctx->killstats_display_sec > 0) {
            const std::string& csv_folder = ctx->kovaaks_stats_folder.empty()
                ? ctx->kovaaks_save_folder
                : ctx->kovaaks_stats_folder;
            std::string latest = FindLatestCsv(csv_folder);
            if (!latest.empty() && latest != ctx->last_csv_processed) {
                ctx->last_csv_processed = latest;
                auto bots = ParseKillStatsCsv(latest);
                
                // Always cache the latest run's stats for manual recall via hotkey/button
                ctx->last_bots = bots;

                // Auto-display only triggers when there are multiple targets (multi-bot runs)
                if (IsGauntletRun(bots)) {
                    ctx->showing_ks_preview = false;
                    auto avgs = ComputeAvgs(csv_folder, latest, 10);
                    ExportKillStats(ctx, bots, &avgs);
                    ctx->killstats_timer = (float)ctx->killstats_display_sec;
                    g_graph_new_csv = true; // notify the ImGui viewer to reload
                }
            }
        }
    }
}

void* kovaaks_create(obs_data_t* settings, obs_source_t* source) {
    try {
    blog(LOG_INFO, "[KovaaksPlugin] kovaaks_create called.");
    auto* ctx = new kovaaks_context();
    g_imgui_ctx = ctx;

    // Read the .ini created by the installer — once, then delete it
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
        // Delete the .ini after reading — this only ever runs once
        try {
            std::filesystem::remove(ini_path);
            blog(LOG_INFO, "[KovaaksPlugin] Loaded and removed installer ini: %s", ini_path.c_str());
        } catch (...) {
            blog(LOG_WARNING, "[KovaaksPlugin] Could not remove installer ini (access denied?): %s", ini_path.c_str());
        }    }

    kovaaks_update(ctx, settings);

    ctx->hotkey_next = obs_hotkey_register_source(source, "kovaaks_next_section", "Kovaaks: Next Section", hotkey_next_section, ctx);
    ctx->hotkey_prev = obs_hotkey_register_source(source, "kovaaks_prev_section", "Kovaaks: Previous Section", hotkey_prev_section, ctx);
    ctx->hotkey_imgui = obs_hotkey_register_source(source, "kovaaks_open_custom_info", "Kovaaks: Open Custom Info Editor", hotkey_open_imgui, ctx);
    ctx->hotkey_show_killstats = obs_hotkey_register_source(source, "kovaaks_show_killstats", "Kovaaks: Show Latest Kill Stats", hotkey_show_killstats, ctx);
    ctx->hotkey_graph = obs_hotkey_register_source(source, "kovaaks_open_graph", "Kovaaks: Open Stats Graph", hotkey_open_graph, ctx);
    ctx->hotkey_evolution = obs_hotkey_register_source(source, "kovaaks_open_evolution", "Kovaaks: Open Stats Evolution Graph", hotkey_open_evolution, ctx);

    obs_data_array_t* next_array  = obs_data_get_array(settings, "hotkey_next_section");
    obs_data_array_t* prev_array  = obs_data_get_array(settings, "hotkey_prev_section");
    obs_data_array_t* imgui_array = obs_data_get_array(settings, "hotkey_open_custom_info");
    obs_data_array_t* ks_array    = obs_data_get_array(settings, "hotkey_show_killstats");
    obs_data_array_t* graph_array = obs_data_get_array(settings, "hotkey_open_graph");
    obs_data_array_t* evo_array   = obs_data_get_array(settings, "hotkey_open_evolution");

    if (next_array)  { obs_hotkey_load(ctx->hotkey_next,  next_array);  obs_data_array_release(next_array); }
    if (prev_array)  { obs_hotkey_load(ctx->hotkey_prev,  prev_array);  obs_data_array_release(prev_array); }
    if (imgui_array) { obs_hotkey_load(ctx->hotkey_imgui, imgui_array); obs_data_array_release(imgui_array); }
    if (ks_array)    { obs_hotkey_load(ctx->hotkey_show_killstats, ks_array); obs_data_array_release(ks_array); }
    if (graph_array) { obs_hotkey_load(ctx->hotkey_graph, graph_array); obs_data_array_release(graph_array); }
    if (evo_array)   { obs_hotkey_load(ctx->hotkey_evolution, evo_array); obs_data_array_release(evo_array); }

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
    if (g_imgui_ctx == ctx) g_imgui_ctx = nullptr;
    if (ctx->hotkey_next  != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_next);
    if (ctx->hotkey_prev  != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_prev);
    if (ctx->hotkey_imgui != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_imgui);
    if (ctx->hotkey_show_killstats != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_show_killstats);
    if (ctx->hotkey_graph          != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_graph);
    if (ctx->hotkey_evolution      != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(ctx->hotkey_evolution);
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
        if (ctx->hotkey_imgui != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_imgui);
            obs_data_set_array(settings, "hotkey_open_custom_info", arr);
            obs_data_array_release(arr);
        }
        if (ctx->hotkey_show_killstats != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_show_killstats);
            obs_data_set_array(settings, "hotkey_show_killstats", arr);
            obs_data_array_release(arr);
        }
        if (ctx->hotkey_graph != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_graph);
            obs_data_set_array(settings, "hotkey_open_graph", arr);
            obs_data_array_release(arr);
        }
        if (ctx->hotkey_evolution != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_evolution);
            obs_data_set_array(settings, "hotkey_open_evolution", arr);
            obs_data_array_release(arr);
        }
    };
    obs_register_source(&kovaaks_info_source);

    // Start the ImGui overlay thread
    if (!g_imgui_running) {
        g_imgui_running = true;
        g_imgui_thread = std::thread(ImGuiMenuThread);
    }

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
    if (g_imgui_running) {
        g_imgui_running = false;
        if (g_imgui_thread.joinable()) g_imgui_thread.join();
    }
}