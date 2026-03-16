#include "pch.h"
#include "PrismaUI_API.h"
#include "keyhandler/keyhandler.h"

// Modular injection components
#include "injections/audio_polyfill.h"
#include "injections/clipboard.h"
#include "injections/drag_select.h"
#include "injections/editor_fixes.h"

// -----------------------------------------------------------------------
//  SkyrimNet Prisma Dashboard
//  Loads the SkyrimNet web dashboard inside Skyrim via PrismaUI.
//  Press F4 to open/close the overlay.
// -----------------------------------------------------------------------

#define PLUGIN_NAME    "SkyrimNetPrismaDashboard"
#define PLUGIN_VERSION "1.0.0"

// ── INI settings ─────────────────────────────────────────────────────────────
// Loaded once at startup from SKSE/Plugins/SkyrimNetPrismaDashboard.ini.
// All mutable settings are stored here and flushed back to the INI via
// /settings-save.  URL is intentionally read-only at runtime (INI only).

struct DashboardSettings {
    // [Dashboard]
    std::string url          = "http://192.168.50.88:8080/";
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

    s_cfg.url         = readStr("URL",    "http://192.168.50.88:8080/");
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

static constexpr uint32_t    ESC_KEY       = 0x01; // Escape
static constexpr uint32_t    INSPECTOR_KEY = 0x3F; // F5
static uint32_t              s_toggleKey   = 0x3E; // runtime toggle key, set from s_cfg at startup

static PRISMA_UI_API::IVPrismaUI1* s_PrismaUI     = nullptr;
static PrismaView                  s_View         = 0;
static KeyHandlerEvent             s_toggleHandle = INVALID_REGISTRATION_HANDLE; // stored so we can re-register on hotkey change

// ── Audio subsystem ───────────────────────────────────────────────────────────
// Forward declaration — FetchResource is defined after the shell helpers below.

// Decode a percent-encoded URL component (e.g. from query strings).
static std::string UrlDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = { s[i+1], s[i+2], 0 };
            char* end = nullptr;
            out += static_cast<char>(std::strtol(hex, &end, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// Default arguments live here so callers above don't need to pass them explicitly.
static std::tuple<std::string,std::string,int>
FetchResource(const std::string& host, uint16_t port, const std::string& method,
              const std::string& path, const std::string& reqContentType = "",
              const std::string& reqBody = "");

// Populated once at startup by parsing SKYRIMNET_URL.
static std::string s_audioHost;
static uint16_t    s_audioPort = 8080;

// Simple JSON field extractor for our own controlled JSON payloads.
// Handles both quoted strings ("value") and unquoted numbers/booleans.
static std::string AudioJsonField(const char* json, const char* key)
{
    if (!json || !key) return {};
    std::string j(json), k = std::string("\"") + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return {};
    pos = j.find(':', pos + k.size());
    if (pos == std::string::npos) return {};
    // Skip whitespace after ':'
    ++pos;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) ++pos;
    if (pos >= j.size()) return {};
    if (j[pos] == '"') {
        // Quoted string value
        ++pos;
        std::string val;
        while (pos < j.size() && j[pos] != '"') {
            if (j[pos] == '\\' && pos + 1 < j.size()) { ++pos; }
            val += j[pos++];
        }
        return val;
    } else {
        // Unquoted value (number, boolean, null) — read until delimiter
        std::string val;
        while (pos < j.size() && j[pos] != ',' && j[pos] != '}' && j[pos] != ' ' && j[pos] != '\r' && j[pos] != '\n') {
            val += j[pos++];
        }
        return val;
    }
}

// Resolve an audio src URL to (host, port, path) for our FetchResource helper.
// Proxy-host URLs (127.0.0.1) are redirected to the real SkyrimNet server.
static std::tuple<std::string, uint16_t, std::string>
AudioParseUrl(const std::string& url)
{
    std::string u = url;
    // strip scheme
    if (u.size() > 7 && u.substr(0, 7) == "http://")  u = u.substr(7);
    else if (u.size() > 8 && u.substr(0, 8) == "https://") u = u.substr(8);

    auto sl = u.find('/');
    std::string hostport = (sl != std::string::npos) ? u.substr(0, sl) : u;
    std::string path     = (sl != std::string::npos) ? u.substr(sl)    : "/";

    std::string host = hostport;
    uint16_t    port = 80;
    auto col = hostport.find(':');
    if (col != std::string::npos) {
        host = hostport.substr(0, col);
        try { port = static_cast<uint16_t>(std::stoi(hostport.substr(col + 1))); } catch (...) {}
    }

    // Redirect our local proxy back to the real SkyrimNet server
    if (host == "127.0.0.1" || host == "localhost") {
        host = s_audioHost;
        port = s_audioPort;
    }
    return { host, port, path };
}

// WAV playback buffer -- must stay alive for the duration of async PlaySound.
// Swapped only after stopping current playback, so no data race.
static std::mutex  s_wavBufMtx;
static std::string s_wavBuf;

// Monotonically-increasing generation stamp: each PlayAudioBytes call claims a slot
// before stopping the previous segment.  Only the latest generation fires
// __onAudioEnded; a pre-empted segment's thread will see a mismatch and skip it.
// StopAudio() also bumps the stamp so playback fired by UI close/toggle is cancelled.
static std::atomic<uint32_t> s_audioGen{0};

// Serialises all MCI open/play/close operations so concurrent PlayAudioBytes calls
// can't step on the shared "snpd" device alias.  Also protects s_hwo/s_hwoHdr.
static std::mutex s_mciMtx;

// waveOut handle used for in-memory PCM WAV playback (no temp file).
// Protected by s_mciMtx.  Non-null while a segment is playing or has been
// reset-but-not-yet-closed by PauseAudioQueue/ClearAudioQueue.
static HWAVEOUT s_hwo    = nullptr;
static WAVEHDR  s_hwoHdr = {};

// ── Diary audio accumulation buffer ──────────────────────────────────────────
// As TTS segments arrive, their PCM data is appended here so the full entry
// can be served by the /diary-audio endpoint for the download button.
static std::mutex            s_diaryMtx;
static std::vector<uint8_t> s_diaryPcm;
static WAVEFORMATEX          s_diaryFmt     = {};
static bool                  s_diaryFmtValid = false;

// ── Streaming audio queue ─────────────────────────────────────────────────────
// TryStreamProxy decodes base64 WAV segments from the TTS NDJSON stream in C++
// and pushes them here so the expensive JS P() function (atob + new Array(N))
// never has to run — eliminating the per-segment 150-200 ms stall that blocks
// D3DPresent via ultralightFuture.get().  Segments play sequentially; the queue
// runner fires __onAudioEnded after each one to advance SkyrimNet's JS queue.
static std::mutex              s_aqMtx;
static std::condition_variable s_aqCv;
static std::deque<std::string> s_aqPending;  // WAV byte strings
static std::atomic<bool>       s_aqRunning{false};
static std::atomic<bool>       s_aqPaused{false};

static void StopAudio()
{
    ++s_audioGen; // invalidate any polling thread so it won't fire __onAudioEnded
    {
        std::lock_guard<std::mutex> lk(s_mciMtx);
        if (s_hwo) waveOutReset(s_hwo); // immediately marks buffer DONE so poll exits
        mciSendStringA("stop snpd wait", nullptr, 0, nullptr);
        mciSendStringA("close snpd wait", nullptr, 0, nullptr);
    }
    logger::info("SkyrimNetDashboard: audio stopped.");
}

// Transcodes any audio file that Media Foundation can read (MP3, AAC, WMA, …)
// into a 16-bit PCM WAV file at dstPath.  Returns true on success.
// This lets us play everything via the waveaudio MCI driver and avoid the
// DirectShow-backed mpegvideo driver entirely (mpegvideo's mciSendStringA calls
// post Win32 messages to the game thread, which never pumps them → freeze).
static bool TranscodeToWav(const std::wstring& srcPath, const std::wstring& dstPath)
{
    // MFStartup is called once at plugin load; do not call it here.
    // Each background thread that uses MF must initialize COM first.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // idempotent if already inited
    struct ComGuard { ~ComGuard() { CoUninitialize(); } } _cg;

    // ── Source reader ──────────────────────────────────────────────────────
    IMFSourceReader* pReader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(srcPath.c_str(), nullptr, &pReader)))
        return false;

    // Tell MF to decode the first audio stream to uncompressed PCM.
    IMFMediaType* pType = nullptr;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pType->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
    pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pType);
    pType->Release();

    // Retrieve the actual output format so we can write the WAV header.
    IMFMediaType* pOutType = nullptr;
    if (FAILED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pOutType))) {
        pReader->Release(); return false;
    }
    UINT32 channels = 0, sampleRate = 0, bitsPerSample = 0;
    pOutType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,           &channels);
    pOutType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,     &sampleRate);
    pOutType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,        &bitsPerSample);
    pOutType->Release();

    if (!channels || !sampleRate || !bitsPerSample) { pReader->Release(); return false; }

    // ── Collect PCM samples ────────────────────────────────────────────────
    std::string pcm;
    pcm.reserve(sampleRate * channels * (bitsPerSample / 8) * 10); // ~10s pre-alloc

    for (;;) {
        DWORD flags = 0;
        IMFSample* pSample = nullptr;
        HRESULT hr = pReader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &pSample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (pSample) pSample->Release();
            break;
        }
        if (!pSample) continue;

        IMFMediaBuffer* pBuf = nullptr;
        if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuf))) {
            BYTE* pData = nullptr; DWORD len = 0;
            if (SUCCEEDED(pBuf->Lock(&pData, nullptr, &len))) {
                pcm.append(reinterpret_cast<char*>(pData), len);
                pBuf->Unlock();
            }
            pBuf->Release();
        }
        pSample->Release();
    }
    pReader->Release();

    if (pcm.empty()) return false;

    // ── Write WAV file ─────────────────────────────────────────────────────
    std::ofstream ofs(dstPath, std::ios::binary);
    if (!ofs) return false;

    uint32_t byteRate    = sampleRate * channels * (bitsPerSample / 8);
    uint16_t blockAlign  = static_cast<uint16_t>(channels * (bitsPerSample / 8));
    uint32_t dataSize    = static_cast<uint32_t>(pcm.size());
    uint32_t riffSize    = 36 + dataSize;

    auto w16 = [&](uint16_t v) { ofs.write(reinterpret_cast<char*>(&v), 2); };
    auto w32 = [&](uint32_t v) { ofs.write(reinterpret_cast<char*>(&v), 4); };

    ofs.write("RIFF", 4); w32(riffSize);
    ofs.write("WAVE", 4);
    ofs.write("fmt ", 4); w32(16);
    w16(1); // PCM
    w16(static_cast<uint16_t>(channels));
    w32(sampleRate);
    w32(byteRate);
    w16(blockAlign);
    w16(static_cast<uint16_t>(bitsPerSample));
    ofs.write("data", 4); w32(dataSize);
    ofs.write(pcm.data(), static_cast<std::streamsize>(pcm.size()));
    return true;
}

// Parses a PCM WAV byte buffer and returns the WAVEFORMATEX descriptor and the
// byte range of the 'data' chunk.  Returns valid=false if the format is not
// plain PCM (fmt tag != 1) or the header is malformed.
struct WavInfo { WAVEFORMATEX wfx{}; size_t dataOff = 0, dataLen = 0; bool valid = false; };
static WavInfo ParseWavHeader(const std::string& b)
{
    WavInfo i; const char* p = b.data(); size_t sz = b.size();
    if (sz < 44) return i;
    bool hasFmt = false, hasData = false;
    for (size_t pos = 12; pos + 8 <= sz; ) {
        uint32_t cSz = 0; std::memcpy(&cSz, p + pos + 4, 4);
        if (cSz == 0xFFFFFFFFu) cSz = static_cast<uint32_t>(sz - pos - 8);
        if (p[pos]=='f'&&p[pos+1]=='m'&&p[pos+2]=='t'&&p[pos+3]==' '&&cSz>=16) {
            uint16_t tag=0,ch=0,ba=0,bps=0; uint32_t sr=0;
            std::memcpy(&tag,p+pos+8,2); std::memcpy(&ch, p+pos+10,2);
            std::memcpy(&sr, p+pos+12,4); std::memcpy(&ba, p+pos+20,2);
            std::memcpy(&bps,p+pos+22,2);
            if (tag != 1) return i; // not PCM — let MCI handle it
            i.wfx = { WAVE_FORMAT_PCM, ch, sr, sr*ba, ba, bps, 0 }; hasFmt = true;
        } else if (p[pos]=='d'&&p[pos+1]=='a'&&p[pos+2]=='t'&&p[pos+3]=='a') {
            i.dataOff = pos + 8;
            i.dataLen = (size_t)cSz < sz - i.dataOff ? (size_t)cSz : sz - i.dataOff;
            hasData = true;
        }
        if (hasFmt && hasData) break;
        if (cSz == 0 || pos + 8 + static_cast<size_t>(cSz) > sz) break;
        pos += 8 + cSz + (cSz & 1); // RIFF chunks are word-aligned
    }
    i.valid = hasFmt && hasData && i.wfx.nChannels && i.wfx.nSamplesPerSec && i.dataLen;
    return i;
}

