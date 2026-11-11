// Testet die Engine mit synthetischen Signalen (ohne Plugin-Host).
// Bauen:  g++ -O2 -std=c++17 -I../Source test_engine.cpp -o test_engine && ./test_engine
#include "FeedbackEngine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

static constexpr double SR = 48000.0;

// Eigene Normalverteilung (Box-Muller): Der C++-Standard legt std::normal_distribution nicht bitgenau fest, libc++ (Mac)
// und libstdc++ (Linux) liefern sonst andere Zahlen. mt19937 selbst ist dagegen auf allen Plattformen identisch.
struct Gauss
{
    double operator() (std::mt19937& r)
    {
        if (have) { have = false; return spare; }
        const double u1 = ((double) r() + 1.0) / 4294967297.0, u2 = ((double) r() + 1.0) / 4294967297.0;
        const double m = std::sqrt (-2.0 * std::log (u1));
        spare = m * std::sin (2.0 * fg::kPi * u2); have = true;
        return m * std::cos (2.0 * fg::kPi * u2);
    }
    bool have = false; double spare = 0.0;
};

struct Speech
{
    std::mt19937 rng { 1234 };
    Gauss nd;
    double phase[40] = {};
    double t = 0;
    bool monotone;
    explicit Speech (bool mono) : monotone (mono) {}

    double next()
    {
        const double dt = 1.0 / SR;
        t += dt;
        const double syl = std::pow (std::max (0.0, std::sin (2 * fg::kPi * 3.3 * t)), 2.0);
        const double f0 = monotone ? 140.0 : 130.0 + 45.0 * std::sin (2 * fg::kPi * 0.37 * t) + 15.0 * std::sin (2 * fg::kPi * 1.9 * t);
        double s = 0;
        for (int h = 1; h <= 36; ++h)
        {
            const double f = f0 * h;
            if (f > 9000) break;
            phase[h] += 2 * fg::kPi * f * dt;
            // grobe Formanten bei 600 / 1700 / 2900 Hz
            const double form = 1.0 + 3.0 * std::exp (-std::pow ((f - 600) / 250, 2)) + 2.0 * std::exp (-std::pow ((f - 1700) / 300, 2))
                                    + 1.5 * std::exp (-std::pow ((f - 2900) / 400, 2));
            s += std::sin (phase[h]) * form / std::pow (h, 1.1);
        }
        const double noise = nd (rng) * 1.0e-4;                   // ~ -80 dBFS Rauschboden
        const double sib   = (std::sin (2 * fg::kPi * 0.8 * t) > 0.95) ? nd (rng) * 0.01 : 0.0; // Zischlaut
        return s * syl * 0.035 + noise + sib;
    }
};

static double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-9)); }

// ---------------------------------------------------------------------------
static float lastSpeechRisk = 0.0f;

static int testSpeech (bool monotone, float minF = 150.0f, float maxF = 10000.0f)
{
    fg::ChannelEngine e;
    e.prepare (SR);
    fg::Settings s;
    s.minFreq = minF; s.maxFreq = maxF;
    e.setSettings (s);
    Speech sp (monotone);

    int blocks = (int) (60 * SR / 128), events = 0;
    float maxRisk = 0.0f;
    std::vector<float> buf (128);
    for (int b = 0; b < blocks; ++b)
    {
        for (auto& x : buf) x = (float) sp.next();
        e.process (buf.data(), 128);
        if (b * 128 / SR > 2.0) maxRisk = std::max (maxRisk, e.uiRisk.load());
        fg::Event ev;
        while (e.popEvent (ev))
        {
            ++events;
            std::printf ("   Fehlalarm bei %.1f s: %.0f Hz, +%.1f dB, %.1f dB/s, Regel %d\n",
                         b * 128 / SR, ev.freq, ev.prominenceDb, ev.slopeDbPerS, ev.rule);
        }
    }
    std::printf ("[Sprache %s, 60 s, Suchbereich %.0f..%.0f Hz]  Notches gesetzt: %d   (max. Feedback-Naehe %.2f)   %s\n", monotone ? "monoton" : "variabel", minF, maxF, events, maxRisk, events == 0 ? "OK" : "PROBLEM");
    lastSpeechRisk = std::max (lastSpeechRisk, maxRisk);
    return events;
}

