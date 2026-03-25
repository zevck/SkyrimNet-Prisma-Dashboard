#pragma once
// ── Full reverse-proxy helpers + HTTP server ─────────────────────────────────

// Forward declarations for functions defined in main.cpp after this header.
static void OnToggle();
// ── Full reverse-proxy helpers ────────────────────────────────────────────────
// All SkyrimNet assets are fetched through our local server so the iframe
// never makes cross-origin requests.  <script type="module"> in particular
// is always CORS-checked by WebKit regardless of the crossorigin attribute,
// so the only way to avoid it is to serve everything from the same origin.

// Thread-safe hostname resolution.  gethostbyname() returns a pointer to
// static storage, so concurrent calls from the per-connection HTTP threads
// would corrupt each other's results.  The mutex serialises the slow path;
// the fast path (dotted-decimal IPs like "127.0.0.1") never takes the lock.
static std::mutex s_dnsMtx;
static bool ResolveHost(const std::string& host, sockaddr_in& addr)
{
    auto ipAddr = inet_addr(host.c_str());
    if (ipAddr != INADDR_NONE) {
        addr.sin_addr.s_addr = ipAddr;
        return true;
    }
    std::lock_guard<std::mutex> lk(s_dnsMtx);
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        logger::warn("SkyrimNetDashboard: DNS resolution failed for '{}' (WSA={})", host, WSAGetLastError());
        return false;
    }
    addr.sin_addr.s_addr = *reinterpret_cast<u_long*>(he->h_addr_list[0]);
    {
        auto ip = addr.sin_addr.s_addr;
        logger::info("SkyrimNetDashboard: resolved '{}' -> {}.{}.{}.{}",
                     host, ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }
    return true;
}

// Connect with a timeout (milliseconds).  Returns true on success.
// Uses non-blocking connect + select so we don't hang forever if the
// backend is unreachable (e.g. SkyrimNet not running → black window).
static bool ConnectWithTimeout(SOCKET s, sockaddr_in& addr, int timeoutMs = 5000)
{
    auto ip = addr.sin_addr.s_addr;
    uint16_t port = ntohs(addr.sin_port);

    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);

    int cr = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (cr == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set wrSet, exSet;
            FD_ZERO(&wrSet); FD_SET(s, &wrSet);
            FD_ZERO(&exSet); FD_SET(s, &exSet);
            timeval tv;
            tv.tv_sec  = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = select(0, nullptr, &wrSet, &exSet, &tv);
            if (sel <= 0 || FD_ISSET(s, &exSet)) {
                // Retrieve the actual socket-level error (more reliable than WSAGetLastError after select)
                int soErr = 0; int soLen = sizeof(soErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
                u_long block = 0; ioctlsocket(s, FIONBIO, &block);
                if (sel == 0)
                    logger::warn("SkyrimNetDashboard: connect to {}.{}.{}.{}:{} timed out after {}ms",
                                 ip & 0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF, port, timeoutMs);
                else
                    logger::warn("SkyrimNetDashboard: connect to {}.{}.{}.{}:{} refused "
                                 "(sel={}, SO_ERROR={}, WSA={}, wrSet={}, exSet={})",
                                 ip & 0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF, port,
                                 sel, soErr, WSAGetLastError(),
                                 FD_ISSET(s, &wrSet) ? 1 : 0, FD_ISSET(s, &exSet) ? 1 : 0);
                return false;
            }
        } else {
            u_long block = 0; ioctlsocket(s, FIONBIO, &block);
            logger::warn("SkyrimNetDashboard: connect to {}.{}.{}.{}:{} failed immediately (WSA={})",
                         ip & 0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF, port, err);
            return false;
        }
    }

    u_long block = 0;
    ioctlsocket(s, FIONBIO, &block);
    return true;
}

// ── IPv6 loopback fallback ───────────────────────────────────────────────────
// ws2tcpip.h (which defines sockaddr_in6) conflicts with the winsock.h that
// CommonLibSSE pulls in, so we define the minimal IPv6 structures manually.
// This is ONLY used for the ::1 loopback fallback — not general IPv6 DNS.
#ifndef AF_INET6
#define AF_INET6 23
#endif
struct SockAddrIn6 {
    short    sin6_family;   // AF_INET6
    u_short  sin6_port;
    u_long   sin6_flowinfo;
    u_char   sin6_addr[16]; // ::1 = all zeros except last byte = 1
    u_long   sin6_scope_id;
};

// Attempt an IPv6 connect.  Parses the host as a raw IPv6 address (no brackets).
// Returns a connected socket or INVALID_SOCKET.
static SOCKET ConnectIPv6(const std::string& host, uint16_t port)
{
    SOCKET s6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s6 == INVALID_SOCKET) {
        logger::warn("SkyrimNetDashboard: IPv6 socket() failed (WSA={})", WSAGetLastError());
        return INVALID_SOCKET;
    }
    SockAddrIn6 addr6{};
    addr6.sin6_family = static_cast<short>(AF_INET6);
    addr6.sin6_port   = htons(port);
    // Parse the IPv6 address into sin6_addr.
    // For loopback "::1" we just set the last byte.  For other addresses
    // we'd need inet_pton (in ws2tcpip.h which we can't include), so we
    // only support the common loopback forms for now.
    if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
        addr6.sin6_addr[15] = 1;
    } else {
        logger::warn("SkyrimNetDashboard: unsupported IPv6 address '{}' (only ::1 is supported)", host);
        closesocket(s6);
        return INVALID_SOCKET;
    }

    // Non-blocking connect with timeout
    u_long nonBlock = 1;
    ioctlsocket(s6, FIONBIO, &nonBlock);
    int cr = connect(s6, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6));
    if (cr == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wrSet, exSet;
        FD_ZERO(&wrSet); FD_SET(s6, &wrSet);
        FD_ZERO(&exSet); FD_SET(s6, &exSet);
        timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        int sel = select(0, nullptr, &wrSet, &exSet, &tv);
        if (sel <= 0 || FD_ISSET(s6, &exSet)) {
            logger::warn("SkyrimNetDashboard: IPv6 [{}]:{} connect failed (sel={})", host, port, sel);
            u_long block = 0; ioctlsocket(s6, FIONBIO, &block);
            closesocket(s6);
            return INVALID_SOCKET;
        }
    } else if (cr == SOCKET_ERROR) {
        logger::warn("SkyrimNetDashboard: IPv6 [{}]:{} connect error (WSA={})", host, port, WSAGetLastError());
        closesocket(s6);
        return INVALID_SOCKET;
    }
    u_long block = 0;
    ioctlsocket(s6, FIONBIO, &block);
    return s6;
}

// Creates a connected socket to host:port.
// - Explicit IPv6 host ("::1"): connects via IPv6 directly.
// - "localhost": tries IPv4 first, falls back to IPv6 [::1].
//   Once IPv6 succeeds for localhost, all future connections skip IPv4.
// - Everything else: IPv4 only.
// Returns INVALID_SOCKET on failure.
static std::atomic<bool> s_useIPv6{false};