// Shared audio processing + playback: called from both PlayAudioUrl and PlayAudioRaw.
// Handles FUZ stripping, RIFF size patching, format detection, and playback.
static void PlayAudioBytes(std::string bytes, const std::string& ct)
{
    if (bytes.empty()) { logger::warn("SkyrimNetDashboard: PlayAudioBytes: empty, aborting"); return; }

    // Strip FUZ container (Bethesda: "FUZE" magic + lipSize(4) at offset 5 + WAV/XWM payload)
    if (bytes.size() >= 9 &&
            bytes[0] == 'F' && bytes[1] == 'U' && bytes[2] == 'Z' && bytes[3] == 'E') {
        uint32_t lipSize = 0;
        std::memcpy(&lipSize, bytes.data() + 5, 4);
        size_t audioOff = 9 + static_cast<size_t>(lipSize);
        if (audioOff < bytes.size()) {
            logger::info("SkyrimNetDashboard: FUZ stripped: lipSize={}, audioOffset={}", lipSize, audioOff);
            bytes.erase(0, audioOff);
        } else {
            logger::warn("SkyrimNetDashboard: FUZ header invalid, aborting");
            return;
        }
    }

    // Detect format from magic bytes
    bool isWav = false;
    if (bytes.size() >= 12 &&
            bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
            bytes[8] == 'W' && bytes[9] == 'A' && bytes[10] == 'V' && bytes[11] == 'E') {
        isWav = true;
    } else if (ct.find("wav") != std::string::npos || ct.find("vnd.wave") != std::string::npos) {
        isWav = true;
    }

    if (bytes.size() >= 12 &&
            bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
            bytes[8] == 'X' && bytes[9] == 'W' && bytes[10] == 'M' && bytes[11] == 'A') {
        logger::warn("SkyrimNetDashboard: XWM format not supported, aborting");
        return;
    }

    // Log first 16 bytes for diagnostics
    {
        std::string hex;
        for (size_t i = 0; i < std::min<size_t>(bytes.size(), 16); ++i) {
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02X ", static_cast<unsigned char>(bytes[i]));
            hex += buf;
        }
        logger::info("SkyrimNetDashboard: audio {} bytes, isWav={}, magic: {}", bytes.size(), isWav, hex);
    }

    // Patch streaming RIFF/WAV that uses 0xFFFFFFFF for unknown chunk sizes.
    if (isWav && bytes.size() >= 12) {
        uint32_t riffSize = 0; std::memcpy(&riffSize, bytes.data() + 4, 4);
        if (riffSize == 0xFFFFFFFFu) {
            uint32_t fixed = static_cast<uint32_t>(bytes.size() - 8);
            std::memcpy(bytes.data() + 4, &fixed, 4);
            logger::info("SkyrimNetDashboard: patched RIFF size -> {}", fixed);
        }
        // Walk sub-chunks and patch any 'data' chunk with unknown size
        size_t pos = 12;
        while (pos + 8 <= bytes.size()) {
            uint32_t chunkSize = 0; std::memcpy(&chunkSize, bytes.data() + pos + 4, 4);
            if (bytes[pos]=='d' && bytes[pos+1]=='a' && bytes[pos+2]=='t' && bytes[pos+3]=='a'
                    && chunkSize == 0xFFFFFFFFu) {
                uint32_t fixed = static_cast<uint32_t>(bytes.size() - pos - 8);
                std::memcpy(bytes.data() + pos + 4, &fixed, 4);
                logger::info("SkyrimNetDashboard: patched data chunk size -> {}", fixed);
                break;
            }
            if (chunkSize == 0xFFFFFFFFu || pos + 8 + chunkSize > bytes.size()) break;
            pos += 8 + chunkSize + (chunkSize & 1); // RIFF chunks are word-aligned
        }
    }

    // Claim a generation slot.  Do NOT call StopAudio() here — that bumps the
    // generation counter a second time and would immediately invalidate myGen.
    // Instead, stop+close the previous segment inline under s_mciMtx.
    uint32_t myGen = ++s_audioGen;

    auto mciErr = [](MCIERROR e) -> std::string {
        if (!e) return "ok"; char b[256] = {}; mciGetErrorStringA(e, b, 256); return b; };

    char tempDir[MAX_PATH] = {}; GetTempPathA(MAX_PATH, tempDir);
    std::string tempDirS = tempDir;

    if (isWav) {
        // ── waveOut in-memory path (no temp file) ─────────────────────────
        // Playing directly from the bytes buffer avoids writing a temp file,
        // which sidesteps the EACCES failures caused by Windows Defender (or
        // the MCI driver itself) holding a file lock on snpd_audio.wav after
        // the previous segment finished.
        auto wi = ParseWavHeader(bytes);
        if (wi.valid) {
            {
                std::lock_guard<std::mutex> lk(s_mciMtx);
                // Clean up any previous waveOut session that wasn't closed yet
                // (e.g. pre-empted by a rapid ClearAudioQueue call).
                if (s_hwo) {
                    waveOutReset(s_hwo);
                    waveOutUnprepareHeader(s_hwo, &s_hwoHdr, sizeof(WAVEHDR));
                    s_hwoHdr = {};
                    waveOutClose(s_hwo); s_hwo = nullptr;
                }
                // Also close any lingering MCI device (fallback path or prior session).
                mciSendStringA("stop snpd wait", nullptr, 0, nullptr);
                mciSendStringA("close snpd wait", nullptr, 0, nullptr);

                MMRESULT mo = waveOutOpen(&s_hwo, WAVE_MAPPER, &wi.wfx, 0, 0, CALLBACK_NULL);
                logger::info("SkyrimNetDashboard: waveOut open gen={}: {}",
                    myGen, mo == MMSYSERR_NOERROR ? "ok" : std::to_string(mo));
                if (mo != MMSYSERR_NOERROR) { s_hwo = nullptr; return; }

                s_hwoHdr = {};
                s_hwoHdr.lpData         = const_cast<char*>(bytes.data() + wi.dataOff);
                s_hwoHdr.dwBufferLength = static_cast<DWORD>(wi.dataLen);
                waveOutPrepareHeader(s_hwo, &s_hwoHdr, sizeof(WAVEHDR));
                waveOutWrite(s_hwo, &s_hwoHdr, sizeof(WAVEHDR));
                logger::info("SkyrimNetDashboard: waveOut play gen={} pcm_bytes={}",
                    myGen, wi.dataLen);
            }

            // Poll until the buffer completes, we are pre-empted (gen bump), or
            // a 30-min safety cap is hit.  While paused, waveOut holds its position
            // and WHDR_DONE is never set, so we sleep longer to avoid spinning.
            // ClearAudioQueue bumps s_audioGen which breaks the loop immediately.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            for (int i = 0; i < 36000; ++i) {  // 36 000 × 50 ms ≈ 30 min max
                if (s_audioGen.load() != myGen) break; // pre-empted by stop/clear
                if (s_aqPaused.load()) {
                    // Paused via waveOutPause — just wait, don't check WHDR_DONE.
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(s_mciMtx);
                    done = s_hwo && (s_hwoHdr.dwFlags & WHDR_DONE);
                }
                if (done) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Unconditional cleanup — bytes must remain alive until here because
            // s_hwoHdr.lpData points directly into bytes.data().
            {
                std::lock_guard<std::mutex> lk(s_mciMtx);
                if (s_hwo) {
                    waveOutUnprepareHeader(s_hwo, &s_hwoHdr, sizeof(WAVEHDR));
                    s_hwoHdr = {};
                    waveOutClose(s_hwo); s_hwo = nullptr;
                }
            }
            logger::info("SkyrimNetDashboard: waveOut playback done gen={}", myGen);

            // Notify JS (same logic as the MCI path below).
            if (s_audioGen.load() == myGen && s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
                s_PrismaUI->Invoke(s_View,
                    "try{"
                    "var _fr=document.getElementById('snpd-frame');"
                    "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
                    "_fr.contentWindow.__onAudioEnded();"
                    "}catch(e){}",
                    nullptr);
                logger::info("SkyrimNetDashboard: __onAudioEnded dispatched gen={}", myGen);
            } else {
                logger::info("SkyrimNetDashboard: __onAudioEnded skipped gen={} current={}",
                    myGen, s_audioGen.load());
            }
            return;
        }
        // WAV but non-PCM fmt (rare) — fall through to MCI with temp file.
    }

    // ── MCI path: non-WAV (transcoded) or non-PCM WAV ─────────────────────
    {
        std::string playPath;
        if (isWav) {
            // WAV but ParseWavHeader failed (non-PCM fmt chunk).
            // Use a per-generation filename so we never collide with a file
            // that a previous MCI session may still hold open.
            playPath = tempDirS + "snpd_audio_" + std::to_string(myGen) + ".wav";
            logger::info("SkyrimNetDashboard: writing WAV {} bytes to '{}' gen={}", bytes.size(), playPath, myGen);
            std::ofstream f(playPath, std::ios::binary);
            if (!f) { logger::warn("SkyrimNetDashboard: temp WAV open failed"); return; }
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        } else {
            // Non-WAV (MP3, AAC, …).  Write to a temp source file, then transcode to
            // PCM WAV via Media Foundation.  This avoids the DirectShow-backed
            // "mpegvideo" MCI driver entirely: that driver requires the calling thread
            // to pump a Win32 message queue (STA requirement), but our background
            // thread has no message loop — so mciSendStringA("play mpegvideo wait")
            // deadlocks, and even async "play" can freeze if the MCI driver window was
            // created on the game thread (which never pumps Win32 messages).
            std::string srcPath  = tempDirS + "snpd_audio_src.mp3";
            std::string dstPath  = tempDirS + "snpd_audio_" + std::to_string(myGen) + ".wav";
            logger::info("SkyrimNetDashboard: writing non-WAV {} bytes to '{}' gen={}", bytes.size(), srcPath, myGen);
            {
                std::ofstream f(srcPath, std::ios::binary);
                if (!f) { logger::warn("SkyrimNetDashboard: temp src open failed"); return; }
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
            // Widen paths for Media Foundation (requires WCHAR)
            auto toWide = [](const std::string& s) -> std::wstring {
                int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                std::wstring w(n - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
                return w;
            };
            if (!TranscodeToWav(toWide(srcPath), toWide(dstPath))) {
                logger::warn("SkyrimNetDashboard: TranscodeToWav failed gen={}", myGen);
                return;
            }
            playPath = dstPath;
            logger::info("SkyrimNetDashboard: transcoded to WAV '{}'", playPath);
        }

        {
            std::lock_guard<std::mutex> lk(s_mciMtx);
            // Stop whatever the previous segment opened, then open this segment.
            mciSendStringA("stop snpd wait", nullptr, 0, nullptr);
            mciSendStringA("close snpd wait", nullptr, 0, nullptr);

            // Always open as waveaudio — mpegvideo is never used.
            std::string oc = "open \"" + playPath + "\" type waveaudio alias snpd";
            MCIERROR oe = mciSendStringA(oc.c_str(), nullptr, 0, nullptr);
            logger::info("SkyrimNetDashboard: MCI open gen={}: {}", myGen, mciErr(oe));
            if (oe) return;

            MCIERROR pe = mciSendStringA("play snpd", nullptr, 0, nullptr);
            logger::info("SkyrimNetDashboard: MCI play async gen={}: {}", myGen, mciErr(pe));
            if (pe) {
                mciSendStringA("close snpd wait", nullptr, 0, nullptr);
                return;
            }
        }

        // Poll until MCI reports the track has stopped, we are pre-empted by a newer
        // segment, or a 30-minute safety cap is hit.  Sleep 200 ms first so MCI has
        // time to transition from "not ready" / "opening" to "playing".
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (int i = 0; i < 12000; ++i) {  // 12 000 × 150 ms ≈ 30 min max
            if (s_audioGen.load() != myGen) break; // pre-empted
            char modeBuf[64] = {};
            mciSendStringA("status snpd mode", modeBuf, sizeof(modeBuf), nullptr);
            if (std::string(modeBuf) != "playing") break; // stopped naturally or error
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        // Close our device only if we still own this generation slot.
        // If a newer segment pre-empted us it already stopped+closed snpd.
        {
            std::lock_guard<std::mutex> lk(s_mciMtx);
            if (s_audioGen.load() == myGen)
                mciSendStringA("close snpd wait", nullptr, 0, nullptr);
        }
        // Clean up per-generation temp file (best-effort; might still be locked
        // by the MCI driver for a moment — that is fine since it has its own name).
        DeleteFileA(playPath.c_str());
    }
    logger::info("SkyrimNetDashboard: MCI playback done gen={}", myGen);

    // Notify JS only for the most recently requested segment.
    // The audio bridge (and _paActive) live in the IFRAME's window context
    // (injected via InjectPatches), not in the shell window context that
    // Invoke() targets.  Reach through snpd-frame's contentWindow so the
    // 'ended' events fire on the correct Audio instances.
    if (s_audioGen.load() == myGen && s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
        s_PrismaUI->Invoke(s_View,
            "try{"
            "var _fr=document.getElementById('snpd-frame');"
            "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
            "_fr.contentWindow.__onAudioEnded();"
            "}catch(e){}",
            nullptr);
        logger::info("SkyrimNetDashboard: __onAudioEnded dispatched gen={}", myGen);
    } else {
        logger::info("SkyrimNetDashboard: __onAudioEnded skipped gen={} current={}",
            myGen, s_audioGen.load());
    }
}

static void PlayAudioUrl(std::string url)
{
    std::thread([url = std::move(url)]() {
        logger::info("SkyrimNetDashboard: audio play requested: {}", url);

        auto [host, port, path] = AudioParseUrl(url);
        logger::info("SkyrimNetDashboard: audio resolved -> host='{}' port={} path='{}'", host, port, path);
        if (host.empty() || path.empty()) {
            logger::warn("SkyrimNetDashboard: audio bad url, aborting");
            return;
        }

        auto [bytes, ct, audioSc] = FetchResource(host, port, "GET", path);
        (void)audioSc;
        logger::info("SkyrimNetDashboard: audio fetch -> {} bytes, content-type='{}'", bytes.size(), ct);
        if (bytes.empty()) {
            logger::warn("SkyrimNetDashboard: audio fetch returned empty body, aborting");
            return;
        }

        PlayAudioBytes(std::move(bytes), ct);
    }).detach();
}

// Called from the /audio-raw endpoint -- JS fetched blob bytes and POSTed them
// as raw binary so blob: URLs (e.g. TTS) work despite being browser-only objects.
static void PlayAudioRaw(std::string bytes, std::string ct)
{
    std::thread([bytes = std::move(bytes), ct = std::move(ct)]() mutable {
        logger::info("SkyrimNetDashboard: audio-raw {} bytes, ct='{}'", bytes.size(), ct);
        PlayAudioBytes(std::move(bytes), ct);
    }).detach();
}

// ── Streaming audio queue implementation ─────────────────────────────────────

static std::string Base64Decode(const std::string& s)
{
    // Standard RFC 4648 base64 decoder — ignores whitespace and padding.
    static const signed char kT[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::string out;
    out.reserve(s.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        int i = kT[c];
        if (i < 0) continue;
        val = (val << 6) | i;
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// Clear the audio queue and stop whatever is currently playing.
// Called when a new TTS stream session begins so stale segments don't leak.
static void ClearAudioQueue()
{
    {
        std::lock_guard<std::mutex> lk(s_aqMtx);
        s_aqPending.clear();
        s_aqPaused.store(false);
    }
    s_aqCv.notify_all();
    ++s_audioGen; // stop any segment currently being played by the runner
    {
        std::lock_guard<std::mutex> lk(s_mciMtx);
        if (s_hwo) waveOutReset(s_hwo); // immediately marks buffer DONE so poll exits
        mciSendStringA("stop snpd wait", nullptr, 0, nullptr);
        mciSendStringA("close snpd wait", nullptr, 0, nullptr);
    }
    {
        std::lock_guard<std::mutex> lk(s_diaryMtx);
        s_diaryPcm.clear();
        s_diaryFmtValid = false;
    }
    logger::info("SkyrimNetDashboard: audio queue cleared");
}

static void PauseAudioQueue()
{
    // Use waveOutPause (not waveOutReset) so the hardware keeps its exact byte
    // position.  The polling loop in PlayAudioBytes just keeps sleeping because
    // WHDR_DONE is never set while paused — no gen bump needed.  On resume,
    // waveOutRestart continues from the same position and the segment finishes
    // naturally, causing __onAudioEnded to fire in-sync for text display.
    {
        std::lock_guard<std::mutex> lk(s_mciMtx);
        if (s_hwo) waveOutPause(s_hwo);
        mciSendStringA("pause snpd wait", nullptr, 0, nullptr); // for MCI fallback path
    }
    s_aqPaused.store(true);
    logger::info("SkyrimNetDashboard: audio queue paused");
}

static void ResumeAudioQueue()
{
    {
        std::lock_guard<std::mutex> lk(s_mciMtx);
        if (s_hwo) waveOutRestart(s_hwo); // continue from exact pause position
        mciSendStringA("resume snpd", nullptr, 0, nullptr); // for MCI fallback path
    }
    s_aqPaused.store(false);
    s_aqCv.notify_all();
    logger::info("SkyrimNetDashboard: audio queue resumed");
}

static void PushToAudioQueue(std::string wavBytes)
{
    {
        std::lock_guard<std::mutex> lk(s_aqMtx);
        s_aqPending.push_back(std::move(wavBytes));
    }
    s_aqCv.notify_one();
}

// Permanent background thread that plays one queued WAV segment at a time.
// PlayAudioBytes blocks until the segment is done, then fires __onAudioEnded
// so SkyrimNet's JS r() chain can advance to the next segment.
static void AudioQueueRunner()
{
    logger::info("SkyrimNetDashboard: AudioQueueRunner started");
    while (s_aqRunning.load()) {
        std::string wavBytes;
        {
            std::unique_lock<std::mutex> lk(s_aqMtx);
            // Wait until: (has segments AND not paused) OR shutdown.
            s_aqCv.wait(lk, []() {
                return (!s_aqPending.empty() && !s_aqPaused.load()) || !s_aqRunning.load();
            });
            if (!s_aqRunning.load()) break;
            if (s_aqPaused.load() || s_aqPending.empty()) continue;
            wavBytes = std::move(s_aqPending.front());
            s_aqPending.pop_front();
        }
        if (!wavBytes.empty())
            PlayAudioBytes(std::move(wavBytes), "audio/wav");
    }
    logger::info("SkyrimNetDashboard: AudioQueueRunner stopped");
}

static void EnsureAudioQueueRunning()
{
    if (!s_aqRunning.exchange(true))
        std::thread(AudioQueueRunner).detach();
}

// Fire-and-forget: response is ignored by the browser; __onAudioEnded is
// dispatched later via Invoke() once C++ playback finishes.
static void OnAudioMessage(const char* json)
{
    if (!json || json[0] == '\0') {
        logger::warn("SkyrimNetDashboard: OnAudioMessage called with empty json");
        return;
    }
    logger::info("SkyrimNetDashboard: OnAudioMessage json='{}'", json);
    std::string action = AudioJsonField(json, "action");
    std::string src    = AudioJsonField(json, "src");
    logger::info("SkyrimNetDashboard: audio action='{}' src='{}'", action, src);

    if (action == "play" && !src.empty()) {
        std::thread([s = src]() { PlayAudioUrl(s); }).detach();
    } else if (action == "pause" || action == "stop") {
        // When the C++ audio queue is active (streaming TTS), pause it so the
        // runner thread doesn't immediately start the next segment.  For plain
        // audio (non-streaming), StopAudio() is sufficient.
        if (s_aqRunning.load())
            PauseAudioQueue();
        else
            StopAudio();
    } else {
        logger::warn("SkyrimNetDashboard: audio unrecognised action='{}' src='{}'", action, src);
    }
}

static void SetupLog()
{
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) return;
    auto logPath = *logsFolder / std::filesystem::path(PLUGIN_NAME).replace_extension(".log");
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    auto logger = std::make_shared<spdlog::logger>(PLUGIN_NAME, fileSink);
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

static void OnToggleInspector()
{
    if (!s_PrismaUI || !s_PrismaUI->IsValid(s_View)) return;

    if (!s_PrismaUI->IsInspectorVisible(s_View)) {
        s_PrismaUI->CreateInspectorView(s_View);
        s_PrismaUI->SetInspectorBounds(s_View, 0.0f, 600.0f, 1920, 400);
        s_PrismaUI->SetInspectorVisibility(s_View, true);
        logger::info("SkyrimNetDashboard: Inspector opened.");
    } else {
        s_PrismaUI->SetInspectorVisibility(s_View, false);
        logger::info("SkyrimNetDashboard: Inspector closed.");
    }
}

// JS snippets injected into the shell to signal the iframe's page that
// visibility has changed.  React, SWR, WebSocket-based libs, etc. all
// listen to visibilitychange and throttle themselves when hidden.
static constexpr const char* JS_HIDE =
    "(function(){"
    "var fr=document.getElementById('snpd-frame');"
    "if(!fr||!fr.contentDocument)return;"
    "var d=fr.contentDocument;"
    "Object.defineProperty(d,'hidden',{get:function(){return true;},configurable:true});"
    "Object.defineProperty(d,'visibilityState',{get:function(){return 'hidden';},configurable:true});"
    "d.dispatchEvent(new Event('visibilitychange'));"
    "})();";

static constexpr const char* JS_SHOW =
    "(function(){"
    "var fr=document.getElementById('snpd-frame');"
    "if(!fr||!fr.contentDocument)return;"
    "var d=fr.contentDocument;"
    "Object.defineProperty(d,'hidden',{get:function(){return false;},configurable:true});"
    "Object.defineProperty(d,'visibilityState',{get:function(){return 'visible';},configurable:true});"
    "d.dispatchEvent(new Event('visibilitychange'));"
    "})();";

static void OnToggle()
{
    if (!s_PrismaUI || !s_PrismaUI->IsValid(s_View)) {
        logger::warn("SkyrimNetDashboard: view is not valid.");
        return;
    }

    if (s_PrismaUI->IsHidden(s_View)) {
        // Show, restore visibility state, then focus
        s_PrismaUI->Show(s_View);
        s_PrismaUI->Invoke(s_View, JS_SHOW);
        [[maybe_unused]] bool focused = s_PrismaUI->Focus(s_View, s_cfg.pauseGame);
        logger::info("SkyrimNetDashboard: opened.");
    }
    else if (s_PrismaUI->HasFocus(s_View)) {
        // Signal hidden, unfocus, optionally hide depending on KeepBackground.
        // Do NOT stop audio here — diary TTS should continue playing in the
        // background while the player is in-game.  The in-player bottom-bar
        // pause button is the intended way to stop audio.
        s_PrismaUI->Invoke(s_View, JS_HIDE);
        s_PrismaUI->Unfocus(s_View);
        if (!s_cfg.keepBg)
            s_PrismaUI->Hide(s_View);
        logger::info("SkyrimNetDashboard: closed.");
    }
    else {
        // Visible but not focused — re-focus
        [[maybe_unused]] bool refocused = s_PrismaUI->Focus(s_View);
    }
}

static void OnClose()
{
    if (!s_PrismaUI || !s_PrismaUI->IsValid(s_View)) return;
    if (!s_PrismaUI->IsHidden(s_View)) {
        // Do NOT stop audio — see comment in OnToggle above.
        s_PrismaUI->Invoke(s_View, JS_HIDE);
        s_PrismaUI->Unfocus(s_View);
        if (!s_cfg.keepBg)
            s_PrismaUI->Hide(s_View);
        logger::info("SkyrimNetDashboard: closed via ESC.");
    }
}

// ── Embedded HTTP shell server ────────────────────────────────────────────────
// Serves a self-contained chrome page (topbar, border, drag) with the SkyrimNet
// dashboard inside an <iframe>.  The outer shell NEVER navigates so Ultralight
// never tears down and rebuilds the chrome — no flash, no hide/show hacks needed.

static std::string BuildShellHtml()
{
    // Raw HTML+CSS+JS for the persistent window chrome.
    // The iframe loads from /proxy (same origin) so contentWindow patching works.
    std::string html = R"SHELL(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%;background:transparent;overflow:hidden}
#W{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:2000px;max-width:95vw;height:1200px;max-height:95vh;display:flex;flex-direction:column;background:#111827;border:2px solid #444;border-radius:8px;box-shadow:0 0 30px rgba(0,0,0,.8);z-index:99999}
#B{display:flex;align-items:center;justify-content:space-between;padding:8px 16px;background:#1f2937;border-bottom:1px solid #374151;border-radius:8px 8px 0 0;user-select:none;-webkit-user-select:none;cursor:grab}
#L{display:flex;align-items:center;gap:8px;color:#9ca3af;font-size:14px;font-family:Consolas,"Courier New",monospace;font-weight:600;pointer-events:none}
#R{display:flex;align-items:center;gap:8px}
.btn{display:flex;align-items:center;gap:4px;padding:4px 12px;background:#374151;border:none;border-radius:4px;color:#fff;font-size:12px;font-family:Consolas,"Courier New",monospace;cursor:pointer;pointer-events:auto}
.btn.icon{padding:4px 7px}
.btn:hover{background:#4b5563}
#XB{display:flex;align-items:center;padding:4px 8px;background:#dc2626;border:none;border-radius:4px;color:#fff;cursor:pointer}
#XB:hover{background:#ef4444}
#C{flex:1;overflow:hidden;min-height:0;border-radius:0 0 6px 6px;position:relative}
iframe{width:100%;height:100%;border:none;display:block}
#OL{display:none;position:absolute;inset:0;z-index:1;cursor:grabbing}
.rh{position:absolute;z-index:200}
.rh[data-r=n]{top:-5px;left:12px;right:12px;height:6px;cursor:n-resize}
.rh[data-r=s]{bottom:-5px;left:12px;right:12px;height:10px;cursor:s-resize}
.rh[data-r=e]{right:-5px;top:12px;bottom:12px;width:10px;cursor:e-resize}
.rh[data-r=w]{left:-5px;top:12px;bottom:12px;width:10px;cursor:w-resize}
.rh[data-r=nw]{top:-5px;left:-5px;width:10px;height:10px;cursor:nw-resize}
.rh[data-r=ne]{top:-5px;right:-5px;width:10px;height:10px;cursor:ne-resize}
.rh[data-r=se]{bottom:-5px;right:-5px;width:18px;height:18px;cursor:se-resize}
.rh[data-r=sw]{bottom:-5px;left:-5px;width:14px;height:14px;cursor:sw-resize}
#W.fs .rh{display:none}
/* Persistent faint L-brackets in each corner */
.rh[data-r=se]::before,.rh[data-r=sw]::before,.rh[data-r=ne]::before,.rh[data-r=nw]::before{content:'';position:absolute;width:7px;height:7px}
.rh[data-r=se]::before{bottom:3px;right:3px;border-right:2px solid rgba(156,163,175,.35);border-bottom:2px solid rgba(156,163,175,.35)}
.rh[data-r=sw]::before{bottom:3px;left:3px;border-left:2px solid rgba(156,163,175,.35);border-bottom:2px solid rgba(156,163,175,.35)}
.rh[data-r=ne]::before{top:3px;right:3px;border-right:2px solid rgba(156,163,175,.35);border-top:2px solid rgba(156,163,175,.35)}
.rh[data-r=nw]::before{top:3px;left:3px;border-left:2px solid rgba(156,163,175,.35);border-top:2px solid rgba(156,163,175,.35)}
/* Hover: semi-transparent fill + bright indicators */
.rh:hover{background:rgba(99,102,241,.15)}
.rh[data-r=se]:hover::before,.rh[data-r=sw]:hover::before,.rh[data-r=ne]:hover::before,.rh[data-r=nw]:hover::before{border-color:rgba(99,102,241,.95)}
.rh[data-r=n]:hover::after,.rh[data-r=s]:hover::after{content:'';position:absolute;left:calc(50% - 18px);top:calc(50% - 1.5px);width:36px;height:3px;border-radius:2px;background:rgba(99,102,241,.85)}
.rh[data-r=e]:hover::after,.rh[data-r=w]:hover::after{content:'';position:absolute;top:calc(50% - 18px);left:calc(50% - 1.5px);width:3px;height:36px;border-radius:2px;background:rgba(99,102,241,.85)}
</style>
</head>
<body>
<div id="W">
  <div class="rh" data-r="n"></div><div class="rh" data-r="s"></div>
  <div class="rh" data-r="e"></div><div class="rh" data-r="w"></div>
  <div class="rh" data-r="nw"></div><div class="rh" data-r="ne"></div>
  <div class="rh" data-r="se"></div><div class="rh" data-r="sw"></div>
  <div id="B">
    <div id="L">
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="5 9 2 12 5 15"/><polyline points="9 5 12 2 15 5"/><polyline points="15 19 12 22 9 19"/><polyline points="19 9 22 12 19 15"/><line x1="2" y1="12" x2="22" y2="12"/><line x1="12" y1="2" x2="12" y2="22"/></svg>
      <span>SkyrimNet</span>
      <button class="btn icon" id="HB" title="Home"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/></svg></button>
      <button class="btn icon" id="RB" title="Refresh"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg></button>
    </div>
    <div id="R">
      <button class="btn" id="ZL" title="Reset zoom" style="font-size:11px;min-width:38px;">100%</button>
      <button class="btn icon" id="SB" title="Settings"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg></button>
      <button class="btn" id="FB"></button>
      <button id="XB"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg></button>
    </div>
  </div>
  <div id="C"><div id="OL"></div><iframe id="snpd-frame" src="/proxy"></iframe></div>
</div>
)SHELL"
    R"SHELL2(
<script>
(function(){
  var W=document.getElementById('W'),
      B=document.getElementById('B'),
      FB=document.getElementById('FB'),
      XB=document.getElementById('XB'),
      HB=document.getElementById('HB'),
      RB=document.getElementById('RB'),
      SB=document.getElementById('SB'),
      ZL=document.getElementById('ZL'),
      OL=document.getElementById('OL'),
      FR=document.getElementById('snpd-frame');
  var fs=localStorage.getItem('snpd-fs')==='true';
  var zoom=parseFloat(localStorage.getItem('snpd-zoom')||'1');
  if(isNaN(zoom)||zoom<0.20||zoom>3)zoom=1;
  function applyZoom(){
    FR.style.transformOrigin='top left';
    FR.style.transform='scale('+zoom+')';
    FR.style.width=(100/zoom)+'%';
    FR.style.height=(100/zoom)+'%';
    ZL.textContent=Math.round(zoom*100)+'%';
    localStorage.setItem('snpd-zoom',String(zoom));
  }
  applyZoom();
  var drag=false,ox=0,oy=0;
  var MAX_SVG='<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/><line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/></svg>';
  var MIN_SVG='<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/><line x1="10" y1="14" x2="21" y2="3"/><line x1="3" y1="21" x2="14" y2="10"/></svg>';
  function applyFs(){
    if(fs){
      W.style.cssText='position:fixed;top:0;left:0;width:100vw;height:100vh;max-width:100vw;max-height:100vh;transform:none;display:flex;flex-direction:column;background:#111827;border:none;border-radius:0;box-shadow:none;z-index:99999';
      W.classList.add('fs');
      B.style.borderRadius='0';B.style.cursor='default';
      FB.innerHTML=MIN_SVG+'<span>Windowed</span>';
    } else {
      W.style.cssText='position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:2000px;max-width:95vw;height:1200px;max-height:95vh;display:flex;flex-direction:column;background:#111827;border:2px solid #444;border-radius:8px;box-shadow:0 0 30px rgba(0,0,0,.8);z-index:99999';
      W.classList.remove('fs');
      B.style.borderRadius='8px 8px 0 0';B.style.cursor='grab';
      FB.innerHTML=MAX_SVG+'<span>Fullscreen</span>';
      var sx=localStorage.getItem('snpd-x'),sy=localStorage.getItem('snpd-y');
      if(sx&&sy){W.style.transform='none';W.style.left=sx;W.style.top=sy;}
      var sw=localStorage.getItem('snpd-w'),sh=localStorage.getItem('snpd-h');
      if(sw&&sh){W.style.width=sw;W.style.height=sh;W.style.maxWidth='none';W.style.maxHeight='none';}
    }
  }
  applyFs();
  FB.addEventListener('mousedown',function(e){e.stopPropagation();});
  FB.addEventListener('click',function(e){
    e.stopPropagation();
    fs=!fs;localStorage.setItem('snpd-fs',String(fs));applyFs();
  });
  B.addEventListener('dblclick',function(e){
    fs=!fs;localStorage.setItem('snpd-fs',String(fs));applyFs();
  });
  HB.addEventListener('mousedown',function(e){e.stopPropagation();});
  HB.addEventListener('click',function(e){
    e.stopPropagation();
    FR.src='/proxy';
  });
  RB.addEventListener('mousedown',function(e){e.stopPropagation();});
  RB.addEventListener('click',function(e){
    e.stopPropagation();
    FR.contentWindow.location.reload();
  });
  ZL.addEventListener('mousedown',function(e){e.stopPropagation();});
  ZL.addEventListener('click',function(e){
    e.stopPropagation();
    zoom=1;applyZoom();
  });
  // Ctrl+Scroll zoom
  document.addEventListener('wheel',function(e){
    if(!e.ctrlKey)return;
    e.preventDefault();
    var delta=e.deltaY<0?0.1:-0.1;
    zoom=Math.min(3,Math.max(0.20,Math.round((zoom+delta)*100)/100));
    applyZoom();
  },{passive:false});
  // Ctrl+Plus / Ctrl+Minus / Ctrl+0 keyboard zoom
  document.addEventListener('keydown',function(e){
    if(!e.ctrlKey)return;
    if(e.key==='='||e.key==='+'||e.keyCode===187||e.keyCode===107){
      e.preventDefault();zoom=Math.min(3,Math.round((zoom+0.1)*100)/100);applyZoom();
    } else if(e.key==='-'||e.keyCode===189||e.keyCode===109){
      e.preventDefault();zoom=Math.max(0.20,Math.round((zoom-0.1)*100)/100);applyZoom();
    } else if(e.key==='0'||e.keyCode===48||e.keyCode===96){
      e.preventDefault();zoom=1;applyZoom();
    }
  },true);
  // ── Settings modal ──────────────────────────────────────────────────────────
  var STMO=null; // settings modal overlay element
  function snpdBuildModal(cfg){
    var ov=document.createElement('div');
    ov.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.75);z-index:999999;display:flex;align-items:center;justify-content:center;';
    var box=document.createElement('div');
    box.style.cssText='background:#1f2937;border:1px solid #374151;border-radius:8px;padding:24px;min-width:340px;max-width:480px;width:90%;color:#e5e7eb;font-family:Consolas,"Courier New",monospace;font-size:13px;';
    var title=document.createElement('div');
    title.style.cssText='font-size:15px;font-weight:700;color:#f9fafb;margin-bottom:16px;border-bottom:1px solid #374151;padding-bottom:10px;';
    title.textContent='Dashboard Settings';
    box.appendChild(title);
    // Hotkey — click-to-capture box
    var hkRow=document.createElement('div');hkRow.style.cssText='margin-bottom:14px;';
    var hkLbl=document.createElement('label');hkLbl.style.cssText='display:block;margin-bottom:4px;color:#9ca3af;';hkLbl.textContent='Toggle Hotkey (click box, then press a key)';
    var hkCode=cfg.hotKey;
    var dxToName={};(cfg.keys||[]).forEach(function(k){dxToName[k.code]=k.name;});
    // event.code to DX (modern browsers)
    var C2D={F1:59,F2:60,F3:61,F4:62,F5:63,F6:64,F7:65,F8:66,F9:67,F10:68,F11:87,F12:88,
      Digit1:2,Digit2:3,Digit3:4,Digit4:5,Digit5:6,Digit6:7,Digit7:8,Digit8:9,Digit9:10,Digit0:11,
      KeyQ:16,KeyW:17,KeyE:18,KeyR:19,KeyT:20,KeyY:21,KeyU:22,KeyI:23,KeyO:24,KeyP:25,
      KeyA:30,KeyS:31,KeyD:32,KeyF:33,KeyG:34,KeyH:35,KeyJ:36,KeyK:37,KeyL:38,
      KeyZ:44,KeyX:45,KeyC:46,KeyV:47,KeyB:48,KeyN:49,KeyM:50,
      Backquote:41,Minus:12,Equal:13,BracketLeft:26,BracketRight:27,
      Semicolon:39,Quote:40,Backslash:86,Comma:51,Period:52,Slash:53,
      Home:199,Insert:210,Delete:211,PageUp:201,PageDown:209,
      ArrowUp:200,ArrowDown:208,ArrowLeft:203,ArrowRight:205,
      Numpad0:82,Numpad1:79,Numpad2:80,Numpad3:81,Numpad4:75,Numpad5:76,
      Numpad6:77,Numpad7:71,Numpad8:72,Numpad9:73,
      NumpadAdd:78,NumpadSubtract:74,NumpadMultiply:55,NumpadDivide:181};
    // event.key to DX (fallback for engines that don\'t populate event.code)
    var K2D={'F1':59,'F2':60,'F3':61,'F4':62,'F5':63,'F6':64,'F7':65,'F8':66,'F9':67,'F10':68,'F11':87,'F12':88,
      '1':2,'2':3,'3':4,'4':5,'5':6,'6':7,'7':8,'8':9,'9':10,'0':11,
      'q':16,'w':17,'e':18,'r':19,'t':20,'y':21,'u':22,'i':23,'o':24,'p':25,
      'a':30,'s':31,'d':32,'f':33,'g':34,'h':35,'j':36,'k':37,'l':38,
      'z':44,'x':45,'c':46,'v':47,'b':48,'n':49,'m':50,
      '`':41,'~':41,'-':12,'_':12,'=':13,'+':13,'[':26,']':27,
      ';':39,':':39,"'":40,'"':40,'\\':86,'|':86,',':51,'<':51,'.':52,'>':52,'/':53,'?':53,
      'Insert':210,'Delete':211,'Home':199,'End':207,'PageUp':201,'PageDown':209,
      'ArrowUp':200,'ArrowDown':208,'ArrowLeft':203,'ArrowRight':205};
    // event.keyCode to DX (last-resort fallback)
    var KC2D={112:59,113:60,114:61,115:62,116:63,117:64,118:65,119:66,120:67,121:68,122:87,123:88,
      49:2,50:3,51:4,52:5,53:6,54:7,55:8,56:9,57:10,48:11,
      81:16,87:17,69:18,82:19,84:20,89:21,85:22,73:23,79:24,80:25,
      65:30,83:31,68:32,70:33,71:34,72:35,74:36,75:37,76:38,
      90:44,88:45,67:46,86:47,66:48,78:49,77:50,
      96:82,97:79,98:80,99:81,100:75,101:76,102:77,103:71,104:72,105:73,
      107:78,109:74,106:55,111:181,
      45:210,46:211,36:199,35:207,33:201,34:209,38:200,40:208,37:203,39:205};
    var hkBox=document.createElement('div');
    hkBox.tabIndex=0;
    hkBox.textContent=dxToName[hkCode]||cfg.hotKeyName||'?';
    hkBox.style.cssText='width:100%;background:#111827;border:1px solid #374151;border-radius:4px;padding:6px 8px;color:#e5e7eb;cursor:pointer;box-sizing:border-box;user-select:none;outline:none;';
    function hkKeyDown(e){
      e.preventDefault();e.stopPropagation();
      var k=e.key||'';var dx=C2D[e.code]||K2D[k]||K2D[k.toLowerCase()]||KC2D[e.keyCode];
      if(dx!==undefined)hkCode=dx;
      hkBox.blur();
    }
    hkBox.addEventListener('focus',function(){hkBox.textContent='Press a key\u2026';hkBox.style.borderColor='#10b981';document.addEventListener('keydown',hkKeyDown,true);});
    hkBox.addEventListener('blur',function(){document.removeEventListener('keydown',hkKeyDown,true);hkBox.textContent=dxToName[hkCode]||('0x'+hkCode.toString(16));hkBox.style.borderColor='#374151';});
    hkRow.appendChild(hkLbl);hkRow.appendChild(hkBox);
    box.appendChild(hkRow);
    // Checkboxes
    function mkCheck(label,checked){
      var row=document.createElement('div');row.style.cssText='margin-bottom:10px;display:flex;align-items:center;gap:8px;';
      var cb=document.createElement('input');cb.type='checkbox';cb.checked=!!checked;cb.style.cssText='width:16px;height:16px;accent-color:#10b981;cursor:pointer;';
      var lbl=document.createElement('label');lbl.style.cssText='color:#d1d5db;cursor:pointer;';lbl.textContent=label;
      lbl.addEventListener('click',function(){cb.checked=!cb.checked;});
      row.appendChild(cb);row.appendChild(lbl);box.appendChild(row);return cb;
    }
    var cbKeepBg  =mkCheck('Keep menu open in background (stay rendered while closed)',cfg.keepBg);
    var cbDefHome =mkCheck('Default to Home page when opening',cfg.defaultHome);
    var cbPause   =mkCheck('Pause game while open',cfg.pauseGame);
    // Note
    var note=document.createElement('div');note.style.cssText='color:#6b7280;font-size:11px;margin-top:4px;margin-bottom:16px;';
    note.textContent='Settings are saved to the plugin INI file immediately.';
    box.appendChild(note);
    // Buttons
    var btnRow=document.createElement('div');btnRow.style.cssText='display:flex;gap:8px;justify-content:flex-end;';
    var cancelBtn=document.createElement('button');cancelBtn.textContent='Cancel';
    cancelBtn.style.cssText='background:#374151;border:1px solid #4b5563;color:#d1d5db;padding:7px 16px;border-radius:4px;cursor:pointer;font-family:inherit;font-size:13px;';
    var saveBtn=document.createElement('button');saveBtn.textContent='Save';
    saveBtn.style.cssText='background:#059669;border:none;color:#fff;padding:7px 16px;border-radius:4px;cursor:pointer;font-family:inherit;font-size:13px;font-weight:600;';
    cancelBtn.addEventListener('click',function(){if(STMO)document.body.removeChild(STMO);STMO=null;});
    saveBtn.addEventListener('click',function(){
      var payload={
        hotKey:hkCode,
        keepBg:cbKeepBg.checked,
        defaultHome:cbDefHome.checked,
        pauseGame:cbPause.checked
      };
      saveBtn.disabled=true;saveBtn.textContent='Saving...';
      fetch('/settings-save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
        .then(function(r){return r.json();})
        .then(function(j){
          if(j.saved){if(STMO)document.body.removeChild(STMO);STMO=null;}
          else{saveBtn.disabled=false;saveBtn.textContent='Save';}
        })
        .catch(function(){saveBtn.disabled=false;saveBtn.textContent='Save';});
    });
    btnRow.appendChild(cancelBtn);btnRow.appendChild(saveBtn);box.appendChild(btnRow);
    ov.appendChild(box);
    ov.addEventListener('mousedown',function(e){e.stopPropagation();});
    return ov;
  }
  SB.addEventListener('mousedown',function(e){e.stopPropagation();});
  SB.addEventListener('click',function(e){
    e.stopPropagation();
    if(STMO){document.body.removeChild(STMO);STMO=null;return;}
    fetch('/settings-get')
      .then(function(r){return r.json();})
      .then(function(cfg){STMO=snpdBuildModal(cfg);document.body.appendChild(STMO);})
      .catch(function(err){console.error('snpd settings-get failed',err);});
  });
  XB.addEventListener('mousedown',function(e){e.stopPropagation();});
  XB.addEventListener('click',function(e){
    e.stopPropagation();
    try{if(typeof window.closeDashboard==='function')window.closeDashboard('');}catch(_){}
  });
  B.addEventListener('mousedown',function(e){
    if(fs||e.button!==0)return;
    var r=W.getBoundingClientRect();
    W.style.transform='none';W.style.top=r.top+'px';W.style.left=r.left+'px';
    ox=e.clientX-r.left;oy=e.clientY-r.top;
    drag=true;OL.style.display='block';B.style.cursor='grabbing';e.preventDefault();
  });
  document.addEventListener('mousemove',function(e){
    if(!drag)return;
    W.style.left=(e.clientX-ox)+'px';W.style.top=(e.clientY-oy)+'px';
  });
  document.addEventListener('mouseup',function(){
    if(!drag)return;
    drag=false;OL.style.display='none';B.style.cursor='grab';
    localStorage.setItem('snpd-x',W.style.left);localStorage.setItem('snpd-y',W.style.top);
  });
  window.confirm=function(){return true;};
  window.alert=function(){};
  window.prompt=function(m,d){return d!==undefined?d:'';}
  // Belt-and-suspenders: patch the iframe's window object directly (same origin).
  // Ultralight may route iframe dialogs through the top-level window context,
  // so overriding here catches those cases too.
  var snpdFr=FR;
  function snpdPatch(){try{var cw=snpdFr.contentWindow;if(!cw)return;cw.confirm=function(){return true;};cw.alert=function(){};cw.prompt=function(m,d){return d!==undefined?d:'';};cw.open=function(url){if(url)cw.location.href=url;return cw;};cw.document.addEventListener('wheel',function(e){if(!e.ctrlKey)return;e.preventDefault();e.stopPropagation();var delta=e.deltaY<0?0.1:-0.1;zoom=Math.min(3,Math.max(0.20,Math.round((zoom+delta)*100)/100));applyZoom();},{passive:false,capture:true});}catch(_){}}
  snpdFr.addEventListener('load',snpdPatch);

  // ── Resize handles ─────────────────────────────────────────────────────────
  var rDir=null,rSX=0,rSY=0,rRect=null;
  var RMIN_W=320,RMIN_H=200;
  document.querySelectorAll('.rh').forEach(function(h){
    h.addEventListener('mousedown',function(e){
      if(fs||e.button!==0)return;
      e.preventDefault();e.stopPropagation();
      rDir=h.dataset.r;rSX=e.clientX;rSY=e.clientY;
      var r=W.getBoundingClientRect();
      rRect={left:r.left,top:r.top,width:r.width,height:r.height};
      W.style.transform='none';W.style.left=r.left+'px';W.style.top=r.top+'px';
      W.style.width=r.width+'px';W.style.height=r.height+'px';
      W.style.maxWidth='none';W.style.maxHeight='none';
      OL.style.display='block';
    });
  });
  document.addEventListener('mousemove',function(e){
    if(!rDir)return;
    var dx=e.clientX-rSX,dy=e.clientY-rSY,r=rRect;
    var nL=r.left,nT=r.top,nW=r.width,nH=r.height;
    if(rDir.indexOf('e')>=0){nW=Math.max(RMIN_W,r.width+dx);}
    if(rDir.indexOf('w')>=0){var w2=Math.max(RMIN_W,r.width-dx);nL=r.left+(r.width-w2);nW=w2;}
    if(rDir.indexOf('s')>=0){nH=Math.max(RMIN_H,r.height+dy);}
    if(rDir.indexOf('n')>=0){var h2=Math.max(RMIN_H,r.height-dy);nT=r.top+(r.height-h2);nH=h2;}
    W.style.left=nL+'px';W.style.top=nT+'px';W.style.width=nW+'px';W.style.height=nH+'px';
  });
  document.addEventListener('mouseup',function(){
    if(!rDir)return;
    rDir=null;OL.style.display='none';
    localStorage.setItem('snpd-x',W.style.left);localStorage.setItem('snpd-y',W.style.top);
    localStorage.setItem('snpd-w',W.style.width);localStorage.setItem('snpd-h',W.style.height);
  });

})();
</script>
</body>
</html>
)SHELL2";

    return html;
}

// ── Full reverse-proxy helpers ────────────────────────────────────────────────
// All SkyrimNet assets are fetched through our local server so the iframe
// never makes cross-origin requests.  <script type="module"> in particular
// is always CORS-checked by WebKit regardless of the crossorigin attribute,
// so the only way to avoid it is to serve everything from the same origin.

// Infer a Content-Type header value from a URL path's file extension.
static std::string ContentTypeFromPath(const std::string& path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (ext == "js"  || ext == "mjs") return "application/javascript";
    if (ext == "css")                 return "text/css";
    if (ext == "html"|| ext == "htm") return "text/html; charset=utf-8";
    if (ext == "json")                return "application/json";
    if (ext == "svg")                 return "image/svg+xml";
    if (ext == "png")                 return "image/png";
    if (ext == "jpg" || ext == "jpeg")return "image/jpeg";
    if (ext == "gif")                 return "image/gif";
    if (ext == "woff")                return "font/woff";
    if (ext == "woff2")               return "font/woff2";
    if (ext == "ttf")                 return "font/ttf";
    if (ext == "ico")                 return "image/x-icon";
    return "application/octet-stream";
}

// Proxy an arbitrary request to host:port.  Forwards method + body; returns {responseBody, contentType, statusCode}.
static std::tuple<std::string,std::string,int>
FetchResource(const std::string& host, uint16_t port, const std::string& method,
              const std::string& path, const std::string& reqContentType,
              const std::string& reqBody)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return {"", "text/plain", 503};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    // inet_addr only handles dotted-decimal; resolve hostnames (e.g. "localhost") via gethostbyname
    auto ipAddr = inet_addr(host.c_str());
    if (ipAddr == INADDR_NONE) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) { closesocket(s); return {"", "text/plain", 503}; }
        ipAddr = *reinterpret_cast<u_long*>(he->h_addr_list[0]);
    }
    addr.sin_addr.s_addr = ipAddr;

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return {"", "text/plain", 503};
    }

    // Use HTTP/1.1 + Connection: close so the server streams freely but closes when done.
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    // Always include Content-Type and Content-Length for non-GET requests so that
    // the upstream server can correctly identify the request as a JSON API call,
    // even when the body is empty (e.g. "reload from disk" sends POST with no body).
    const bool hasBody = method != "GET" && method != "HEAD";
    if (hasBody) {
        std::string ct = reqContentType.empty() ? "application/json" : reqContentType;
        req += "Content-Type: " + ct + "\r\n";
        req += "Content-Length: " + std::to_string(reqBody.size()) + "\r\n";
    } else if (!reqBody.empty()) {
        if (!reqContentType.empty())
            req += "Content-Type: " + reqContentType + "\r\n";
        req += "Content-Length: " + std::to_string(reqBody.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += reqBody;
    send(s, req.c_str(), static_cast<int>(req.size()), 0);

    std::string response;
    char buf[65536];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
        response.append(buf, n);
    closesocket(s);

    // Separate headers from body
    auto sep = response.find("\r\n\r\n");
    std::string rawBody = (sep != std::string::npos) ? response.substr(sep + 4) : response;

    // Extract HTTP status code from response line (e.g. "HTTP/1.1 200 OK")
    int statusCode = 200;
    {
        auto sp1 = response.find(' ');
        auto sp2 = response.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos)
            try { statusCode = std::stoi(response.substr(sp1 + 1, sp2 - sp1 - 1)); } catch (...) {}
    }

    // Extract Content-Type and Transfer-Encoding from response headers
    std::string ct = ContentTypeFromPath(path);
    bool chunked = false;
    if (sep != std::string::npos) {
        std::string headers = response.substr(0, sep);
        std::string hdrlower; hdrlower.reserve(headers.size());
        for (auto c : headers) hdrlower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        auto ctPos = hdrlower.find("content-type:");
        if (ctPos != std::string::npos) {
            auto eol = headers.find("\r\n", ctPos);
            ct = headers.substr(ctPos + 13, eol - ctPos - 13);
            while (!ct.empty() && ct.front() == ' ') ct.erase(ct.begin());
        }
        chunked = hdrlower.find("transfer-encoding: chunked") != std::string::npos;
    }

    // Decode chunked transfer encoding if needed.
    // Format: <hex-size>\r\n<data>\r\n ... 0\r\n\r\n
    std::string body;
    if (chunked) {
        std::size_t pos = 0;
        while (pos < rawBody.size()) {
            auto crlf = rawBody.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            std::string szHex = rawBody.substr(pos, crlf - pos);
            // Strip chunk extensions (anything after a semicolon)
            auto semi = szHex.find(';');
            if (semi != std::string::npos) szHex = szHex.substr(0, semi);
            std::size_t chunkSize = 0;
            try { chunkSize = std::stoull(szHex, nullptr, 16); } catch (...) { break; }
            if (chunkSize == 0) break; // terminal chunk
            pos = crlf + 2;
            if (pos + chunkSize > rawBody.size()) break;
            body.append(rawBody, pos, chunkSize);
            pos += chunkSize + 2; // skip trailing \r\n
        }
    } else {
        body = std::move(rawBody);
    }

    return {body, ct, statusCode};
}

// ── Win32 clipboard helpers ──────────────────────────────────────────────────
static std::string GetClipboardTextW32()
{
    if (!OpenClipboard(nullptr)) return "";
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return ""; }
    auto* pWide = static_cast<wchar_t*>(GlobalLock(hData));
    std::string result;
    if (pWide) {
        int sz = WideCharToMultiByte(CP_UTF8, 0, pWide, -1, nullptr, 0, nullptr, nullptr);
        if (sz > 1) { result.resize(sz - 1); WideCharToMultiByte(CP_UTF8, 0, pWide, -1, result.data(), sz, nullptr, nullptr); }
        GlobalUnlock(hData);
    }
    CloseClipboard();
    return result;
}

static void SetClipboardTextW32(const std::string& text)
{
    int sz = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (sz <= 0) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(sz) * sizeof(wchar_t));
    if (!hMem) return;
    auto* pWide = static_cast<wchar_t*>(GlobalLock(hMem));
    if (!pWide) { GlobalFree(hMem); return; }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pWide, sz);
    GlobalUnlock(hMem);
    if (!OpenClipboard(nullptr)) { GlobalFree(hMem); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) GlobalFree(hMem); // OS owns hMem on success
    CloseClipboard();
}