// ---------------------------------------------------------------------------
// Geschlossener Regelkreis: Mikro -> Engine -> (Verzögerung + Raum-Resonanz) -> Mikro
// Die Verzögerung beträgt 480 Samples (10 ms); fr ist ein Vielfaches von 100 Hz und fällt damit
// genau auf einen Kammfilter-Zahn, an dem die Schleifenverstärkung "loopGain" beträgt.
struct LoopResult { double firstRealS = -1; double maxToneDb = -200; double finalToneDb = -200; int real = 0, falseAlarms = 0; int notches = 0; double notchHz = 0; };

static double bandMaxDb (const std::vector<float>& x, double f0, double f1)
{
    const int N = 16384;      // feine Auflösung, damit auch 20-Hz-Töne sauber gemessen werden
    static fg::FFT fft; static bool init = false;
    if (! init) { fft.init (N); init = true; }
    std::vector<float> re (N), im (N, 0.0f);
    for (int i = 0; i < N; ++i)
        re[(size_t) i] = x[x.size() - N + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2 * fg::kPi * i / N));
    fft.forward (re.data(), im.data());
    double m = -200;
    for (int k = 1; k < N / 2; ++k)
    {
        const double f = k * SR / N;
        if (f < f0 || f > f1) continue;
        const double p = ((double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k]) * 16.0 / ((double) N * N);
        m = std::max (m, 10.0 * std::log10 (p + 1.0e-20));
    }
    return m;
}

struct LoopOpt { bool monitor = false; double T = 40.0; int delay = 480; double bw = 120.0; float minF = 150.0f, maxF = 10000.0f; bool ringOut = false; float hold = 60.0f; double loopEnd = 1.0e9; double roomQ = 15.0; bool useRef = false; };

