#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <vector>
#include <cstdint>

// Stub - audio debugger disabled in guardian build
class AudioDebugger {
public:
    AudioDebugger() {}
    ~AudioDebugger() {}
    void Feed(const std::vector<int16_t>& data) {}
};

#endif