// Injects compat patches (confirm/alert/prompt/open) and a fast-typing
// newline guard into an HTML document string.
static std::string InjectPatches(std::string body)
{
    // Build injection from modular components
    std::string injection = 
        Injections::GetEditorFixes() +
        "<script>\n" +
        Injections::GetClipboardIntegration() +
        Injections::GetAudioPolyfill() +
        Injections::GetDragSelect() +
        "</script>\n";

    std::string lower = body;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    auto headPos = lower.find("<head>");
    if (headPos != std::string::npos) {
        body.insert(headPos + 6, injection);
    } else {
        auto bodyPos = lower.find("<body");
        if (bodyPos != std::string::npos) {
            auto closeAngle = body.find('>', bodyPos);
            if (closeAngle != std::string::npos)
                body.insert(closeAngle + 1, injection);
            else
                body = injection + body;
        } else {
            body = injection + body;
        }
    }
    return body;
}

// Patches CSS bundles before serving them.
// Replaces prefers-color-scheme:dark media queries with @media all (Ultralight
// never fires the dark query, so Tailwind dark: variants would otherwise be dead).
// Also strips backdrop-filter declarations that Ultralight doesn't support.
static std::string PatchCSS(std::string body)
{
    // Ultralight never fires prefers-color-scheme:dark, so Tailwind's compiled
    // dark: variants are dead code. Replace the media query wrapper with
    // @media all so all dark: rules always apply. They sit later in the CSS
    // than their light counterparts, so they win the cascade automatically.
    {
        static const std::string needle   = "@media (prefers-color-scheme: dark)";
        static const std::string replaced = "@media all";
        std::string::size_type pos = 0;
        int count = 0;
        while ((pos = body.find(needle, pos)) != std::string::npos) {
            body.replace(pos, needle.size(), replaced);
            pos += replaced.size();
            ++count;
        }
        if (count)
            logger::info("SkyrimNetDashboard: PatchCSS: replaced {} dark media queries", count);
    }

    // Ultralight doesn't support backdrop-filter at all.
    // Strip every  backdrop-filter: ...; declaration so it can't affect layout.
    // (The CSS still contains --tw-backdrop-blur etc. custom properties and the
    // backdrop-filter: var(...) rule; removing those declarations is harmless.)
    {
        int count = 0;
        std::string::size_type pos = 0;
        static const std::string bfProp = "backdrop-filter:";
        while ((pos = body.find(bfProp, pos)) != std::string::npos) {
            // Find end of declaration (next semicolon)
            auto semi = body.find(';', pos);
            if (semi == std::string::npos) break;
            // Replace  backdrop-filter: ...;  with whitespace of the same length
            // so we don't shift any other positions in the file.
            std::fill(body.begin() + pos, body.begin() + semi + 1, ' ');
            pos = semi + 1;
            ++count;
        }
        if (count)
            logger::info("SkyrimNetDashboard: PatchCSS: stripped {} backdrop-filter declarations", count);
    }

    return body;
}

