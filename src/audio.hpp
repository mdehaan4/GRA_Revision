// Procedural sound for Coral Bay. Every sound is synthesised in code, so the
// game needs no audio files: two 80s synthwave radio stations, a car engine,
// distant police sirens and one-shot effects (punch, horn, cash, phone, crash).
#pragma once
#include "raylib.h"

#include <cmath>
#include <cstdint>
#include <vector>

enum Sfx { SFX_PUNCH, SFX_HORN, SFX_CASH, SFX_RING, SFX_PASS, SFX_CRASH, SFX_BUSTED, SFX_DOOR };

struct Station {
    const char* name;
    float bpm;
    int roots[4];   // MIDI note of each bar's chord root
    bool minor[4];
};
static const Station STATIONS[2] = {
    {"Neon FM 86.4", 112, {45, 41, 48, 43}, {true, false, false, false}},   // Am F C G
    {"Wave 103", 96, {40, 48, 43, 50}, {true, false, false, false}},        // Em C G D
};

class Synth {
public:
    bool ok = false;
    bool radio = false;      // radio playing (in a car with a station on)
    int station = 0;
    float engine = 0;        // 0..1 engine volume
    float engineRev = 0;     // vehicle speed, drives pitch
    float throttle = 0;      // 0..1
    float siren = 0;         // 0..1 siren volume

    void Init() {
        InitAudioDevice();
        if (!IsAudioDeviceReady()) return;
        SetAudioStreamBufferSizeDefault(N);
        stream = LoadAudioStream(44100, 16, 1);
        PlayAudioStream(stream);
        ok = true;
    }
    void Close() {
        if (!ok) return;
        UnloadAudioStream(stream);
        CloseAudioDevice();
    }
    void Play(Sfx s) { if (ok) shots.push_back({s, 0.0f}); }
    void Update() {
        if (!ok) return;
        while (IsAudioStreamProcessed(stream)) {
            Fill();
            UpdateAudioStream(stream, buf, N);
        }
    }

private:
    static constexpr int N = 2048;
    static constexpr float SR = 44100.0f;
    AudioStream stream{};
    short buf[N]{};
    struct Shot { Sfx s; float t; };
    std::vector<Shot> shots;
    double mt = 0;
    float ph[12]{};
    float lpBass = 0, lpPad = 0, lpEng = 0;
    float radioVol = 0, engVol = 0, sirVol = 0, sirT = 0;
    uint32_t seed = 22222;

