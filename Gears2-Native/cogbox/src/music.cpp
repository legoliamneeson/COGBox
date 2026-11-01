// COGBox - the library's music (Shagg 1 Nov, after Athelas): the chosen game's menu theme plays while its box is in
// front of the carousel and on into that game's settings page, crossfades when another box stops in front, and stops
// at once when a game starts. MUSIC ON / OFF on the library screen ([Launcher] Music in CogBox.ini; [Launcher]
// MusicVolume, 0-100, default 35).
//
// COGBox ships no game audio. A game's track is music\<key>.wav beside CogBox.exe (music\gears2.wav ...), made from the
// player's own copy - COGBox can't make it yet: the menu music sits in the game's UE3 packages as XMA, and COGBox has
// no package reader or XMA decoder (see the patch notes). Any WAV SDL reads works (PCM, float, ADPCM); it loops.
// No track, no audio device or a file SDL can't read: silence, nothing else changes. The audio device is opened only
// when there is a track to play.
#include "app.h"

#include <algorithm>

namespace {
struct Track {
    int game = -1;
    SDL_AudioStream *s = nullptr;
    Uint8 *buf = nullptr;
    Uint32 len = 0, pos = 0;
    SDL_AudioSpec spec{};
    float gain = 0, target = 0;
};
Track g_t[2];                 // [0] the chosen game's (playing or fading in), [1] the one before it, fading out
bool g_audioTried = false, g_audio = false;
int g_have[G_COUNT];          // music\<key>.wav: 1 there, 0 not there, 2 there but it couldn't be played (not tried again)
double g_scanAt = -100;
const float kFade = 0.8f;     // seconds for a full fade in or out

void track_close(Track &t)
{
    if (t.s) SDL_DestroyAudioStream(t.s);
    if (t.buf) SDL_free(t.buf);
    t = Track();
}

bool audio_ready()
{
    if (!g_audioTried) { g_audioTried = true; g_audio = SDL_InitSubSystem(SDL_INIT_AUDIO); }
    return g_audio;
}

bool track_open(Track &t, int g)
{
    SDL_AudioSpec spec;
    Uint8 *buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(music_path(g).c_str(), &spec, &buf, &len)) return false;
    const Uint32 fs = (Uint32)SDL_AUDIO_FRAMESIZE(spec);
    len -= fs ? len % fs : len;
    if (!fs || len == 0 || spec.freq <= 0) { SDL_free(buf); return false; }
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) { SDL_free(buf); return false; }
    SDL_SetAudioStreamGain(s, 0.0f);
    SDL_ResumeAudioStreamDevice(s);
    t.game = g; t.s = s; t.buf = buf; t.len = len; t.pos = 0; t.spec = spec; t.gain = 0; t.target = 0;
    return true;
}

// keeps about half a second queued, a quarter of a second at a time; at the end the track starts again
void track_feed(Track &t)
{
    const int fs = SDL_AUDIO_FRAMESIZE(t.spec), chunk = std::max(1, t.spec.freq / 4) * fs;
    while (SDL_GetAudioStreamQueued(t.s) < chunk * 2) {
        const Uint32 n = std::min<Uint32>(t.len - t.pos, (Uint32)chunk);
        if (!SDL_PutAudioStreamData(t.s, t.buf + t.pos, (int)n)) break;
        t.pos += n;
        if (t.pos >= t.len) t.pos = 0;
    }
}
}  // namespace

std::string music_path(int g) { return join(join(exe_dir(), "music"), std::string(game_def(g).key) + ".wav"); }

void music_rescan()
{
    for (int g = 0; g < G_COUNT; g++) g_have[g] = !file_exists(music_path(g)) ? 0 : g_have[g] == 2 ? 2 : 1;
    g_scanAt = SDL_GetTicks() / 1000.0;
}

bool music_has_track(int g) { return g >= 0 && g < G_COUNT && g_have[g] == 1; }

bool music_any_track()
{
    for (int g = 0; g < G_COUNT; g++) if (g_have[g] == 1) return true;
    return false;
}

bool music_on(const App &a) { return a.ini.geti("Launcher", "Music", 1) != 0; }

void music_set_on(App &a, bool on)
{
    a.ini.seti("Launcher", "Music", on ? 1 : 0);
    a.ini.save();
}

void music_stop()
{
    track_close(g_t[0]);
    track_close(g_t[1]);
}

void music_update(App &a)
{
    const double now = SDL_GetTicks() / 1000.0;
    static double last = now;
    const float dt = (float)std::max(0.0, std::min(0.1, now - last));
    last = now;
    if (now - g_scanAt > 5) music_rescan();   // a track put in the music folder while COGBox is open
    // a game is running (or starting): no launcher music at all, so it never plays over the game
    if (a.run.proc) { music_stop(); return; }

    int want = -1;
    if (music_on(a)) {
        const int slot = library_slot(a, a.front);
        if (a.screen == SCR_PAGE || a.zoomTo == SCR_PAGE) want = a.page;   // the game's own settings page: its music goes on
        else if (slot >= 0 && a.carPos == (float)slot && ImGui::GetTime() - a.restSince > 0.35) want = a.front;
        else if (slot >= 0) want = g_t[0].game;   // turning (or just stopped): what plays keeps playing for now
    }
    if (!music_has_track(want)) want = -1;
    if (want != g_t[0].game) {
        track_close(g_t[1]);
        g_t[1] = g_t[0];          // the one playing fades out ...
        g_t[0] = Track();
        g_t[1].target = 0;
        if (want >= 0 && (!audio_ready() || !track_open(g_t[0], want))) g_have[want] = 2;   // ... not tried again
    }
    const float vol = std::max(0, std::min(100, a.ini.geti("Launcher", "MusicVolume", 35))) / 100.0f;
    if (g_t[0].s) g_t[0].target = vol;
    for (Track &t : g_t) {
        if (!t.s) continue;
        const float step = dt / kFade * std::max(vol, 0.05f);
        t.gain = t.gain < t.target ? std::min(t.target, t.gain + step) : std::max(t.target, t.gain - step);
        SDL_SetAudioStreamGain(t.s, t.gain);
        track_feed(t);
    }
    if (g_t[1].s && g_t[1].gain <= 0.0f) track_close(g_t[1]);
}
