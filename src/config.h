#pragma once
// ── INI settings + config helpers ─────────────────────────────────────────────

#define PLUGIN_NAME    "SkyrimNetPrismaDashboard"
#define PLUGIN_VERSION "1.0.0"

// Parse host and port from a URL string (after scheme is stripped).
// Handles IPv6 bracket notation: "[::1]:8080/path" → host="::1", port=8080
// Also handles plain: "localhost:8080/path" → host="localhost", port=8080
static void ParseHostPort(const std::string& url, std::string& host, uint16_t& port)
{
    std::string u = url;
    // Strip http:// or https:// scheme (case-insensitive)
    {
        std::string lower;
        lower.reserve(u.size());
        for (auto c : u) lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower.size() >= 8 && lower.substr(0, 8) == "https://") u = u.substr(8);
        else if (lower.size() >= 7 && lower.substr(0, 7) == "http://") u = u.substr(7);
    }

    if (!u.empty() && u[0] == '[') {
        // IPv6 bracket notation: [::1]:port/path
        auto closeBracket = u.find(']');
        if (closeBracket != std::string::npos) {
            host = u.substr(1, closeBracket - 1); // strip brackets
            auto rest = u.substr(closeBracket + 1);
            if (!rest.empty() && rest[0] == ':') {
                try { port = static_cast<uint16_t>(std::stoi(rest.substr(1))); } catch (...) {}
            }
        }
    } else {
        // IPv4 / hostname: host:port/path
        auto sl  = u.find('/');
        auto col = u.find(':');
        if (col != std::string::npos && (sl == std::string::npos || col < sl)) {
            host = u.substr(0, col);
            try { port = static_cast<uint16_t>(std::stoi(u.substr(col + 1, sl != std::string::npos ? sl - col - 1 : std::string::npos))); } catch (...) {}
        } else {
            host = u.substr(0, sl);
        }
    }
}

// ── INI settings ─────────────────────────────────────────────────────────────
// Loaded once at startup from SKSE/Plugins/SkyrimNetPrismaDashboard.ini.
// All mutable settings are stored here and flushed back to the INI via
// /settings-save.  URL is intentionally read-only at runtime (INI only).

struct DashboardSettings {
    // [Dashboard]
    std::string url          = "http://localhost:8080/";
    std::string lastPage     = "";
    int         hotKey       = 0x3E;  // DX scancode — default F4
    bool        keepBg       = false; // keep page alive when closed
    bool        defaultHome  = false; // always open base URL instead of lastPage
    bool        pauseGame    = false; // pause while focused
    // Window layout (persisted across game sessions)
    std::string winX     = "";  // CSS left value, e.g. "423px"
    std::string winY     = "";
    std::string winW     = "";
    std::string winH     = "";
    std::string winZoom  = "";  // e.g. "1.2"; empty = not yet saved
    bool        winFs    = false;
};

static DashboardSettings s_cfg;
static std::mutex        s_cfgMtx;  // protects concurrent reads/writes of s_cfg from HTTP handler threads
static std::string       s_iniPath; // full path to the INI, populated in Load

// Escape a string for safe inclusion inside a JSON double-quoted value.
// Handles \, ", and control characters (U+0000 through U+001F).
static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char h[8]; snprintf(h, sizeof(h), "\\u%04X", c);
                out += h;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// DX scancode → display name table (shared by DxKeyName and SettingsToJson)
