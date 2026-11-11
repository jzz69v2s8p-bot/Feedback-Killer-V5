// FeedbackEngine.h
// ---------------------------------------------------------------------------
// Feedback-Erkennung und -Unterdrückung für einen Mono-Kanal.
// Reines C++17, keine JUCE-Abhängigkeit (deshalb auch ohne Plugin-Host testbar).
//
// Unterschiede zur Python-Version (feedback_guard_2ch.py):
//   * FFT 4096 mit echter Frequenzauflösung (statt 256 Samples + Zero-Padding)
//   * Parabolische Interpolation für die Frequenz
//   * Peak-Tracking über mehrere Frames (Anstiegsrate, Frequenzstabilität)
//   * Harmonik-Test: Sprache/Gesang hat Obertonreihen, Feedback ist ein Einzelton
//   * Notches als schmale Peaking-Filter mit weichem Ein-/Ausblenden
//   * Kein Datei-I/O, kein print(), keine Allokation im Audio-Thread
//   * Null Latenz: Analyse läuft seitlich, das Signal geht nur durch die Filter
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <utility>
#include <vector>
#ifdef FG_DEBUG
#include <cstdio>
#endif

namespace fg
{
constexpr int kMaxNotches  = 12;
constexpr int kMaxTracks   = 8;
constexpr int kHistory     = 16;
constexpr int kDisplayBins = 200;
constexpr double kPi       = 3.14159265358979323846;

struct Settings
{
    bool  enabled      = true;
    bool  monitorOnly  = false;   // erkennen und anzeigen, aber nicht filtern
    float prominenceDb = 14.0f;   // Mindest-Überhöhung des Peaks über die Umgebung
    int   speed        = 1;       // 0 vorsichtig, 1 normal, 2 schnell
    int   maxNotches   = 8;
    float maxDepthDb   = 18.0f;   // maximale Absenkung pro Notch (positiver Wert)
    float holdSeconds  = 60.0f;   // danach Notch ausblenden; >= 600 = dauerhaft
    float minFreq      = 150.0f;
    float maxFreq      = 10000.0f;
    bool  duckEnabled  = true;
    float duckDb       = 6.0f;    // Notbremse bei sehr schnellem Aufschaukeln
    float notchQ       = 30.0f;
    bool  ringOut      = false;   // Einmessen: neue Filter werden dauerhaft festgehalten
    bool  useRef       = false;   // PA-Referenz (Sidechain) als zusätzliche Bedingung nutzen
    bool  mergeCluster = false;   // wandernde Töne: dicht beieinander liegende neue Filter zu einem breiteren bündeln
};

enum class Rule : uint8_t { Runaway = 0, Growing = 1, Sustained = 2 };

struct Event
{
    float   freq         = 0;
    float   depthDb      = 0;     // Zielabsenkung (negativ)
    float   prominenceDb = 0;
    float   slopeDbPerS  = 0;
    uint8_t rule         = 0;
    bool    deepened     = false; // bestehende Notch vertieft statt neu gesetzt
    bool    monitorOnly  = false;
    bool    pinned       = false; // Filter bleibt dauerhaft stehen
    bool    merged       = false; // bestehendes Filter verbreitert statt neues zu setzen
    float   bwLo = 0, bwHi = 0;   // Frequenzspanne des (verbreiterten) Filters
};

// ---------------------------------------------------------------------------
class FFT
{
public:
    void init (int n)
    {
        N = n;
        int l = 0;
        while ((1 << l) < n) ++l;
        cosT.resize ((size_t) n / 2);
        sinT.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
        {
            cosT[(size_t) i] = (float) std::cos (2.0 * kPi * i / n);
            sinT[(size_t) i] = (float) -std::sin (2.0 * kPi * i / n);
        }
        rev.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < l; ++b)
                if (i & (1 << b)) r |= 1 << (l - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    void forward (float* re, float* im) const
    {
        for (int i = 0; i < N; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (int size = 2; size <= N; size <<= 1)
        {
            const int half = size >> 1, step = N / size;
            for (int start = 0; start < N; start += size)
                for (int k = 0; k < half; ++k)
                {
                    const float wr = cosT[(size_t) (k * step)];
                    const float wi = sinT[(size_t) (k * step)];
                    const int a = start + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;  im[b] = im[a] - ti;
                    re[a] += tr;         im[a] += ti;
                }
        }
    }

private:
    int N = 0;
    std::vector<float> cosT, sinT;
    std::vector<int> rev;
};

// ---------------------------------------------------------------------------
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    inline double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setPeaking (double f, double gainDb, double q, double sr)
    {
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 2.0 * kPi * f / sr;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c     = std::cos (w0);
        const double a0    = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * c) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * c) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    void clear() { z1 = z2 = 0; }
};

// ---------------------------------------------------------------------------
class ChannelEngine
{
public:
    static constexpr int kFFT = 4096;

    ChannelEngine() = default;

    void prepare (double sampleRate)
    {
        // Fixierte Filter über ein erneutes prepare() hinweg behalten (Host ändert Samplerate/Puffer)
        std::array<std::pair<double, double>, kMaxNotches> keep {};
        int nKeep = 0;
        for (auto& sl : slots)
            if (sl.active && sl.pinned) keep[(size_t) nKeep++] = { sl.freq, sl.targetDb };

        sr   = sampleRate;
        // 8192 Punkte (bei >50 kHz 16384): ca. 5,9 Hz Rasterbreite, damit auch Töne ab 20 Hz sauber aufgelöst werden.
        // Die Analyse läuft seitlich, das Signal selbst wird dadurch nicht verzögert.
        N    = (sr > 50000.0) ? 16384 : 8192;
        hop  = (sr > 50000.0) ? 1024 : 512;
        nb   = N / 2 + 1;
        binHz = sr / N;
        frameRate = sr / hop;

        fft.init (N);
        window.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));

