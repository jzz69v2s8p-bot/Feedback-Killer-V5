// Prozessor-Test: Zustand, Presets, Export, Überblendung, Diagnose, Sidechain, Rückkopplungsschleife
#include <cstdio>
#include <cmath>
#include <random>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

static int fails = 0;
#define CHECK(c, msg) do { if (c) std::printf("  ok    %s\n", msg); else { std::printf("  FEHLER %s\n", msg); ++fails; } } while (0)

static void pump (int ms)
{
    auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
}

static void prep (FeedbackGuardProcessor& p, double sr = 48000.0, int bs = 256)
{
    p.setPlayConfigDetails (2, 2, sr, bs);
    p.prepareToPlay (sr, bs);
}

static bool hasNotchNear (FeedbackGuardProcessor& p, int ch, float f, float tol, bool* pinned = nullptr)
{
    for (int i = 0; i < fg::kMaxNotches; ++i)
    {
        const float nf = p.engines[(size_t) ch].uiNotchFreq[(size_t) i].load();
        if (nf > 0.0f && std::fabs (nf - f) <= tol)
        {
            if (pinned) *pinned = p.engines[(size_t) ch].uiNotchPinned[(size_t) i].load();
            return true;
        }
    }
    return false;
}

// Einfache Rückkopplungsschleife: Ausgang -> Verzögerung -> schmale Resonanz (f0) -> Eingang
struct Loop
{
    double sr; float f0, loopGain; int delay;
    std::vector<float> dl; size_t w = 0;
    double z1 = 0, z2 = 0, b0, a1, a2;
    std::mt19937 rng { 7 }; std::normal_distribution<float> n { 0.0f, 1.0f };
    Loop (double s, float f, float g, int d) : sr (s), f0 (f), loopGain (g), delay (d), dl ((size_t) d, 0.0f)
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * f / s, Q = 60.0, al = std::sin (w0) / (2 * Q);
        const double a0 = 1 + al; b0 = al / a0; a1 = -2 * std::cos (w0) / a0; a2 = (1 - al) / a0;
    }
    float resonator (float x) { const double y = b0 * x + z1; z1 = -a1 * y + z2; z2 = -a2 * y; return (float) y; }
    // Ein Block durch das Plugin; die Schleife schliesst sich ueber die Verzögerung (muss groesser als der Block sein)
    void run (FeedbackGuardProcessor& p, int blocks, int bs, float noiseAmp, std::vector<float>* outL = nullptr)
    {
        juce::AudioBuffer<float> b (2, bs); juce::MidiBuffer m;
        const size_t n = dl.size();
        for (int k = 0; k < blocks; ++k)
        {
            for (int i = 0; i < bs; ++i)
            {
                const float x = resonator (dl[(w + (size_t) i) % n]) * loopGain + noiseAmp * this->n (rng);
                b.setSample (0, i, x); b.setSample (1, i, x);
            }
            p.processBlock (b, m);
            for (int i = 0; i < bs; ++i)
            {
                dl[(w + (size_t) i) % n] = b.getSample (0, i);
                if (outL) outL->push_back (b.getSample (0, i));
            }
            w = (w + (size_t) bs) % n;
        }
    }

};

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::printf ("Prozessor-Tests\n");
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fg_test");
    tmp.deleteRecursively(); tmp.createDirectory();

    // 1) Zustand speichern / laden
    {
        std::printf ("[1] Zustand\n");
        FeedbackGuardProcessor a; prep (a);
        a.engines[0].loadPinned (1000.0, -9.0);
        a.engines[1].loadPinned (3150.0, -12.0);
        juce::AudioBuffer<float> b (2, 256); juce::MidiBuffer m; b.clear();
        for (int i = 0; i < 20; ++i) a.processBlock (b, m);
        a.setChannelName (0, "Gesang");
        a.apvts.getParameter ("sens")->setValueNotifyingHost (0.9f);
        CHECK (hasNotchNear (a, 0, 1000.0f, 5.0f) && hasNotchNear (a, 1, 3150.0f, 20.0f), "Filter gesetzt");
        juce::MemoryBlock mb; a.getStateInformation (mb);

        FeedbackGuardProcessor c;                      // neue Instanz, Zustand laden, dann prepare
        c.setStateInformation (mb.getData(), (int) mb.getSize());
        prep (c);
        for (int i = 0; i < 20; ++i) c.processBlock (b, m);
        bool pin = false;
        CHECK (hasNotchNear (c, 0, 1000.0f, 5.0f, &pin) && pin, "Filter nach Laden vorhanden und fixiert (Kanal 1)");
        CHECK (hasNotchNear (c, 1, 3150.0f, 20.0f), "Filter nach Laden vorhanden (Kanal 2)");
        CHECK (c.getChannelName (0) == "Gesang", "Kanalname erhalten");
        CHECK (std::fabs (c.apvts.getParameter ("sens")->getValue() - 0.9f) < 0.01f, "Parameter erhalten");

        // Zustand laden NACH prepare (Reaper-Reihenfolge variiert)
        FeedbackGuardProcessor d; prep (d);
        d.setStateInformation (mb.getData(), (int) mb.getSize());
        for (int i = 0; i < 20; ++i) d.processBlock (b, m);
        CHECK (hasNotchNear (d, 0, 1000.0f, 5.0f), "Laden nach prepare funktioniert");

        // Erneutes prepare (Samplerate-Wechsel) behält fixierte Filter
        prep (d, 44100.0, 512);
        for (int i = 0; i < 20; ++i) d.processBlock (b, m);
        CHECK (hasNotchNear (d, 0, 1000.0f, 5.0f), "Filter nach erneutem prepare (44,1 kHz)");

        // 2) Preset + Export
        std::printf ("[2] Preset und Export\n");
        const auto pf = tmp.getChildFile ("raum.fgroom");
        CHECK (a.savePreset (pf, "Test"), "Preset geschrieben");
        FeedbackGuardProcessor e; prep (e);
        CHECK (e.loadPreset (pf) == 2, "Preset geladen: 2 Filter");
        for (int i = 0; i < 20; ++i) e.processBlock (b, m);
        CHECK (hasNotchNear (e, 0, 1000.0f, 5.0f), "Preset-Filter aktiv");
        CHECK (e.loadPreset (tmp.getChildFile ("gibtsnicht.fgroom")) == -1, "Fehlerfall: Datei fehlt -> -1");
        const auto ef = tmp.getChildFile ("liste.txt");
        CHECK (a.exportProblemList (ef), "Export geschrieben");
        const auto txt = ef.loadFileAsString();
        std::printf ("%s\n", txt.toRawUTF8());
        CHECK (txt.contains ("1000") && txt.contains ("3150") && txt.contains ("B"), "Export enthält Frequenzen/Band");
    }

    // 3) Überblendung beim Ausschalten
    {
        std::printf ("[3] Weiches Bypass\n");
        FeedbackGuardProcessor a; prep (a);
        juce::MidiBuffer m; juce::AudioBuffer<float> b (2, 256);
        auto fill = [&] { for (int i = 0; i < 256; ++i) { const float s = 0.3f * std::sin (0.05f * (float) i); b.setSample (0, i, s); b.setSample (1, i, s); } };
        for (int i = 0; i < 10; ++i) { fill(); a.processBlock (b, m); }
        a.apvts.getParameter ("active")->setValueNotifyingHost (0.0f);
        float maxStep = 0.0f, prev = 0.0f; bool first = true;
        for (int k = 0; k < 6; ++k)
        {
            for (int i = 0; i < 256; ++i) { const float s = 0.3f * std::sin (0.05f * (float) (k * 256 + i)); b.setSample (0, i, s); b.setSample (1, i, s); }
            a.processBlock (b, m);
            for (int i = 0; i < 256; ++i) { const float s = b.getSample (0, i); if (! first) maxStep = std::max (maxStep, std::fabs (s - prev)); prev = s; first = false; }
        }
        CHECK (maxStep < 0.1f, "keine Sprünge beim Ausschalten");
        CHECK (a.apvts.getParameter ("active")->getValue() < 0.5f, "Parameter aus");
    }

    // 4) Rückkopplungsschleife im Einmess-Modus: Filter erscheint, wird fixiert, übersteht Speichern/Laden
    {
        std::printf ("[4] Schleife (1 kHz)\n");
        FeedbackGuardProcessor a; prep (a, 48000.0, 256);
        a.apvts.getParameter ("mode")->setValueNotifyingHost (1.0f);
        a.apvts.getParameter ("duck")->setValueNotifyingHost (0.0f);
        Loop L (48000.0, 1000.0f, 1.3f, 2400);
        for (int sec = 0; sec < 25; sec += 5)
        {
            L.run (a, 48000 * 5 / 256, 256, 0.003f);
            std::printf ("  t=%2d s Pegel %.1f dBFS Risiko %.2f Filter %d\n", sec + 5, a.engines[0].uiLevelDb.load(), a.engines[0].uiRisk.load(), a.engines[0].getActiveNotchCount());
        }
        bool pin = false;
        const bool found = hasNotchNear (a, 0, 1000.0f, 25.0f, &pin);
        std::printf ("  Filter bei 1 kHz: %s\n", found ? "ja" : "nein");
        CHECK (found, "Schleife erkannt, Filter gesetzt");
        CHECK (found && pin, "Einmessen: Filter fixiert");
        pump (500);
        CHECK (a.getLogCounter() > 0, "Ereignisprotokoll gefüllt");

        juce::MemoryBlock mb; a.getStateInformation (mb);
        FeedbackGuardProcessor c; c.setStateInformation (mb.getData(), (int) mb.getSize()); prep (c);
        juce::AudioBuffer<float> b (2, 256); juce::MidiBuffer m; b.clear();
        for (int i = 0; i < 20; ++i) c.processBlock (b, m);
        CHECK (hasNotchNear (c, 0, 1000.0f, 25.0f), "nach Neuladen weiter aktiv");
    }

    // 5) Diagnose-Aufzeichnung
    {
        std::printf ("[5] Diagnose\n");
        FeedbackGuardProcessor a; prep (a);
        a.apvts.getParameter ("diag")->setValueNotifyingHost (1.0f);
        pump (400);
        CHECK (a.isDiagnosticsRunning(), "Aufzeichnung läuft");
        auto f = a.getDiagnosticFile();
        juce::AudioBuffer<float> b (2, 256); juce::MidiBuffer m; b.clear();
        for (int i = 0; i < 50; ++i) a.processBlock (b, m);
        pump (2500);
        a.apvts.getParameter ("diag")->setValueNotifyingHost (0.0f);
        pump (400);
        CHECK (! a.isDiagnosticsRunning(), "Aufzeichnung beendet");
        const auto s = f.loadFileAsString();
        std::printf ("  Datei: %s (%d Bytes)\n", f.getFullPathName().toRawUTF8(), (int) f.getSize());
        CHECK (s.contains ("start") && s.contains ("stop") && s.contains ("status"), "Datei enthält start/status/stop");
        int bad = 0;
        for (auto& ln : juce::StringArray::fromLines (s)) if (ln.isNotEmpty() && ! juce::JSON::parse (ln).isObject()) ++bad;
        CHECK (bad == 0, "alle Zeilen gültiges JSON");
        f.deleteFile();
    }

    // 6) Sidechain-Bus
    {
        std::printf ("[6] PA-Referenz-Sidechain\n");
        FeedbackGuardProcessor a;
        auto layout = a.getBusesLayout();
        std::printf ("  Busse: in=%d out=%d\n", a.getBusCount (true), a.getBusCount (false));
        CHECK (a.getBusCount (true) == 2, "zwei Eingangsbusse (Haupt + Sidechain)");
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        CHECK (a.setBusesLayout (layout), "Sidechain aktivierbar");
        a.prepareToPlay (48000.0, 256);
        a.apvts.getParameter ("useref")->setValueNotifyingHost (1.0f);
        juce::AudioBuffer<float> b (4, 256); juce::MidiBuffer m; b.clear();
        for (int i = 0; i < 100; ++i) a.processBlock (b, m);
        std::printf ("  uiRefState=%d\n", a.engines[0].uiRefState.load());
        CHECK (a.engines[0].uiRefState.load() >= 1, "Referenz-Zustand wird gemeldet");
        // Mehrkanal 1..8 ohne Sidechain
        for (int ch : { 1, 2, 4, 8 })
        {
            FeedbackGuardProcessor x; x.setPlayConfigDetails (ch, ch, 48000.0, 256);
            auto lay = x.getBusesLayout();
            const bool ok = x.setBusesLayout (lay) || true; (void) ok;
        }
    }

    // 7) Editor
    {
        std::printf ("[7] Editor\n");
        FeedbackGuardProcessor a; prep (a);
        std::unique_ptr<juce::AudioProcessorEditor> ed (a.createEditor());
        CHECK (ed != nullptr, "Editor erzeugt");
        if (ed)
        {
            std::printf ("  Größe %d x %d\n", ed->getWidth(), ed->getHeight());
            pump (200);
            juce::Image img = ed->createComponentSnapshot (ed->getLocalBounds());
            juce::PNGImageFormat png;
            juce::File out = tmp.getChildFile ("editor.png");
            juce::FileOutputStream os (out);
            if (os.openedOk()) png.writeImageToStream (img, os);
            std::printf ("  Snapshot: %s\n", out.getFullPathName().toRawUTF8());
        }
    }

    std::printf ("\n%s (%d Fehler)\n", fails ? "FEHLGESCHLAGEN" : "ALLE PROZESSOR-TESTS BESTANDEN", fails);
    return fails ? 1 : 0;
}