static SOCKET ConnectToHost(const std::string& host, uint16_t port)
{
    // ── Explicit IPv6 address (from INI like "http://[::1]:8080/") ──────
    if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
        return ConnectIPv6(host, port);
    }

    // ── IPv4 attempt (skip if we already know localhost needs IPv6) ──────
    if (!s_useIPv6.load()) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (ResolveHost(host, addr) && ConnectWithTimeout(s, addr))
                return s;
            closesocket(s);
        }
    }

    // ── IPv6 loopback fallback for "localhost" ──────────────────────────
    if (host == "localhost") {
        if (!s_useIPv6.load())
            logger::info("SkyrimNetDashboard: IPv4 connect to localhost:{} failed, trying IPv6 [::1]", port);
        SOCKET s6 = ConnectIPv6("::1", port);
        if (s6 != INVALID_SOCKET) {
            if (!s_useIPv6.load()) {
                s_useIPv6.store(true);
                logger::info("SkyrimNetDashboard: IPv6 cached for future localhost connections");
            }
            return s6;
        }
    }

    return INVALID_SOCKET;
}

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
    SOCKET s = ConnectToHost(host, port);
    if (s == INVALID_SOCKET) {
        logger::error("SkyrimNetDashboard: FetchResource cannot connect to {}:{}", host, port);
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
    if (n == SOCKET_ERROR) {
        logger::warn("SkyrimNetDashboard: FetchResource recv error for {} {} (WSA={}, {} bytes so far)",
                     method, path, WSAGetLastError(), response.size());
    }
    closesocket(s);

    if (response.empty()) {
        logger::warn("SkyrimNetDashboard: FetchResource got empty response for {} {}:{}{}", method, host, port, path);
        return {"", "text/plain", 503};
    }

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
    // Restore persisted localStorage synchronously BEFORE any app scripts run.
    // This must be inline (not async fetch) so the React app sees the data
    // on its very first localStorage read.
    std::string storageRestore;
    {
        std::string storageJson = ReadStorage();
        if (storageJson.size() > 2) {
            std::string b64 = Base64Encode(storageJson);
            storageRestore = "<script>try{var d=JSON.parse(atob('";
            storageRestore += b64;
            storageRestore += "'));var k=Object.keys(d);for(var i=0;i<k.length;i++)localStorage.setItem(k[i],d[k[i]]);}catch(e){}</script>\n";
        }
    }

    // Build injection from modular components
    std::string injection =
        storageRestore +
        Injections::GetEditorFixes() +
        "<script>\n" +
        Injections::GetClipboardIntegration() +
        Injections::GetAudioPolyfill() +
        Injections::GetDragSelect() +
        Injections::GetFileInputPolyfill() +
        Injections::GetAutoscroll() +
        Injections::GetStoragePersist() +
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

    // Ultralight renders CSS transitions in software — every transition-property
    // frame is a full repaint.  Tailwind compiles hundreds of transition-*
    // utilities; strip them to avoid continuous repainting on hover/focus/etc.
    {
        int count = 0;
        std::string::size_type pos = 0;
        // Covers transition:, transition-property:, transition-duration:, etc.
        static const std::string tp = "transition";
        while ((pos = body.find(tp, pos)) != std::string::npos) {
            // Must be at start or after { or ; (i.e. a CSS property, not a substring)
            if (pos > 0) {
                char prev = body[pos - 1];
                if (prev != '{' && prev != ';' && prev != ' ' && prev != '\n') {
                    pos += tp.size();
                    continue;
                }
            }
            // Check it looks like a declaration (followed by - or :)
            auto after = pos + tp.size();
            if (after < body.size() && body[after] != ':' && body[after] != '-') {
                pos = after;
                continue;
            }
            auto semi = body.find(';', pos);
            if (semi == std::string::npos) break;
            std::fill(body.begin() + pos, body.begin() + semi + 1, ' ');
            pos = semi + 1;
            ++count;
        }
        if (count)
            logger::info("SkyrimNetDashboard: PatchCSS: stripped {} transition declarations", count);
    }

    // CSS animations (keyframes) also cause continuous repaints in Ultralight.
    // Strip animation: and animation-* declarations, but preserve spin/rotate
    // animations used for loading spinners (important for perceived responsiveness).
    {
        int count = 0;
        std::string::size_type pos = 0;
        static const std::string ap = "animation";
        while ((pos = body.find(ap, pos)) != std::string::npos) {
            if (pos > 0) {
                char prev = body[pos - 1];
                if (prev != '{' && prev != ';' && prev != ' ' && prev != '\n') {
                    pos += ap.size();
                    continue;
                }
            }
            auto after = pos + ap.size();
            if (after < body.size() && body[after] != ':' && body[after] != '-') {
                pos = after;
                continue;
            }
            auto semi = body.find(';', pos);
            if (semi == std::string::npos) break;
            // Check if this animation value references a spinner/rotate
            std::string val = body.substr(pos, semi - pos);
            if (val.find("spin") != std::string::npos ||
                val.find("rotate") != std::string::npos ||
                val.find("loading") != std::string::npos) {
                pos = semi + 1;
                continue;  // preserve spinner animations
            }
            std::fill(body.begin() + pos, body.begin() + semi + 1, ' ');
            pos = semi + 1;
            ++count;
        }
        if (count)
            logger::info("SkyrimNetDashboard: PatchCSS: stripped {} animation declarations", count);
    }

    // box-shadow with blur radii cause expensive software rasterization each frame.
    // Strip all box-shadow declarations — the app's visual design doesn't need them
    // in Ultralight (elements already have borders/backgrounds for contrast).
    {
        int count = 0;
        std::string::size_type pos = 0;
        static const std::string bs = "box-shadow:";
        while ((pos = body.find(bs, pos)) != std::string::npos) {
            auto semi = body.find(';', pos);
            if (semi == std::string::npos) break;
            std::fill(body.begin() + pos, body.begin() + semi + 1, ' ');
            pos = semi + 1;
            ++count;
        }
        if (count)
            logger::info("SkyrimNetDashboard: PatchCSS: stripped {} box-shadow declarations", count);
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

// ── Identifier extraction helpers for manual pattern matching ───────────────
// Reads a JS identifier (\w+) forwards from `pos`.
static std::string IdentFwd(const std::string& s, std::size_t pos) {
    std::string id;
    while (pos < s.size() && (isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_' || s[pos] == '$'))
        id += s[pos++];
    return id;
}
// Reads a JS identifier backwards, ending just before `pos`.
static std::string IdentBwd(const std::string& s, std::size_t pos) {
    std::string id;
    while (pos > 0 && (isalnum(static_cast<unsigned char>(s[pos - 1])) || s[pos - 1] == '_' || s[pos - 1] == '$'))
        id = s[--pos] + id;
    return id;
}

static std::string PatchBundle(std::string body)
{
    std::string patches;  // accumulates names of applied patches for summary log
    auto mark = [&](const char* name) {
        if (!patches.empty()) patches += ", ";
        patches += name;
    };

    // ── Playback banner: fix h.cardBg → h.colors.cardBg ──────────────────────
    // The diary playback banner accesses h.cardBg / h.border directly on the
    // theme object, but those live under h.colors.cardBg / h.colors.border.
    // Result: both class slots are undefined → fully transparent background.
    // Also remove the pointless backdrop-blur-lg (Ultralight ignores it).
    bool hasOpacity = body.find("glassEffect:\"bg-white/30") != std::string::npos;
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
    if (hasOpacity) mark("opacity");

    // ── Log file download fix ─────────────────────────────────────────────────
    // The app's download handler creates a blob: URL then calls a.click() to
    // trigger a browser Save-As dialog. Ultralight doesn't support <a download>
    // and just navigates the frame to the blob URL, showing raw text.
    // We replace the anchor trick entirely: read the blob as an ArrayBuffer and
    // POST it directly to our /save-file proxy endpoint, which writes the file
    // to Documents\SkyrimNet Logs\ and returns JSON {saved,path}.  A toast
    // shows the saved path — no navigation ever happens.
    // Variable names are extracted from structural anchors at runtime so the
    // patch survives minifier renames.
    {
        static const std::string anchor = "=window.URL.createObjectURL(";
        auto ap = body.find(anchor);
        if (ap != std::string::npos) {
            std::string urlVar  = IdentBwd(body, ap);
            std::string blobVar = IdentFwd(body, ap + anchor.size());
            auto awaitPos = body.rfind("=await ", ap);
            std::string respVar = (awaitPos != std::string::npos)
                ? IdentFwd(body, awaitPos + 7) : "";
            auto elemPos = body.find("=document.createElement(", ap);
            std::string elemVar = (elemPos != std::string::npos)
                ? IdentBwd(body, elemPos) : "";
            auto dlPos = body.find(".download=", ap);
            std::string fnameVar = (dlPos != std::string::npos)
                ? IdentFwd(body, dlPos + 10) : "";

            if (!urlVar.empty() && !blobVar.empty() && !respVar.empty() &&
                !elemVar.empty() && !fnameVar.empty()) {
                // Reconstruct the exact needle from extracted variable names.
                std::string constructed =
                    "const " + blobVar + "=await " + respVar + ".blob()," +
                    urlVar + "=window.URL.createObjectURL(" + blobVar + ")," +
                    elemVar + "=document.createElement(\"a\");" +
                    elemVar + ".href=" + urlVar + "," +
                    elemVar + ".download=" + fnameVar + "," +
                    "document.body.appendChild(" + elemVar + ")," +
                    elemVar + ".click(),window.URL.revokeObjectURL(" + urlVar + ")," +
                    "document.body.removeChild(" + elemVar + ")";
                auto cp = body.find(constructed);
                if (cp != std::string::npos) {
                    std::string rep =
                        "const " + blobVar + "=await " + respVar + ".blob();"
                        "await(async function(){"
                        "try{"
                        "const dr=await fetch('/save-dialog?name='+encodeURIComponent(" + fnameVar + "));"
                        "const dj=await dr.json();"
                        "if(dj.cancelled)return;"
                        "const ab=await " + blobVar + ".arrayBuffer();"
                        "const jr=await fetch('/save-file?path='+encodeURIComponent(dj.path),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:ab});"
                        "const j=await jr.json();"
                        "const d=document.createElement('div');"
                        "d.style.cssText='position:fixed;bottom:20px;right:20px;z-index:2147483647;padding:10px 16px;border-radius:6px;font-size:12px;font-family:Consolas,monospace;color:#fff;pointer-events:none;max-width:420px;word-break:break-all;transition:opacity 0.4s;'+(j&&j.saved?'background:#16a34a;border:1px solid #15803d;':'background:#dc2626;border:1px solid #b91c1c;');"
                        "d.textContent=j&&j.saved?'\\u2713 Saved: '+(j.path||" + fnameVar + "):'Save failed: '+(j.error||'unknown');"
                        "document.body.appendChild(d);"
                        "setTimeout(function(){d.style.opacity='0';setTimeout(function(){d.remove();},500);},3500);"
                        "}catch(err){console.error('snpd save-file failed',err);}"
                        "})()";
                    body.replace(cp, constructed.size(), rep);
                    mark("log-download");
                } else {
                    logger::warn("PatchBundle: log download needle not found (vars: blob={} resp={} url={} elem={} fname={})",
                        blobVar, respVar, urlVar, elemVar, fnameVar);
                }
            } else {
                logger::warn("PatchBundle: log download vars incomplete (blob={} resp={} url={} elem={} fname={})",
                    blobVar, respVar, urlVar, elemVar, fnameVar);
            }
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
            mark("diary-url");
        }
    }
    // Download button: replace the <a download>+click pattern with our
    // fetch → /save-dialog → /save-file flow (same as log downloads).
    // Variable names extracted at runtime via structural anchors.
    // Anchor on ".download=" near the diary template literal, then replace
    // just the download-link mechanism (EL.download=…removeChild(EL)}).
    {
        // Use ".id}." to locate the ternary (memory/diary filename) region.
        static const std::string anchor = ".id}.";
        auto ap = body.find(anchor);
        if (ap != std::string::npos) {
            // Find the nearest .download= after the template ternary.
            auto dlPos = body.find(".download=", ap);
            if (dlPos != std::string::npos && dlPos - ap < 200) {
                std::string elVar    = IdentBwd(body, dlPos);
                std::string fnameVar = IdentFwd(body, dlPos + 10);
                // URL var: find "EL.href=URL" before the template region.
                std::string urlVar;
                if (!elVar.empty()) {
                    auto hrefStr = elVar + ".href=";
                    auto hrefPos = body.rfind(hrefStr, ap);
                    if (hrefPos != std::string::npos)
                        urlVar = IdentFwd(body, hrefPos + hrefStr.size());
                }

                if (!elVar.empty() && !fnameVar.empty() && !urlVar.empty()) {
                    // Needle: just the download-link mechanism ending with }
                    // (arrow function body close — NOT })  ).
                    std::string constructed =
                        elVar + ".download=" + fnameVar + "," +
                        "document.body.appendChild(" + elVar + ")," +
                        elVar + ".click(),document.body.removeChild(" + elVar + ")}";
                    auto cp = body.find(constructed, ap);
                    if (cp != std::string::npos) {
                        std::string rep =
                            "(async function(){"
                            "try{"
                            "const dr=await fetch('/save-dialog?name='+encodeURIComponent(" + fnameVar + "));"
                            "const dj=await dr.json();"
                            "if(dj.cancelled)return;"
                            "const ab=await(await fetch(" + urlVar + ")).arrayBuffer();"
                            "const jr=await fetch('/save-file?path='+encodeURIComponent(dj.path),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:ab});"
                            "const j=await jr.json();"
                            "const d=document.createElement('div');"
                            "d.style.cssText='position:fixed;bottom:20px;right:20px;z-index:2147483647;padding:10px 16px;border-radius:6px;font-size:12px;font-family:Consolas,monospace;color:#fff;pointer-events:none;max-width:420px;word-break:break-all;transition:opacity 0.4s;'+(j&&j.saved?'background:#16a34a;border:1px solid #15803d;':'background:#dc2626;border:1px solid #b91c1c;');"
                            "d.textContent=j&&j.saved?'\\u2713 Saved: '+(j.path||" + fnameVar + "):'Save failed: '+(j.error||'unknown');"
                            "document.body.appendChild(d);"
                            "setTimeout(function(){d.style.opacity='0';setTimeout(function(){d.remove();},500);},3500);"
                            "}catch(err){console.error('snpd diary save failed',err);}})()}";
                        body.replace(cp, constructed.size(), rep);
                        mark("diary-download");
                    } else {
                        logger::warn("PatchBundle: diary download needle not found (el={} url={} fname={})",
                            elVar, urlVar, fnameVar);
                    }
                } else {
                    logger::warn("PatchBundle: diary download vars incomplete (el={} url={} fname={})",
                        elVar, urlVar, fnameVar);
                }
            } else {
                logger::warn("PatchBundle: diary download .download= not found near anchor");
            }
        }
    }

    // CodeMirror updateListener fires onChange(text) every keystroke via setTimeout(fn,0).
    // Patch: use a module-scoped timer variable (_snpdCmTmr) for a 600ms debounce.
    // Variable names extracted at runtime via ".state.doc.toString()" anchor.
    {
        static const std::string cmAnchor = ".state.doc.toString()";
        auto ap = body.find(cmAnchor);
        if (ap != std::string::npos) {
            std::string updateVar = IdentBwd(body, ap);
            std::string dcPrefix = updateVar + ".docChanged){const ";
            auto dcPos = body.rfind(dcPrefix, ap);
            std::string textVar = (dcPos != std::string::npos)
                ? IdentFwd(body, dcPos + dcPrefix.size()) : "";
            auto stPos = body.find("setTimeout(()=>", ap);
            std::string cbVar = (stPos != std::string::npos)
                ? IdentFwd(body, stPos + 15) : "";

            if (!updateVar.empty() && !textVar.empty() && !cbVar.empty()) {
                // Reconstruct the exact needle from extracted vars.
                std::string constructed =
                    updateVar + ".docChanged){const " + textVar + "=" +
                    updateVar + ".state.doc.toString();setTimeout(()=>" +
                    cbVar + "(" + textVar + "),0)}";
                auto cp = body.find(constructed);
                if (cp != std::string::npos) {
                    std::string rep =
                        updateVar + ".docChanged){const _ss=" + updateVar + ".state;"
                        "self._snpdView=" + updateVar + ".view;"
                        "self._snpdCmCb=" + cbVar + ";"
                        "clearTimeout(self._snpdCmTmr);self._snpdCmTmr=setTimeout(()=>" +
                        cbVar + "(_ss.doc.toString()),1500)}";
                    body.replace(cp, constructed.size(), rep);
                    mark("codemirror-debounce");
                } else {
                    logger::warn("PatchBundle: CodeMirror needle not found (update={} text={} cb={})",
                        updateVar, textVar, cbVar);
                }
            } else {
                logger::warn("PatchBundle: CodeMirror vars incomplete (update={} text={} cb={})",
                    updateVar, textVar, cbVar);
            }
        }
    }
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
            mark("prompt-dialog");
        }
    }

    // ── Test TTS audio: bypass blob URL → new Audio path ──────────────────────
    // The test page plays a TTS sample with:
    //   fetch(ttsEndpoint) → t.blob() → URL.createObjectURL(blob) → new Audio(blobUrl) → play()
    // Our C++ proxy plays audio via waveOut and returns {"status":"playing"}.
    // The JS never receives binary audio — just a small JSON response. So we patch
    // the blob/Audio chain to skip all of that and wait for the snpd:testttsended
    // event that C++ fires when waveOut finishes.
    // A single regex covers all minified variants — captures the variable names
    // for the audio element and setPlaying/setLoading state setters.
    // Regex runs on the full body; proven safe alone on MSVC (Phase 4 validated).
    {
        static const std::regex kTtsRe(
            R"(const s=await t\.blob\(\),r=URL\.createObjectURL\(s\),(\w+)=new Audio\(r\);)"
            R"((\w+)\(!0\),\1\.onended=\(\)=>\{(\w+)\(!1\),setTimeout\(\(\)=>\2\(!1\),2e3\),)"
            R"(URL\.revokeObjectURL\(r\)\},\1\.onerror=\w+=>\{(\w+)\([^)]+\),\3\(!1\),\2\(!1\),)"
            R"(URL\.revokeObjectURL\(r\)\},await \1\.play\(\))",
            std::regex::ECMAScript | std::regex::optimize);
        int patchCount = 0;
        std::smatch m;
        while (std::regex_search(body, m, kTtsRe)) {
            std::string rep = m.str(2) + "(!0),document.addEventListener('snpd:testttsended',"
                "function _snpdE(){" + m.str(3) + "(!1)," + m.str(2) +
                "(!1),document.removeEventListener('snpd:testttsended',_snpdE)})";
            body.replace(static_cast<std::size_t>(m.position()), static_cast<std::size_t>(m.length()), rep);
            ++patchCount;
        }
        if (patchCount > 0)
            mark("tts-audio");
    }

    if (!patches.empty())
        logger::info("PatchBundle: applied [{}]", patches);

    return body;
}

// Fetches the SkyrimNet root HTML and injects compat patches.
// Retries several times with a delay to tolerate startup race conditions
// (SkyrimNet's HTTP server may not be listening yet when kDataLoaded fires).
static std::string FetchAndInject(const std::string& host, uint16_t port)
{
    constexpr int    maxAttempts = 6;
    constexpr int    retryDelayMs = 2000; // 2 s between attempts → up to ~10 s of waiting
    std::string body, ct;
    int sc = 0;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        logger::info("SkyrimNetDashboard: FetchAndInject attempt {}/{} → {}:{}",
                     attempt, maxAttempts, host, port);
        std::tie(body, ct, sc) = FetchResource(host, port, "GET", "/");
        if (!body.empty()) {
            logger::info("SkyrimNetDashboard: FetchAndInject got {} bytes (status {}, ct={})",
                         body.size(), sc, ct);
            break;
        }
        if (attempt < maxAttempts) {
            logger::warn("SkyrimNetDashboard: FetchAndInject attempt {}/{} failed (status {}) "
                         "— retrying in {} ms", attempt, maxAttempts, sc, retryDelayMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }

    if (body.empty()) {
        logger::error("SkyrimNetDashboard: FetchAndInject all {} attempts to {}:{} failed "
                      "— showing error page", maxAttempts, host, port);
        return "<html><head><style>"
               "body{background:#111827;color:#e5e7eb;font-family:Consolas,'Courier New',monospace;"
               "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center}"
               ".box{background:#1f2937;border:1px solid #374151;border-radius:8px;padding:32px;max-width:480px}"
               "h2{color:#f87171;margin:0 0 12px}p{margin:8px 0;color:#9ca3af;font-size:13px}"
               "code{background:#374151;padding:2px 6px;border-radius:3px;color:#fbbf24}"
               "</style></head><body><div class='box'>"
               "<h2>Cannot reach SkyrimNet</h2>"
               "<p>Could not connect to <code>" + host + ":" + std::to_string(port) + "</code></p>"
               "<p>Make sure SkyrimNet is running before opening the dashboard.</p>"
               "<p style='margin-top:16px;font-size:11px'>To change the URL, edit the <code>URL=</code> line in<br>"
               "your <code>SkyrimNetPrismaDashboard.ini</code> file.</p>"
               "</div></body></html>";
    }
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
    SOCKET s = ConnectToHost(host, port);
    if (s == INVALID_SOCKET) {
        logger::warn("SkyrimNetDashboard: TryStreamProxy cannot connect to {}:{}", host, port);
        return false;
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
    // bufferable response.  For GET we hand off to FetchResource (which
    // applies InjectPatches / PatchBundle / PatchCSS / asset caching).
    // For non-GET we MUST forward here — returning false would cause
    // FetchResource to re-send the request, double-firing any
    // state-changing POST (e.g. a toggle would flip twice → no change).
    std::string hdrlower;
    hdrlower.reserve(headerSection.size());
    for (auto c : headerSection)
        hdrlower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (hdrlower.find("content-length:") != std::string::npos) {
        if (method == "GET") {
            closesocket(s);
            return false;
        }
        // Non-GET: read the full body and forward with original headers.
        std::size_t clVal = 0;
        {
            auto clp = hdrlower.find("content-length:");
            auto cle = headerSection.find("\r\n", clp);
            try { clVal = std::stoull(headerSection.substr(clp + 15, cle - clp - 15)); }
            catch (...) {}
        }
        std::string respBody = alreadyRead;
        while (respBody.size() < clVal) {
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            respBody.append(tmp, n);
        }
        closesocket(s);
        std::string fullResp = headerSection + "\r\n\r\n" + respBody;
        send(client, fullResp.c_str(), static_cast<int>(fullResp.size()), 0);
        logger::info("SkyrimNetDashboard: TryStreamProxy forwarded buffered {} {} ({} bytes)",
                     method, path, respBody.size());
        return true;
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
    bool     isAudioStream = false; // set on first confirmed segment/status line
    uint32_t myAqSession   = 0;     // session gen captured when stream starts; gates pushes

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
                // Require "audio":" to be present so we don't false-positive on
                // other NDJSON streams (e.g. bulk-clone progress) that also use
                // {"type":"segment"} or {"type":"status"} without audio data.
                line.find("\"type\":\"segment\"") != std::string::npos &&
                line.find("\"audio\":\"") != std::string::npos) {
            isAudioStream = true;
            ClearAudioQueue();        // flush any leftovers from a previous run
            EnsureAudioQueueRunning();
            // Capture the session gen AFTER clearing.  If nav-stop fires
            // ClearAudioQueue from another thread it bumps s_aqSessionGen, making
            // myAqSession stale — all subsequent PushToAudioQueue calls will bail.
            myAqSession = s_aqSessionGen.load();
            logger::info("SkyrimNetDashboard: TTS audio stream detected on first audio segment, C++ queue active");
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
                        // Only push if this session is still active
                        // (nav-stop bumps s_aqSessionGen, making myAqSession stale).
                        if (s_aqSessionGen.load() == myAqSession)
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
    int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsaErr != 0) {
        logger::critical("SkyrimNetDashboard: WSAStartup failed with error {}", wsaErr);
        return 0;
    }
    logger::info("SkyrimNetDashboard: Winsock {}.{} initialised",
                 LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion));

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

    // Parse host and port from dashboardUrl ("http://host:port/" or "http://[::1]:port/")
    std::string proxyHost;
    uint16_t    proxyPort = 8080;
    ParseHostPort(dashboardUrl, proxyHost, proxyPort);
    logger::info("SkyrimNetDashboard: proxy target {}:{} (from '{}')", proxyHost, proxyPort, dashboardUrl);

    // Generate a per-session auth token so only our Ultralight browser
    // (which receives the token via Set-Cookie) can hit sensitive endpoints.
    std::string sessionToken;
    {
        std::random_device rd;
        std::mt19937_64    gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        uint64_t a = dist(gen), b = dist(gen);
        char buf[33];
        snprintf(buf, sizeof(buf), "%016llx%016llx",
                 static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b));
        sessionToken = buf;
    }
    logger::info("SkyrimNetDashboard: session token generated");

    std::thread([srv, shellHtml, proxyHost, proxyPort, sessionToken]() {

        // Per-connection handler — runs on its own thread so all requests are
        // served in parallel.  This is critical for React apps which fire
        // 10-20 simultaneous asset/API requests on every page transition.
        auto handleClient = [&shellHtml, &proxyHost, proxyPort, &sessionToken](SOCKET client) {
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
                    int contentLen = 0;
                    try { contentLen = std::stoi(rawReq.substr(clPos + 15, clEnd - clPos - 15)); }
                    catch (...) { break; }
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

            // Parse method, path, request content-type, cookie, and body
            std::string method = "GET", path = "/", reqCT, reqCookie, reqBody;
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

                    // Extract Cookie header for session-token auth
                    auto ckPos = hdrlower.find("\r\ncookie:");
                    if (ckPos != std::string::npos) {
                        ckPos += 2; // skip past \r\n
                        auto eol2 = hdrs.find("\r\n", ckPos);
                        reqCookie = hdrs.substr(ckPos + 7,
                            eol2 != std::string::npos ? eol2 - ckPos - 7 : std::string::npos);
                        while (!reqCookie.empty() && reqCookie.front() == ' ')
                            reqCookie.erase(reqCookie.begin());
                    }
                }
            }

            // Session-token gate: sensitive endpoints require the auth cookie.
            // The cookie is set automatically when /shell is first loaded.
            bool needsToken = path.substr(0, 10) == "/save-file"
                           || path.substr(0, 10) == "/read-file"
                           || path.substr(0, 12) == "/save-dialog"
                           || path.substr(0, 12) == "/open-dialog";
            if (needsToken) {
                std::string needle = "__snpdt=" + sessionToken;
                if (reqCookie.find(needle) == std::string::npos) {
                    logger::warn("SkyrimNetDashboard: rejected {} (missing session token)", path);
                    std::string resp403 =
                        "HTTP/1.1 403 Forbidden\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 9\r\n"
                        "Connection: close\r\n"
                        "\r\nForbidden";
                    send(client, resp403.c_str(), static_cast<int>(resp403.size()), 0);
                    closesocket(client);
                    return;
                }
            }

            std::string body;
            std::string contentType = "text/html; charset=utf-8";
            std::string extraHeaders; // additional headers for this response
            int upstreamStatus = 200;

            if (path == "/shell" || path.empty()) {
                body = shellHtml;
                // Set session-token cookie so all subsequent requests are authenticated
                extraHeaders = "Set-Cookie: __snpdt=" + sessionToken
                             + "; SameSite=Strict; Path=/; HttpOnly\r\n";
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
                        body = "{\"path\":\"" + JsonEscape(std::string(fileBuf)) + "\"}";
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
                            // Security: only absolute Windows paths; no ".." traversal
                            bool ok = savePath.size() >= 3
                                && isalpha(static_cast<unsigned char>(savePath[0]))
                                && savePath[1] == ':'
                                && (savePath[2] == '\\' || savePath[2] == '/')
                                && savePath.find("..") == std::string::npos;
                            if (!ok) {
                                logger::warn("SkyrimNetDashboard: /save-file rejected path '{}'", savePath);
                                savePath.clear();
                            }
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
                    body = "{\"saved\":true,\"path\":\"" + JsonEscape(savePath) + "\"}";
                    logger::info("SkyrimNetDashboard: saved file '{}' ({} bytes)", savePath, reqBody.size());
                } else {
                    body = "{\"saved\":false,\"error\":\"Could not write file\"}";
                    logger::warn("SkyrimNetDashboard: /save-file failed to write '{}'", savePath);
                }
                contentType = "application/json";
            } else if (path.substr(0, 12) == "/open-dialog") {
                // Opens a native Windows file-open dialog and returns the chosen path.
                // ?accept=<mime>  e.g. "audio/*"  drives the filter list.
                // Returns JSON: {"path":"C:\\...\\file.mp3","mimeType":"audio/mpeg"}
                //           or: {"cancelled":true}
                std::string accept = "*/*";
                {
                    auto qp = path.find('?');
                    if (qp != std::string::npos) {
                        std::string q = path.substr(qp + 1);
                        auto ap = q.find("accept=");
                        if (ap != std::string::npos)
                            accept = UrlDecode(q.substr(ap + 7));
                    }
                }
                // Build a double-null-terminated filter string from the accept attribute.
                // If accept contains explicit extensions (e.g. ".wav,.fuz,.xwm"), build a
                // custom filter from those.  Otherwise fall back to MIME-type groups.
                //
                // A double-null-terminated filter has the form:
                //   "Label\0*.ext1;*.ext2\0All Files\0*.*\0\0"
                // We store it in a std::string so embedded nulls are safe.
                std::string filterBuf;
                const char* filter = nullptr; // will point into filterBuf or a static

                // Helper: append one filter entry (label + pattern) to filterBuf.
                auto addEntry = [&](const std::string& label, const std::string& pattern) {
                    filterBuf += label;  filterBuf += '\0';
                    filterBuf += pattern; filterBuf += '\0';
                };

                // Parse individual ".ext" tokens from the accept string.
                std::vector<std::string> exts;
                {
                    std::string tok;
                    for (char ch : accept + ",") {
                        if (ch == ',' || ch == ' ') {
                            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
                            if (tok.size() > 1 && tok[0] == '.') exts.push_back(tok);
                            tok.clear();
                        } else { tok += ch; }
                    }
                }

                if (!exts.empty()) {
                    // Build pattern from explicit extensions, e.g. "*.wav;*.fuz;*.xwm"
                    std::string label = "Supported Files", pattern;
                    for (auto& e : exts) {
                        if (!pattern.empty()) pattern += ';';
                        pattern += '*'; pattern += e;
                    }
                    addEntry(label, pattern);
                    addEntry("All Files", "*.*");
                    filterBuf += '\0'; // extra terminator
                    filter = filterBuf.c_str();
                } else {
                    // No explicit extensions — fall back to MIME group or all-files.
                    static const char kFiltAudio[] =
                        "Audio Files\0*.mp3;*.wav;*.ogg;*.flac;*.m4a;*.aac;*.webm;*.fuz;*.xwm\0All Files\0*.*\0";
                    static const char kFiltImage[] =
                        "Image Files\0*.png;*.jpg;*.jpeg;*.gif;*.webp\0All Files\0*.*\0";
                    static const char kFiltVideo[] =
                        "Video Files\0*.mp4;*.webm;*.mkv;*.avi\0All Files\0*.*\0";
                    static const char kFiltText[]  =
                        "Text Files\0*.txt;*.log;*.csv;*.json\0All Files\0*.*\0";
                    static const char kFiltAll[]   = "All Files\0*.*\0";
                    if      (accept.find("audio") != std::string::npos) filter = kFiltAudio;
                    else if (accept.find("image") != std::string::npos) filter = kFiltImage;
                    else if (accept.find("video") != std::string::npos) filter = kFiltVideo;
                    else if (accept.find("text")  != std::string::npos) filter = kFiltText;
                    else filter = kFiltAll;
                }
                {
                    char fileBuf[MAX_PATH] = {};
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize  = sizeof(ofn);
                    ofn.hwndOwner    = nullptr;
                    ofn.lpstrFilter  = filter;
                    ofn.nFilterIndex = 1;
                    ofn.lpstrFile    = fileBuf;
                    ofn.nMaxFile     = MAX_PATH;
                    ofn.lpstrTitle   = "Select file";
                    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameA(&ofn)) {
                        std::string fp = fileBuf;
                        // Derive MIME type from extension
                        std::string mime = "application/octet-stream";
                        auto dot = fp.rfind('.');
                        if (dot != std::string::npos) {
                            std::string ext = fp.substr(dot + 1);
                            for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                            if      (ext == "mp3")               mime = "audio/mpeg";
                            else if (ext == "wav")               mime = "audio/wav";
                            else if (ext == "ogg")               mime = "audio/ogg";
                            else if (ext == "flac")              mime = "audio/flac";
                            else if (ext == "m4a")               mime = "audio/mp4";
                            else if (ext == "aac")               mime = "audio/aac";
                            else if (ext == "webm")              mime = "audio/webm";
                            else if (ext == "png")               mime = "image/png";
                            else if (ext == "jpg" || ext == "jpeg") mime = "image/jpeg";
                            else if (ext == "gif")               mime = "image/gif";
                            else if (ext == "txt" || ext == "log") mime = "text/plain";
                            else if (ext == "mp4")               mime = "video/mp4";
                        }
                        // Escape backslashes for JSON
                        body = "{\"path\":\"" + JsonEscape(fp) + "\",\"mimeType\":\"" + mime + "\"}";
                        logger::info("SkyrimNetDashboard: /open-dialog: user chose '{}'", fp);
                    } else {
                        body = "{\"cancelled\":true}";
                        logger::info("SkyrimNetDashboard: /open-dialog: cancelled");
                    }
                }
                contentType = "application/json";
            } else if (path.substr(0, 10) == "/read-file") {
                // Reads a local file (chosen via /open-dialog) and returns its bytes.
                // ?path=<url-encoded-absolute-windows-path>
                std::string filePath;
                {
                    auto qp = path.find('?');
                    if (qp != std::string::npos) {
                        std::string q = path.substr(qp + 1);
                        auto pp = q.find("path=");
                        if (pp != std::string::npos)
                            filePath = UrlDecode(q.substr(pp + 5));
                    }
                }
                // Security: only absolute Windows paths; no ".." traversal.
                bool pathOk = filePath.size() >= 3
                    && isalpha(static_cast<unsigned char>(filePath[0]))
                    && filePath[1] == ':'
                    && (filePath[2] == '\\' || filePath[2] == '/')
                    && filePath.find("..") == std::string::npos;
                // Canonicalize to defeat encoded traversal or junction tricks
                if (pathOk) {
                    std::error_code ec;
                    auto canon = std::filesystem::weakly_canonical(filePath, ec);
                    if (!ec) {
                        filePath = canon.string();
                        pathOk = filePath.size() >= 3
                            && isalpha(static_cast<unsigned char>(filePath[0]))
                            && filePath[1] == ':'
                            && (filePath[2] == '\\' || filePath[2] == '/')
                            && filePath.find("..") == std::string::npos;
                    } else {
                        pathOk = false;
                    }
                }
                if (!pathOk) {
                    body           = "{\"error\":\"invalid path\"}";
                    contentType    = "application/json";
                    upstreamStatus = 400;
                } else {
                    std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
                    if (!ifs) {
                        body           = "{\"error\":\"cannot open file\"}";
                        contentType    = "application/json";
                        upstreamStatus = 404;
                    } else {
                        auto sz = static_cast<std::streamoff>(ifs.tellg());
                        constexpr std::streamoff kMaxBytes = 256LL * 1024 * 1024;
                        if (sz > kMaxBytes) {
                            body           = "{\"error\":\"file too large\"}";
                            contentType    = "application/json";
                            upstreamStatus = 413;
                        } else {
                            ifs.seekg(0);
                            body.resize(static_cast<std::size_t>(sz));
                            ifs.read(body.data(), sz);
                            // Content-Type from extension
                            contentType = "application/octet-stream";
                            auto dot = filePath.rfind('.');
                            if (dot != std::string::npos) {
                                std::string ext = filePath.substr(dot + 1);
                                for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                                if      (ext == "mp3")               contentType = "audio/mpeg";
                                else if (ext == "wav")               contentType = "audio/wav";
                                else if (ext == "ogg")               contentType = "audio/ogg";
                                else if (ext == "flac")              contentType = "audio/flac";
                                else if (ext == "m4a")               contentType = "audio/mp4";
                                else if (ext == "aac")               contentType = "audio/aac";
                                else if (ext == "webm")              contentType = "audio/webm";
                                else if (ext == "png")               contentType = "image/png";
                                else if (ext == "jpg" || ext == "jpeg") contentType = "image/jpeg";
                                else if (ext == "gif")               contentType = "image/gif";
                                else if (ext == "mp4")               contentType = "video/mp4";
                                else if (ext == "txt" || ext == "log") contentType = "text/plain; charset=utf-8";
                            }
                            logger::info("SkyrimNetDashboard: /read-file: served '{}' ({} bytes)", filePath, sz);
                        }
                    }
                }
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
            } else if (path == "/snpd-toggle-keepbg") {
                {
                    std::lock_guard<std::mutex> lk(s_cfgMtx);
                    s_cfg.keepBg = !s_cfg.keepBg;
                    SaveSettings();
                    body = std::string("{\"keepBg\":") + (s_cfg.keepBg ? "true" : "false") + "}";
                }
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
                if (newHotKey != static_cast<int>(s_toggleKey.load()) && newHotKey > 0) {
                    auto* kh = KeyHandler::GetSingleton();
                    if (s_toggleHandle != INVALID_REGISTRATION_HANDLE)
                        kh->Unregister(s_toggleHandle);
                    s_toggleKey.store(static_cast<uint32_t>(newHotKey));
                    s_toggleHandle = kh->Register(s_toggleKey.load(), KeyEventType::KEY_DOWN, OnToggle);
                    logger::info("SkyrimNetDashboard: hotkey changed to {} (0x{:02X})",
                        DxKeyName(s_toggleKey.load()), s_toggleKey.load());
                }
                body        = "{\"saved\":true}";
                contentType = "application/json";
            } else if (path == "/snpd-save-layout") {
                // Persists window position/size/zoom/fullscreen to the INI so they
                // survive across game sessions.  Values come from JS localStorage.
                auto extractLayoutStr = [&](const std::string& key) -> std::string {
                    auto needle = '"' + key + "\":\"";
                    auto pos = reqBody.find(needle);
                    if (pos == std::string::npos) return "";
                    pos += needle.size();
                    auto end = reqBody.find('"', pos);
                    if (end == std::string::npos) return "";
                    auto val = reqBody.substr(pos, end - pos);
                    // Limit length — CSS values like "1200px" / zoom "1.2" are always short
                    if (val.size() > 32) val.clear();
                    return val;
                };
                {
                    std::lock_guard<std::mutex> lk(s_cfgMtx);
                    s_cfg.winX    = extractLayoutStr("x");
                    s_cfg.winY    = extractLayoutStr("y");
                    s_cfg.winW    = extractLayoutStr("w");
                    s_cfg.winH    = extractLayoutStr("h");
                    s_cfg.winZoom = extractLayoutStr("zoom");
                    if (!s_cfg.winZoom.empty()) {
                        float z = static_cast<float>(std::atof(s_cfg.winZoom.c_str()));
                        if (z < 0.20f || z > 3.0f) s_cfg.winZoom = "";
                    }
                    auto fsStr = extractLayoutStr("fs");
                    s_cfg.winFs = (fsStr == "true");
                    SaveSettings();
                }
                body        = "{\"saved\":true}";
                contentType = "application/json";
            } else if (path == "/snpd-storage-get") {
                body        = ReadStorage();
                contentType = "application/json";
            } else if (path == "/snpd-storage-save") {
                WriteStorage(reqBody);
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
            } else if (
                // Test-TTS endpoints: fetch audio in C++ and play via waveOut so
                // the browser never has to deal with binary audio bytes.
                // Matches:
                //   POST /test?api=tts           (test/setup page — returns JSON with audio_id)
                //   POST /characters?api=test-custom-tts  (returns audio bytes directly)
                //   POST /voice-samples?api=test-tts      (returns audio bytes directly)
                (method == "POST" && path.find("/test") == 0 &&
                    path.find("api=tts") != std::string::npos &&
                    path.find("api=tts-") == std::string::npos) ||
                (path.find("/characters") == 0 && path.find("api=test-custom-tts") != std::string::npos) ||
                (path.find("/voice-samples") == 0 && path.find("api=test-tts") != std::string::npos)
            ) {
                // For /test?api=tts: SkyrimNet returns JSON like
                //   {"audio_id":"tts_NNN","audio_size":NNN,...}
                // and the audio is fetched separately via GET /test?api=audio&id=tts_NNN.
                // We fetch the metadata once, return it to the browser so the UI displays
                // correct Speaker/AudioSize/Message, then fetch + play the audio in a
                // background thread (audio fetch is the slow part).
                //
                // For the other two endpoints the response IS the audio bytes directly —
                // fetch and play on background thread, return {"status":"queued"} to browser.
                logger::info("SkyrimNetDashboard: test-TTS intercept (async): {}", path);
                bool isTestTts = (method == "POST" && path.find("/test") == 0 &&
                                  path.find("api=tts") != std::string::npos &&
                                  path.find("api=tts-") == std::string::npos);
                if (isTestTts) {
                    // Fetch metadata synchronously (fast, ~1s for TTS generation,
                    // returns a small JSON payload — well within Ultralight's timeout).
                    auto [metaBody, metaCt, metaSc] = FetchResource(proxyHost, proxyPort, method, path, reqCT, reqBody);
                    // Strip "audio_id" from the JSON before returning to the browser.
                    // The JS would otherwise make a GET /test?api=audio&id=NNN request
                    // which returns binary audio bytes — those crash Ultralight when
                    // buffered in the renderer's fetch response handler.
                    // Our C++ background thread fetches the audio independently.
                    std::string safeBody = metaBody;
                    {
                        auto aid = safeBody.find("\"audio_id\":");
                        if (aid != std::string::npos) {
                            // Find value start (skip optional whitespace after colon)
                            auto vs = safeBody.find('"', aid + 11);
                            if (vs != std::string::npos) {
                                auto ve = safeBody.find('"', vs + 1);
                                if (ve != std::string::npos)
                                    safeBody.replace(vs + 1, ve - vs - 1, ""); // "audio_id":""
                            }
                        }
                    }
                    body           = safeBody;
                    contentType    = std::move(metaCt);
                    upstreamStatus = metaSc;
                    // Extract audio_id and fetch+play audio on background thread
                    if (!metaBody.empty() && metaSc >= 200 && metaSc < 300) {
                        std::thread([ph = proxyHost, pp = proxyPort, meta = std::move(metaBody)]() {
                            std::string audioId;
                            auto k = meta.find("\"audio_id\"");
                            if (k != std::string::npos) {
                                auto q1 = meta.find('"', k + 10);
                                if (q1 != std::string::npos) {
                                    auto q2 = meta.find('"', q1 + 1);
                                    if (q2 != std::string::npos)
                                        audioId = meta.substr(q1 + 1, q2 - q1 - 1);
                                }
                            }
                            if (audioId.empty()) {
                                logger::warn("SkyrimNetDashboard: test-TTS no audio_id in JSON: {}", meta);
                                return;
                            }
                            std::string audioPath = "/test?api=audio&id=" + audioId;
                            logger::info("SkyrimNetDashboard: test-TTS fetching audio: {}", audioPath);
                            auto [audioBytes, audioCt, audioSc] = FetchResource(ph, pp, "GET", audioPath);
                            if (!audioBytes.empty() && audioSc >= 200 && audioSc < 300) {
                                logger::info("SkyrimNetDashboard: test-TTS playing {} bytes ct='{}'",
                                    audioBytes.size(), audioCt);
                                PlayAudioRaw(std::move(audioBytes), audioCt, true);
                            } else {
                                logger::warn("SkyrimNetDashboard: test-TTS audio fetch {} for {}", audioSc, audioPath);
                            }
                        }).detach();
                    }
                } else {
                    // Characters/voice-samples: response is audio bytes directly.
                    // Fetch+play async, return status immediately.
                    std::thread([ph = proxyHost, pp = proxyPort,
                                 m = method, p = path, ct = reqCT, b = reqBody]() mutable {
                        auto [audioBytes, audioCt, audioSc] = FetchResource(ph, pp, m, p, ct, b);
                        if (!audioBytes.empty() && audioSc >= 200 && audioSc < 300) {
                            logger::info("SkyrimNetDashboard: test-TTS playing {} bytes ct='{}'",
                                audioBytes.size(), audioCt);
                            PlayAudioRaw(std::move(audioBytes), audioCt, true);
                        } else {
                            logger::warn("SkyrimNetDashboard: test-TTS upstream {} for {}", audioSc, p);
                        }
                    }).detach();
                    body        = "{\"status\":\"queued\"}";
                    contentType    = "application/json";
                    upstreamStatus = 200;
                }
            } else if (
                // Safety net: if the browser somehow still requests the audio file
                // (e.g. audio_id stripping failed), intercept and return a stub so
                // binary audio bytes never reach Ultralight's response buffer.
                method == "GET" && path.find("/test") == 0 &&
                path.find("api=audio") != std::string::npos
            ) {
                logger::info("SkyrimNetDashboard: test audio stub intercept: {}", path);
                body        = "{}";
                contentType = "application/json";
                upstreamStatus = 200;
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
                    // EXCLUDE bulk-clone and bulk-clone-cancel: those are REST
                    // API calls whose responses must be buffered as JSON to be
                    // consumed by response.json() in the UI.
                    // EXCLUDE PUT/PATCH/DELETE: never streaming, and TryStreamProxy
                    // sends the request then discards it if response has
                    // Content-Length, causing a double-send via FetchResource.
                    // (PATCH was the cause of the custom-actions disable toggle
                    // bug — two PATCHes toggled enabled→disabled→enabled.)
                    const bool isBulkClone = path.find("bulk-clone") != std::string::npos;
                    const bool skipStream  = isBulkClone || method == "PUT" || method == "PATCH" || method == "DELETE";
                    if (!skipStream &&
                        TryStreamProxy(client, proxyHost, proxyPort, method, path, reqCT, reqBody)) {
                        closesocket(client);
                        return;
                    }
                    auto [resBody, resCt, resSc] = FetchResource(proxyHost, proxyPort, method, path, reqCT, reqBody);
                    body        = std::move(resBody);
                    contentType = std::move(resCt);
                    upstreamStatus = resSc;
                    // Log non-GET API calls for debugging save issues
                    if (method != "GET") {
                        logger::info("SkyrimNetDashboard: {} {} -> {} (ct={}, bodyLen={})",
                            method, path, upstreamStatus, contentType, body.size());
                        if (upstreamStatus >= 400)
                            logger::warn("SkyrimNetDashboard: error response body: {}",
                                body.substr(0, 500));
                    }
                    if (contentType.find("text/html") != std::string::npos && !body.empty()
                        && upstreamStatus >= 200 && upstreamStatus < 300)
                        body = InjectPatches(std::move(body));
                    // Convert HTML error pages to JSON so the React app shows
                    // a clean message instead of raw HTML markup.
                    else if (contentType.find("text/html") != std::string::npos && !body.empty()
                             && upstreamStatus >= 400) {
                        // Extract text between <p>...</p> for a clean message
                        std::string msg = "Server error";
                        auto p1 = body.find("<p>");
                        auto p2 = body.find("</p>");
                        if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1)
                            msg = body.substr(p1 + 3, p2 - p1 - 3);
                        body = "{\"error\":\"" + JsonEscape(msg) + "\"}";
                        contentType = "application/json";
                    }
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

            // Immutable assets (fonts, images) get long-lived browser cache headers.
            // JavaScript and CSS are always served no-store: our in-process s_assetCache
            // makes re-fetches instantaneous, and no-store ensures Ultralight always
            // requests from our proxy so the patched bundle is never stale in disk cache.
            bool immutable = IsImmutableAsset(contentType, path);
            bool isScript  = contentType.find("javascript") != std::string::npos
                          || contentType.find("text/css")   != std::string::npos;
            std::string cacheControl = (immutable && !isScript)
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
                + extraHeaders +
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
