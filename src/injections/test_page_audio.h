#pragma once
#include <string>

namespace Injections {

// Test page specific audio handling.
// The "Test TTS" buttons on the setup/voice page use:
//   fetch(ttsEndpoint) → t.blob() → URL.createObjectURL → new Audio(blobUrl) → play()
// Ultralight's fetch() cannot reliably read a blob: URL backed by a large binary
// audio response, so the play() call silently drops the audio (zero sound, stuck
// "Testing TTS..." spinner).
//
// The main fix is in PatchBundle (main.cpp): the three minified test-TTS handlers
// are patched at proxy-serve time to use t.arrayBuffer() + POST /audio-raw directly,
// then await the snpd:audioended event that C++ fires when waveOut finishes.
//
// This injected script handles the audio-ended cleanup for any handler that was not
// caught by PatchBundle (e.g. an unknown bundle version), by listening for the
// snpd:audioended event and notifying the UI.
inline std::string GetTestPageAudio()
{
    return R"TESTPAGEAUDIO(
console.log('[snpd] test page audio handler loaded');
)TESTPAGEAUDIO";
}

} // namespace Injections