        ring.assign ((size_t) N, 0.0f);
        re.assign ((size_t) N, 0.0f);
        im.assign ((size_t) N, 0.0f);
        refRing.assign ((size_t) N, 0.0f);
        reRef.assign ((size_t) N, 0.0f);
        imRef.assign ((size_t) N, 0.0f);
        refDb.assign ((size_t) nb, -200.0f);
        refPrefix.assign ((size_t) nb + 1, 0.0);
        db.assign ((size_t) nb, -200.0f);
        prefix.assign ((size_t) nb + 1, 0.0);
        prefixP.assign ((size_t) nb + 1, 0.0);
        floorDb.assign ((size_t) nb, -200.0f);
        dbOffset = 10.0 * std::log10 (16.0 / ((double) N * (double) N));

        // logarithmische Anzeige-Bins 20 Hz .. 22 kHz
        const double fLo = dispFLo, fHi = dispFHi = std::min (22000.0, sr * 0.46);
        for (int d = 0; d < kDisplayBins; ++d)
        {
            const double f0 = fLo * std::pow (fHi / fLo, (double) d / kDisplayBins);
            const double f1 = fLo * std::pow (fHi / fLo, (double) (d + 1) / kDisplayBins);
            dispLo[(size_t) d] = std::clamp ((int) std::floor (f0 / binHz), 1, nb - 2);
            dispHi[(size_t) d] = std::clamp ((int) std::ceil  (f1 / binHz), dispLo[(size_t) d], nb - 2);
        }

        duckAttack  = 1.0 - std::exp (-1.0 / (0.005 * sr));
        duckRelease = 1.0 - std::exp (-1.0 / (1.5 * sr));
        rampDownStep = 0.5;                                   // dB pro 32 Samples beim Absenken
        rampUpStep   = 3.6 * 32.0 / sr;                       // dB pro 32 Samples beim Ausblenden (~5 s für 18 dB)
        reset();
        for (int i = 0; i < nKeep; ++i) loadPinned (keep[(size_t) i].first, keep[(size_t) i].second);
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        for (auto& t : tracks) t = Track{};
        for (auto& s : slots)  s = Slot{};
        writePos = 0; hopCount = 0; rampCount = 0;
        sampleClock = 0;
        duckGain = 1.0; duckHold = 0;
        numCand = numPk = 0;
        std::fill (refRing.begin(), refRing.end(), 0.0f);
        haveRef = refReady = refPaSilent = false;
        riskSmooth = 0.0f;
        uiRisk.store (0.0f); uiRefState.store (0);
        oorFreq = 0.0f; oorCount = 0; uiOutOfRangeHz.store (0.0f);
        for (auto& a : uiNotchFreq)  a.store (0.0f);
        for (auto& a : uiNotchDepth) a.store (0.0f);
        for (auto& a : uiNotchPinned) a.store (false);
        uiDuckDb.store (0.0f);
        for (auto& a : uiSpectrum)   a.store (-120.0f);
    }

    void setSettings (const Settings& s) { cfg = s; }

    // Verarbeitet in-place. Audio-Thread-sicher (keine Allokation, keine Locks).
    // ref: optionales Referenzsignal (was zur PA geht), gleiche Länge wie data. nullptr = nicht vorhanden.
    void process (float* data, int n, const float* ref = nullptr)
    {
        haveRef = (ref != nullptr);

        if (pinRequest.exchange (false))
            for (auto& sl : slots)
                if (sl.active) { sl.pinned = true; sl.releasing = false; }

        if (! cfg.enabled)
        {
            // Durchreichen; Filter langsam abbauen lassen wäre hier unnötig – Bypass soll hart sein.
            return;
        }

        float blockPeak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];
            blockPeak = std::max (blockPeak, std::fabs (x));

            ring[(size_t) writePos] = x;
            refRing[(size_t) writePos] = ref ? ref[i] : 0.0f;
            writePos = (writePos + 1) % N;
            ++sampleClock;

            if (++hopCount >= hop) { hopCount = 0; analyseFrame(); }
            if (++rampCount >= 32) { rampCount = 0; updateNotches(); }

            double y = x;
            if (! cfg.monitorOnly)
            {
                for (auto& s : slots)
                    if (s.active) y = s.bq.process (y);

                const double target = (cfg.duckEnabled && duckHold > 0) ? std::pow (10.0, -cfg.duckDb / 20.0) : 1.0;
                duckGain += (target - duckGain) * (target < duckGain ? duckAttack : duckRelease);
                y *= duckGain;
            }
            data[i] = (float) y;
        }

        const float lvl = 20.0f * std::log10 (std::max (blockPeak, 1.0e-6f));
        uiLevelDb.store (lvl, std::memory_order_relaxed);
    }

    // ---- Daten für die Oberfläche (aus beliebigem Thread lesbar) ----------
    std::array<std::atomic<float>, kDisplayBins> uiSpectrum {};   // dBFS (Sinus-äquivalent)
    std::array<std::atomic<float>, kMaxNotches>  uiNotchFreq {};  // 0 = Slot frei
    std::array<std::atomic<float>, kMaxNotches>  uiNotchDepth {}; // dB (negativ)
    std::array<std::atomic<bool>,  kMaxNotches>  uiNotchPinned {};
    std::atomic<float>    uiDuckDb  { 0.0f };
    std::atomic<float>    uiRisk    { 0.0f };   // Feedback-Nähe: 0 = ruhig, 1 = an der Auslöseschwelle
    std::atomic<int>      uiRefState { 0 };     // 0 = Referenz aus/fehlt, 1 = PA still, 2 = PA aktiv
    std::atomic<float>    uiLevelDb { -120.0f };
    std::atomic<float>    uiOutOfRangeHz { 0.0f };   // >0: starker stehender Ton außerhalb des Suchbereichs
    std::atomic<uint32_t> detections { 0 };

    // Single-producer (Audio) / single-consumer (UI) Ereignis-Queue
    bool popEvent (Event& e)
    {
        const uint32_t r = evRead.load (std::memory_order_relaxed);
        if (r == evWrite.load (std::memory_order_acquire)) return false;
        e = events[r % kEvents];
        evRead.store (r + 1, std::memory_order_release);
        return true;
    }

    double getSampleRate() const { return sr; }
    double displayLo() const { return dispFLo; }
    double displayHi() const { return dispFHi; }

    // Alle aktuell gesetzten Filter dauerhaft festhalten (aus beliebigem Thread aufrufbar).
    void pinAll() { pinRequest.store (true); }

    // Fixierten Filter direkt setzen (Audio-Thread bzw. vor dem Start). Wird beim Laden eines Projekts benutzt.
    bool loadPinned (double f, double depthDb)
    {
        if (! (f >= 15.0 && f < sr * 0.5)) return false;
        depthDb = std::clamp (depthDb, -40.0, -1.0);
        for (auto& sl : slots)
            if (sl.active && std::fabs (sl.freq - f) < 0.5) return true;
        for (int i = 0; i < kMaxNotches; ++i)
        {
            auto& sl = slots[(size_t) i];
            if (sl.active) continue;
            sl = Slot{};
            sl.active = true; sl.pinned = true;
            sl.freq = f; sl.fLo = sl.fHi = f; sl.q = effectiveQ (f);
            sl.currentDb = sl.targetDb = depthDb;
            sl.lastRefresh = sampleClock;
            sl.bq.setPeaking (f, depthDb, sl.q, sr);
            uiNotchFreq[(size_t) i].store ((float) f);
            uiNotchDepth[(size_t) i].store ((float) depthDb);
            uiNotchPinned[(size_t) i].store (true);
            return true;
        }
        return false;
    }
    // Gesamtübertragung aller aktiven Filter bei Frequenz f in dB (für Tests und Anzeigen)
    double notchResponseDb (double f) const
    {
        const double w = 2.0 * kPi * f / sr;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
        double total = 0.0;
        for (const auto& s : slots)
        {
            if (! s.active) continue;
            const auto num = s.bq.b0 + s.bq.b1 * z1 + s.bq.b2 * z2;
            const auto den = 1.0 + s.bq.a1 * z1 + s.bq.a2 * z2;
            total += 20.0 * std::log10 (std::max (1.0e-9, std::abs (num / den)));
        }
        return total;
    }

    int    getActiveNotchCount() const
    {
        int c = 0;
        for (auto& s : slots) if (s.active) ++c;
        return c;
    }

    // Für Tests
    struct NotchInfo { double freq; double currentDb; double targetDb; };
    std::vector<NotchInfo> getNotchInfo() const
    {
        std::vector<NotchInfo> v;
        for (auto& s : slots) if (s.active) v.push_back ({ s.freq, s.currentDb, s.targetDb });
        return v;
    }