static std::string ReplaceAll(std::string body, const std::string& needle, const std::string& rep)
{
    std::string::size_type pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        body.replace(pos, needle.size(), rep);
        pos += rep.size();
    }
    return body;
}

static std::string PatchBundle(std::string body)
{
    // ── Playback banner: fix h.cardBg → h.colors.cardBg ──────────────────────
    // The diary playback banner accesses h.cardBg / h.border directly on the
    // theme object, but those live under h.colors.cardBg / h.colors.border.
    // Result: both class slots are undefined → fully transparent background.
    // Also remove the pointless backdrop-blur-lg (Ultralight ignores it).
    body = ReplaceAll(std::move(body),
        "fixed bottom-0 left-0 right-0 z-50 ${h.cardBg} ${h.border} border-t shadow-2xl backdrop-blur-lg",
        "fixed bottom-0 left-0 right-0 z-50 ${h.colors.cardBg} ${h.colors.border} border-t shadow-2xl");

    // ── glassEffect / cardBg opacity fix ─────────────────────────────────────
    // Ultralight doesn't support backdrop-filter, so classes like
    // bg-white/30 + backdrop-blur-lg render as near-transparent boxes (the blur
    // that was supposed to make them readable does nothing).
    // Fix: replace low-opacity glass colours with fully-opaque equivalents.
    // Light theme
    body = ReplaceAll(std::move(body),
        "glassEffect:\"bg-white/30 backdrop-blur-lg border-white/40\"",
        "glassEffect:\"bg-white border-gray-200\"");
    // Dark / matrix theme
    body = ReplaceAll(std::move(body),
        "glassEffect:\"bg-black/60 backdrop-blur-md border-green-500/20\"",
        "glassEffect:\"bg-gray-900 border-green-500/40\"");
    // cardBg light
    body = ReplaceAll(std::move(body),
        "cardBg:\"bg-white/80\"",
        "cardBg:\"bg-white\"");
    // cardBg dark
    body = ReplaceAll(std::move(body),
        "cardBg:\"bg-gray-900/95\"",
        "cardBg:\"bg-gray-900\"");
    // Modal overlay: keep it dark but remove the unsupported backdrop-blur
    body = ReplaceAll(std::move(body),
        "\"fixed inset-0 bg-black/50 backdrop-blur-sm flex items-center justify-center z-50\"",
        "\"fixed inset-0 bg-black/80 flex items-center justify-center z-50\"");
    {
        auto cnt = [](const std::string&) { return 0; }; (void)cnt;
        logger::info("SkyrimNetDashboard: PatchBundle: patched glassEffect/cardBg opacity");
    }

    // ── Log file download fix ─────────────────────────────────────────────────
    // The app's download handler creates a blob: URL then calls a.click() to
    // trigger a browser Save-As dialog. Ultralight doesn't support <a download>
    // and just navigates the frame to the blob URL, showing raw text.
    // We replace the anchor trick entirely: read the blob as an ArrayBuffer and
    // POST it directly to our /save-file proxy endpoint, which writes the file
    // to Documents\SkyrimNet Logs\ and returns JSON {saved,path}.  A toast
    // shows the saved path — no navigation ever happens.
    {
        static const std::string dlNeedle =
            "const s=await t.blob(),r=window.URL.createObjectURL(s),a=document.createElement(\"a\");a.href=r,a.download=e,document.body.appendChild(a),a.click(),window.URL.revokeObjectURL(r),document.body.removeChild(a)";
        static const std::string dlReplace =
            "const s=await t.blob();"
            "await(async function(){"
            "try{"
            // First ask C++ to open a native Save-As dialog
            "const dr=await fetch('/save-dialog?name='+encodeURIComponent(e));"
            "const dj=await dr.json();"
            "if(dj.cancelled)return;"
            // User chose a path — POST the bytes there
            "const ab=await s.arrayBuffer();"
            "const jr=await fetch('/save-file?path='+encodeURIComponent(dj.path),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:ab});"
            "const j=await jr.json();"
            "const d=document.createElement('div');"
            "d.style.cssText='position:fixed;bottom:20px;right:20px;z-index:2147483647;padding:10px 16px;border-radius:6px;font-size:12px;font-family:Consolas,monospace;color:#fff;pointer-events:none;max-width:420px;word-break:break-all;transition:opacity 0.4s;'+(j&&j.saved?'background:#16a34a;border:1px solid #15803d;':'background:#dc2626;border:1px solid #b91c1c;');"
            "d.textContent=j&&j.saved?'\\u2713 Saved: '+(j.path||e):'Save failed: '+(j.error||'unknown');"
            "document.body.appendChild(d);"
            "setTimeout(function(){d.style.opacity='0';setTimeout(function(){d.remove();},500);},3500);"
            "}catch(err){console.error('snpd save-file failed',err);}"
            "})()";
        auto p = body.find(dlNeedle);
        if (p != std::string::npos) {
            body.replace(p, dlNeedle.size(), dlReplace);
            logger::info("SkyrimNetDashboard: PatchBundle: patched log download anchor");
        } else {
            logger::warn("SkyrimNetDashboard: PatchBundle: log download anchor needle not found");
        }
    }

    // ── Diary audio: complete-message handler + download button ────────────────
    // The complete handler tries to create a blob and compress to MP3. t.audio
    // is now "" (stripped), so just redirect downloadUrl to /diary-audio.
    // We replace just the `b(s)` calls (which set downloadUrl) with `b("/diary-audio")`.
    {
        // First b(s) after MP3 compression
        static const std::string n1 = "URL.createObjectURL(t);b(s),f(\"mp3\")";
        static const std::string r1 = "URL.createObjectURL(t);b(\"/diary-audio\"),f(\"wav\")";
        auto p1 = body.find(n1);
        if (p1 != std::string::npos) {
            body.replace(p1, n1.size(), r1);
        }
        // Second b(s) in the catch fallback
        static const std::string n2 = "URL.createObjectURL(e);b(s),f(\"wav\")";
        static const std::string r2 = "URL.createObjectURL(e);b(\"/diary-audio\"),f(\"wav\")";
        auto p2 = body.find(n2);
        if (p2 != std::string::npos) {
            body.replace(p2, n2.size(), r2);
            logger::info("SkyrimNetDashboard: PatchBundle: complete-handler downloadUrl redirected");
        } else {
            logger::warn("SkyrimNetDashboard: PatchBundle: complete-handler needle not found");
        }
    }
    // Download button: replace the <a download>+click pattern with our
    // fetch → /save-dialog → /save-file flow (same as log downloads).
    // Needle is unique: the template-literal filename ending + click sequence.
    {
        static const std::string dlDiaryNeedle =
            R"(}_${s.id}.${r}`;e.download=a,document.body.appendChild(e),e.click(),document.body.removeChild(e)})";
        static const std::string dlDiaryReplace =
            R"(}_${s.id}.${r}`;)"
            "(async function(){"
            "try{"
            "const dr=await fetch('/save-dialog?name='+encodeURIComponent(a));"
            "const dj=await dr.json();"
            "if(dj.cancelled)return;"
            "const ab=await(await fetch(n)).arrayBuffer();"
            "const jr=await fetch('/save-file?path='+encodeURIComponent(dj.path),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:ab});"
            "const j=await jr.json();"
            "const d=document.createElement('div');"
            "d.style.cssText='position:fixed;bottom:20px;right:20px;z-index:2147483647;padding:10px 16px;border-radius:6px;font-size:12px;font-family:Consolas,monospace;color:#fff;pointer-events:none;max-width:420px;word-break:break-all;transition:opacity 0.4s;'+(j&&j.saved?'background:#16a34a;border:1px solid #15803d;':'background:#dc2626;border:1px solid #b91c1c;');"
            "d.textContent=j&&j.saved?'\\u2713 Saved: '+(j.path||a):'Save failed: '+(j.error||'unknown');"
            "document.body.appendChild(d);"
            "setTimeout(function(){d.style.opacity='0';setTimeout(function(){d.remove();},500);},3500);"
            R"(}catch(err){console.error('snpd diary save failed',err);}})()})";
        auto dp = body.find(dlDiaryNeedle);
        if (dp != std::string::npos) {
            body.replace(dp, dlDiaryNeedle.size(), dlDiaryReplace);
            logger::info("SkyrimNetDashboard: PatchBundle: diary download button patched");
        } else {
            logger::warn("SkyrimNetDashboard: PatchBundle: diary download button needle not found");
        }
    }

    // CodeMirror updateListener fires onChange(text) every keystroke via setTimeout(fn,0).
    // Patch: use a module-scoped timer variable (_snpdCmTmr) for a 600ms debounce.
    static const std::string needle =
        "e.docChanged){const t=e.state.doc.toString();setTimeout(()=>b(t),0)}";
    // Move doc.toString() inside the timer so it only fires once per typing burst,
    // not on every keystroke — avoids allocating a full doc string copy each keypress
    // and removes the GC pressure that causes the periodic "every so often" stutter.
    static const std::string replacement =
        "e.docChanged){const _ss=e.state;"
        // Also stash the live EditorView on a known global so InjectPatches drag
        // handler can reach it without traversing CM6 internals.
        "self._snpdView=e.view;"
        "clearTimeout(self._snpdCmTmr);self._snpdCmTmr=setTimeout(()=>b(_ss.doc.toString()),600)}";
    // Patch 2: Replace synchronous prompt() call in "Add new prompt" handler with
    // an async modal dialog, since Ultralight doesn't support window.prompt().
    // The original: S=e=>{const t=prompt("Enter prompt name...");if(t){...}k()}
    // Replacement: show a custom modal, then run the same logic on confirm.
    static const std::string promptNeedle =
        R"(S=e=>{const t=prompt("Enter prompt name (without .prompt extension):");)"
        R"(if(t){const s=e.path?`${e.path}/${t}.prompt`:`${t}.prompt`;n(s,e)}k()})";
    static const std::string promptReplacement =
        "S=e=>{self._snpdPromptCb=t=>{if(t){const s=e.path?`${e.path}/${t}.prompt`:`${t}.prompt`;n(s,e)}k()};"
        "self._snpdShowPrompt('Enter prompt name (without .prompt extension):','')}";
    {
        auto pos2 = body.find(promptNeedle);
        if (pos2 != std::string::npos) {
            body.replace(pos2, promptNeedle.size(), promptReplacement);
            logger::info("SkyrimNetDashboard: PatchBundle: prompt() dialog patch applied");
        }
    }
    auto pos = body.find(needle);
    if (pos != std::string::npos) {
        body.replace(pos, needle.size(), replacement);
        logger::info("SkyrimNetDashboard: PatchBundle: CodeMirror debounce applied");
    }
    return body;
}