static LoopResult testLoop (double loopGain, double fr, bool withSpeech, bool monoSpeech, const char* name, LoopOpt o = {})
{
    fg::ChannelEngine e;
    e.prepare (SR);
    fg::Settings s;
    s.monitorOnly = o.monitor; s.minFreq = o.minF; s.maxFreq = o.maxF; s.ringOut = o.ringOut; s.holdSeconds = o.hold; s.useRef = o.useRef;
    e.setSettings (s);
    const double T = o.T;
    // Nach dem Filtern kann die Rueckkopplung auf den Nachbarzahn des Kammfilters wandern (echtes Feedback, kein Fehlalarm)
    const double matchHz = fr < 400.0 ? 0.25 * fr : (fr < 10000.0 ? 100.0 : 0.1 * fr);

    const int delay = o.delay;
    const double g0 = 0.3;
    fg::Biquad room;
    room.setPeaking (fr, toDb (loopGain / g0), o.roomQ, SR);

    std::vector<float> dline ((size_t) delay, 0.0f);
    int dpos = 0;
    Speech sp (monoSpeech);
    std::mt19937 rng (7);
    Gauss nd;

    LoopResult r;
    const double loopStart = 2.0;
    const int total = (int) (T * SR / 128);
    std::vector<float> buf (128), refBuf (128, 0.0f);
    std::vector<float> outHist ((size_t) SR, 0.0f);     // letzte Sekunde Ausgang
    size_t oh = 0;

    for (int b = 0; b < total; ++b)
    {
        const double tNow = b * 128 / SR;
        for (int i = 0; i < 128; ++i)
        {
            double x = (withSpeech ? sp.next() : 0.0) + nd (rng) * 3.0e-4;       // Raumrauschen ~ -70 dBFS
            if (tNow >= loopStart && tNow < o.loopEnd) x += dline[(size_t) dpos];
            buf[(size_t) i] = (float) std::clamp (x, -1.0, 1.0);
            dpos = (dpos + 1) % delay;
        }
        e.process (buf.data(), 128, o.useRef ? refBuf.data() : nullptr);
        if (o.useRef) refBuf = buf;          // PA-Signal = Ausgang der Engine (einen Block verzoegert)
        const int start = (dpos - 128 + delay) % delay;
        for (int i = 0; i < 128; ++i)
            dline[(size_t) ((start + i) % delay)] = (float) room.process (buf[(size_t) i] * g0);

        for (float v : buf) { outHist[oh] = v; oh = (oh + 1) % outHist.size(); }

        if (b % 64 == 0 && tNow > loopStart + 0.5)
        {
            std::vector<float> lin (outHist.size());
            for (size_t i = 0; i < lin.size(); ++i) lin[i] = outHist[(oh + i) % outHist.size()];
            const double tone = bandMaxDb (lin, fr - o.bw, fr + o.bw);
            r.maxToneDb = std::max (r.maxToneDb, tone);
            if (tNow > T - 1.5) r.finalToneDb = std::max (r.finalToneDb, tone);
        }

        fg::Event ev;
        while (e.popEvent (ev))
        {
            const bool real = std::fabs (ev.freq - fr) < matchHz;
            if (real) { ++r.real; if (r.firstRealS < 0) r.firstRealS = tNow - loopStart; } else ++r.falseAlarms;
            std::printf ("   t=%5.2f s  %s %s %5.0f Hz  +%.1f dB  Anstieg %.1f dB/s  Regel %d  Tiefe %.0f dB\n",
                         tNow, real ? "ECHT  " : "FALSCH", ev.deepened ? "vertieft" : "neu     ", ev.freq, ev.prominenceDb, ev.slopeDbPerS, ev.rule, ev.depthDb);
        }
    }
    r.notches = e.getActiveNotchCount();
    for (auto& n : e.getNotchInfo()) if (std::fabs (n.freq - fr) < matchHz) r.notchHz = n.freq;
    std::printf ("[%s]\n   erste echte Erkennung nach %.2f s | Ton-Pegel max %.1f dBFS, am Ende %.1f dBFS | echte Events %d, Fehlalarme %d | Notch bei %.1f Hz\n",
                 name, r.firstRealS, r.maxToneDb, r.finalToneDb, r.real, r.falseAlarms, r.notchHz);
    return r;
}

// Fremdton nur im Mikro (nicht in der PA): mit Referenz keine Erkennung, ohne Referenz schon.
static int testForeign (bool useRef, bool paSilent)
{
    fg::ChannelEngine e; e.prepare (SR);
    fg::Settings s; s.useRef = useRef; e.setSettings (s);
    Speech sp (false), spRef (false);
    double ph = 0; int events = 0;
    std::vector<float> buf (128), ref (128);
    for (int b = 0; b < (int) (12 * SR / 128); ++b)
    {
        for (int i = 0; i < 128; ++i)
        {
            const double tt = (b * 128 + i) / SR;
            const double sig = sp.next();
            ref[(size_t) i] = paSilent ? 0.0f : (float) spRef.next();     // PA enthaelt die Sprache, aber nicht den Fremdton
            double x = sig;
            if (tt > 2.0) { ph += 2 * fg::kPi * 3000.0 / SR; x += std::min (0.05, 1.0e-4 * std::pow (10.0, (tt - 2.0) * 25.0 / 20.0)) * std::sin (ph); }
            buf[(size_t) i] = (float) x;
        }
        e.process (buf.data(), 128, useRef ? ref.data() : nullptr);
        fg::Event ev; while (e.popEvent (ev)) ++events;
    }
    return events;
}