private:
    struct Track
    {
        bool  active = false;
        float freq = 0, prom = 0;
        int   count = 0, missed = 0, sinceAct = 1 << 20;
        bool  acted = false;
        float refScore = 0.0f;           // gleitender Anteil der Frames, in denen der Ton auch in der PA-Referenz steht
        std::array<float, kHistory> lvl {}, frq {}, bb {};
        int   head = 0;
    };

    struct Slot
    {
        bool   active = false, releasing = false, pinned = false;
        double q = 30.0;
        double freq = 0, currentDb = 0, targetDb = 0;
        double fLo = 0, fHi = 0;  // abgedeckte Spanne (bei einem unverbreiterten Filter = freq)
        int64_t lastRefresh = 0;
        int    hits = 1;          // wie oft dieser Ton erkannt wurde
        float  peakProm = 0.0f;   // stärkste Überhöhung
        Biquad bq;
    };

    static constexpr float kActLevel = -62.0f;
    static constexpr double kMergeMinHz = 4000.0;

    struct Cand { float freq, level, prom; int bin; bool used; };

    // ----------------------------------------------------------------------
    void analyseFrame()
    {
        // Letzte N Samples gefenstert in den FFT-Puffer
        for (int i = 0; i < N; ++i)
        {
            re[(size_t) i] = ring[(size_t) ((writePos + i) % N)] * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        fft.forward (re.data(), im.data());

        prefixP[0] = 0.0;
        for (int k = 0; k < nb; ++k)
        {
            const double p = (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
            db[(size_t) k] = (float) (10.0 * std::log10 (p + 1.0e-20) + dbOffset);
            prefixP[(size_t) k + 1] = prefixP[(size_t) k] + p;
        }

        // Lokaler Boden: Mittel der dB-Werte in [k-25,k-5] und [k+5,k+25]
        prefix[0] = 0.0;
        for (int k = 0; k < nb; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + db[(size_t) k];
        for (int k = 0; k < nb; ++k)
        {
            double sum = 0; int cnt = 0;
            const int l0 = std::max (0, k - 25), l1 = k - 5;          // inkl.
            if (l1 >= l0) { sum += prefix[(size_t) l1 + 1] - prefix[(size_t) l0]; cnt += l1 - l0 + 1; }
            const int r0 = k + 5, r1 = std::min (nb - 1, k + 25);
            if (r1 >= r0) { sum += prefix[(size_t) r1 + 1] - prefix[(size_t) r0]; cnt += r1 - r0 + 1; }
            floorDb[(size_t) k] = cnt > 0 ? (float) (sum / cnt) : db[(size_t) k];
        }

        publishSpectrum();

        // ---- PA-Referenz analysieren (nur wenn genutzt und vorhanden) -------
        refReady = false; refPaSilent = false;
        if (cfg.useRef && haveRef)
        {
            for (int i = 0; i < N; ++i)
            {
                reRef[(size_t) i] = refRing[(size_t) ((writePos + i) % N)] * window[(size_t) i];
                imRef[(size_t) i] = 0.0f;
            }
            fft.forward (reRef.data(), imRef.data());
            refPrefix[0] = 0.0;
            float mx = -200.0f;
            for (int k = 0; k < nb; ++k)
            {
                const double pw = (double) reRef[(size_t) k] * reRef[(size_t) k] + (double) imRef[(size_t) k] * imRef[(size_t) k];
                refDb[(size_t) k] = (float) (10.0 * std::log10 (pw + 1.0e-20) + dbOffset);
                refPrefix[(size_t) k + 1] = refPrefix[(size_t) k] + refDb[(size_t) k];
                if (k >= 2) mx = std::max (mx, refDb[(size_t) k]);
            }
            refReady = true;
            refPaSilent = mx < -70.0f;      // PA praktisch still: Rückkopplung ist dann nicht möglich
        }
        uiRefState.store (refReady ? (refPaSilent ? 1 : 2) : 0, std::memory_order_relaxed);

        // ---- Peaks einsammeln ---------------------------------------------
        numCand = numPk = 0;
        const int kLo = std::max (2, (int) std::floor (15.0 / binHz));
        const int kHi = std::min (nb - 4, (int) std::ceil (std::max ((double) cfg.maxFreq, 8000.0) / binHz));
        const float candThresh = cfg.prominenceDb - 3.0f;
        constexpr float kGate = -75.0f;     // absolute Mindestlautstärke (dBFS, Sinus-äquivalent) fürs Verfolgen

        for (int k = kLo; k <= kHi; ++k)
        {
            const float b = db[(size_t) k];
            if (! (b > db[(size_t) k - 1] && b >= db[(size_t) k + 1])) continue;
            if (b < kGate) continue;
            const float prom = b - floorDb[(size_t) k];
            if (prom < 8.0f && prom < candThresh) continue;

            const float a = db[(size_t) k - 1], c = db[(size_t) k + 1];
            const float denom = a - 2.0f * b + c;
            float delta = denom < -1.0e-6f ? 0.5f * (a - c) / denom : 0.0f;
            delta = std::clamp (delta, -0.5f, 0.5f);
            const float level = b - 0.25f * (a - c) * delta;
            const float freq  = (float) ((k + delta) * binHz);

            if (prom >= 8.0f && numPk < (int) pk.size()) { pk[(size_t) numPk] = freq; pkProm[(size_t) numPk] = prom; ++numPk; }

            if (freq >= cfg.minFreq - 0.5 * binHz && freq <= cfg.maxFreq + 0.5 * binHz && prom >= candThresh)
            {
                Cand cd { freq, level, prom, k, false };
                if (numCand < (int) cand.size()) cand[(size_t) numCand++] = cd;
                else
                {
                    int w = 0;
                    for (int j = 1; j < numCand; ++j) if (cand[(size_t) j].prom < cand[(size_t) w].prom) w = j;
                    if (cand[(size_t) w].prom < prom) cand[(size_t) w] = cd;
                }
            }
        }

        // ---- Starker stehender Ton AUSSERHALB des Suchbereichs? (nur melden, nicht filtern) ----
        {
            float bestLvl = -200.0f, bestF = 0.0f;
            const int oLo = std::max (2, (int) std::floor (20.0 / binHz));
            const int oHi = std::min (nb - 3, (int) std::floor (dispFHi / binHz));
            for (int k = oLo; k <= oHi; ++k)
            {
                const double fk = k * binHz;
                if (fk >= cfg.minFreq && fk <= cfg.maxFreq) continue;
                const float b = db[(size_t) k];
                if (b < -35.0f || ! (b > db[(size_t) k - 1] && b >= db[(size_t) k + 1])) continue;
                if (b - floorDb[(size_t) k] < 25.0f) continue;
                if (b > bestLvl) { bestLvl = b; bestF = (float) fk; }
            }
            if (bestF > 0.0f && (oorFreq <= 0.0f || std::fabs (bestF - oorFreq) <= 0.04f * oorFreq))
            { oorFreq = bestF; oorCount = std::min (oorCount + 1, 1000); }
            else if (bestF > 0.0f) { oorFreq = bestF; oorCount = 1; }
            else oorCount = std::max (0, oorCount - 2);
            if (oorCount <= 0) oorFreq = 0.0f;
            uiOutOfRangeHz.store (oorCount >= (int) frameRate ? oorFreq : 0.0f, std::memory_order_relaxed);
        }

        // ---- Tracks fortschreiben -----------------------------------------
        const float matchRadius = (float) (1.5 * binHz);
        for (auto& t : tracks)
        {
            if (! t.active) continue;
            ++t.sinceAct;
            int best = -1; float bestD = matchRadius;
            for (int j = 0; j < numCand; ++j)
            {
                if (cand[(size_t) j].used) continue;
                const float d = std::fabs (cand[(size_t) j].freq - t.freq);
                if (d <= bestD) { bestD = d; best = j; }
            }
            if (best >= 0)
            {
                auto& c = cand[(size_t) best];
                c.used = true;
                t.freq = c.freq; t.prom = c.prom; t.missed = 0; ++t.count;
                t.lvl[(size_t) t.head] = c.level;
                t.frq[(size_t) t.head] = c.freq;
                t.bb[(size_t) t.head]  = broadbandDb (c.bin);
                t.head = (t.head + 1) % kHistory;
            }
            else if (++t.missed > 1) t.active = false;
        }

        for (int j = 0; j < numCand; ++j)
        {
            auto& c = cand[(size_t) j];
            if (c.used || c.prom < cfg.prominenceDb) continue;
            int slot = -1;
            for (int t = 0; t < kMaxTracks; ++t) if (! tracks[(size_t) t].active) { slot = t; break; }
            if (slot < 0)   // schwächsten Track verdrängen
            {
                slot = 0;
                for (int t = 1; t < kMaxTracks; ++t)
                    if (tracks[(size_t) t].prom < tracks[(size_t) slot].prom) slot = t;
                if (tracks[(size_t) slot].prom >= c.prom) continue;
            }
            Track nt;
            nt.active = true; nt.freq = c.freq; nt.prom = c.prom; nt.count = 1;
            nt.lvl[0] = c.level; nt.frq[0] = c.freq; nt.bb[0] = broadbandDb (c.bin); nt.head = 1;
            tracks[(size_t) slot] = nt;
        }

        // ---- Regeln auswerten ---------------------------------------------
        const double sc[3]       = { 1.5, 1.0, 0.7 };
        const float  growMin[3]  = { 4.0f, 3.0f, 2.0f };
        const float  runMin[3]   = { 15.0f, 12.0f, 9.0f };
        const int sp = std::clamp (cfg.speed, 0, 2);
        const int fastN = (int) std::ceil (5  * sc[sp]);
        const int growN = (int) std::ceil (12 * sc[sp]);
        const int sustN = (int) std::ceil (40 * sc[sp]);
        const int cooldown = (int) (0.3 * frameRate);

        bool severe = false;
        auto lowScale = [] (int n, float f)
        {
            const double k = f < 300.0f ? 2.0 : (f < 600.0f ? 1.5 : 1.0);
            return (int) std::ceil ((double) n * k);
        };

        float frameRisk = 0.0f;
        const bool refGate = cfg.useRef && refReady;

        for (auto& t : tracks)
        {
            if (! t.active || t.count < 4) continue;

            // PA-Referenz: Feedback kann nur entstehen, wenn der Ton auch aus der PA kommt.
            // Ein zufälliges Zusammentreffen mit einer Stimm-Oberschwingung in der PA hält nicht über viele Frames an.
            if (refGate)
            {
                const bool refOk = ! refPaSilent && refPromAt ((int) std::lround (t.freq / binHz)) >= 10.0f;
                t.refScore += 0.2f * ((refOk ? 1.0f : 0.0f) - t.refScore);
                if (t.refScore < 0.7f) continue;
            }

            const int m = std::min (t.count, 12);
            float slopeFrame = 0, jitterBins = 0, bbFrame = 0;
            lsSlope (t, m, slopeFrame, jitterBins, bbFrame);
            const float slopeAbs = slopeFrame * (float) frameRate;
            const float bbSlope  = bbFrame * (float) frameRate;
            // Feedback steht frequenzstabil (gemessen 0,01..0,2 Bins), Stimm-Teiltöne wandern (0,5..1,5 Bins).
            if (jitterBins > 0.5f) continue;

            // Feedback wächst ALLEIN. Wächst das restliche Spektrum mit (Silbenanfang,
            // Atemgeräusch, Lautstärkesprung), ist es Sprache und kein Feedback.
            const bool isolated = bbSlope <= std::max (6.0f, 0.4f * slopeAbs);
            const float slope = isolated ? (slopeAbs - std::max (0.0f, bbSlope)) : -999.0f;

            // Handeln nur ab hörbarem Pegel. Verfolgt wird weiter ab -75 dBFS, sonst würde ein Ton, der die Schwelle
            // überschreitet, wie ein künstlicher Einsatz aussehen. Praxisdaten: unter -62 dBFS nur Fehlalarme.
            const float curLvl = t.lvl[(size_t) ((t.head + kHistory - 1) % kHistory)];
            const bool loud = curLvl >= kActLevel;
            const bool canAct = loud && (! t.acted || t.sinceAct >= cooldown);
            bool harmChecked = false, harm = false;
            auto isHarm = [&]() { if (! harmChecked) { harm = isHarmonic (t.freq, t.prom); harmChecked = true; } return harm; };

            // Feedback-Nähe: wie weit ragt der auffälligste stabile, nicht-harmonische Ton über die Auslöseschwelle?
            // Je länger ein Ton schon steht, desto mehr zählt er (Sprache kommt und geht, ein klingelnder Raum bleibt).
            // Nur Töne, die in diesem Frame wirklich gefunden wurden und nicht abklingen (Silbenenden fallen mit ca. -300 dB/s).
            if (t.missed == 0 && slopeAbs > -10.0f && isolated && t.prom >= 0.5f * cfg.prominenceDb && ! isHarm())
            {
                const float evidence = std::min (1.0f, (float) (t.count - 3) / 25.0f);
                frameRisk = std::max (frameRisk, evidence * std::min (1.5f, t.prom / std::max (1.0f, cfg.prominenceDb)));
#ifdef FG_DEBUG
                if (evidence * t.prom / cfg.prominenceDb > 0.9f)
                    std::printf ("RISK f=%.1f prom=%.1f count=%d jitter=%.2f slopeAbs=%.1f bb=%.1f numPk=%d\n", t.freq, t.prom, t.count, jitterBins, slopeAbs, bbSlope, numPk);
#endif
            }

            // Notbremse: sehr schnell und deutlich -> Ducking, auch bevor Notch sitzt
            if (t.count >= lowScale (3, t.freq) && slope >= 20.0f && slope <= 150.0f && t.prom >= 20.0f && loud && ! isHarm())
                severe = true;

            if (! canAct) continue;

            if (t.count >= lowScale (fastN, t.freq) && slope >= runMin[sp] && slope <= 150.0f && t.prom >= cfg.prominenceDb && ! isHarm())
            { act (t, Rule::Runaway, slope); continue; }

            if (t.count >= lowScale (growN, t.freq) && slope >= growMin[sp] && jitterBins <= 1.0f && ! isHarm())
            { act (t, Rule::Growing, slope); continue; }

            if (! t.acted && t.count >= sustN && t.prom >= cfg.prominenceDb + 4.0f
                && jitterBins <= 0.6f && slope >= -1.0f && ! isHarm())
            { act (t, Rule::Sustained, slope); continue; }
        }

        if (severe) duckHold = (int) (0.5 * frameRate);
        else if (duckHold > 0) --duckHold;

        riskSmooth = std::max (frameRisk, riskSmooth * 0.96f);        // schnell hoch, ca. 1 s runter
        uiRisk.store (riskSmooth, std::memory_order_relaxed);
    }

    // Überhöhung der Referenz an dieser Stelle (dB über lokalem Boden), Maximum über +-1 Bin
    float refPromAt (int bin) const
    {
        float best = -200.0f;
        for (int k = std::max (1, bin - 1); k <= std::min (nb - 2, bin + 1); ++k)
        {
            double sum = 0; int cnt = 0;
            const int l0 = std::max (0, k - 25), l1 = k - 5;
            if (l1 >= l0) { sum += refPrefix[(size_t) l1 + 1] - refPrefix[(size_t) l0]; cnt += l1 - l0 + 1; }
            const int r0 = k + 5, r1 = std::min (nb - 1, k + 25);
            if (r1 >= r0) { sum += refPrefix[(size_t) r1 + 1] - refPrefix[(size_t) r0]; cnt += r1 - r0 + 1; }
            const float fl = cnt > 0 ? (float) (sum / cnt) : refDb[(size_t) k];
            best = std::max (best, refDb[(size_t) k] - fl);
        }
        return best;
    }

    void lsSlope (const Track& t, int m, float& slope, float& jitterBins, float& bbSlope) const
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, sb = 0, sxb = 0;
        float fmin = 1e9f, fmax = -1e9f;
        for (int i = 0; i < m; ++i)
        {
            const int idx = (t.head - m + i + 2 * kHistory) % kHistory;
            const double y = t.lvl[(size_t) idx];
            const double yb = t.bb[(size_t) idx];
            sx += i; sy += y; sxx += (double) i * i; sxy += i * y; sb += yb; sxb += i * yb;
            fmin = std::min (fmin, t.frq[(size_t) idx]);
            fmax = std::max (fmax, t.frq[(size_t) idx]);
        }
        const double den = m * sxx - sx * sx;
        slope   = den > 0 ? (float) ((m * sxy - sx * sy) / den) : 0.0f;
        bbSlope = den > 0 ? (float) ((m * sxb - sx * sb) / den) : 0.0f;
        jitterBins = (float) ((fmax - fmin) / binHz);
    }

    // Pegel des restlichen Spektrums (100 Hz .. 8 kHz) ohne die Umgebung (+-6 Bins) des Peaks
    float broadbandDb (int bin) const
    {
        // Umgebung: eine Oktave nach unten und oben, mindestens aber +-30..60 Bins. So wird ein Zischlaut bei 15 kHz
        // an seiner eigenen Umgebung gemessen und nicht an einem Band, in dem er gar nicht vorkommt.
        const int bbLo = std::max (2, std::min (bin / 2, bin - 30));
        const int bbHi = std::min (nb - 2, std::max (bin * 2, bin + 60));
        const double total = prefixP[(size_t) bbHi + 1] - prefixP[(size_t) bbLo];
        const int e0 = std::max (bin - 6, bbLo), e1 = std::min (bin + 6, bbHi);
        const double excl = e1 >= e0 ? prefixP[(size_t) e1 + 1] - prefixP[(size_t) e0] : 0.0;
        return (float) (10.0 * std::log10 (std::max (total - excl, 0.0) + 1.0e-20) + dbOffset);
    }

    // Gibt es im aktuellen Frame eine Obertonreihe, zu der dieser Peak gehört?
    // Partner zählen nur, wenn sie im Verhältnis zum geprüften Peak nennenswert sind;
    // so erzeugen zufällige Rauschspitzen neben einem starken Feedback-Ton keinen falschen Treffer.
    bool isHarmonic (float f, float prom) const
    {
        return partnerChain (f, prom) || fundamentalSeries (f);
    }

    // Schätzt aus den tiefen Peaks (< 1,5 kHz) einen Grundton f0 und prüft, ob f ein
    // ganzzahliges Vielfaches davon ist. Fängt auch sehr dichte Reihen (tiefe Stimmen) ab.
    bool fundamentalSeries (float f) const
    {
        const float self = (float) (1.5 * binHz);
        float low[32]; int nl = 0;
        for (int j = 0; j < numPk && nl < 32; ++j)
            if (pk[(size_t) j] < 1500.0f && std::fabs (pk[(size_t) j] - f) > self) low[nl++] = pk[(size_t) j];
        if (nl < 3) return false;

        // Toleranz wächst mit der Ordnung: gleitet die Stimme während des Analysefensters, wandert Oberton n n-mal so weit
        auto tolFor = [&] (int n, float d) { return (float) (0.7 * binHz) + 0.3f * (float) n + 0.005f * (float) n * d; };

        float bestD = 0; int bestM = 0;
        auto tryD = [&] (float d)
        {
            // Kandidat d dreimal per Least Squares nachziehen: die Peaks einer Stimme liegen wegen
            // Tonhöhenschwankung nie exakt auf einem Raster, ein grober Startwert verfehlt sonst die Reihe.
            int m = 0;
            for (int it = 0; it < 3; ++it)
            {
                if (d < 55.0f || d > 500.0f) return;
                m = 0; double sn = 0, snf = 0;
                for (int i = 0; i < nl; ++i)
                {
                    const int n = (int) std::lround (low[i] / d);
                    if (n >= 1 && std::fabs (low[i] - n * d) <= tolFor (n, d)) { ++m; sn += (double) n * n; snf += (double) n * low[i]; }
                }
                if (sn <= 0) return;
                if (it < 2) d = (float) (snf / sn);
            }
            if (m > bestM || (m == bestM && d > bestD)) { bestM = m; bestD = d; }
        };
        for (int i = 0; i < nl; ++i)
        {
            tryD (low[i]);
            if (i + 1 < nl) tryD (low[i + 1] - low[i]);
        }
        // Bei nur 3 tiefen Peaks müssen alle drei auf dem Raster liegen
        if (bestM < std::min (4, nl) || bestM < (int) std::ceil (0.4f * nl)) return false;

        const double d = bestD;
        const int n = (int) std::lround (f / d);
        if (n < 1) return false;
        // Unsicherheit der Vorhersage wächst mit der Ordnung n. Wird sie größer als ein Drittel des
        // Obertonabstands, lässt sich ein Einzelton nicht mehr von einem Oberton trennen -> als Stimme werten.
        const double tol = 0.7 * binHz + 0.6 * n + 0.005 * n * d;
        if (tol > 0.3 * d) return true;
        return std::fabs (f - n * d) <= tol;
    }

    bool partnerChain (float f, float prom) const
    {
        const float self = (float) (1.5 * binHz);
        const float partnerMin = std::max (8.0f, prom - 25.0f);

        for (int j = 0; j < numPk; ++j)
        {
            if (pkProm[(size_t) j] < partnerMin) continue;
            const float d = std::fabs (pk[(size_t) j] - f);
            if (d <= self || d < 50.0f || d > 500.0f) continue;

            int matches = 0;
            for (int l = 0; l < numPk; ++l)
            {
                if (pkProm[(size_t) l] < partnerMin) continue;
                const float dist = std::fabs (pk[(size_t) l] - f);
                if (dist <= self) continue;
                const int n = (int) std::lround (dist / d);
                if (n < 1 || n > 14) continue;
                const float tol = (float) (0.6 * binHz) + 0.004f * n * d;
                if (std::fabs (dist - n * d) <= tol) ++matches;
            }
            if (matches >= 3) return true;
        }
        return false;
    }

    // Wandernde Töne: liegt ein frisches Filter dicht (innerhalb ca. 18 %), wird es verbreitert statt ein neues zu setzen.
    bool mergeInto (Track& t, Rule rule, Event& ev)
    {
        const double f = t.freq;
        // Nur im Hochtonbereich bündeln: dort wandert Pfeifen. Darunter liegen Stimm-Grundtöne und Obertöne; breite Filter dort
        // würden die Stimme ausdünnen (Praxisdaten V5: Bündeln bei 250-1300 Hz war schädlich).
        if (f < kMergeMinHz) return false;
        const double stepDeeper[3] = { 6.0, 4.0, 4.0 };
        const double startDepth[3] = { 12.0, 9.0, 6.0 };
        const int r = (int) rule;
        int best = -1; double bestD = 1.0e30;
        for (int i = 0; i < kMaxNotches; ++i)
        {
            const auto& s = slots[(size_t) i];
            if (! s.active || s.releasing || s.fLo < kMergeMinHz * 0.85) continue;
            if ((double) (sampleClock - s.lastRefresh) > 20.0 * sr) continue;
            const double lo = std::min (s.fLo, f), hi = std::max (s.fHi, f), c = std::sqrt (lo * hi);
            if (hi - lo > 0.30 * c) continue;
            if (std::fabs (std::log (f / s.freq)) > 0.18) continue;
            const double d = std::fabs (f - s.freq);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) return false;

        auto& s = slots[(size_t) best];
        s.fLo = std::min (s.fLo, f); s.fHi = std::max (s.fHi, f);
        s.freq = std::sqrt (s.fLo * s.fHi);
        const double bw = (s.fHi - s.fLo) + s.freq / effectiveQ (s.freq);
        s.q = std::clamp (s.freq / bw, 3.0, effectiveQ (s.freq));
        const double cap = std::min ((double) cfg.maxDepthDb, 15.0);
        s.targetDb = -std::min (cap, std::max (-s.targetDb, startDepth[r]) + 0.5 * stepDeeper[r]);
        s.lastRefresh = sampleClock;
        s.releasing = false;
        s.hits = std::min (s.hits + 1, 1000);
        s.peakProm = std::max (s.peakProm, t.prom);
        if (cfg.ringOut) s.pinned = true;
        s.bq.setPeaking (s.freq, s.currentDb, s.q, sr);
        ev.merged = true; ev.bwLo = (float) s.fLo; ev.bwHi = (float) s.fHi;
        ev.pinned = s.pinned; ev.depthDb = (float) s.targetDb;
        return true;
    }

    void act (Track& t, Rule rule, float slope)
    {
#ifdef FG_DEBUG
        {
            float sf = 0, jb = 0, bf = 0; lsSlope (t, std::min (t.count, 12), sf, jb, bf);
            std::printf ("DEBUG act f=%.1f count=%d slopeRel=%.1f abs=%.1f bb=%.1f jitter=%.2f numPk=%d peaks:", t.freq, t.count, slope, sf * frameRate, bf * frameRate, jb, numPk);
            for (int i = 0; i < numPk; ++i) std::printf (" %.0f", pk[(size_t) i]);
            std::printf ("\n");
        }
#endif
        t.acted = true;
        t.sinceAct = 0;

        const double f = t.freq;
        const double tol = std::max (1.5 * binHz, 0.5 * f / effectiveQ (f));
        const double startDepth[3] = { 12.0, 9.0, 6.0 };
        const double stepDeeper[3] = { 6.0, 4.0, 4.0 };
        const int r = (int) rule;

        Event ev;
        ev.freq = (float) f; ev.prominenceDb = t.prom; ev.slopeDbPerS = slope;
        ev.rule = (uint8_t) rule; ev.monitorOnly = cfg.monitorOnly;

        detections.fetch_add (1, std::memory_order_relaxed);

        if (cfg.monitorOnly)
        {
            ev.depthDb = (float) -std::min (startDepth[r], (double) cfg.maxDepthDb);
            pushEvent (ev);
            return;
        }

        int found = -1;
        for (int i = 0; i < kMaxNotches; ++i)
            if (slots[(size_t) i].active && f >= slots[(size_t) i].fLo - tol && f <= slots[(size_t) i].fHi + tol) { found = i; break; }

        if (found >= 0)
        {
            auto& s = slots[(size_t) found];
            s.targetDb = -std::min ((double) cfg.maxDepthDb, -s.targetDb + stepDeeper[r]);
            s.lastRefresh = sampleClock;
            s.releasing = false;
            s.hits = std::min (s.hits + 1, 1000);
            s.peakProm = std::max (s.peakProm, t.prom);
            if (cfg.ringOut) s.pinned = true;
            ev.deepened = true;
            ev.pinned = s.pinned;
            ev.depthDb = (float) s.targetDb;
        }
        else if (cfg.mergeCluster && mergeInto (t, rule, ev))
        {
            // Filter wurde verbreitert, Ereignis ist gefüllt
        }
        else
        {
            int free = -1, used = 0;
            for (int i = 0; i < kMaxNotches; ++i)
            {
                if (! slots[(size_t) i].active) { if (free < 0) free = i; }
                else ++used;
            }
            if (used >= std::min (cfg.maxNotches, kMaxNotches) || free < 0)
            {
                // Den unwichtigsten verdrängen (nicht fixierte zuerst): wenig Treffer, geringe Überhöhung, lange her.
                // Ein oft bestätigter, starker Ton (echte Rückkopplung) bleibt so vor Fehlalarmen geschützt.
                auto importance = [&] (const Slot& sl)
                {
                    const double ageS = (double) (sampleClock - sl.lastRefresh) / sr;
                    return 20.0 * std::min (sl.hits, 5) + (double) sl.peakProm - 0.5 * ageS;
                };
                int old = -1;
                for (int i = 0; i < kMaxNotches; ++i)
                    if (slots[(size_t) i].active && ! slots[(size_t) i].pinned
                        && (old < 0 || importance (slots[(size_t) i]) < importance (slots[(size_t) old]))) old = i;
                if (old < 0)
                    for (int i = 0; i < kMaxNotches; ++i)
                        if (slots[(size_t) i].active && (old < 0 || importance (slots[(size_t) i]) < importance (slots[(size_t) old]))) old = i;
                if (old < 0) return;
                free = old;
            }
            auto& s = slots[(size_t) free];
            s = Slot{};
            s.active = true; s.freq = f; s.fLo = s.fHi = f; s.currentDb = 0.0;
            s.targetDb = -std::min (startDepth[r], (double) cfg.maxDepthDb);
            s.lastRefresh = sampleClock;
            s.hits = 1; s.peakProm = t.prom;
            s.pinned = cfg.ringOut;
            s.q = effectiveQ (f);
            s.bq.setPeaking (f, 0.0, s.q, sr);
            ev.pinned = s.pinned;
            ev.depthDb = (float) s.targetDb;
        }

        if (rule == Rule::Runaway) duckHold = (int) (0.5 * frameRate);
        pushEvent (ev);
    }

    void updateNotches()
    {
        const bool holdForever = cfg.holdSeconds >= 600.0f;
        const int64_t holdSamples = (int64_t) ((double) cfg.holdSeconds * sr);

        for (int i = 0; i < kMaxNotches; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.active)
            {
                uiNotchFreq[(size_t) i].store (0.0f, std::memory_order_relaxed);
                uiNotchPinned[(size_t) i].store (false, std::memory_order_relaxed);
                continue;
            }

            if (! holdForever && ! s.pinned && ! s.releasing && sampleClock - s.lastRefresh > holdSamples)
                s.releasing = true;

            const double target = s.releasing ? 0.0 : s.targetDb;
            double next = s.currentDb;
            if (target < next)      next = std::max (target, next - rampDownStep);
            else if (target > next) next = std::min (target, next + rampUpStep);

            if (next != s.currentDb)
            {
                s.currentDb = next;
                s.bq.setPeaking (s.freq, s.currentDb, s.q, sr);
            }

            if (s.releasing && s.currentDb > -0.05)
            {
                s = Slot{};
                uiNotchFreq[(size_t) i].store (0.0f, std::memory_order_relaxed);
                uiNotchPinned[(size_t) i].store (false, std::memory_order_relaxed);
                continue;
            }

            uiNotchFreq[(size_t) i].store ((float) s.freq, std::memory_order_relaxed);
            uiNotchDepth[(size_t) i].store ((float) s.currentDb, std::memory_order_relaxed);
            uiNotchPinned[(size_t) i].store (s.pinned, std::memory_order_relaxed);
        }
        uiDuckDb.store ((float) (20.0 * std::log10 (std::max (duckGain, 1.0e-4))), std::memory_order_relaxed);
    }

    // Konstante Breite von ca. 4 Hz im Bass: bei 20 Hz Q=5, ab 120 Hz volles Q. So trifft das Filter auch
    // dann, wenn die gemessene Frequenz einige Zehntel Hz danebenliegt.
    double effectiveQ (double f) const { return std::clamp (f / 4.0, 3.0, (double) cfg.notchQ); }

    void publishSpectrum()
    {
        for (int d = 0; d < kDisplayBins; ++d)
        {
            float m = -200.0f;
            for (int k = dispLo[(size_t) d]; k <= dispHi[(size_t) d]; ++k) m = std::max (m, db[(size_t) k]);
            uiSpectrum[(size_t) d].store (m, std::memory_order_relaxed);
        }
    }

    static constexpr uint32_t kEvents = 64;
    void pushEvent (const Event& e)
    {
        const uint32_t w = evWrite.load (std::memory_order_relaxed);
        if (w - evRead.load (std::memory_order_acquire) >= kEvents) return;   // voll: verwerfen
        events[w % kEvents] = e;
        evWrite.store (w + 1, std::memory_order_release);
    }

    // ---- Zustand ----------------------------------------------------------
    Settings cfg;
    double sr = 48000.0, binHz = 48000.0 / 4096, frameRate = 48000.0 / 512, dbOffset = 0;
    int N = 4096, hop = 512, nb = 2049;
    FFT fft;
    std::vector<float> window, ring, re, im, db, floorDb, refRing, reRef, imRef, refDb;
    std::vector<double> refPrefix;
    bool haveRef = false, refReady = false, refPaSilent = false;
    float riskSmooth = 0.0f;
    std::vector<double> prefix, prefixP;
    int writePos = 0, hopCount = 0, rampCount = 0;
    int64_t sampleClock = 0;

    std::array<Track, kMaxTracks> tracks {};
    std::array<Slot,  kMaxNotches> slots {};
    std::array<Cand, 16> cand {};
    std::array<float, 96> pk {}, pkProm {};
    int numCand = 0, numPk = 0;
    float oorFreq = 0.0f; int oorCount = 0;
    std::array<int, kDisplayBins> dispLo {}, dispHi {};

    double duckGain = 1.0, duckAttack = 0.01, duckRelease = 0.0001;
    int duckHold = 0;
    double rampDownStep = 0.5, rampUpStep = 0.0024;
    double dispFLo = 20.0, dispFHi = 22000.0;
    std::atomic<bool> pinRequest { false };

    std::array<Event, kEvents> events {};
    std::atomic<uint32_t> evWrite { 0 }, evRead { 0 };
};

} // namespace fg