// Fetches the SkyrimNet root HTML and injects compat patches.
static std::string FetchAndInject(const std::string& host, uint16_t port)
{
    auto [body, ct, sc] = FetchResource(host, port, "GET", "/");
    (void)sc;
    if (body.empty())
        return "<html><body>Proxy error: could not fetch dashboard from " +
               host + ":" + std::to_string(port) + "</body></html>";
    return InjectPatches(std::move(body));
}

// ── Static asset cache ──────────────────────────────────────────────────────
// JS/CSS/font/image assets from the SkyrimNet app use content-hashed filenames
// (e.g. main.f6f3c786.js) and never change between requests. Caching them
// in memory eliminates repeated 1.2 MB+ network fetches that cause FPS drops
// on every React page transition.
struct CachedAsset { std::string body; std::string contentType; };
static std::mutex                          s_cacheMtx;
static std::unordered_map<std::string, CachedAsset> s_assetCache;

// Returns true for paths whose content cannot change (content-hash in filename
// or well-known immutable extensions like fonts).
static bool IsImmutableAsset(const std::string& ct, const std::string& path)
{
    // Content-hashed webpack output: filename contains 8+ hex chars before extension
    static const std::string hexChars = "0123456789abcdefABCDEF";
    auto dot = path.rfind('.');
    auto dash = (dot != std::string::npos) ? path.rfind('.', dot - 1) : std::string::npos;
    if (dash != std::string::npos && dot > dash + 1) {
        std::string seg = path.substr(dash + 1, dot - dash - 1);
        if (seg.size() >= 8 && seg.find_first_not_of(hexChars) == std::string::npos)
            return true;
    }
    // Fonts and icons are always immutable
    if (ct.find("font") != std::string::npos) return true;
    if (ct.find("image/") != std::string::npos) return true;
    return false;
}