static const std::pair<int,const char*> s_dxKeyNames[] = {
    {0x3B,"F1"},{0x3C,"F2"},{0x3D,"F3"},{0x3E,"F4"},
    {0x3F,"F5"},{0x40,"F6"},{0x41,"F7"},{0x42,"F8"},
    {0x43,"F9"},{0x44,"F10"},{0x57,"F11"},{0x58,"F12"},
    {0x02,"1"},{0x03,"2"},{0x04,"3"},{0x05,"4"},{0x06,"5"},
    {0x07,"6"},{0x08,"7"},{0x09,"8"},{0x0A,"9"},{0x0B,"0"},
    {0x10,"Q"},{0x11,"W"},{0x12,"E"},{0x13,"R"},{0x14,"T"},
    {0x15,"Y"},{0x16,"U"},{0x17,"I"},{0x18,"O"},{0x19,"P"},
    {0x1E,"A"},{0x1F,"S"},{0x20,"D"},{0x21,"F"},{0x22,"G"},
    {0x23,"H"},{0x24,"J"},{0x25,"K"},{0x26,"L"},
    {0x2C,"Z"},{0x2D,"X"},{0x2E,"C"},{0x2F,"V"},{0x30,"B"},
    {0x31,"N"},{0x32,"M"},
    {0x29,"Tilde (~)"},{0x0C,"Minus (-)"},{0x0D,"Equals (=)"},
    {0x1A,"["},{0x1B,"]"},{0x27,";"},{0x28,"'"},{0x56,"\\"},
    {0x33,","},{0x34,"."},{0x35,"/"},
    {0xC7,"Home"},{0xD2,"Insert"},{0xD3,"Delete"},
    {0xC9,"Page Up"},{0xD1,"Page Down"},{0xC8,"Up"},{0xD0,"Down"},
    {0xCB,"Left"},{0xCD,"Right"},
    {0x52,"Numpad 0"},{0x4F,"Numpad 1"},{0x50,"Numpad 2"},
    {0x51,"Numpad 3"},{0x4B,"Numpad 4"},{0x4C,"Numpad 5"},
    {0x4D,"Numpad 6"},{0x47,"Numpad 7"},{0x48,"Numpad 8"},
    {0x49,"Numpad 9"},{0x4E,"Numpad +"},{0x4A,"Numpad -"},
    {0x37,"Numpad *"},{0xB5,"Numpad /"},
};

static std::string DxKeyName(int dx)
{
    for (auto& p : s_dxKeyNames) if (p.first == dx) return p.second;
    char buf[8]; snprintf(buf, sizeof(buf), "0x%02X", dx);
    return buf;
}

// Build JSON for current settings (for /settings-get)
static std::string SettingsToJson()
{
    std::string lp = JsonEscape(s_cfg.lastPage);
    std::string url = JsonEscape(s_cfg.url);
    std::string keys = "[";
    for (auto& p : s_dxKeyNames) {
        if (keys.size() > 1) keys += ',';
        keys += "{\"code\":" + std::to_string(p.first) + ",\"name\":\"" + JsonEscape(p.second) + "\"}";
    }
    keys += "]";
    return std::string("{") +
        "\"url\":\""          + url  + "\"," +
        "\"hotKey\":"         + std::to_string(s_cfg.hotKey) + "," +
        "\"hotKeyName\":\""   + DxKeyName(s_cfg.hotKey) + "\"," +
        "\"keepBg\":"         + (s_cfg.keepBg      ? "true" : "false") + "," +
        "\"defaultHome\":"    + (s_cfg.defaultHome  ? "true" : "false") + "," +
        "\"pauseGame\":"      + (s_cfg.pauseGame    ? "true" : "false") + "," +
        "\"keys\":"           + keys +
        "}";
}

static void SaveSettings();

// ── Manual INI reader/writer ────────────────────────────────────────────────
// Replaces GetPrivateProfileStringA / WritePrivateProfileStringA which corrupt
// URLs containing "://" on certain Windows 11 builds.

// Read all key=value pairs from the [Dashboard] section into a map.
static std::unordered_map<std::string, std::string> ReadIniSection()
{
    std::unordered_map<std::string, std::string> kv;
    std::ifstream in(s_iniPath);
    if (!in.is_open()) return kv;

    bool inSection = false;
    std::string line;
    while (std::getline(in, line)) {
        // Trim trailing \r (handles both \r\n and \n)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Trim leading whitespace
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line[0] == '[') {
            inSection = (line.find("[Dashboard]") == 0);
            continue;
        }
        if (!inSection || line[0] == ';' || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim whitespace from key and value
        auto kEnd = key.find_last_not_of(" \t");
        if (kEnd != std::string::npos) key = key.substr(0, kEnd + 1);
        auto vStart = val.find_first_not_of(" \t");
        if (vStart != std::string::npos) val = val.substr(vStart);
        auto vEnd = val.find_last_not_of(" \t");
        if (vEnd != std::string::npos) val = val.substr(0, vEnd + 1);

        kv[key] = val;
    }
    return kv;
}

