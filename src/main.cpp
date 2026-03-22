#include "pch.h"
#include "PrismaUI_API.h"
#include "keyhandler/keyhandler.h"

// Modular injection components
#include "injections/audio_polyfill.h"
#include "injections/clipboard.h"
#include "injections/drag_select.h"
#include "injections/editor_fixes.h"
#include "injections/file_input.h"
#include "injections/autoscroll.h"

// -----------------------------------------------------------------------
//  SkyrimNet Prisma Dashboard
//  Loads the SkyrimNet web dashboard inside Skyrim via PrismaUI.
//  Press F4 to open/close the overlay.
// -----------------------------------------------------------------------

#include "config.h"
#include "audio.h"
#include "shell_html.h"
#include "http_proxy.h"

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

static constexpr const char* JS_GO_HOME =
    "(function(){var fr=document.getElementById('snpd-frame');if(fr)fr.src='/proxy';})();";

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
        // Navigate home while the view is hidden/background so the user
        // sees the correct page immediately on next open (no visible reload).
        // Only meaningful when keepBg=false (view will be hidden); with keepBg
        // on the page stays alive so defaultHome would disrupt the current page.
        if (s_cfg.defaultHome && !s_cfg.keepBg)
            s_PrismaUI->Invoke(s_View, JS_GO_HOME);
        if (!s_cfg.keepBg)
            s_PrismaUI->Hide(s_View);
        logger::info("SkyrimNetDashboard: closed.");
    }
    else {
        // Visible but not focused — re-focus (pause if configured)
        [[maybe_unused]] bool refocused = s_PrismaUI->Focus(s_View, s_cfg.pauseGame);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!s_PrismaUI || !s_View) { prevC = prevV = prevX = false; continue; }
            if (s_PrismaUI->IsHidden(s_View) || !s_PrismaUI->HasFocus(s_View)) { prevC = prevV = prevX = false; continue; }
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
        s_audioHost = "localhost";
        s_audioPort = 8080;
        ParseHostPort(s_cfg.url, s_audioHost, s_audioPort);
        logger::info("SkyrimNetDashboard: audio backend {}:{}", s_audioHost, s_audioPort);
    }

    // 4. Build the chrome shell page and start the local HTTP server
    std::string shellHtml = BuildShellHtml();
    uint16_t    port      = StartShellServer(shellHtml, s_cfg.url);
    if (port == 0) {
        logger::critical("SkyrimNetDashboard: Failed to start shell server.");
        return;
    }
    std::string shellUrl = "http://127.0.0.1:" + std::to_string(port) + "/shell";

    // 5. Create a view that loads the shell page (chrome + iframe pointing at SkyrimNet)
    s_View = s_PrismaUI->CreateView(shellUrl.c_str(), OnDomReady);
    s_PrismaUI->SetScrollingPixelSize(s_View, 120);
    s_PrismaUI->Hide(s_View); // Always start hidden; keepBg only affects close-to-background behavior

    StartClipboardMonitor(); // C++ Ctrl+C/V polling — bypasses Ultralight event quirks

    // Close button in the HTML calls window.closeDashboard('') — wire it to OnToggle
    s_PrismaUI->RegisterJSListener(s_View, "closeDashboard", [](const char*) { OnToggle(); });

    logger::info("SkyrimNetDashboard: Shell view created at {} (iframe -> {})", shellUrl, s_cfg.url);

    // 6. Register hotkey to toggle the overlay
    s_toggleKey.store(static_cast<uint32_t>(s_cfg.hotKey));
    KeyHandler::RegisterSink();
    s_toggleHandle = KeyHandler::GetSingleton()->Register(
        s_toggleKey.load(), KeyEventType::KEY_DOWN, OnToggle);
    [[maybe_unused]] auto escHandle = KeyHandler::GetSingleton()->Register(
        ESC_KEY, KeyEventType::KEY_DOWN, OnClose);

    logger::info("SkyrimNetDashboard: Ready. {} = open/close.",
        DxKeyName(s_toggleKey.load()));
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