// Attempts to proxy a request by piping the upstream response directly to the
// client socket without buffering.  Used for streaming/chunked responses (e.g.
// SkyrimNet TTS diary streams) that send NDJSON segments as they are generated.
// Streaming responses have no Content-Length header; we detect this after reading
// just the response headers, then pipe the body verbatim so the browser's
// response.body.getReader() receives segments in real-time.
//
// Returns true if the request was handled (response forwarded + client socket
// left closed by this function).  Returns false if the upstream response had a
// Content-Length header, meaning it is a regular sized response that should be
// handled by the normal FetchResource buffering path instead.
static bool TryStreamProxy(SOCKET client,
                            const std::string& host, uint16_t port,
                            const std::string& method, const std::string& path,
                            const std::string& reqCT, const std::string& reqBody)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    auto ipAddr = inet_addr(host.c_str());
    if (ipAddr == INADDR_NONE) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) { closesocket(s); return false; }
        ipAddr = *reinterpret_cast<u_long*>(he->h_addr_list[0]);
    }
    addr.sin_addr.s_addr = ipAddr;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s); return false;
    }

    // Build and send the HTTP request (same format as FetchResource).
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    const bool hasBody = method != "GET" && method != "HEAD";
    if (hasBody) {
        std::string ct = reqCT.empty() ? "application/json" : reqCT;
        req += "Content-Type: " + ct + "\r\n";
        req += "Content-Length: " + std::to_string(reqBody.size()) + "\r\n";
    } else if (!reqBody.empty()) {
        if (!reqCT.empty()) req += "Content-Type: " + reqCT + "\r\n";
        req += "Content-Length: " + std::to_string(reqBody.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += reqBody;
    send(s, req.c_str(), static_cast<int>(req.size()), 0);

    // Read upstream response headers (until \r\n\r\n).
    std::string headerBuf;
    char tmp[16384];
    std::size_t sepPos = std::string::npos;
    while (sepPos == std::string::npos) {
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) { closesocket(s); return false; }
        headerBuf.append(tmp, n);
        sepPos = headerBuf.find("\r\n\r\n");
    }
    std::string headerSection = headerBuf.substr(0, sepPos);
    std::string alreadyRead   = headerBuf.substr(sepPos + 4);

    // If the upstream response carries a Content-Length, it is a normal
    // bufferable response.  Hand off to FetchResource instead.
    std::string hdrlower;
    hdrlower.reserve(headerSection.size());
    for (auto c : headerSection)
        hdrlower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (hdrlower.find("content-length:") != std::string::npos) {
        closesocket(s);
        return false;
    }

    // No Content-Length → streaming response.
    // For TTS audio streams (SkyrimNet NDJSON) we intercept each
    // {"type":"segment","audio":"<base64>",...} line: we decode the WAV bytes
    // in C++ and push them to the audio queue runner, then forward the line
    // with the "audio" value blanked out so SkyrimNet's JS receives an empty
    // string instead of 267 KB of base64.  P("", "audio/wav") creates a 0-byte
    // Blob in ~1 µs instead of the usual ~200 ms, eliminating the burst of
    // per-segment stalls that was blocking D3DPresent.
    logger::info("SkyrimNetDashboard: streaming proxy {} {}", method, path);
    send(client, headerSection.c_str(), static_cast<int>(headerSection.size()), 0);
    send(client, "\r\n\r\n", 4, 0);

    // Helpers for sending to the client (returns false if the client hung up).
    auto sendToClient = [&](const char* data, int len) -> bool {
        if (len <= 0) return true;
        return send(client, data, len, 0) != SOCKET_ERROR;
    };

    // Line-buffer: assemble complete NDJSON lines before processing.
    std::string lineBuf = alreadyRead;
    bool isAudioStream  = false; // set on first confirmed segment/status line

    // Strip the "audio":"<base64>" field from a JSON line in-place.
    // Returns the extracted base64 string (empty if not found).
    auto stripAudioField = [](std::string& line) -> std::string {
        auto audioKey = line.find("\"audio\"");
        if (audioKey == std::string::npos) return {};
        auto colon = line.find(':', audioKey + 7);
        if (colon == std::string::npos) return {};
        auto q1 = line.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        auto b64Start = q1 + 1;
        auto b64End   = line.find('"', b64Start);
        if (b64End == std::string::npos || b64End <= b64Start) return {};
        std::string b64 = line.substr(b64Start, b64End - b64Start);
        line.erase(b64Start, b64End - b64Start); // blank value → P("") ≈ 1 µs in JS
        return b64;
    };

    auto processLine = [&](std::string& line) -> bool {
        // Detect TTS audio stream on first NDJSON line that looks like one.
        if (!isAudioStream &&
                line.find("\"type\"") != std::string::npos &&
                (line.find("\"segment\"") != std::string::npos ||
                 line.find("\"status\"")  != std::string::npos)) {
            isAudioStream = true;
            ClearAudioQueue();        // flush any leftovers from a previous run
            EnsureAudioQueueRunning();
            logger::info("SkyrimNetDashboard: TTS audio stream detected, C++ queue active");
        }

        if (isAudioStream) {
            if (line.find("\"type\":\"segment\"") != std::string::npos) {
                // Decode WAV in C++ and push to queue; blank the field so
                // JS receives P("","audio/wav") ≈ 1 µs instead of ~200 ms.
                std::string b64 = stripAudioField(line);
                if (!b64.empty()) {
                    std::string wav = Base64Decode(b64);
                    if (!wav.empty()) {
                        // Accumulate PCM for /diary-audio download endpoint.
                        {
                            std::lock_guard<std::mutex> lk(s_diaryMtx);
                            WavInfo wi = ParseWavHeader(wav);
                            if (wi.valid && wi.dataLen > 0) {
                                if (!s_diaryFmtValid) {
                                    s_diaryFmt      = wi.wfx;
                                    s_diaryFmtValid = true;
                                }
                                s_diaryPcm.insert(s_diaryPcm.end(),
                                    reinterpret_cast<const uint8_t*>(wav.data()) + wi.dataOff,
                                    reinterpret_cast<const uint8_t*>(wav.data()) + wi.dataOff + wi.dataLen);
                            }
                        }
                        PushToAudioQueue(std::move(wav));
                    }
                }
            } else if (line.find("\"type\":\"complete\"") != std::string::npos) {
                // The "complete" message carries the full-entry WAV for the
                // download button.  Strip it unconditionally: Ultralight cannot
                // trigger file downloads, and passing multi-MB base64 to JS
                // P() would freeze the game thread for several seconds via
                // ultralightFuture.get() in D3DPresent.
                stripAudioField(line);
                logger::info("SkyrimNetDashboard: stripped complete-message audio (download blob)");
            }
        }

        return sendToClient(line.c_str(), static_cast<int>(line.size()));
    };

    while (true) {
        // Drain all complete lines from the buffer before reading more data.
        std::string::size_type nl;
        while ((nl = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, nl + 1);
            lineBuf.erase(0, nl + 1);
            if (!processLine(line)) goto done; // client disconnected
        }
        {
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            lineBuf.append(tmp, n);
        }
    }
done:
    // Flush any partial (unterminated) line.
    if (!lineBuf.empty())
        sendToClient(lineBuf.c_str(), static_cast<int>(lineBuf.size()));
    closesocket(s);
    return true;
}

