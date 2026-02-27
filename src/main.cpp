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
static constexpr uint32_t    INSPECTOR_KEY = 0x3F; // F5

static PRISMA_UI_API::IVPrismaUI1* s_PrismaUI = nullptr;
static PrismaView                  s_View      = 0;

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

static void OnToggle()
{
    if (!s_PrismaUI || !s_PrismaUI->IsValid(s_View)) {
        logger::warn("SkyrimNetDashboard: view is not valid.");
        return;
    }

    if (s_PrismaUI->IsHidden(s_View)) {
        // Show and focus
        s_PrismaUI->Show(s_View);
        [[maybe_unused]] bool focused = s_PrismaUI->Focus(s_View);
        logger::info("SkyrimNetDashboard: opened.");
    }
    else if (s_PrismaUI->HasFocus(s_View)) {
        // Close
        s_PrismaUI->Unfocus(s_View);
        s_PrismaUI->Hide(s_View);
        logger::info("SkyrimNetDashboard: closed.");
    }
    else {
        // Visible but not focused — re-focus
        [[maybe_unused]] bool refocused = s_PrismaUI->Focus(s_View);
    }
}

static void OnDomReady(PrismaView view)
{
    s_PrismaUI->Invoke(view, R"js(
        (function() {
            // ── Window chrome ──────────────────────────────────────────────────────
            // Default: windowed mode (centered, draggable, resizable).
            // The PrismaUI view is always fullscreen/transparent; the window
            // appearance is achieved purely through CSS/JS on top of the React app.

            var WIN_W = 1400, WIN_H = 860; // default window size (px)
            var _windowed = true;

            var style = document.createElement('style');
            style.id = 'snpd-style';
            style.textContent = `
                #snpd-chrome {
                    position: fixed;
                    z-index: 2147483647;
                    display: flex;
                    flex-direction: column;
                    background: transparent;
                    /* box-shadow and border-radius applied to titlebar+content together */
                    filter: drop-shadow(0 20px 60px rgba(0,0,0,0.85));
                }
                #snpd-titlebar {
                    display: flex;
                    align-items: center;
                    justify-content: space-between;
                    padding: 0 12px;
                    height: 36px;
                    background: #0f172a;
                    border-radius: 8px 8px 0 0;
                    cursor: grab;
                    user-select: none;
                    flex-shrink: 0;
                    border-bottom: 1px solid rgba(255,255,255,0.08);
                }
                #snpd-titlebar:active { cursor: grabbing; }
                #snpd-title {
                    font-family: system-ui, sans-serif;
                    font-size: 13px;
                    font-weight: 600;
                    color: rgba(255,255,255,0.7);
                    letter-spacing: 0.02em;
                }
                #snpd-controls {
                    display: flex;
                    gap: 8px;
                    align-items: center;
                }
                .snpd-btn {
                    width: 14px; height: 14px;
                    border-radius: 50%;
                    border: none;
                    cursor: pointer;
                    font-size: 0;
                    flex-shrink: 0;
                }
                #snpd-btn-fs  { background: #3b82f6; }
                #snpd-btn-close { background: #ef4444; }
                .snpd-btn:hover { filter: brightness(1.3); }
                #snpd-content {
                    flex: 1;
                    position: relative;
                    overflow: hidden;
                    border-radius: 0 0 8px 8px;
                    background: transparent;
                }
                /* React root fills the chrome content area */
                #snpd-content > #root {
                    position: absolute !important;
                    inset: 0 !important;
                    width: 100% !important;
                    height: 100% !important;
                    overflow: auto;
                }
                /* Resize handle */
                #snpd-resize {
                    position: absolute;
                    right: 0; bottom: 0;
                    width: 16px; height: 16px;
                    cursor: se-resize;
                    z-index: 1;
                }
                /* Fullscreen mode */
                body.snpd-fullscreen #snpd-chrome {
                    top: 0 !important; left: 0 !important;
                    width: 100vw !important; height: 100vh !important;
                    filter: none;
                }
                body.snpd-fullscreen #snpd-titlebar { border-radius: 0; }
                body.snpd-fullscreen #snpd-content  { border-radius: 0; }
            `;
            document.head.appendChild(style);

            // Build chrome elements
            var chrome   = document.createElement('div'); chrome.id = 'snpd-chrome';
            var titlebar = document.createElement('div'); titlebar.id = 'snpd-titlebar';
            var title    = document.createElement('span'); title.id = 'snpd-title'; title.textContent = 'SkyrimNet';
            var controls = document.createElement('div'); controls.id = 'snpd-controls';
            var btnFs    = document.createElement('button'); btnFs.className = 'snpd-btn'; btnFs.id = 'snpd-btn-fs';  btnFs.title = 'Toggle fullscreen';
            var btnClose = document.createElement('button'); btnClose.className = 'snpd-btn'; btnClose.id = 'snpd-btn-close'; btnClose.title = 'Close';
            var content  = document.createElement('div'); content.id = 'snpd-content';
            var resizeH  = document.createElement('div'); resizeH.id = 'snpd-resize';

            controls.appendChild(btnFs);
            controls.appendChild(btnClose);
            titlebar.appendChild(title);
            titlebar.appendChild(controls);
            content.appendChild(resizeH);
            chrome.appendChild(titlebar);
            chrome.appendChild(content);

            // Move React root into chrome content
            var root = document.getElementById('root');
            if (root) content.appendChild(root);
            document.body.appendChild(chrome);

            // Set initial windowed position/size
            function applyWindowed() {
                var vw = window.innerWidth  || 1920;
                var vh = window.innerHeight || 1080;
                chrome.style.width  = WIN_W + 'px';
                chrome.style.height = WIN_H + 'px';
                chrome.style.left   = Math.round((vw - WIN_W) / 2) + 'px';
                chrome.style.top    = Math.round((vh - WIN_H) / 2) + 'px';
            }
            applyWindowed();

            // Toggle fullscreen
            btnFs.addEventListener('click', function(e) {
                e.stopPropagation();
                _windowed = !_windowed;
                if (_windowed) {
                    document.body.classList.remove('snpd-fullscreen');
                    applyWindowed();
                } else {
                    document.body.classList.add('snpd-fullscreen');
                }
            });

            // Close button — calls back to SKSE via interop (falls back to nothing if not wired)
            btnClose.addEventListener('click', function(e) {
                e.stopPropagation();
                try { window.skybridge && window.skybridge('closeView', ''); } catch(_) {}
            });

            // ── Drag ───────────────────────────────────────────────────────────────
            (function() {
                var dragging = false, ox = 0, oy = 0;
                titlebar.addEventListener('mousedown', function(e) {
                    if (e.target !== titlebar && e.target !== title) return;
                    if (!_windowed) return;
                    dragging = true;
                    ox = e.clientX - chrome.offsetLeft;
                    oy = e.clientY - chrome.offsetTop;
                    e.preventDefault();
                });
                document.addEventListener('mousemove', function(e) {
                    if (!dragging) return;
                    chrome.style.left = (e.clientX - ox) + 'px';
                    chrome.style.top  = (e.clientY - oy) + 'px';
                });
                document.addEventListener('mouseup', function() { dragging = false; });
            })();

            // ── Resize ─────────────────────────────────────────────────────────────
            (function() {
                var resizing = false, sx = 0, sy = 0, sw = 0, sh = 0;
                resizeH.addEventListener('mousedown', function(e) {
                    if (!_windowed) return;
                    resizing = true;
                    sx = e.clientX; sy = e.clientY;
                    sw = chrome.offsetWidth; sh = chrome.offsetHeight;
                    e.preventDefault(); e.stopPropagation();
                });
                document.addEventListener('mousemove', function(e) {
                    if (!resizing) return;
                    var nw = Math.max(600, sw + (e.clientX - sx));
                    var nh = Math.max(400, sh + (e.clientY - sy));
                    chrome.style.width  = nw + 'px';
                    chrome.style.height = nh + 'px';
                    WIN_W = nw; WIN_H = nh;
                });
                document.addEventListener('mouseup', function() { resizing = false; });
            })();

            // ── Ultralight compat patches ──────────────────────────────────────────

            // window.open / target=_blank → in-page navigation
            window.open = function(url) {
                if (url) window.location.href = url;
                return window;
            };
            document.addEventListener('click', function(e) {
                var el = e.target;
                while (el && el.tagName !== 'A') el = el.parentElement;
                if (el && el.tagName === 'A' && el.target === '_blank' && el.href) {
                    e.preventDefault();
                    window.location.href = el.href;
                }
            }, true);

            // Stub native dialogs
            window.confirm = function() { return true; };
            window.alert   = function() {};
            window.prompt  = function(msg, def) { return def !== undefined ? def : ''; };

            // React Router: fire popstate after pushState/replaceState
            (function() {
                function patchHistory(method) {
                    var orig = history[method].bind(history);
                    history[method] = function(state, title, url) {
                        orig(state, title, url);
                        window.dispatchEvent(new PopStateEvent('popstate', { state: state }));
                    };
                }
                patchHistory('pushState');
                patchHistory('replaceState');
            })();

            // Repaint nudge — debounced 150ms after last DOM mutation
            var _repaintTimer = null;
            var _obs = new MutationObserver(function() {
                if (_repaintTimer) clearTimeout(_repaintTimer);
                _repaintTimer = setTimeout(function() {
                    _repaintTimer = null;
                    _obs.disconnect();
                    document.body.style.outline = 'none';
                    void document.body.offsetHeight;
                    _obs.observe(document.body, { childList: true, subtree: true, characterData: true });
                }, 150);
            });
            _obs.observe(document.body, { childList: true, subtree: true, characterData: true });

            // Synthetic click (mousedown → click) + suppress native follow-up
            var _clicking = false;
            var _suppressNextClick = false;
            document.addEventListener('click', function(e) {
                if (_suppressNextClick && !_clicking) {
                    _suppressNextClick = false;
                    e.stopImmediatePropagation();
                    e.preventDefault();
                }
            }, true);
            document.addEventListener('mousedown', function(e) {
                if (_clicking) return;
                var el = e.target;
                if (!el || el === document || el === document.body) return;
                _clicking = true;
                try {
                    el.dispatchEvent(new MouseEvent('click', {
                        bubbles: true, cancelable: true, view: window,
                        button: 0, buttons: 1,
                        clientX: e.clientX, clientY: e.clientY,
                        screenX: e.screenX, screenY: e.screenY
                    }));
                } finally {
                    _clicking = false;
                }
                _suppressNextClick = true;
            }, true);
        })();
    )js");
    logger::info("SkyrimNetDashboard: window chrome injected.");
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

    // 2. Create a view that loads the SkyrimNet dashboard URL directly
    s_View = s_PrismaUI->CreateView(SKYRIMNET_URL, OnDomReady);
    s_PrismaUI->SetScrollingPixelSize(s_View, 50); // comfortable scroll speed
    s_PrismaUI->Hide(s_View); // Start hidden until the user opens it

    logger::info("SkyrimNetDashboard: View created for {}", SKYRIMNET_URL);

    // 3. Register F4 hotkey to toggle the overlay
    KeyHandler::RegisterSink();
    [[maybe_unused]] auto toggleHandle = KeyHandler::GetSingleton()->Register(
        TOGGLE_KEY, KeyEventType::KEY_DOWN, OnToggle);
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
    SetupLog();

    SKSE::Init(a_skse);

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener(MessageHandler)) {
        logger::error("SkyrimNetDashboard: Failed to register messaging listener.");
        return false;
    }

    return true;
}