// Stehender Ton (+ leises Rauschen) direkt in die Engine: Ergebnis = Anzahl Filter, gemeldete Frequenz ausserhalb
static void testTone (double f, double levelDb, float minF, float maxF, double seconds, int& notches, float& outOfRange)
{
    fg::ChannelEngine e; e.prepare (SR);
    fg::Settings s; s.minFreq = minF; s.maxFreq = maxF; e.setSettings (s);
    Gauss g; std::mt19937 rng (5); std::vector<float> blk (256);
    const double amp = std::pow (10.0, levelDb / 20.0);
    const double noise = std::pow (10.0, (levelDb - 50.0) / 20.0);
    double ph = 0;
    const int blocks = (int) (seconds * SR / 256);
    for (int b = 0; b < blocks; ++b)
    {
        for (auto& x : blk) { x = (float) (amp * std::sin (ph) + noise * g (rng)); ph += 2.0 * 3.14159265358979 * f / SR; }
        e.process (blk.data(), 256);
    }
    notches = e.getActiveNotchCount();
    outOfRange = e.uiOutOfRangeHz.load();
}

// Wandernder Pfeifton: springt alle 2,5 s auf eine neue Frequenz und schaukelt sich dort mit ca. 40 dB/s auf
static void testWander (bool merge, int& notches, double& worstAttenDb)
{
    fg::ChannelEngine e; e.prepare (SR);
    fg::Settings s; s.minFreq = 150.0f; s.maxFreq = 16000.0f; s.mergeCluster = merge; e.setSettings (s);
    std::mt19937 rng (11); Gauss g; std::vector<float> blk (256);
    const double hops[5] = { 10200.0, 10650.0, 11100.0, 11600.0, 12100.0 };
    double ph = 0;
    for (double hf : hops)
    {
        const int blocks = (int) (2.5 * SR / 256);
        for (int b = 0; b < blocks; ++b)
        {
            const double t = b * 256.0 / SR;
            const double lvl = std::min (-25.0, -72.0 + 40.0 * t);
            const double amp = std::pow (10.0, lvl / 20.0), noise = std::pow (10.0, -95.0 / 20.0);
            for (auto& x : blk) { x = (float) (amp * std::sin (ph) + noise * g (rng)); ph += 2.0 * 3.14159265358979 * hf / SR; }
            e.process (blk.data(), 256);
        }
    }
    notches = e.getActiveNotchCount();
    worstAttenDb = -1.0e9;
    for (double hf : hops) worstAttenDb = std::max (worstAttenDb, e.notchResponseDb (hf));   // 0 = nicht gedaempft, negativ = gedaempft
}

// Sprachnahe Töne (300-1300 Hz) dürfen nicht zu breiten Filtern gebündelt werden; Maximalzahl der Filter wird eingehalten
static void testNoLowMerge (int& maxSeen, double& lowWidthRatio)
{
    fg::ChannelEngine e; e.prepare (SR);
    fg::Settings s; s.minFreq = 150.0f; s.maxFreq = 16000.0f; s.mergeCluster = true; s.maxNotches = 3; e.setSettings (s);
    std::mt19937 rng (3); Gauss g; std::vector<float> blk (256);
    const double hops[8] = { 250.0, 280.0, 380.0, 420.0, 6000.0, 6300.0, 9000.0, 9500.0 };
    double ph = 0; maxSeen = 0; lowWidthRatio = 0;
    for (double hf : hops)
    {
        const int blocks = (int) (2.5 * SR / 256);
        for (int b = 0; b < blocks; ++b)
        {
            const double t = b * 256.0 / SR;
            const double lvl = std::min (-25.0, -72.0 + 40.0 * t);
            const double amp = std::pow (10.0, lvl / 20.0), noise = std::pow (10.0, -95.0 / 20.0);
            for (auto& x : blk) { x = (float) (amp * std::sin (ph) + noise * g (rng)); ph += 2.0 * 3.14159265358979 * hf / SR; }
            e.process (blk.data(), 256);
            maxSeen = std::max (maxSeen, e.getActiveNotchCount());
        }
    }
    // Falls bei 250..420 Hz ein Filter existiert: wie breit wirkt es bei 330 Hz (zwischen den Tönen)? Gebündelt wäre das stark gedämpft.
    lowWidthRatio = e.notchResponseDb (330.0);
}