static uint16_t StartShellServer(const std::string& shellHtml, const std::string& dashboardUrl)
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        logger::error("SkyrimNetDashboard: socket() failed: {}", WSAGetLastError());
        return 0;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; // OS picks a free port

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        logger::error("SkyrimNetDashboard: bind() failed: {}", WSAGetLastError());
        closesocket(srv);
        return 0;
    }
    int addrLen = sizeof(addr);
    getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    uint16_t port = ntohs(addr.sin_port);
    listen(srv, 10);
    logger::info("SkyrimNetDashboard: shell server on 127.0.0.1:{}", port);

    // Parse host and port from dashboardUrl ("http://host:port/")
    std::string proxyHost;
    uint16_t    proxyPort = 80;
    {
        std::string u = dashboardUrl;
        if (u.substr(0, 7) == "http://") u = u.substr(7);
        auto col = u.find(':');
        auto sl  = u.find('/');
        if (col != std::string::npos && col < sl) {
            proxyHost = u.substr(0, col);
            proxyPort = static_cast<uint16_t>(std::stoi(u.substr(col + 1, sl - col - 1)));
        } else {
            proxyHost = u.substr(0, sl);
        }
    }

    std::thread([srv, shellHtml, proxyHost, proxyPort]() {

        // Per-connection handler — runs on its own thread so all requests are
        // served in parallel.  This is critical for React apps which fire
        // 10-20 simultaneous asset/API requests on every page transition.
        auto handleClient = [&shellHtml, &proxyHost, proxyPort](SOCKET client) {
            // Read the full request (headers + possible body)
            std::string rawReq;
            {
                char tmp[16384];
                int nr;
                while ((nr = recv(client, tmp, sizeof(tmp), 0)) > 0) {
                    rawReq.append(tmp, nr);
                    auto headerEnd = rawReq.find("\r\n\r\n");
                    if (headerEnd == std::string::npos) continue;
                    // Read declared Content-Length body bytes if present
                    std::string hdrlower; hdrlower.reserve(headerEnd);
                    for (std::size_t i = 0; i < headerEnd; ++i)
                        hdrlower += static_cast<char>(tolower(static_cast<unsigned char>(rawReq[i])));
                    std::size_t clPos = hdrlower.find("content-length:");
                    if (clPos == std::string::npos) break; // no body
                    auto clEnd = rawReq.find("\r\n", clPos);
                    int contentLen = std::stoi(rawReq.substr(clPos + 15, clEnd - clPos - 15));
                    int bodyReceived = static_cast<int>(rawReq.size()) -
                                       static_cast<int>(headerEnd + 4);
                    while (bodyReceived < contentLen) {
                        int toRead = contentLen - bodyReceived;
                        if (toRead > static_cast<int>(sizeof(tmp))) toRead = static_cast<int>(sizeof(tmp));
                        nr = recv(client, tmp, toRead, 0);
                        if (nr <= 0) break;
                        rawReq.append(tmp, nr);
                        bodyReceived += nr;
                    }
                    break;
                }
            }

            // Parse method, path, request content-type, and body
            std::string method = "GET", path = "/", reqCT, reqBody;
            {
                auto lineEnd = rawReq.find("\r\n");
                std::string requestLine = (lineEnd != std::string::npos)
                    ? rawReq.substr(0, lineEnd) : rawReq;
                auto sp1 = requestLine.find(' ');
                auto sp2 = requestLine.find(' ', sp1 + 1);
                if (sp1 != std::string::npos) {
                    method = requestLine.substr(0, sp1);
                    path   = requestLine.substr(sp1 + 1,
                             sp2 != std::string::npos ? sp2 - sp1 - 1 : std::string::npos);
                }
                auto headerEnd = rawReq.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    std::string hdrs = rawReq.substr(0, headerEnd);
                    std::string hdrlower; hdrlower.reserve(hdrs.size());
                    for (auto c : hdrs) hdrlower += static_cast<char>(tolower(
                                                    static_cast<unsigned char>(c)));
                    auto ctPos = hdrlower.find("content-type:");
                    if (ctPos != std::string::npos) {
                        auto eol = hdrs.find("\r\n", ctPos);
                        reqCT = hdrs.substr(ctPos + 13, eol - ctPos - 13);
                        while (!reqCT.empty() && reqCT.front() == ' ') reqCT.erase(reqCT.begin());
                    }
                    reqBody = rawReq.substr(headerEnd + 4);
                }
            }

            std::string body;
            std::string contentType = "text/html; charset=utf-8";
            int upstreamStatus = 200;

            if (path == "/shell" || path.empty()) {
                body = shellHtml;
            } else if (path == "/audio") {
                // Audio control endpoint — called by the injected JS bridge via fetch POST.
                // Fire-and-forget: response is ignored by the browser.
                logger::info("SkyrimNetDashboard: /audio POST received body='{}'", reqBody);
                OnAudioMessage(reqBody.c_str());
                body        = "ok";
                contentType = "text/plain";
            } else if (path == "/diary-audio") {
                // Serve the accumulated diary WAV for the download button.
                std::vector<uint8_t> pcmCopy;
                WAVEFORMATEX fmtCopy = {};
                bool fmtOk = false;
                {
                    std::lock_guard<std::mutex> lk(s_diaryMtx);
                    pcmCopy = s_diaryPcm;
                    fmtCopy = s_diaryFmt;
                    fmtOk   = s_diaryFmtValid;
                }
                if (fmtOk && !pcmCopy.empty()) {
                    auto pcmSz = static_cast<uint32_t>(pcmCopy.size());
                    uint32_t riffSz = 36 + pcmSz;
                    body.resize(44 + pcmSz);
                    auto* p = reinterpret_cast<uint8_t*>(body.data());
                    std::memcpy(p, "RIFF", 4);               p += 4;
                    std::memcpy(p, &riffSz, 4);              p += 4;
                    std::memcpy(p, "WAVE", 4);               p += 4;
                    std::memcpy(p, "fmt ", 4);               p += 4;
                    uint32_t fmtChunkSz = 16;
                    std::memcpy(p, &fmtChunkSz, 4);          p += 4;
                    uint16_t tag = 1;
                    std::memcpy(p, &tag,                   2); p += 2;
                    std::memcpy(p, &fmtCopy.nChannels,     2); p += 2;
                    std::memcpy(p, &fmtCopy.nSamplesPerSec,4); p += 4;
                    std::memcpy(p, &fmtCopy.nAvgBytesPerSec,4);p += 4;
                    std::memcpy(p, &fmtCopy.nBlockAlign,   2); p += 2;
                    std::memcpy(p, &fmtCopy.wBitsPerSample,2); p += 2;
                    std::memcpy(p, "data", 4);               p += 4;
                    std::memcpy(p, &pcmSz, 4);               p += 4;
                    std::memcpy(p, pcmCopy.data(), pcmSz);
                    contentType = "audio/wav";
                    logger::info("SkyrimNetDashboard: /diary-audio: serving {} PCM bytes", pcmSz);
                } else {
                    body          = "{\"error\":\"No diary audio available\"}";
                    contentType   = "application/json";
                    upstreamStatus = 404;
                }
            } else if (path.substr(0, 12) == "/save-dialog") {
                // Opens a native Windows Save-As dialog on the game thread and
                // returns JSON {"path":"..."}  or  {"cancelled":true}.
                // The ?name= query param sets the suggested filename.
                std::string suggestName = "download.bin";
                {
                    auto qp = path.find('?');
                    if (qp != std::string::npos) {
                        std::string q = path.substr(qp + 1);
                        auto np = q.find("name=");
                        if (np != std::string::npos)
                            suggestName = UrlDecode(q.substr(np + 5));
                    }
                }
                // Sanitise display name (strip dirs but keep extension)
                {
                    auto sl = suggestName.find_last_of("/\\");
                    if (sl != std::string::npos) suggestName = suggestName.substr(sl + 1);
                }
                {
                    char fileBuf[MAX_PATH] = {};
                    // suggestName may be up to MAX_PATH-1 chars; copy safely
                    strncpy_s(fileBuf, suggestName.c_str(), _TRUNCATE);

                    // Build initial directory: Documents folder
                    std::string initDir;
                    {
                        char* up = nullptr; std::size_t len = 0;
                        _dupenv_s(&up, &len, "USERPROFILE");
                        initDir = (up ? std::string(up) : "C:\\Users\\Default") + "\\Documents";
                        if (up) free(up);
                    }

                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize  = sizeof(ofn);
                    ofn.hwndOwner    = nullptr;   // no parent HWND
                    ofn.lpstrFilter  = "All Files\0*.*\0Log Files\0*.log\0Text Files\0*.txt\0WAV Audio\0*.wav\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrFile    = fileBuf;
                    ofn.nMaxFile     = MAX_PATH;
                    ofn.lpstrInitialDir = initDir.c_str();
                    ofn.lpstrTitle   = "Save file";
                    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

                    if (GetSaveFileNameA(&ofn)) {
                        std::string jsonPath;
                        for (char c : std::string(fileBuf))
                            if (c == '\\') jsonPath += "\\\\"; else jsonPath += c;
                        body = "{\"path\":\"" + jsonPath + "\"}";
                    } else {
                        body = "{\"cancelled\":true}";
                    }
                }
                contentType = "application/json";
            } else if (path.substr(0, 10) == "/save-file") {
                // Receives binary file content as POST body and writes it to disk.
                // Accepts either:
                //   ?path=<full-encoded-path>  (from /save-dialog)
                //   ?name=<filename>           (falls back to Documents\SkyrimNet Logs\)
                std::string savePath;
                {
                    auto qPos = path.find('?');
                    if (qPos != std::string::npos) {
                        std::string query = path.substr(qPos + 1);
                        auto pathPos = query.find("path=");
                        auto namePos = query.find("name=");
                        if (pathPos != std::string::npos) {
                            // Full path supplied by the save dialog
                            savePath = UrlDecode(query.substr(pathPos + 5));
                        } else if (namePos != std::string::npos) {
                            std::string fname = UrlDecode(query.substr(namePos + 5));
                            // Strip path components and dangerous chars from bare filename
                            auto sl = fname.find_last_of("/\\");
                            if (sl != std::string::npos) fname = fname.substr(sl + 1);
                            for (auto& c : fname)
                                if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                                    c = '_';
                            if (fname.empty()) fname = "download.bin";
                            char* up = nullptr; std::size_t len = 0;
                            _dupenv_s(&up, &len, "USERPROFILE");
                            std::string dir = (up ? std::string(up) : "C:\\Users\\Default") + "\\Documents\\SkyrimNet Logs";
                            if (up) free(up);
                            CreateDirectoryA(dir.c_str(), nullptr);
                            savePath = dir + "\\" + fname;
                        }
                    }
                    if (savePath.empty()) savePath = "download.bin";
                }
                std::ofstream ofs(savePath, std::ios::binary);
                if (ofs) {
                    ofs.write(reqBody.data(), static_cast<std::streamsize>(reqBody.size()));
                    ofs.close();
                    std::string jsonPath;
                    for (auto c : savePath)
                        if (c == '\\') jsonPath += "\\\\"; else jsonPath += c;
                    body = "{\"saved\":true,\"path\":\"" + jsonPath + "\"}";
                    logger::info("SkyrimNetDashboard: saved file '{}' ({} bytes)", savePath, reqBody.size());
                } else {
                    body = "{\"saved\":false,\"error\":\"Could not write file\"}";
                    logger::warn("SkyrimNetDashboard: /save-file failed to write '{}'", savePath);
                }
                contentType = "application/json";
            } else if (path == "/clipboard-get") {
                body        = GetClipboardTextW32();
                contentType = "text/plain; charset=utf-8";
            } else if (path == "/clipboard-set") {
                SetClipboardTextW32(reqBody);
                body        = "ok";
                contentType = "text/plain";
            } else if (path == "/is-ctrl-held") {
                body        = ((GetKeyState(VK_CONTROL) & 0x8000) != 0) ? "1" : "0";
                contentType = "text/plain";
            } else if (path == "/settings-get") {
                { std::lock_guard<std::mutex> lk(s_cfgMtx); body = SettingsToJson(); }
                contentType = "application/json";
            } else if (path == "/settings-save") {
                // Minimal JSON field extractor — no full parser needed
                auto extractStr = [&](const std::string& key) -> std::string {
                    auto needle = '"' + key + "\":\""; // "key":"..."
                    auto pos = reqBody.find(needle);
                    if (pos == std::string::npos) return "";
                    pos += needle.size();
                    auto end = reqBody.find('"', pos);
                    return end == std::string::npos ? "" : reqBody.substr(pos, end - pos);
                };
                auto extractInt = [&](const std::string& key, int def) -> int {
                    auto needle = '"' + key + "\":";
                    auto pos = reqBody.find(needle);
                    if (pos == std::string::npos) return def;
                    pos += needle.size();
                    while (pos < reqBody.size() && (reqBody[pos]==' '||reqBody[pos]=='\t')) ++pos;
                    if (pos >= reqBody.size()) return def;
                    bool neg = (reqBody[pos] == '-');
                    if (neg) ++pos;
                    int val = 0; bool got = false;
                    while (pos < reqBody.size() && reqBody[pos] >= '0' && reqBody[pos] <= '9') {
                        val = val*10 + (reqBody[pos++]-'0'); got = true; }
                    return got ? (neg ? -val : val) : def;
                };
                auto extractBool = [&](const std::string& key, bool def) -> bool {
                    auto needle = '"' + key + "\":";
                    auto pos = reqBody.find(needle);
                    if (pos == std::string::npos) return def;
                    pos += needle.size();
                    while (pos < reqBody.size() && (reqBody[pos]==' '||reqBody[pos]=='\t')) ++pos;
                    if (reqBody.substr(pos, 4) == "true")  return true;
                    if (reqBody.substr(pos, 5) == "false") return false;
                    return def;
                };
                int  newHotKey; bool newKeepBg, newDefHome, newPause;
                {
                    std::lock_guard<std::mutex> lk(s_cfgMtx);
                    newHotKey  = extractInt("hotKey",       s_cfg.hotKey);
                    newKeepBg  = extractBool("keepBg",       s_cfg.keepBg);
                    newDefHome = extractBool("defaultHome",  s_cfg.defaultHome);
                    newPause   = extractBool("pauseGame",    s_cfg.pauseGame);
                    s_cfg.hotKey      = newHotKey;
                    s_cfg.keepBg      = newKeepBg;
                    s_cfg.defaultHome = newDefHome;
                    s_cfg.pauseGame   = newPause;
                    SaveSettings();
                }
                // Re-register hotkey if it changed (KeyHandler has its own lock; no s_cfgMtx needed)
                if (newHotKey != static_cast<int>(s_toggleKey) && newHotKey > 0) {
                    auto* kh = KeyHandler::GetSingleton();
                    if (s_toggleHandle != INVALID_REGISTRATION_HANDLE)
                        kh->Unregister(s_toggleHandle);
                    s_toggleKey    = static_cast<uint32_t>(newHotKey);
                    s_toggleHandle = kh->Register(s_toggleKey, KeyEventType::KEY_DOWN, OnToggle);
                    logger::info("SkyrimNetDashboard: hotkey changed to {} (0x{:02X})",
                        DxKeyName(s_toggleKey), s_toggleKey);
                }
                body        = "{\"saved\":true}";
                contentType = "application/json";
            } else if (path == "/audio-raw") {
                // Binary audio endpoint -- JS fetches blob bytes and POSTs them here.
                // When the C++ audio queue is active, JS Audio.play() on an empty blob
                // (from a stripped segment line) posts an empty body.  Treat that as a
                // resume signal; ignore it otherwise.  Non-empty bodies are real audio.
                if (reqBody.empty()) {
                    if (s_aqPaused.load())
                        ResumeAudioQueue();
                    // else: first play() on a streamed segment — C++ queue handles it.
                } else {
                    logger::info("SkyrimNetDashboard: /audio-raw POST {} bytes, ct='{}'", reqBody.size(), reqCT);
                    PlayAudioRaw(reqBody, reqCT);
                }
                body        = "ok";
                contentType = "text/plain";
            } else if (path == "/proxy" || path == "/proxy/") {
                body = FetchAndInject(proxyHost, proxyPort);
            } else {
                // Check in-memory cache first (GET only, static assets)
                bool fromCache = false;
                if (method == "GET") {
                    std::lock_guard<std::mutex> lk(s_cacheMtx);
                    auto it = s_assetCache.find(path);
                    if (it != s_assetCache.end()) {
                        body        = it->second.body;
                        contentType = it->second.contentType;
                        fromCache   = true;
                    }
                }
                if (!fromCache) {
                    // For responses without a Content-Length (streaming/chunked,
                    // e.g. SkyrimNet TTS diary/memory streams using NDJSON), pipe
                    // bytes directly to the client so the browser's
                    // response.body.getReader() receives segments in real-time
                    // rather than waiting for the entire generation to complete.
                    if (TryStreamProxy(client, proxyHost, proxyPort, method, path, reqCT, reqBody)) {
                        closesocket(client);
                        return;
                    }
                    auto [resBody, resCt, resSc] = FetchResource(proxyHost, proxyPort, method, path, reqCT, reqBody);
                    body        = std::move(resBody);
                    contentType = std::move(resCt);
                    upstreamStatus = resSc;
                    if (contentType.find("text/html") != std::string::npos && !body.empty())
                        body = InjectPatches(std::move(body));
                    else if (contentType.find("javascript") != std::string::npos && !body.empty())
                        body = PatchBundle(std::move(body));
                    else if (contentType.find("text/css") != std::string::npos && !body.empty())
                        body = PatchCSS(std::move(body));
                    // For API calls (non-GET): if the body is empty and upstream
                    // returned a 2xx success, serve '{}' so axios JSON.parse
                    // doesn't blow up on an empty string (WebKit throws
                    // SyntaxError: "The string did not match the expected pattern.").
                    // We also force the status to 200 because HTTP 204 MUST NOT
                    // carry a body — Ultralight would discard it and we'd be back
                    // to JSON.parse("") throwing the same error.
                    if (method != "GET" && body.empty() && resSc >= 200 && resSc < 300) {
                        body           = "{}";
                        contentType    = "application/json";
                        upstreamStatus = 200;   // promote 204→200 so the body is delivered
                    }
                    // Cache immutable assets (content-hashed JS/CSS, fonts, images)
                    if (method == "GET" && !body.empty() && IsImmutableAsset(contentType, path)) {
                        std::lock_guard<std::mutex> lk(s_cacheMtx);
                        s_assetCache[path] = { body, contentType };
                        logger::info("SkyrimNetDashboard: cached {} ({} bytes)", path, body.size());
                    }
                }
            }

            // Immutable assets get long-lived browser cache headers too,
            // so Ultralight won't re-request them across soft navigations.
            bool immutable = IsImmutableAsset(contentType, path);
            std::string cacheControl = immutable
                ? "max-age=31536000, immutable"
                : "no-store";

            // Forward the upstream HTTP status code so axios error handling works
            // correctly (e.g. 4xx → onError, 2xx → onSuccess).
            std::string statusLine = "HTTP/1.1 200 OK";
            if (upstreamStatus == 201) statusLine = "HTTP/1.1 201 Created";
            else if (upstreamStatus == 204) statusLine = "HTTP/1.1 204 No Content";
            else if (upstreamStatus >= 400 && upstreamStatus < 500) statusLine = "HTTP/1.1 " + std::to_string(upstreamStatus) + " Client Error";
            else if (upstreamStatus >= 500) statusLine = "HTTP/1.1 " + std::to_string(upstreamStatus) + " Server Error";
            std::string resp =
                statusLine + "\r\n"
                "Content-Type: " + contentType + "\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Cache-Control: " + cacheControl + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            send(client, resp.c_str(), static_cast<int>(resp.size()), 0);
            closesocket(client);
        };

        while (true) {
            SOCKET client = accept(srv, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            // Each connection gets its own thread — parallel asset fetching.
            std::thread(handleClient, client).detach();
        }
        closesocket(srv);
    }).detach();

    return port;
}