    float Noise() { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 9) & 0xffff) / 32768.0f - 1.0f; }
    static float Hz(float m) { return 440.0f * std::pow(2.0f, (m - 69.0f) / 12.0f); }
    static float Saw(float p) { return 2.0f * (p - std::floor(p)) - 1.0f; }
    static float Sq(float p) { return (p - std::floor(p)) < 0.5f ? 1.0f : -1.0f; }
    static float LpA(float fc) { return 1.0f - std::exp(-2.0f * PI * fc / SR); }
    void Adv(int i, float f) { ph[i] += f / SR; if (ph[i] > 1.0f) ph[i] -= std::floor(ph[i]); }

    float Music() {
        const Station& st = STATIONS[station];
        double spb = 60.0 / st.bpm / 4.0;                      // one 16th note
        long step = (long)(mt / spb);
        float fs = (float)(mt - step * spb);                    // time since this step began
        int s16 = (int)(step % 16), bar = (int)((step / 16) % 4);
        int root = st.roots[bar], third = st.minor[bar] ? 3 : 4;
        float out = 0;

        if (s16 == 0 || s16 == 8 || s16 == 10) {               // kick
            float f = 2.0f * PI * (50.0f * fs + 110.0f * (1.0f - std::exp(-fs * 30.0f)) / 30.0f);
            out += std::sin(f) * std::exp(-fs * 18.0f) * 0.9f;
        }
        float ts = -1;                                          // snare with a long "gated reverb" tail
        if (s16 >= 4 && s16 < 8) ts = (s16 - 4) * (float)spb + fs;
        if (s16 >= 12) ts = (s16 - 12) * (float)spb + fs;
        if (ts >= 0 && ts < 0.32f)
            out += Noise() * (std::exp(-ts * 14.0f) * 0.45f + 0.12f) + std::sin(2 * PI * 185.0f * ts) * std::exp(-ts * 25.0f) * 0.3f;
        if (s16 % 4 == 2) out += Noise() * std::exp(-fs * 70.0f) * 0.18f;   // hi-hat

        float fb = Hz((float)(root + ((s16 % 2) ? 12 : 0)));  // driving 16th bass
        Adv(0, fb);
        lpBass += LpA(650.0f) * (Saw(ph[0]) - lpBass);
        out += lpBass * std::exp(-fs * 7.0f) * 0.5f;

        const int iv[3] = {12, 12 + third, 19};                // pad: two detuned saws per note
        float pad = 0;
        for (int k = 0; k < 3; k++) {
            Adv(1 + k * 2, Hz((float)(root + iv[k])) * 1.003f);
            Adv(2 + k * 2, Hz((float)(root + iv[k])) * 0.997f);
            pad += Saw(ph[1 + k * 2]) + Saw(ph[2 + k * 2]);
        }
        lpPad += LpA(1100.0f) * (pad - lpPad);
        out += lpPad * 0.045f;

        const int arp[4] = {24, 24 + third, 31, 36};           // square-wave arpeggio
        Adv(7, Hz((float)(root + arp[s16 % 4])));
        out += Sq(ph[7]) * std::exp(-fs * 12.0f) * 0.07f;
        return out;
    }

    float ShotSample(Shot& s) {
        float t = s.t;
        switch (s.s) {
            case SFX_PUNCH: return Noise() * std::exp(-t * 30.0f) * 0.6f + std::sin(2 * PI * 65.0f * t) * std::exp(-t * 14.0f) * 0.8f;
            case SFX_HORN: {
                float env = std::min(1.0f, t * 60.0f) * std::min(1.0f, std::max(0.0f, (0.45f - t) * 30.0f));
                return (Sq(400.0f * t) + Sq(505.0f * t)) * 0.08f * env;
            }
            case SFX_CASH: return std::sin(2 * PI * (t < 0.08f ? 988.0f : 1319.0f) * t) * std::exp(-t * 9.0f) * 0.3f;
            case SFX_RING: {
                bool on = std::fmod(t, 0.6f) < 0.4f;
                return on ? std::sin(2 * PI * 1250.0f * t) * (std::sin(2 * PI * 20.0f * t) > 0 ? 1.0f : 0.35f) * 0.12f : 0.0f;
            }
            case SFX_PASS: {
                const float notes[4] = {72, 76, 79, 84};
                int i = std::min(3, (int)(t / 0.15f));
                return Sq(Hz(notes[i]) * t) * 0.09f * std::exp(-(t - i * 0.15f) * 3.0f);
            }
            case SFX_CRASH: return Noise() * std::exp(-t * 7.0f) * 0.5f + std::sin(2 * PI * 48.0f * t) * std::exp(-t * 10.0f) * 0.5f;
            case SFX_BUSTED: {
                const float notes[4] = {67, 63, 60, 55};
                int i = std::min(3, (int)(t / 0.28f));
                return Sq(Hz(notes[i]) * t * (1.0f + 0.01f * std::sin(t * 40.0f))) * 0.09f;
            }
            case SFX_DOOR: return std::sin(2 * PI * 110.0f * t) * std::exp(-t * 25.0f) * 0.6f + Noise() * std::exp(-t * 50.0f) * 0.3f;
        }
        return 0;
    }
    static float ShotLen(Sfx s) {
        switch (s) {
            case SFX_PUNCH: return 0.25f; case SFX_HORN: return 0.45f; case SFX_CASH: return 0.3f;
            case SFX_RING: return 2.4f; case SFX_PASS: return 1.2f; case SFX_CRASH: return 0.6f;
            case SFX_BUSTED: return 1.2f; case SFX_DOOR: return 0.2f;
        }
        return 0.5f;
    }

    void Fill() {
        const float dt = 1.0f / SR;
        for (int i = 0; i < N; i++) {
            radioVol += ((radio ? 0.55f : 0.0f) - radioVol) * 0.0005f;
            engVol += (engine - engVol) * 0.001f;
            sirVol += (siren - sirVol) * 0.0005f;
            float x = 0;
            float m = Music();
            mt += dt;
            x += m * radioVol;
            if (engVol > 0.001f) {
                float f = 38.0f + engineRev * 3.0f;
                Adv(8, f); Adv(9, f * 0.5f);
                float e = Saw(ph[8]) * 0.6f + Saw(ph[9]) * 0.4f;
                lpEng += LpA(300.0f + throttle * 900.0f) * (e - lpEng);
                x += lpEng * engVol * (0.12f + 0.1f * throttle);
            }
            if (sirVol > 0.001f) {
                sirT += dt;
                Adv(10, 760.0f + 340.0f * std::sin(2 * PI * 0.7f * sirT));
                x += (std::sin(2 * PI * ph[10]) * 0.6f + Sq(ph[10]) * 0.08f) * sirVol * 0.1f;
            }
            for (auto& s : shots) { x += ShotSample(s); s.t += dt; }
            buf[i] = (short)(std::tanh(x) * 0.9f * 32767.0f);
        }
        for (int i = (int)shots.size() - 1; i >= 0; i--)
            if (shots[i].t > ShotLen(shots[i].s)) shots.erase(shots.begin() + i);
    }
};
