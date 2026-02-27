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

            // F4 closes the dashboard from within the browser context
            document.addEventListener('keydown', function(e) {
                if (e.key === 'F4' || e.keyCode === 115) {
                    e.preventDefault();
                    try { if (typeof window.closeDashboard === 'function') window.closeDashboard(''); } catch(_) {}
                }
            }, true);
        })();
    )js");
    logger::info("SkyrimNetDashboard: compat patches injected.");
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
