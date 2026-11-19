#include "app.h"
#include <mmsystem.h>
#include <cstring>

// A short, soft "tick" synthesised once into an in-memory WAV, so the exe needs no sound files.
static std::vector<BYTE> s_tick;

static void Put16(std::vector<BYTE>& v, size_t at, WORD x)  { v[at] = x & 0xFF; v[at + 1] = x >> 8; }
static void Put32(std::vector<BYTE>& v, size_t at, DWORD x) { for (int i = 0; i < 4; i++) v[at + i] = (x >> (8 * i)) & 0xFF; }

static void BuildTick() {
    const int rate = 44100;
    const int n = rate * 90 / 1000;
    const DWORD dataBytes = n * 2;
    s_tick.assign(44 + dataBytes, 0);
    memcpy(&s_tick[0], "RIFF", 4);  Put32(s_tick, 4, 36 + dataBytes);
    memcpy(&s_tick[8], "WAVEfmt ", 8);
    Put32(s_tick, 16, 16); Put16(s_tick, 20, 1); Put16(s_tick, 22, 1);
    Put32(s_tick, 24, rate); Put32(s_tick, 28, rate * 2); Put16(s_tick, 32, 2); Put16(s_tick, 34, 16);
    memcpy(&s_tick[36], "data", 4); Put32(s_tick, 40, dataBytes);
    const double pi2 = 6.283185307179586;
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double attack = i < 90 ? i / 90.0 : 1.0;
        double env = attack * std::exp(-t * 48.0);
        double s = 0.62 * std::sin(pi2 * 1480 * t) + 0.28 * std::sin(pi2 * 2220 * t) + 0.10 * std::sin(pi2 * 740 * t);
        int v = (int)(s * env * 0.30 * 32767.0);
        Put16(s_tick, 44 + i * 2, (WORD)(short)v);
    }
}

void PlaySoundKind(int kind) {
    switch (kind) {
    case SOUND_TICK:
        if (s_tick.empty()) BuildTick();
        PlaySoundW((LPCWSTR)s_tick.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        break;
    case SOUND_SYSTEM:
        if (!PlaySoundW(L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT))
            PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        break;
    case SOUND_CUSTOM:
        if (!g_settings.soundFile.empty())
            PlaySoundW(g_settings.soundFile.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        break;
    }
}

void PlayCopySound() {
    if (g_settings.soundEnabled) PlaySoundKind(g_settings.soundKind);
}