// ── C++ clipboard helpers ─────────────────────────────────────────────────────
// Called by the monitor thread when the Invoke result arrives with copied text.
static void OnCopyResult(const char* result)
{
    if (result && *result) SetClipboardTextW32(result);
}

// Produces a JS double-quoted string literal from arbitrary UTF-8 text.
static std::string EscapeJsStr(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20)  { char h[8]; snprintf(h, sizeof(h), "\\u%04X", c); out += h; }
        else                out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

// Polls GetAsyncKeyState every ~16ms; when the dashboard is focused and the user
// presses Ctrl+C/V, drives clipboard operations via s_PrismaUI->Invoke().
// This bypasses ALL Ultralight JS event quirks (no e.ctrlKey, no sync XHR needed).
// The Invoke script reaches into the iframe via document.getElementById('snpd-frame')
// and calls the __snpdCopy/__snpdPaste functions injected by InjectPatches.
static void StartClipboardMonitor()
{
    std::thread([]() {
        bool prevC = false, prevV = false, prevX = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            if (!s_PrismaUI || !s_View) { prevC = prevV = prevX = false; continue; }
            if (!s_PrismaUI->HasFocus(s_View)) { prevC = prevV = prevX = false; continue; }
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool curC  = (GetAsyncKeyState('C') & 0x8000) != 0;
            bool curV  = (GetAsyncKeyState('V') & 0x8000) != 0;
            bool curX  = (GetAsyncKeyState('X') & 0x8000) != 0;
            if (ctrl) {
                if (curC && !prevC) {
                    // Copy: invoke __snpdCopy in the iframe, write result to Win32 clipboard
                    s_PrismaUI->Invoke(s_View,
                        "(function(){"
                        "var f=document.getElementById('snpd-frame');"
                        "var cw=f&&f.contentWindow;"
                        "return cw&&cw.__snpdCopy?cw.__snpdCopy():'';}) ()",
                        OnCopyResult);
                }
                if (curX && !prevX) {
                    // Cut: __snpdCut deletes the selection and returns the text
                    s_PrismaUI->Invoke(s_View,
                        "(function(){"
                        "var f=document.getElementById('snpd-frame');"
                        "var cw=f&&f.contentWindow;"
                        "return cw&&cw.__snpdCut?cw.__snpdCut():'';}) ()",
                        OnCopyResult);
                }
                if (curV && !prevV) {
                    // Paste: read Win32 clipboard and inject via __snpdPaste in the iframe
                    std::string txt = GetClipboardTextW32();
                    if (!txt.empty()) {
                        std::string script =
                            "(function(){"
                            "var f=document.getElementById('snpd-frame');"
                            "var cw=f&&f.contentWindow;"
                            "if(cw&&cw.__snpdPaste)cw.__snpdPaste(" + EscapeJsStr(txt) + ");}) ()";
                        s_PrismaUI->Invoke(s_View, script.c_str(), nullptr);
                    }
                }
            }
            prevC = curC; prevV = curV; prevX = curX;
        }
    }).detach();
}

static void OnDomReady(PrismaView view)
{
    // The shell page is static and never navigates — nothing to inject into the outer frame.
    // Patches (confirm/alert/open) are injected directly into the proxied SkyrimNet HTML
    // by FetchAndInject, so they run in the dashboard's own JS context from the first load.
    // Since SkyrimNet is a React SPA using pushState, no full page reload ever occurs
    // after the initial load, so the patches remain in effect permanently.
    logger::info("SkyrimNetDashboard: shell OnDomReady.");
    (void)view;
}

static void MessageHandler(SKSE::MessagingInterface::Message* a_message)
{
    if (a_message->type != SKSE::MessagingInterface::kDataLoaded) {
        return;
    }

    // 1. Load settings from INI
    LoadSettings();

    // 2. Get PrismaUI API
    s_PrismaUI = static_cast<PRISMA_UI_API::IVPrismaUI1*>(
        PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));

    if (!s_PrismaUI) {
        logger::critical("SkyrimNetDashboard: Failed to get PrismaUI API. Is PrismaUI installed?");
        return;
    }

    // 3. Parse the SkyrimNet server address for the audio subsystem
    {
        std::string u = s_cfg.url;
        if (u.size() > 7 && u.substr(0, 7) == "http://") u = u.substr(7);
        auto col = u.find(':');
        auto sl  = u.find('/');
        if (col != std::string::npos && (sl == std::string::npos || col < sl)) {
            s_audioHost = u.substr(0, col);
            try { s_audioPort = static_cast<uint16_t>(std::stoi(u.substr(col + 1, sl - col - 1))); }
            catch (...) {}
        } else {
            s_audioHost = u.substr(0, sl);
            s_audioPort = 80;
        }
        logger::info("SkyrimNetDashboard: audio backend {}:{}", s_audioHost, s_audioPort);
    }

    // 4. Build the chrome shell page and start the local HTTP server
    std::string shellHtml = BuildShellHtml();
    std::string startUrl  = (s_cfg.defaultHome || s_cfg.lastPage.empty())
                            ? s_cfg.url : s_cfg.lastPage;
    uint16_t    port      = StartShellServer(shellHtml, startUrl);
    if (port == 0) {
        logger::critical("SkyrimNetDashboard: Failed to start shell server.");
        return;
    }
    std::string shellUrl = "http://127.0.0.1:" + std::to_string(port) + "/shell";

    // 5. Create a view that loads the shell page (chrome + iframe pointing at SkyrimNet)
    s_View = s_PrismaUI->CreateView(shellUrl.c_str(), OnDomReady);
    s_PrismaUI->SetScrollingPixelSize(s_View, 120);
    if (!s_cfg.keepBg)
        s_PrismaUI->Hide(s_View); // Start hidden unless KeepBackground=1

    StartClipboardMonitor(); // C++ Ctrl+C/V polling — bypasses Ultralight event quirks

    // Close button in the HTML calls window.closeDashboard('') — wire it to OnToggle
    s_PrismaUI->RegisterJSListener(s_View, "closeDashboard", [](const char*) { OnToggle(); });

    logger::info("SkyrimNetDashboard: Shell view created at {} (iframe -> {})", shellUrl, startUrl);

    // 6. Register hotkey to toggle the overlay
    s_toggleKey    = static_cast<uint32_t>(s_cfg.hotKey);
    KeyHandler::RegisterSink();
    s_toggleHandle = KeyHandler::GetSingleton()->Register(
        s_toggleKey, KeyEventType::KEY_DOWN, OnToggle);
    [[maybe_unused]] auto escHandle = KeyHandler::GetSingleton()->Register(
        ESC_KEY, KeyEventType::KEY_DOWN, OnClose);

    logger::info("SkyrimNetDashboard: Ready. {} = open/close.",
        DxKeyName(s_toggleKey));
}

// Declare the plugin to SKSE for all runtime variants (SE/AE/VR).
// SKSEPluginInfo emits both the modern SKSEPlugin_Version constinit struct (read by SKSE NG)
// and a legacy SKSEPlugin_Query shim (for older SKSE builds), so one binary works everywhere.
// RuntimeCompatibility = AddressLibrary means version-independent via Address Library (CommonLib NG default).
using namespace REL::literals;
SKSEPluginInfo(
    .Version              = { 1, 0, 0, 0 },
    .Name                 = PLUGIN_NAME ""sv,
    .Author               = ""sv,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary
);

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    logger::info("SkyrimNetDashboard: Plugin loaded.");

    // Initialize Media Foundation once for the process lifetime.
    // Doing this at load time pre-warms the codec DLLs and thread pool so that
    // the first TranscodeToWav call during gameplay doesn't spike.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        logger::warn("SkyrimNetDashboard: CoInitializeEx failed (non-fatal)");
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
        logger::warn("SkyrimNetDashboard: MFStartup failed (non-fatal)");

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener(MessageHandler)) {
        logger::error("SkyrimNetDashboard: Failed to register messaging listener.");
        return false;
    }

    return true;
}