int main()
{
    int fails = 0;

    std::printf ("=== 1) Sprache darf nichts auslösen ===\n");
    fails += testSpeech (false) > 0;
    fails += testSpeech (true)  > 0;
    fails += testSpeech (false, 20.0f, 22000.0f) > 0;     // voller Bereich
    fails += testSpeech (true,  20.0f, 22000.0f) > 0;

    std::printf ("\n=== 2) Feedback im geschlossenen Regelkreis (Verstaerkung 1.03 = ca. 25 dB/s, 1.004 = ca. 3 dB/s) ===\n");
    auto a = testLoop (1.03,  2900.0, true,  false, "schnell (~25 dB/s) + Sprache, 2900 Hz");
    auto b = testLoop (1.004, 1000.0, true,  false, "langsam (~3 dB/s) + Sprache, 1000 Hz");
    auto c = testLoop (1.03,  6100.0, false, false, "schnell ohne Sprache, 6100 Hz");
    auto d = testLoop (1.03,  2900.0, true,  false, "Monitor-Modus (nur erkennen), 2900 Hz", LoopOpt { true, 4.0 });
    auto m = testLoop (1.03,  4200.0, true,  true,  "schnell + monotone Sprache (Worst Case), 4200 Hz");
    auto e2 = testLoop (1.004, 1500.0, false, false, "langsam (~3 dB/s) ohne Sprache, 1500 Hz");
    auto f2 = testLoop (1.01,  1700.0, true,  false, "mittel (~8 dB/s) + Sprache, 1700 Hz");

    // Erweiterter Bereich 20 Hz .. 22 kHz
    LoopOpt wide; wide.minF = 20.0f; wide.maxF = 22000.0f;
    LoopOpt lo40 = wide;  lo40.delay = 1200; lo40.bw = 15.0; lo40.roomQ = 2.0;
    LoopOpt lo20 = wide;  lo20.delay = 2400; lo20.bw = 8.0; lo20.roomQ = 1.5;
    LoopOpt hi20 = wide;  hi20.bw = 150.0;
    auto l40 = testLoop (1.03, 40.0,    true,  false, "Bass-Feedback 40 Hz + Sprache", lo40);
    auto l20 = testLoop (1.03, 20.0,    false, false, "Subbass-Feedback 20 Hz", lo20);
    auto h20 = testLoop (1.03, 20000.0, true,  false, "Hoehen-Feedback 20 kHz + Sprache", hi20);
    auto h22 = testLoop (1.03, 22000.0, false, false, "Hoehen-Feedback 22 kHz", hi20);

    // Einmessen: Filter bleiben nach Ende des Feedbacks stehen, im Live-Betrieb werden sie nach der Haltezeit freigegeben
    LoopOpt pinOn;  pinOn.ringOut = true;  pinOn.hold = 10.0f; pinOn.loopEnd = 8.0; pinOn.T = 45.0;
    LoopOpt pinOff; pinOff.ringOut = false; pinOff.hold = 10.0f; pinOff.loopEnd = 8.0; pinOff.T = 45.0;
    auto pOn  = testLoop (1.03, 2900.0, true, false, "Einmessen: Filter muss nach 37 s noch stehen", pinOn);
    auto pOff = testLoop (1.03, 2900.0, true, false, "Live: Filter muss nach Haltezeit (10 s) wieder weg sein", pinOff);

    const int fOff = testForeign (false, false);
    const int fOn  = testForeign (true,  false);
    const int fSil = testForeign (true,  true);
    std::printf ("[Fremdton nur im Mikro] ohne Referenz: %d Erkennungen | mit PA-Referenz: %d | PA still: %d\n", fOff, fOn, fSil);
    LoopOpt withRef; withRef.useRef = true;
    auto r1 = testLoop (1.03, 2900.0, true, false, "schnell + Sprache mit PA-Referenz, 2900 Hz", withRef);
    auto r2 = testLoop (1.004, 1000.0, true, false, "langsam + Sprache mit PA-Referenz, 1000 Hz", withRef);

    auto ok = [&] (bool cond, const char* what) { std::printf ("  %s  %s\n", cond ? "OK     " : "PROBLEM", what); if (! cond) ++fails; };
    std::printf ("\n=== Bewertung (Ton-Pegel < -30 dBFS; Sprachspitzen liegen bei ca. -16 dBFS) ===\n");
    ok (a.real > 0 && a.maxToneDb < -30.0 && a.falseAlarms == 0, "schnell + Sprache: gefangen, keine Fehlalarme");
    ok (b.real > 0 && b.maxToneDb < -30.0 && b.falseAlarms == 0, "langsam + Sprache: gefangen, keine Fehlalarme");
    ok (c.real > 0 && c.maxToneDb < -30.0 && c.falseAlarms == 0, "ohne Sprache: gefangen");
    ok (d.real > 0 && d.notches == 0 && d.firstRealS >= 0, "Monitor-Modus meldet, filtert aber nicht");
    ok (m.real > 0 && m.maxToneDb < -30.0 && m.falseAlarms == 0, "monotone Sprache + Feedback: gefangen, keine Fehlalarme");
    ok (e2.real > 0 && e2.maxToneDb < -30.0 && e2.falseAlarms == 0, "langsames Aufschaukeln ohne Sprache gefangen");
    ok (f2.real > 0 && f2.maxToneDb < -30.0 && f2.falseAlarms == 0, "mittleres Aufschaukeln mit Sprache gefangen");
    // Haertefall: Suchbereich bis 20 Hz abgesenkt UND Stimme mit 25 ms Raumecho (zwei verschachtelte Obertonreihen).
    // Hier ist ein einzelner Fehlalarm auf einem Stimm-Grundton bekannt und dokumentiert (README, Abschnitt Grenzen).
    ok (l40.real > 0 && l40.maxToneDb < -30.0 && l40.falseAlarms <= 1 && std::fabs (l40.notchHz - 40.0) < 3.0, "40 Hz: gefangen, Filter sitzt auf +-3 Hz (max. 1 Fehlalarm im Haertefall)");
    ok (l20.real > 0 && l20.maxToneDb < -30.0 && l20.falseAlarms == 0 && std::fabs (l20.notchHz - 20.0) < 3.0, "20 Hz: gefangen, Filter sitzt auf +-3 Hz");
    ok (h20.real > 0 && h20.maxToneDb < -30.0 && h20.falseAlarms == 0, "20 kHz gefangen");
    ok (h22.real > 0 && h22.maxToneDb < -30.0 && h22.falseAlarms == 0, "22 kHz gefangen");
    ok (pOn.notches == 1 && pOff.notches == 0, "Einmessen haelt Filter dauerhaft, Live gibt sie frei");
    ok (fOff > 0 && fOn == 0 && fSil == 0, "PA-Referenz unterdrueckt Fremdtoene, die nicht aus der PA kommen");
    ok (r1.real > 0 && r1.maxToneDb < -30.0 && r1.falseAlarms == 0, "mit Referenz: schnelles Feedback weiter gefangen");
    ok (r2.real > 0 && r2.maxToneDb < -30.0 && r2.falseAlarms == 0, "mit Referenz: langsames Feedback weiter gefangen");
    ok (lastSpeechRisk < 0.85f, "Feedback-Naehe bleibt bei reiner Sprache unter 0,85");
    ok (std::fabs (a.notchHz - 2900.0) < 15.0 && std::fabs (c.notchHz - 6100.0) < 25.0, "Notch sitzt auf der Feedback-Frequenz");


    // Praxisdaten (MacBook-Mikro + Lautsprecher): leise Töne dürfen nichts auslösen, ein lauter Ton ausserhalb des Bereichs wird gemeldet
    int nQuiet = 0, nLoud = 0, nIn = 0; float oorQuiet = 0, oorLoud = 0, oorIn = 0;
    testTone (1000.0, -72.0, 150.0f, 16000.0f, 6.0, nQuiet, oorQuiet);
    testTone (10400.0, -12.0, 150.0f, 5000.0f, 4.0, nLoud, oorLoud);
    testTone (10400.0, -12.0, 150.0f, 16000.0f, 4.0, nIn, oorIn);
    std::printf ("[Praxis] leiser Dauerton -72 dBFS: %d Filter | lauter Ton ausserhalb (Bereich bis 5 kHz): %d Filter, Meldung %.0f Hz | im Bereich: %d Filter, Meldung %.0f Hz\n", nQuiet, nLoud, oorLoud, nIn, oorIn);
    ok (nQuiet == 0, "leiser Dauerton (-72 dBFS) loest keinen Filter aus");
    ok (nLoud == 0 && std::fabs (oorLoud - 10400.0f) < 80.0f, "lauter Ton ausserhalb des Suchbereichs wird gemeldet, nicht gefiltert");
    ok (nIn >= 1 && oorIn == 0.0f, "derselbe Ton im Suchbereich wird gefiltert, keine Warnung");


    // Wandernder Pfeifton (V5): mit Bündeln weniger Filter, trotzdem alle Sprungfrequenzen gedämpft
    int nwOn = 0, nwOff = 0; double awOn = 0, awOff = 0;
    testWander (true, nwOn, awOn);
    testWander (false, nwOff, awOff);
    std::printf ("[Wandernder Ton] ohne Buendeln: %d Filter, schwaechste Daempfung %.1f dB | mit Buendeln: %d Filter, schwaechste Daempfung %.1f dB\n", nwOff, awOff, nwOn, awOn);
    ok (nwOn < nwOff && nwOn <= 3, "Buendeln: deutlich weniger Filter fuer wandernden Ton");
    ok (awOn < -3.0, "Buendeln: jede Sprungfrequenz bleibt um mindestens 3 dB gedaempft");


    int nlMax = 0; double nlResp = 0;
    testNoLowMerge (nlMax, nlResp);
    std::printf ("[Kein Buendeln im Sprachbereich] max. gleichzeitige Filter %d (Grenze 3), Daempfung bei 330 Hz zwischen den Toenen: %.1f dB\n", nlMax, nlResp);
    ok (nlMax <= 3, "maximale Filterzahl wird auch mit Buendeln eingehalten");
    ok (nlResp > -3.0, "unter 4 kHz wird nicht gebuendelt (kein breites Filter zwischen 250 und 420 Hz)");

    // Fixierte Filter ueberleben prepare() (Samplerate-Wechsel) und lassen sich laden
    {
        fg::ChannelEngine e; e.prepare (SR);
        e.loadPinned (3141.0, -12.0);
        e.loadPinned (777.0, -9.0);
        e.prepare (44100.0);
        auto info = e.getNotchInfo();
        ok (info.size() == 2, "fixierte Filter ueberleben prepare()");
        e.reset();
        ok (e.getActiveNotchCount() == 0, "Zuruecksetzen entfernt auch fixierte Filter");
    }

    // Rechenaufwand
    {
        fg::ChannelEngine e; e.prepare (SR);
        fg::Settings s; e.setSettings (s);
        Speech sp (false);
        std::vector<float> buf (256);
        const int blocks = (int) (30 * SR / 256);
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) { for (auto& x : buf) x = (float) sp.next(); e.process (buf.data(), 256); }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("\nRechenaufwand: 30 s Audio in %.2f s  (%.1f %% einer CPU-Kern-Last pro Kanal, inkl. Signalerzeugung)\n", sec, 100.0 * sec / 30.0);
    }

    std::printf ("\n%s\n", fails == 0 ? "ALLE TESTS BESTANDEN" : "ES GIBT PROBLEME");
    return fails;
}
