#pragma once
// ── Runtime globals + audio subsystem ──────────────────────────────────────────
static constexpr uint32_t    ESC_KEY       = 0x01; // Escape
static constexpr uint32_t    INSPECTOR_KEY = 0x3F; // F5
static std::atomic<uint32_t>  s_toggleKey{0x3E}; // runtime toggle key, set from s_cfg at startup

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
static HWAVEOUT  s_hwo    = nullptr;
static WAVEHDR   s_hwoHdr = {};
// Generation counter that opened s_hwo.  Used to prevent a pre-empted
// PlayAudioBytes call from closing a *newer* call's active waveOut device.
static uint32_t  s_hwoGen = 0;

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
// Bumped by ClearAudioQueue (inside s_aqMtx).  The SSE handler captures
// this right after calling ClearAudioQueue and only pushes segments while
// it still matches — preventing late segment pushes after a nav-stop clear.
static std::atomic<uint32_t>   s_aqSessionGen{0};

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
// testTts=true fires snpd:testttsended in the iframe instead of __onAudioEnded,
// preventing diary TTS listeners from reacting to test audio completion.
// notifyOnCancel=true fires the end event even when pre-empted by a newer
// segment — used by direct (non-queue) plays so UI buttons always reset.
static void PlayAudioBytes(std::string bytes, const std::string& ct, bool testTts = false, bool notifyOnCancel = false)
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
                    waveOutClose(s_hwo); s_hwo = nullptr; s_hwoGen = 0;
                }
                // Also close any lingering MCI device (fallback path or prior session).
                mciSendStringA("stop snpd wait", nullptr, 0, nullptr);
                mciSendStringA("close snpd wait", nullptr, 0, nullptr);

                MMRESULT mo = waveOutOpen(&s_hwo, WAVE_MAPPER, &wi.wfx, 0, 0, CALLBACK_NULL);
                logger::info("SkyrimNetDashboard: waveOut open gen={}: {}",
                    myGen, mo == MMSYSERR_NOERROR ? "ok" : std::to_string(mo));
                if (mo != MMSYSERR_NOERROR) { s_hwo = nullptr; return; }
                s_hwoGen = myGen; // mark ownership: only this call may close this handle

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

            // Cleanup: bytes must remain alive until here because
            // s_hwoHdr.lpData points directly into bytes.data().
            // Only close the handle if WE opened it (s_hwoGen == myGen).
            // A concurrent PlayAudioBytes call may have already closed our handle
            // and opened a new one — closing that new handle would corrupt its
            // playback and cause hissing/crashes.
            {
                std::lock_guard<std::mutex> lk(s_mciMtx);
                if (s_hwo && s_hwoGen == myGen) {
                    waveOutUnprepareHeader(s_hwo, &s_hwoHdr, sizeof(WAVEHDR));
                    s_hwoHdr = {};
                    waveOutClose(s_hwo); s_hwo = nullptr; s_hwoGen = 0;
                }
            }
            logger::info("SkyrimNetDashboard: waveOut playback done gen={}", myGen);

            // Notify JS: for test TTS fire snpd:testttsended (safe custom event);
            // for regular audio fire __onAudioEnded (advances diary TTS queue).
            if (s_audioGen.load() == myGen && s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
                if (testTts) {
                    s_PrismaUI->Invoke(s_View,
                        "try{"
                        "var _fr=document.getElementById('snpd-frame');"
                        "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onTestTtsEnded)"
                        "_fr.contentWindow.__onTestTtsEnded();"
                        "}catch(e){}",
                        nullptr);
                    logger::info("SkyrimNetDashboard: __onTestTtsEnded dispatched gen={}", myGen);
                } else {
                    s_PrismaUI->Invoke(s_View,
                        "try{"
                        "var _fr=document.getElementById('snpd-frame');"
                        "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
                        "_fr.contentWindow.__onAudioEnded();"
                        "}catch(e){}",
                        nullptr);
                    logger::info("SkyrimNetDashboard: __onAudioEnded dispatched gen={}", myGen);
                }
            } else {
                if (notifyOnCancel && s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
                    // Pre-empted direct play: still notify so UI buttons reset.
                    s_PrismaUI->Invoke(s_View,
                        testTts ?
                        "try{var _fr=document.getElementById('snpd-frame');"
                        "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onTestTtsEnded)"
                        "_fr.contentWindow.__onTestTtsEnded();}catch(e){}" :
                        "try{var _fr=document.getElementById('snpd-frame');"
                        "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
                        "_fr.contentWindow.__onAudioEnded();}catch(e){}",
                        nullptr);
                    logger::info("SkyrimNetDashboard: audio notify-on-cancel (waveOut) gen={}", myGen);
                } else {
                    logger::info("SkyrimNetDashboard: audio notify skipped gen={} current={}",
                        myGen, s_audioGen.load());
                }
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
    if (s_audioGen.load() == myGen && s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
        if (testTts) {
            s_PrismaUI->Invoke(s_View,
                "try{"
                "var _fr=document.getElementById('snpd-frame');"
                "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onTestTtsEnded)"
                "_fr.contentWindow.__onTestTtsEnded();"
                "}catch(e){}",
                nullptr);
            logger::info("SkyrimNetDashboard: __onTestTtsEnded dispatched gen={}", myGen);
        } else {
            s_PrismaUI->Invoke(s_View,
                "try{"
                "var _fr=document.getElementById('snpd-frame');"
                "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
                "_fr.contentWindow.__onAudioEnded();"
                "}catch(e){}",
                nullptr);
            logger::info("SkyrimNetDashboard: __onAudioEnded dispatched gen={}", myGen);
        }
    } else {
        if (notifyOnCancel && s_PrismaUI && s_PrismaUI->IsValid(s_View)) {
            // Pre-empted direct play: still notify so UI buttons reset.
            s_PrismaUI->Invoke(s_View,
                testTts ?
                "try{var _fr=document.getElementById('snpd-frame');"
                "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onTestTtsEnded)"
                "_fr.contentWindow.__onTestTtsEnded();}catch(e){}" :
                "try{var _fr=document.getElementById('snpd-frame');"
                "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
                "_fr.contentWindow.__onAudioEnded();}catch(e){}",
                nullptr);
            logger::info("SkyrimNetDashboard: audio notify-on-cancel (MCI) gen={}", myGen);
        } else {
            logger::info("SkyrimNetDashboard: audio notify skipped gen={} current={}",
                myGen, s_audioGen.load());
        }
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

        PlayAudioBytes(std::move(bytes), ct, /*testTts=*/false, /*notifyOnCancel=*/true);
    }).detach();
}

