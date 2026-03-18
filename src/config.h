#pragma once
// ── INI settings + config helpers ─────────────────────────────────────────────

#define PLUGIN_NAME    "SkyrimNetPrismaDashboard"
#define PLUGIN_VERSION "1.0.0"

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
};

static DashboardSettings s_cfg;
static std::mutex        s_cfgMtx;  // protects concurrent reads/writes of s_cfg from HTTP handler threads
static std::string       s_iniPath; // full path to the INI, populated in Load

// DX scancode → display name for the settings UI
static std::string DxKeyName(int dx)
{
    // Common keys a user would pick for a toggle
    static const std::pair<int,const char*> kNames[] = {
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
    for (auto& p : kNames) if (p.first == dx) return p.second;
    char buf[8]; snprintf(buf, sizeof(buf), "0x%02X", dx);
    return buf;
}

// Build JSON for current settings (for /settings-get)
static std::string SettingsToJson()
{
    // Escape backslashes in lastPage URL (shouldn't have any but be safe)
    std::string lp; for (auto c : s_cfg.lastPage) { if(c=='"'||c=='\\') lp+='\\'; lp+=c; }
    std::string url; for (auto c : s_cfg.url) { if(c=='"'||c=='\\') url+='\\'; url+=c; }
    // Build key list JSON array for the UI dropdown
    static const std::pair<int,const char*> kNames[] = {
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
        {0x1A,"["},{0x1B,"]"},{0x27,";"},{0x28,"'"},{0x56,"\\\\"},
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
    std::string keys = "[";
    for (auto& p : kNames) {
        if (keys.size() > 1) keys += ',';
        keys += "{\"code\":" + std::to_string(p.first) + ",\"name\":\"" + p.second + "\"}";
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

static void LoadSettings()
{
    // Locate the INI next to the DLL: <SKSE log dir>/../../Plugins/...ini
    // The most reliable path is from the module filename itself.
    {
        char modPath[MAX_PATH] = {};
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LoadSettings), &hm);
        GetModuleFileNameA(hm, modPath, MAX_PATH);
        // Replace .dll extension with .ini
        std::string mp(modPath);
        auto dot = mp.rfind('.');
        s_iniPath = (dot != std::string::npos ? mp.substr(0, dot) : mp) + ".ini";
    }

    auto readStr = [&](const char* key, const char* def) -> std::string {
        char buf[512] = {};
        GetPrivateProfileStringA("Dashboard", key, def, buf, sizeof(buf), s_iniPath.c_str());
        return buf;
    };
    auto readInt = [&](const char* key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntA("Dashboard", key, def, s_iniPath.c_str()));
    };

    s_cfg.url         = readStr("URL",    "http://localhost:8080/");
    s_cfg.lastPage    = readStr("LastPage", s_cfg.url.c_str());
    s_cfg.hotKey      = readInt("HotKey",         0x3E);
    s_cfg.keepBg      = readInt("KeepBackground", 0) != 0;
    s_cfg.defaultHome = readInt("DefaultHome",    0) != 0;
    s_cfg.pauseGame   = readInt("PauseGame",      0) != 0;

    logger::info("SkyrimNetDashboard: INI loaded from '{}'", s_iniPath);
    logger::info("  URL={} HotKey=0x{:02X} keepBg={} defaultHome={} pauseGame={}",
        s_cfg.url, s_cfg.hotKey, s_cfg.keepBg, s_cfg.defaultHome, s_cfg.pauseGame);
}

static void SaveSettings()
{
    if (s_iniPath.empty()) return;
    auto ws = [&](const char* k, const std::string& v) {
        WritePrivateProfileStringA("Dashboard", k, v.c_str(), s_iniPath.c_str());
    };
    auto wi = [&](const char* k, int v) {
        WritePrivateProfileStringA("Dashboard", k, std::to_string(v).c_str(), s_iniPath.c_str());
    };
    // URL intentionally not written back (read-only at runtime)
    wi("Version",       2);
    ws("LastPage",      s_cfg.lastPage);
    wi("HotKey",        s_cfg.hotKey);
    wi("KeepBackground",s_cfg.keepBg      ? 1 : 0);
    wi("DefaultHome",   s_cfg.defaultHome  ? 1 : 0);
    wi("PauseGame",     s_cfg.pauseGame    ? 1 : 0);
    logger::info("SkyrimNetDashboard: settings saved to INI");
}