// Write the INI file from scratch, preserving comment structure.
static void WriteIniFile()
{
    std::ofstream out(s_iniPath);
    if (!out.is_open()) {
        logger::warn("SkyrimNetDashboard: failed to write INI at '{}'", s_iniPath);
        return;
    }
    out << "[Dashboard]\n"
        << "; Full URL to your SkyrimNet instance, including port.\n"
        << "; Default http://localhost:8080/ works for most setups.\n"
        << "; If it doesn't, replace with your local IP, e.g. http://192.168.1.100:8080/\n"
        << "URL=" << s_cfg.url << "\n"
        << "\n"
        << "; Last visited page - updated automatically, do not edit manually.\n"
        << "LastPage=" << s_cfg.lastPage << "\n"
        << "\n"
        << "; Keep the menu rendered without focus (1 = yes, 0 = no).\n"
        << "KeepBackground=" << (s_cfg.keepBg ? 1 : 0) << "\n"
        << "\n"
        << "; Always open the base URL instead of resuming the last visited page (1 = yes, 0 = no).\n"
        << "DefaultHome=" << (s_cfg.defaultHome ? 1 : 0) << "\n"
        << "\n"
        << "; Pause Skyrim while the dashboard is focused (1 = yes, 0 = no).\n"
        << "PauseGame=" << (s_cfg.pauseGame ? 1 : 0) << "\n"
        << "\n"
        << "; Hotkey to open/close the dashboard (DirectInput scan code, decimal).\n"
        << "; Default 62 = F4. Common keys: 59=F1, 60=F2, 61=F3, 62=F4, 63=F5,\n"
        << "; 64=F6, 65=F7, 66=F8, 67=F9, 68=F10, 87=F11, 88=F12.\n"
        << "HotKey=" << s_cfg.hotKey << "\n"
        << "\n"
        << "; Internal version marker - do not edit.\n"
        << "Version=2\n"
        << "WinX=" << s_cfg.winX << "\n"
        << "WinY=" << s_cfg.winY << "\n"
        << "WinW=" << s_cfg.winW << "\n"
        << "WinH=" << s_cfg.winH << "\n"
        << "WinZoom=" << s_cfg.winZoom << "\n"
        << "WinFs=" << (s_cfg.winFs ? 1 : 0) << "\n";
}

static void LoadSettings()
{
    // Locate the INI next to the DLL
    {
        char modPath[MAX_PATH] = {};
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LoadSettings), &hm);
        GetModuleFileNameA(hm, modPath, MAX_PATH);
        std::string mp(modPath);
        auto dot = mp.rfind('.');
        s_iniPath = (dot != std::string::npos ? mp.substr(0, dot) : mp) + ".ini";
    }

    // Auto-generate INI with defaults if it doesn't exist
    if (GetFileAttributesA(s_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Set defaults so WriteIniFile has something to write
        s_cfg = DashboardSettings{};
        WriteIniFile();
        logger::info("SkyrimNetDashboard: INI auto-generated with defaults");
    }

    // Read all values
    auto kv = ReadIniSection();
    auto str = [&](const char* key, const char* def) -> std::string {
        auto it = kv.find(key);
        return (it != kv.end() && !it->second.empty()) ? it->second : def;
    };
    auto num = [&](const char* key, int def) -> int {
        auto it = kv.find(key);
        if (it == kv.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    };

    s_cfg.url         = str("URL",          "http://localhost:8080/");
    s_cfg.lastPage    = str("LastPage",     s_cfg.url.c_str());
    s_cfg.hotKey      = num("HotKey",       0x3E);
    s_cfg.keepBg      = num("KeepBackground", 0) != 0;
    s_cfg.defaultHome = num("DefaultHome",  0) != 0;
    s_cfg.pauseGame   = num("PauseGame",    0) != 0;
    s_cfg.winX        = str("WinX",    "");
    s_cfg.winY        = str("WinY",    "");
    s_cfg.winW        = str("WinW",    "");
    s_cfg.winH        = str("WinH",    "");
    s_cfg.winZoom     = str("WinZoom", "");
    if (!s_cfg.winZoom.empty()) {
        float z = static_cast<float>(std::atof(s_cfg.winZoom.c_str()));
        if (z < 0.20f || z > 3.0f) s_cfg.winZoom = "";
    }
    s_cfg.winFs       = num("WinFs", 0) != 0;

    logger::info("SkyrimNetDashboard: INI loaded from '{}'", s_iniPath);
    logger::info("  URL={} HotKey=0x{:02X} keepBg={} defaultHome={} pauseGame={}",
        s_cfg.url, s_cfg.hotKey, s_cfg.keepBg, s_cfg.defaultHome, s_cfg.pauseGame);
}

static void SaveSettings()
{
    if (s_iniPath.empty()) return;
    WriteIniFile();
    logger::info("SkyrimNetDashboard: settings saved to INI");
}