// Called from the /audio-raw endpoint -- JS fetched blob bytes and POSTed them
// as raw binary so blob: URLs (e.g. TTS) work despite being browser-only objects.
static void PlayAudioRaw(std::string bytes, std::string ct, bool testTts = false)
{
    std::thread([bytes = std::move(bytes), ct = std::move(ct), testTts]() mutable {
        logger::info("SkyrimNetDashboard: audio-raw {} bytes, ct='{}'", bytes.size(), ct);
        PlayAudioBytes(std::move(bytes), ct, testTts, /*notifyOnCancel=*/true);
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
        ++s_audioGen;     // bump gen INSIDE the lock so the runner's dequeue+snapshot sees it
        ++s_aqSessionGen; // invalidate the SSE handler's push gate
        s_aqPending.clear();
        s_aqPaused.store(false);
    }
    s_aqCv.notify_all();
    // (gen already bumped above)
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
        uint32_t genSnap = 0;
        {
            std::unique_lock<std::mutex> lk(s_aqMtx);
            // Wait until: (has segments AND not paused) OR shutdown.
            s_aqCv.wait(lk, []() {
                return (!s_aqPending.empty() && !s_aqPaused.load()) || !s_aqRunning.load();
            });
            if (!s_aqRunning.load()) break;
            if (s_aqPaused.load() || s_aqPending.empty()) continue;
            // Snapshot gen INSIDE the lock: ClearAudioQueue bumps gen under this
            // same lock, so if a clear raced with this dequeue we'll see it here.
            genSnap = s_audioGen.load();
            wavBytes = std::move(s_aqPending.front());
            s_aqPending.pop_front();
        }
        // If gen changed between our snapshot and now, a clear fired — discard.
        if (!wavBytes.empty() && s_audioGen.load() == genSnap)
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
    } else if (action == "resume") {
        // Resume after pause.
        if (s_aqRunning.load())
            ResumeAudioQueue();
    } else if (action == "pause") {
        // Pause: suspend playback, keeping queue intact so resume can continue.
        if (s_aqRunning.load())
            PauseAudioQueue();
        else
            StopAudio();
    } else if (action == "stop") {
        // Stop / close banner: clear the queue and stop immediately.
        if (s_aqRunning.load())
            ClearAudioQueue();
        else
            StopAudio();
        // Notify JS that playback ended so UI components (e.g. voice sample
        // play buttons) reset their state — they only revert on __onAudioEnded
        // which PlayAudioBytes skips when pre-empted by a gen bump.
        if (s_PrismaUI && s_PrismaUI->IsValid(s_View))
            s_PrismaUI->Invoke(s_View,
                "try{var _fr=document.getElementById('snpd-frame');"
                "if(_fr&&_fr.contentWindow&&_fr.contentWindow.__onAudioEnded)"
                "_fr.contentWindow.__onAudioEnded();}catch(e){}",
                nullptr);
    } else {
        logger::warn("SkyrimNetDashboard: audio unrecognised action='{}' src='{}'", action, src);
    }
}

