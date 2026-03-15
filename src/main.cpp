#include "pch.h"
#include "PrismaUI_API.h"
#include "keyhandler/keyhandler.h"

// -----------------------------------------------------------------------
//  SkyrimNet Prisma Dashboard
//  Loads the SkyrimNet web dashboard inside Skyrim via PrismaUI.
//  Press F4 to open/close the overlay.
// -----------------------------------------------------------------------

#define PLUGIN_NAME    "SkyrimNetPrismaDashboard"
#define PLUGIN_VERSION "1.0.0"

static constexpr const char* SKYRIMNET_URL = "http://192.168.50.88:8080/";
static constexpr uint32_t    TOGGLE_KEY    = 0x3E; // F4
static constexpr uint32_t    ESC_KEY       = 0x01; // Escape
static constexpr uint32_t    INSPECTOR_KEY = 0x3F; // F5

static PRISMA_UI_API::IVPrismaUI1* s_PrismaUI = nullptr;
static PrismaView                  s_View      = 0;

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

// Simple JSON string-field extractor for our own controlled JSON payloads.
static std::string AudioJsonField(const char* json, const char* key)
{
    if (!json || !key) return {};
    std::string j(json), k = std::string("\"") + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return {};
    pos = j.find(':', pos + k.size());
    if (pos == std::string::npos) return {};
    pos = j.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string val;
    while (pos < j.size() && j[pos] != '"') {
        if (j[pos] == '\\' && pos + 1 < j.size()) { ++pos; }
        val += j[pos++];
    }
    return val;
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

static void StopAudio()
{
    // Stop in-memory WAV playback (PlaySound is synchronous when called with NULL).
    PlaySound(nullptr, nullptr, 0);
    // Stop/close MCI in case an MP3 is playing via the fallback path.
    mciSendStringA("stop snpd wait", nullptr, 0, nullptr);
    mciSendStringA("close snpd wait", nullptr, 0, nullptr);
    logger::info("SkyrimNetDashboard: audio stopped.");
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

    // Stop current playback before swapping the buffer / writing a file.
    StopAudio();

    if (isWav) {
        // WAV: play directly from memory -- no temp file, no file lock.
        {
            std::lock_guard<std::mutex> lk(s_wavBufMtx);
            s_wavBuf = std::move(bytes);
        }
        // PlaySound with SND_MEMORY (synchronous -- no SND_ASYNC) blocks this background
        // thread until playback finishes, so we can fire the JS completion callback below.
        // This is safe because we are already on a detached worker thread.
        BOOL ok = PlaySound(reinterpret_cast<LPCSTR>(s_wavBuf.data()), nullptr,
                            SND_MEMORY | SND_NODEFAULT);
        logger::info("SkyrimNetDashboard: PlaySound finished: {}", ok ? "ok" : "failed");
        // Notify JS that audio has ended so the UI can reset button states.
        if (s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
            s_PrismaUI->Invoke(s_View,
                "window.__onAudioEnded&&window.__onAudioEnded();", nullptr);
            logger::info("SkyrimNetDashboard: __onAudioEnded dispatched");
        }
    } else {
        // Non-WAV fallback (MP3 etc.) still uses MCI + a temp file.
        // MCI stop/close with 'wait' already done by StopAudio() above.
        char tempDir[MAX_PATH] = {}; GetTempPathA(MAX_PATH, tempDir);
        std::string tempPath = std::string(tempDir) + "snpd_audio.mp3";
        logger::info("SkyrimNetDashboard: MCI fallback writing {} bytes to '{}'", bytes.size(), tempPath);
        {
            std::ofstream f(tempPath, std::ios::binary);
            if (!f) { logger::warn("SkyrimNetDashboard: MCI temp file open failed"); return; }
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        auto mciErr = [](MCIERROR e) -> std::string {
            if (!e) return "ok"; char b[256] = {}; mciGetErrorStringA(e, b, 256); return b; };
        std::string oc = "open \"" + tempPath + "\" type mpegvideo alias snpd";
        MCIERROR oe = mciSendStringA(oc.c_str(), nullptr, 0, nullptr);
        logger::info("SkyrimNetDashboard: MCI open result: {}", mciErr(oe));
        if (oe) return;
        MCIERROR pe = mciSendStringA("play snpd", nullptr, 0, nullptr);
        logger::info("SkyrimNetDashboard: MCI play result: {}", mciErr(pe));
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

// Called from the /audio-raw endpoint -- JS fetched blob bytes and POSTed them as
// raw binary so that blob: URLs (e.g. TTS) work despite being browser-only objects.
static void PlayAudioRaw(std::string bytes, std::string ct)
{
    std::thread([bytes = std::move(bytes), ct = std::move(ct)]() mutable {
        logger::info("SkyrimNetDashboard: audio-raw {} bytes, ct='{}'", bytes.size(), ct);
        PlayAudioBytes(std::move(bytes), ct);
    }).detach();
}
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
        PlayAudioUrl(src);
    } else if (action == "pause" || action == "stop") {
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
        [[maybe_unused]] bool focused = s_PrismaUI->Focus(s_View);
        logger::info("SkyrimNetDashboard: opened.");
    }
    else if (s_PrismaUI->HasFocus(s_View)) {
        // Signal hidden, unfocus, hide
        StopAudio();
        s_PrismaUI->Invoke(s_View, JS_HIDE);
        s_PrismaUI->Unfocus(s_View);
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
        StopAudio();
        s_PrismaUI->Invoke(s_View, JS_HIDE);
        s_PrismaUI->Unfocus(s_View);
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
      <button class="btn" id="FB"></button>
      <button id="XB"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg></button>
    </div>
  </div>
  <div id="C"><div id="OL"></div><iframe id="snpd-frame" src="/proxy"></iframe></div>
</div>
<script>
(function(){
  var W=document.getElementById('W'),
      B=document.getElementById('B'),
      FB=document.getElementById('FB'),
      XB=document.getElementById('XB'),
      HB=document.getElementById('HB'),
      RB=document.getElementById('RB'),
      OL=document.getElementById('OL'),
      FR=document.getElementById('snpd-frame');
  var fs=localStorage.getItem('snpd-fs')==='true';
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
  document.addEventListener('keydown',function(e){
    if(e.key==='F4'||e.keyCode===115){
      e.preventDefault();
      try{if(typeof window.closeDashboard==='function')window.closeDashboard('');}catch(_){}
    }
  },true);
  window.confirm=function(){return true;};
  window.alert=function(){};
  window.prompt=function(m,d){return d!==undefined?d:'';}
  // Belt-and-suspenders: patch the iframe's window object directly (same origin).
  // Ultralight may route iframe dialogs through the top-level window context,
  // so overriding here catches those cases too.
  var snpdFr=FR;
  function snpdPatch(){try{var cw=snpdFr.contentWindow;if(!cw)return;cw.confirm=function(){return true;};cw.alert=function(){};cw.prompt=function(m,d){return d!==undefined?d:'';};cw.open=function(url){if(url)cw.location.href=url;return cw;};}catch(_){}}
  snpdFr.addEventListener('load',snpdPatch);
  // C++ Invoke() runs in this (shell) frame -- bridge __onAudioEnded into the same-origin iframe.
  window.__onAudioEnded=function(){try{var cw=snpdFr.contentWindow;if(cw&&cw.__onAudioEnded)cw.__onAudioEnded();}catch(_){}};

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
)SHELL";

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
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

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

// Injects compat patches (confirm/alert/prompt/open) and a fast-typing
// newline guard into an HTML document string.
static std::string InjectPatches(std::string body)
{
    static const std::string injection =
        // ── Editor selection + key-repeat fixes ──────────────────────────────
        // Injected before the app's own scripts so these rules take effect
        // as soon as CM6 mounts its DOM.
        // Ultralight doesn't fire prefers-color-scheme:dark, so Tailwind dark:
        // variants never activate. We patch the CSS bundle instead (see PatchCSS).
        "<style>\n"
        // Force text selection on in CM6's contenteditable.
        // Ultralight defaults contenteditable to user-select:none which
        // breaks drag-to-select entirely.
        ".cm-editor,.cm-content,.cm-line{"
        "-webkit-user-select:text!important;user-select:text!important;"
        "cursor:text!important;}\n"
        // Make CM6's custom selection-background divs visible even if the
        // engine doesn't render ::selection pseudo-elements.
        ".cm-selectionBackground{"
        "background:#3b82f6!important;opacity:0.35!important;"
        "pointer-events:none!important;}\n"
        ".cm-focused .cm-selectionBackground{"
        "background:#3b82f6!important;opacity:0.45!important;}\n"
        "</style>\n"
        "<script>\n"
        // ── Key-repeat throttle for CM6 editors ──────────────────────────────
        // When holding Backspace/Delete Ultralight fires keydown at the OS
        // key-repeat rate (~30/s). Each event triggers a full CM6 transaction
        // + DOM update, creating noticeable lag. Throttle repeats to ≤15/s
        // (66 ms gap) — still fast enough to feel continuous.
        "(function(){"
        "var _krT=0;"
        "document.addEventListener('keydown',function(e){"
        "if(!e.repeat){_krT=0;return;}"
        "if(e.key!=='Backspace'&&e.key!=='Delete')return;"
        // Only throttle inside a CM6 editor so normal inputs aren't affected
        "var p=e.target;"
        "while(p){if(p.className&&typeof p.className==='string'"
        "&&p.className.indexOf('cm-content')>=0)break;p=p.parentElement;}"
        "if(!p)return;"
        "var n=Date.now();"
        "if(n-_krT<66){e.stopImmediatePropagation();e.preventDefault();return;}"
        "_krT=n;"
        "},true);"
        "})();\n"
        // ── CM6 drag-selection relay ──────────────────────────────────────────
        // Ultralight suppresses mousemove/pointermove while a button is held AND
        // may report e.buttons=0 on move events even when a button is down.
        // Two-pronged fix:
        //  1. setPointerCapture on pointerdown forces pointermove delivery to the
        //     captured element regardless of Ultralight's normal suppression.
        //  2. Use a _dragDown boolean flag instead of checking e.buttons, so we
        //     don't bail out if the browser incorrectly reports buttons=0.
        // On each move event we synthesize a shift+mousedown at the new position;
        // CM6 treats that as "extend selection to here" — the same path shift+click uses.
        "(function(){"
        "var _dragCm=null,_dragDown=false,_dragStarted=false,_lastDragMs=0,_dragStartX=0,_dragStartY=0;"
        // pointerdown: record cm-content target + force capture so pointermove fires.
        "document.addEventListener('pointerdown',function(e){"
        "if(e.button!==0||e.shiftKey)return;"
        "_dragDown=true;_dragStarted=false;_dragCm=null;_dragStartX=e.clientX;_dragStartY=e.clientY;"
        "var p=e.target;"
        "while(p){"
        "if(p.className&&typeof p.className==='string'"
        "&&p.className.indexOf('cm-content')>=0){"
        "_dragCm=p;"
        "try{p.setPointerCapture(e.pointerId);}catch(_){}"
        "break;}"
        "p=p.parentElement;}"
        "},true);"
        // mousedown fallback (if pointerdown doesn't fire).
        "document.addEventListener('mousedown',function(e){"
        "if(e.button!==0||e.shiftKey||_dragCm)return;"
        "_dragDown=true;_dragStarted=false;_dragStartX=e.clientX;_dragStartY=e.clientY;"
        "var p=e.target;"
        "while(p){"
        "if(p.className&&typeof p.className==='string'"
        "&&p.className.indexOf('cm-content')>=0){_dragCm=p;break;}"
        "p=p.parentElement;}"
        "},true);"
        "var _clear=function(){_dragDown=false;_dragStarted=false;_dragCm=null;};"
        "document.addEventListener('mouseup',_clear,true);"
        "document.addEventListener('pointerup',_clear,true);"
        "document.addEventListener('pointercancel',_clear,true);"
        "function _onDragMove(e){"
        "if(!_dragCm||!_dragDown)return;"
        // One-shot 4px threshold — only gates the very first movement so that
        // a stationary click doesn't fire. Once exceeded, _dragStarted=true and
        // all subsequent moves (including back toward the start) fire normally.
        "if(!_dragStarted){"
        "var dx=e.clientX-_dragStartX,dy=e.clientY-_dragStartY;"
        "if(dx*dx+dy*dy<16)return;"
        "_dragStarted=true;}"
        "var n=Date.now();if(n-_lastDragMs<16)return;_lastDragMs=n;"
        // Reach the EditorView via _dragCm.cmView.
        // _dragCm = .cm-content = view.contentDOM in CM6.
        // CM6 sets contentDOM.cmView = DocView; DocView.view = EditorView.
        // Property names are never mangled by terser so posAtCoords/dispatch work.
        // Fall back to self._snpdView (set by the bundle patch on each doc change).
        "var cv=_dragCm.cmView;"
        "var view=(cv&&cv.view&&cv.view.posAtCoords)?cv.view"
        ":((cv&&cv.posAtCoords)?cv:self._snpdView);"
        "if(!view||!view.posAtCoords||!view.dispatch)return;"
        "var pos=view.posAtCoords({x:e.clientX,y:e.clientY},false);"
        "if(pos===null||pos===undefined)return;"
        "var anchor=view.state.selection.main.anchor;"
        "view.dispatch({selection:{anchor:anchor,head:pos}});"
        "}"
        "document.addEventListener('mousemove',_onDragMove,{capture:true,passive:true});"
        "document.addEventListener('pointermove',_onDragMove,{capture:true,passive:true});"
        "})();\n"
        // ── Dialog / nav compat ───────────────────────────────────────────────
        "window.confirm=function(){return true;};\n"
        "window.alert=function(){};\n"
        "window.prompt=function(m,d){return d!==undefined?d:'';}\n"
        "window.open=function(url){if(url)window.location.href=url;return window;};\n"
        // ── Custom prompt modal ───────────────────────────────────────────────
        // Ultralight has no native window.prompt. Provide _snpdShowPrompt(msg, def)
        // which renders a modal overlay with a text input. On OK/Enter calls
        // self._snpdPromptCb(value); on Cancel calls self._snpdPromptCb(null).
        "(function(){"
        "var _ov=null;"
        "self._snpdShowPrompt=function(msg,def){"
        "if(_ov)_ov.remove();"
        "_ov=document.createElement('div');"
        "_ov.style.cssText='position:fixed;inset:0;z-index:2147483647;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,.6)';"
        "var box=document.createElement('div');"
        "box.style.cssText='background:#1f2937;border:1px solid #374151;border-radius:8px;padding:24px;min-width:340px;max-width:480px;box-shadow:0 20px 60px rgba(0,0,0,.8);display:flex;flex-direction:column;gap:12px';"
        "var lbl=document.createElement('div');"
        "lbl.style.cssText='color:#f9fafb;font-size:14px;font-family:Consolas,monospace';"
        "lbl.textContent=msg||'';"
        "var inp=document.createElement('input');"
        "inp.type='text';inp.value=def||'';"
        "inp.style.cssText='background:#111827;border:1px solid #4b5563;border-radius:4px;color:#f9fafb;font-size:14px;font-family:Consolas,monospace;padding:8px 10px;outline:none;width:100%;box-sizing:border-box';"
        "var row=document.createElement('div');"
        "row.style.cssText='display:flex;gap:8px;justify-content:flex-end';"
        "var ok=document.createElement('button');"
        "ok.textContent='OK';"
        "ok.style.cssText='padding:6px 18px;background:#3b82f6;border:none;border-radius:4px;color:#fff;font-size:13px;cursor:pointer';"
        "var cancel=document.createElement('button');"
        "cancel.textContent='Cancel';"
        "cancel.style.cssText='padding:6px 18px;background:#374151;border:none;border-radius:4px;color:#fff;font-size:13px;cursor:pointer';"
        "function _ok(){var v=inp.value.trim();_ov.remove();_ov=null;if(self._snpdPromptCb)self._snpdPromptCb(v||null);}"
        "function _cancel(){_ov.remove();_ov=null;if(self._snpdPromptCb)self._snpdPromptCb(null);}"
        "ok.addEventListener('click',_ok);"
        "cancel.addEventListener('click',_cancel);"
        "inp.addEventListener('keydown',function(e){"
        "if(e.key==='Enter'){e.preventDefault();_ok();}"
        "else if(e.key==='Escape'){e.preventDefault();_cancel();}"
        "e.stopPropagation();});"  // prevent CM6 key handlers eating keystrokes
        "row.appendChild(cancel);row.appendChild(ok);"
        "box.appendChild(lbl);box.appendChild(inp);box.appendChild(row);"
        "_ov.appendChild(box);document.body.appendChild(_ov);"
        "setTimeout(function(){inp.focus();inp.select();},50);"
        "};"
        "})();\n"
        // ── Audio bridge ─────────────────────────────────────────────────────
        // Ultralight has no HTMLMediaElement/Web Audio support. Override the
        // Audio constructor and HTMLAudioElement prototype so SkyrimNet's calls
        // POST to /audio on our local proxy server, which fetches the URL and
        // plays it via Windows MCI. Using fetch avoids cross-frame JS issues.
        "(function(){"
        // Active Audio instances waiting for completion.
        "var _paActive=[];"
        // Called from C++ via Invoke() when PlaySound finishes.
        // Fires onended on every tracked Audio instance and dispatches a document event
        // so that React UI components can reset their button/state.
        "window.__onAudioEnded=function(){"
        "var a=_paActive.splice(0,_paActive.length);"
        "for(var i=0;i<a.length;i++){"
        "try{a[i].paused=true;a[i].ended=true;}catch(e){}"
        "try{if(typeof a[i].onended==='function')a[i].onended(new Event('ended'));}catch(e){}"
        "}"
        "try{document.dispatchEvent(new Event('snpd:audioended'));}catch(e){}"
        "};"
        "var _pa=function(action,src){"
        "try{"
        "var r=src||'';"
        "try{r=(new URL(src||'',window.location.href)).href;}catch(e){}"
        // blob: URLs only exist in browser memory — fetch bytes here and POST binary to /audio-raw
        "if(action==='play'&&r.indexOf('blob:')===0){"
        "fetch(r).then(function(res){"
        "var ct=res.headers.get('Content-Type')||'audio/mpeg';"
        "return res.arrayBuffer().then(function(ab){"
        "return fetch('/audio-raw',{method:'POST',headers:{'Content-Type':ct},body:ab});});"
        "}).catch(function(){});"
        "}else{"
        "fetch('/audio',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({action:action,src:r})}).catch(function(){});"
        "}"
        "}catch(e){}"
        "};"
        "window.Audio=function AB(src){this._src=src||'';"
        "this.currentTime=0;this.volume=1;this.paused=true;"
        "this.ended=false;this.loop=false;"
        "this.onended=null;this.onerror=null;this.oncanplaythrough=null;"
        "};"
        "Object.defineProperty(window.Audio.prototype,'src',{"
        "get:function(){return this._src;},set:function(v){this._src=v;}});"
        "window.Audio.prototype.play=function(){"
        // Track this instance so __onAudioEnded can fire .onended on it.
        "_paActive.push(this);"
        "this.paused=false;_pa('play',this._src);"
        "return{then:function(fn){if(fn)fn();return this;},catch:function(){return this;}};"
        "};"
        "window.Audio.prototype.pause=function(){this.paused=true;_pa('pause',this._src);};"
        "window.Audio.prototype.load=function(){};"
        "window.Audio.prototype.addEventListener=function(){};"
        "window.Audio.prototype.removeEventListener=function(){};"
        "if(typeof HTMLAudioElement!=='undefined'){"
        "try{"
        "HTMLAudioElement.prototype.play=function(){"
        "_pa('play',this.src||this.currentSrc||'');"
        "return{then:function(fn){if(fn)fn();return this;},catch:function(){return this;}};"
        "};"
        "HTMLAudioElement.prototype.pause=function(){"
        "_pa('pause',this.src||this.currentSrc||'');"
        "};"
        "}catch(e){}}"
        "})();\n"
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

// Patches the React app JS bundle before serving it.
// Currently: debounces the CodeMirror onChange -> validation call from 0ms to 600ms
// so syntax validation doesn't run on every keystroke, eliminating typing lag.
static std::string PatchCSS(std::string body)
{
    // Ultralight never fires prefers-color-scheme:dark, so Tailwind's compiled
    // dark: variants are dead code. Replace the media query wrapper with
    // @media all so all dark: rules always apply. They sit later in the CSS
    // than their light counterparts, so they win the cascade automatically.
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
    return body;
}

static std::string PatchBundle(std::string body)
{
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
                    ofn.lpstrFilter  = "All Files\0*.*\0Log Files\0*.log\0Text Files\0*.txt\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrFile    = fileBuf;
                    ofn.nMaxFile     = MAX_PATH;
                    ofn.lpstrInitialDir = initDir.c_str();
                    ofn.lpstrTitle   = "Save log file";
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
            } else if (path == "/audio-raw") {
                // Binary audio endpoint — used for blob: URLs (e.g. TTS).
                // JS fetches the blob and POSTs raw bytes here with the correct Content-Type.
                logger::info("SkyrimNetDashboard: /audio-raw POST {} bytes, ct='{}'", reqBody.size(), reqCT);
                PlayAudioRaw(reqBody, reqCT);
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

    // 1. Get PrismaUI API
    s_PrismaUI = static_cast<PRISMA_UI_API::IVPrismaUI1*>(
        PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));

    if (!s_PrismaUI) {
        logger::critical("SkyrimNetDashboard: Failed to get PrismaUI API. Is PrismaUI installed?");
        return;
    }

    // 2. Parse the SkyrimNet server address for the audio subsystem
    {
        std::string u = SKYRIMNET_URL;
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

    // 3. Build the chrome shell page and start the local HTTP server
    std::string shellHtml = BuildShellHtml();
    uint16_t    port      = StartShellServer(shellHtml, SKYRIMNET_URL);
    if (port == 0) {
        logger::critical("SkyrimNetDashboard: Failed to start shell server.");
        return;
    }
    std::string shellUrl = "http://127.0.0.1:" + std::to_string(port) + "/shell";

    // 3. Create a view that loads the shell page (chrome + iframe pointing at SkyrimNet)
    s_View = s_PrismaUI->CreateView(shellUrl.c_str(), OnDomReady);
    s_PrismaUI->SetScrollingPixelSize(s_View, 120);
    s_PrismaUI->Hide(s_View); // Start hidden until the user opens it

    // Close button in the HTML calls window.closeDashboard('') — wire it to OnToggle
    s_PrismaUI->RegisterJSListener(s_View, "closeDashboard", [](const char*) { OnToggle(); });

    logger::info("SkyrimNetDashboard: Shell view created at {} (iframe -> {})", shellUrl, SKYRIMNET_URL);

    // 3. Register F4 hotkey to toggle the overlay
    KeyHandler::RegisterSink();
    [[maybe_unused]] auto toggleHandle = KeyHandler::GetSingleton()->Register(
        TOGGLE_KEY, KeyEventType::KEY_DOWN, OnToggle);
    [[maybe_unused]] auto escHandle = KeyHandler::GetSingleton()->Register(
        ESC_KEY, KeyEventType::KEY_DOWN, OnClose);
    [[maybe_unused]] auto inspectorHandle = KeyHandler::GetSingleton()->Register(
        INSPECTOR_KEY, KeyEventType::KEY_DOWN, OnToggleInspector);

    logger::info("SkyrimNetDashboard: Ready. F4 = open/close, F5 = toggle inspector.");
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name        = PLUGIN_NAME;
    a_info->version     = 1;

    if (a_skse->IsEditor()) {
        return false;
    }

    const auto ver = a_skse->RuntimeVersion();
    if (ver < SKSE::RUNTIME_SSE_1_5_39) {
        return false;
    }

    return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse, false);  // false = don't let SKSE init the logger (plugin uses old-style SKSEPlugin_Query, so GetPluginName() is empty and log::init() would create ".log" instead of "SkyrimNetPrismaDashboard.log")
    SetupLog();
    logger::info("SkyrimNetDashboard: Plugin loaded.");

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener(MessageHandler)) {
        logger::error("SkyrimNetDashboard: Failed to register messaging listener.");
        return false;
    }

    return true;
}
