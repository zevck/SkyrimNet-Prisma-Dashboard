# Code Review: SkyrimNet Prisma Dashboard

**Date:** March 15, 2026  
**Scope:** Full source review of `src/` (main.cpp, pch.h, PrismaUI_API.h, keyhandler/)

---

## Critical / Bugs

### 1. Race condition on `s_wavBuf` (data race / potential crash)

**File:** `src/main.cpp` ~L262-275

The mutex protects the *assignment* to `s_wavBuf`, but `PlaySound` reads from `s_wavBuf.data()` *after* the lock is released. If a second audio thread calls `PlayAudioBytes` concurrently, `StopAudio()` + the swap could reallocate the buffer while `PlaySound` is still reading it.

```cpp
// Current: lock released before PlaySound reads the buffer
{
    std::lock_guard<std::mutex> lk(s_wavBufMtx);
    s_wavBuf = std::move(bytes);
}
BOOL ok = PlaySound(reinterpret_cast<LPCSTR>(s_wavBuf.data()), ...);
```

**Fix:** Keep the lock held for the entire `PlaySound` call, or use a separate dedicated buffer per playback (since `PlaySound` with `SND_MEMORY` is synchronous on this thread).

---

### 2. `gethostbyname` is not thread-safe

**File:** `src/main.cpp` ~L926-928

`FetchResource` is called from many threads simultaneously (server handler threads, audio threads). `gethostbyname` returns a pointer to static internal storage and is not thread-safe. Use `getaddrinfo` instead.

---

### 3. Shared globals accessed without synchronization

`s_PrismaUI`, `s_View`, `s_toggleKey`, `s_toggleHandle`, and `s_cfg` are read/written from the game thread, the clipboard monitor thread, the HTTP server threads, and audio threads with no synchronization. This is undefined behavior. Consider making `s_View` an `std::atomic<PrismaView>` and protecting `s_cfg` with a mutex or making fields atomic.

---

### 4. Step numbering typo in `MessageHandler`

**File:** `src/main.cpp` ~L2155-2195

Steps are numbered 1, 2, 2, 3, 3, 3. Should be sequential (1–6).

---

## Security

### 5. `/save-file?path=` accepts arbitrary paths

**File:** `src/main.cpp` ~L1835-1860

When the `path` query parameter is supplied, the file is written to whatever path is provided with no validation. The native save-dialog guarantees a user-chosen path, but since the HTTP server is reachable by any local process on the loopback interface, a malicious local application could POST to `/save-file?path=C:\Windows\...` and write arbitrary files. Consider at minimum validating the path is under a user-writable directory (Documents, AppData, etc.).

---

### 6. Unbounded HTTP request buffer

**File:** `src/main.cpp` ~L1733-1756

The `rawReq` string grows without limit. A misbehaving client could send an enormous request and exhaust memory. Add a maximum request size check (e.g., 64 MB).

---

## Code Quality

### 7. Duplicated key name table

The `kNames` array is copy-pasted between `DxKeyName()` (~L38-65) and `SettingsToJson()` (~L71-101). Extract it to a single `static const` at file scope. Note: the `SettingsToJson` copy has `"\\\\"` for scan code `0x56` vs `"\\"` in the other — one of them is likely wrong.

---

### 8. Duplicated URL parsing logic

Host/port extraction from a URL string appears in at least three places: `AudioParseUrl` (~L190), `StartShellServer` (~L1705), and `MessageHandler` (~L2170). Factor into a shared `ParseHostPort` helper.

---

### 9. `using namespace std;` in pch.h

**File:** `src/pch.h` L25

This pollutes every translation unit's global namespace. In a Skyrim modding context with CommonLib (which has its own `RE::` types), this can cause ambiguous name resolution. Remove it.

---

### 10. Dead constant `SKYRIMNET_URL`

**File:** `src/main.cpp` ~L135

Marked as a "fallback" but never referenced anywhere — the INI value or its default always wins. Remove or use it.

---

### 11. Monolithic file structure

`src/main.cpp` is ~2,240 lines containing embedded HTML (~400 lines), injected JS (~500 lines), HTTP server logic, audio playback, clipboard management, INI parsing, and SKSE bootstrapping. Consider splitting into logical units:

| File | Responsibility |
|------|---------------|
| `shell.h/cpp` | `BuildShellHtml()` |
| `proxy.h/cpp` | `FetchResource`, `StartShellServer`, `ContentTypeFromPath` |
| `audio.h/cpp` | `PlayAudio*`, `StopAudio`, `OnAudioMessage` |
| `clipboard.h/cpp` | `Get/SetClipboardTextW32`, `StartClipboardMonitor` |
| `patches.h/cpp` | `InjectPatches`, `PatchBundle`, `PatchCSS` |

---

### 12. Detached threads with no shutdown mechanism

The clipboard monitor thread, the server accept loop, and per-connection handler threads are all `detach()`-ed. If the DLL unloads (e.g., game shutdown while threads are mid-flight), this is undefined behavior. Use `std::jthread` with a stop token, or at minimum set an atomic flag and join on shutdown.

---

## Minor / Nits

### 13. `UrlDecode` doesn't validate hex digits

`UrlDecode` doesn't validate that the two chars after `%` are actually hex digits — `strtol` will silently consume partial input.

### 14. Integer overflow in `extractInt`

`extractInt` in the `/settings-save` handler has no overflow protection on the manual `val = val * 10 + ...` accumulation.

### 15. `PatchCSS` doc comment is wrong

`PatchCSS` is misnamed in its doc comment — says "Patches the React app JS bundle" (copy-paste from `PatchBundle`).

### 16. Redundant lambda `mciErr`

`auto mciErr = [](MCIERROR e) -> ...` is redefined every call to `PlayAudioBytes`. Make it a file-scope helper.

### 17. Redundant `confirm`/`alert`/`prompt` overrides

The overrides are set both in `BuildShellHtml` (shell frame) *and* in `InjectPatches` (iframe). The shell-frame overrides are redundant since the iframe patches its own `contentWindow` in `snpdPatch`.

---

## What's Done Well

- **KeyHandler** (`src/keyhandler/keyhandler.h` / `keyhandler.cpp`) is cleanly separated, correctly uses `shared_mutex` for read-heavy access, collects callbacks before invoking them (avoids holding the lock during callbacks), and handles registration/unregistration edge cases carefully.
- The chunked transfer-encoding decoder in `FetchResource` is correct and handles chunk extensions.
- The RIFF/WAV patching logic for streaming headers with `0xFFFFFFFF` sizes is a nice touch.
- The asset cache with content-hash detection is a pragmatic optimization.
- The CM6 drag-selection relay and clipboard bridge are creative solutions to real Ultralight limitations.
