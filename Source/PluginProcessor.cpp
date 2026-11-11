#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout FeedbackGuardProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "active", 1 }, "Aktiv", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "monitor", 1 }, "Nur beobachten", false));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Betrieb",
                                                         StringArray { "Live", "Einmessen" }, 0));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Empfindlichkeit",
                                                        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
                                                        AudioParameterFloatAttributes().withLabel ("%")));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 }, "Reaktion",
                                                         StringArray { "Vorsichtig", "Normal", "Schnell" }, 1));

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "maxnotches", 1 }, "Max. Filter", 1, fg::kMaxNotches, 8));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "depth", 1 }, "Max. Absenkung",
                                                        NormalisableRange<float> (6.0f, 30.0f, 1.0f), 18.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "hold", 1 }, "Haltezeit",
                                                        NormalisableRange<float> (10.0f, 600.0f, 1.0f, 0.4f), 60.0f,
                                                        AudioParameterFloatAttributes().withLabel ("s")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "minf", 1 }, "Untere Grenze",
                                                        NormalisableRange<float> (20.0f, 2000.0f, 1.0f, 0.5f), 150.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "maxf", 1 }, "Obere Grenze",
                                                        NormalisableRange<float> (2000.0f, 22000.0f, 1.0f, 0.5f), 16000.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "duck", 1 }, "Notbremse", true));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "duckdb", 1 }, "Notbremse Tiefe",
                                                        NormalisableRange<float> (0.0f, 12.0f, 0.5f), 6.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "useref", 1 }, "PA-Referenz nutzen", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "merge", 1 }, "Wandernde Töne bündeln", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "diag", 1 }, "Diagnose aufzeichnen", false));

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        const String n (ch + 1);
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "ch" + n + "on", 1 }, "Kanal " + n + " aktiv", true));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "ch" + n + "off", 1 }, "Kanal " + n + " Empfindlichkeit +/-",
                                                            NormalisableRange<float> (-30.0f, 30.0f, 1.0f), 0.0f,
                                                            AudioParameterFloatAttributes().withLabel ("%")));
    }

    return { p.begin(), p.end() };
}

FeedbackGuardProcessor::FeedbackGuardProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("PA-Referenz", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "FeedbackGuard", createLayout())
{
    pActive     = apvts.getRawParameterValue ("active");
    pMode       = apvts.getRawParameterValue ("mode");
    pMonitor    = apvts.getRawParameterValue ("monitor");
    pSens       = apvts.getRawParameterValue ("sens");
    pSpeed      = apvts.getRawParameterValue ("speed");
    pMaxNotches = apvts.getRawParameterValue ("maxnotches");
    pDepth      = apvts.getRawParameterValue ("depth");
    pHold       = apvts.getRawParameterValue ("hold");
    pMinF       = apvts.getRawParameterValue ("minf");
    pMaxF       = apvts.getRawParameterValue ("maxf");
    pDuck       = apvts.getRawParameterValue ("duck");
    pDuckDb     = apvts.getRawParameterValue ("duckdb");
    pUseRef     = apvts.getRawParameterValue ("useref");
    pMerge      = apvts.getRawParameterValue ("merge");
    pDiag       = apvts.getRawParameterValue ("diag");

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        pChOn[(size_t) ch]  = apvts.getRawParameterValue ("ch" + juce::String (ch + 1) + "on");
        pChOff[(size_t) ch] = apvts.getRawParameterValue ("ch" + juce::String (ch + 1) + "off");
        channelNames[(size_t) ch] = "Kanal " + juce::String (ch + 1);
    }

    mixGain.fill (1.0f);
    startTimerHz (10);
}

FeedbackGuardProcessor::~FeedbackGuardProcessor()
{
    stopTimer();
    closeDiagnostics();
}

bool FeedbackGuardProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out || in.isDisabled()) return false;
    if (in.size() < 1 || in.size() > kMaxChannels) return false;

    if (layouts.inputBuses.size() > 1)       // optionale PA-Referenz: aus, mono oder stereo
    {
        const auto sc = layouts.inputBuses[1];
        if (! sc.isDisabled() && sc.size() > 2) return false;
    }
    return true;
}

void FeedbackGuardProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    for (auto& e : engines) e.prepare (sampleRate);     // fixierte Filter bleiben dabei erhalten
    mixInitialised = false;
    setLatencySamples (0);
}

//==============================================================================
fg::Settings FeedbackGuardProcessor::readSettings (int ch) const
{
    fg::Settings s;
    s.enabled      = true;                       // Ein-/Ausschalten übernimmt der Prozessor (mit Überblendung)
    s.monitorOnly  = pMonitor->load() > 0.5f;
    // Empfindlichkeit 0..100 % -> geforderte Überhöhung 22..6 dB (50 % = 14 dB); Kanal-Versatz +-30 Punkte
    const float sens = juce::jlimit (0.0f, 100.0f, pSens->load() + pChOff[(size_t) ch]->load());
    s.prominenceDb = 22.0f - 0.16f * sens;
    s.speed        = (int) pSpeed->load();
    s.maxNotches   = (int) pMaxNotches->load();
    s.maxDepthDb   = pDepth->load();
    s.holdSeconds  = pHold->load();
    s.minFreq      = pMinF->load();
    // obere Grenze nie über 0,46 x Samplerate (bei 44,1 kHz also ca. 20,3 kHz)
    s.maxFreq      = juce::jlimit (s.minFreq + 100.0f, (float) (0.46 * currentSampleRate), pMaxF->load());
    s.duckEnabled  = pDuck->load() > 0.5f;
    s.duckDb       = pDuckDb->load();
    s.useRef       = pUseRef->load() > 0.5f;
    s.mergeCluster = pMerge->load() > 0.5f;

    // Einmessen: neue Filter bleiben dauerhaft stehen, Erkennung etwas früher und empfindlicher
    if (pMode->load() > 0.5f)
    {
        s.ringOut      = true;
        s.speed        = 2;
        s.prominenceDb = juce::jmax (6.0f, s.prominenceDb - 2.0f);
    }
    return s;
}

void FeedbackGuardProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (getMainBusNumInputChannels(), buffer.getNumChannels(), kMaxChannels);
    const int n = buffer.getNumSamples();

    if (resetRequested.exchange (false))
        for (auto& e : engines) e.reset();

    // Fixierte Filter aus Projekt/Preset übernehmen (SpinLock nur per try, blockiert nie)
    {
        const juce::SpinLock::ScopedTryLockType l (pendingLock);
        if (l.isLocked() && pendingPresent)
        {
            for (int i = 0; i < numPending; ++i)
                engines[(size_t) pending[(size_t) i].channel].loadPinned (pending[(size_t) i].freq, pending[(size_t) i].depthDb);
            pendingPresent = false;
        }
    }

    // Optionale PA-Referenz (Sidechain-Bus)
    juce::AudioBuffer<float> sc;
    bool haveRef = false;
    if (pUseRef->load() > 0.5f)
        if (auto* bus = getBus (true, 1))
            if (bus->isEnabled() && bus->getNumberOfChannels() > 0)
            {
                sc = getBusBuffer (buffer, true, 1);
                haveRef = sc.getNumChannels() > 0 && sc.getNumSamples() >= n;
            }

    const bool globalOn = pActive->load() > 0.5f;
    std::array<bool, kMaxChannels> chOn {};
    for (int ch = 0; ch < numCh; ++ch)
    {
        engines[(size_t) ch].setSettings (readSettings (ch));
        chOn[(size_t) ch] = globalOn && pChOn[(size_t) ch]->load() > 0.5f;
    }

    const float step = 1.0f / (float) juce::jmax (1.0, 0.015 * currentSampleRate);    // 15 ms Überblendung

    for (int start = 0; start < n; start += kChunk)
    {
        const int len = juce::jmin (kChunk, n - start);

        const float* refPtr = nullptr;
        if (haveRef)
        {
            const float* l = sc.getReadPointer (0) + start;
            const float* r = sc.getNumChannels() > 1 ? sc.getReadPointer (1) + start : nullptr;
            for (int i = 0; i < len; ++i) refScratch[(size_t) i] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            refPtr = refScratch.data();
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& g = mixGain[(size_t) ch];
            const float target = chOn[(size_t) ch] ? 1.0f : 0.0f;
            if (! mixInitialised) g = target;

            float* data = buffer.getWritePointer (ch) + start;

            if (g <= 0.0f && target <= 0.0f) continue;                       // vollständig überbrückt
            if (g >= 1.0f && target >= 1.0f) { engines[(size_t) ch].process (data, len, refPtr); continue; }

            // Überblendung zwischen Original und bearbeitetem Signal
            auto& dry = dryScratch[(size_t) ch];
            std::copy (data, data + len, dry.begin());
            engines[(size_t) ch].process (data, len, refPtr);
            for (int i = 0; i < len; ++i)
            {
                g = target > g ? juce::jmin (target, g + step) : juce::jmax (target, g - step);
                data[i] = dry[(size_t) i] + g * (data[i] - dry[(size_t) i]);
            }
        }
    }
    mixInitialised = true;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, n);
}

juce::AudioProcessorEditor* FeedbackGuardProcessor::createEditor()
{
    return new FeedbackGuardEditor (*this);
}

//==============================================================================
juce::String FeedbackGuardProcessor::getChannelName (int ch) const
{
    return channelNames[(size_t) juce::jlimit (0, kMaxChannels - 1, ch)];
}

void FeedbackGuardProcessor::setChannelName (int ch, const juce::String& name)
{
    channelNames[(size_t) juce::jlimit (0, kMaxChannels - 1, ch)] = name.trim().isEmpty() ? "Kanal " + juce::String (ch + 1) : name.trim();
}

//==============================================================================
// Fixierte Filter: sammeln, in Warteschlange stellen, speichern
std::vector<PinnedFilter> FeedbackGuardProcessor::collectPinned() const
{
    std::vector<PinnedFilter> v;
    for (int ch = 0; ch < kMaxChannels; ++ch)
        for (int i = 0; i < fg::kMaxNotches; ++i)
        {
            const auto& e = engines[(size_t) ch];
            const float f = e.uiNotchFreq[(size_t) i].load();
            if (f > 0.0f && e.uiNotchPinned[(size_t) i].load())
                v.push_back ({ ch, f, e.uiNotchDepth[(size_t) i].load() });
        }
    return v;
}

void FeedbackGuardProcessor::queuePinned (const std::vector<PinnedFilter>& list, bool replace)
{
    const juce::SpinLock::ScopedLockType l (pendingLock);
    if (replace || ! pendingPresent) numPending = 0;
    for (const auto& f : list)
    {
        if (numPending >= (int) pending.size()) break;
        if (! (f.freq > 0.0f)) continue;
        pending[(size_t) numPending++] = { juce::jlimit (0, kMaxChannels - 1, f.channel), f.freq, f.depthDb };
    }
    pendingPresent = numPending > 0;
}

void FeedbackGuardProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (const char* tag : { "Pinned", "Names" })
        if (auto old = state.getChildWithName (tag); old.isValid())
            state.removeChild (old, nullptr);

    // Filter, die gerade aktiv und fixiert sind, plus solche, die noch auf Übernahme warten
    auto pinnedList = collectPinned();
    {
        const juce::SpinLock::ScopedLockType l (pendingLock);
        if (pendingPresent)
            for (int i = 0; i < numPending; ++i)
            {
                const auto& pf = pending[(size_t) i];
                bool dup = false;
                for (const auto& q : pinnedList)
                    if (q.channel == pf.channel && std::fabs (q.freq - pf.freq) < 0.5f) dup = true;
                if (! dup) pinnedList.push_back (pf);
            }
    }

    juce::ValueTree pinned ("Pinned");
    for (const auto& f : pinnedList)
    {
        juce::ValueTree n ("F");
        n.setProperty ("ch", f.channel, nullptr);
        n.setProperty ("f", (double) f.freq, nullptr);
        n.setProperty ("d", (double) f.depthDb, nullptr);
        pinned.appendChild (n, nullptr);
    }
    state.appendChild (pinned, nullptr);

    juce::ValueTree names ("Names");
    for (int ch = 0; ch < kMaxChannels; ++ch)
        names.setProperty ("n" + juce::String (ch), channelNames[(size_t) ch], nullptr);
    state.appendChild (names, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FeedbackGuardProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;

    auto tree = juce::ValueTree::fromXml (*xml);

    std::vector<PinnedFilter> list;
    if (auto pinned = tree.getChildWithName ("Pinned"); pinned.isValid())
        for (int i = 0; i < pinned.getNumChildren(); ++i)
        {
            const auto n = pinned.getChild (i);
            list.push_back ({ (int) n.getProperty ("ch", 0), (float) (double) n.getProperty ("f", 0.0), (float) (double) n.getProperty ("d", 0.0) });
        }
    queuePinned (list, true);

    if (auto names = tree.getChildWithName ("Names"); names.isValid())
        for (int ch = 0; ch < kMaxChannels; ++ch)
            setChannelName (ch, names.getProperty ("n" + juce::String (ch), "").toString());

    apvts.replaceState (tree);
}

//==============================================================================
// Raum-Presets und Export
bool FeedbackGuardProcessor::savePreset (const juce::File& file, const juce::String& name) const
{
    juce::XmlElement root ("FeedbackGuardV5Room");
    root.setAttribute ("name", name);
    root.setAttribute ("version", 1);
    root.setAttribute ("sampleRate", currentSampleRate);
    for (const auto& f : collectPinned())
    {
        auto* e = root.createNewChildElement ("F");
        e->setAttribute ("ch", f.channel);
        e->setAttribute ("f", (double) f.freq);
        e->setAttribute ("d", (double) f.depthDb);
    }
    return root.writeTo (file);
}

int FeedbackGuardProcessor::loadPreset (const juce::File& file)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! (xml->hasTagName ("FeedbackGuardV5Room") || xml->hasTagName ("FeedbackGuardV4Room") || xml->hasTagName ("FeedbackGuardV3Room") || xml->hasTagName ("FeedbackGuardV2Room"))) return -1;

    std::vector<PinnedFilter> list;
    for (auto* e : xml->getChildWithTagNameIterator ("F"))
        list.push_back ({ e->getIntAttribute ("ch", 0), (float) e->getDoubleAttribute ("f", 0.0), (float) e->getDoubleAttribute ("d", 0.0) });

    queuePinned (list, false);
    return (int) list.size();
}

static juce::String noteName (double f)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
    const int n = juce::roundToInt (midi);
    const int cents = juce::roundToInt ((midi - n) * 100.0);
    const int idx = ((n % 12) + 12) % 12;
    return juce::String (names[idx]) + juce::String (n / 12 - 1) + (cents >= 0 ? " +" : " ") + juce::String (cents) + " ct";
}

static double nearestGeqBand (double f)
{
    static const double bands[] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                                    1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };
    double best = bands[0], bestD = 1e9;
    for (double b : bands)
    {
        const double d = std::fabs (std::log (f / b));
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}

bool FeedbackGuardProcessor::exportProblemList (const juce::File& file) const
{
    juce::String t;
    t << "Feedback Guard V5 - Problemfrequenzen\n";
    t << "Erstellt: " << juce::Time::getCurrentTime().toString (true, true) << "\n\n";
    t << "Hinweis: Die Liste zeigt die Filter, die das Plugin gerade gesetzt hat. 'fix' = dauerhaft gehalten.\n";
    t << "Die Zuordnung zum 31-Band-GEQ (1/3-Oktav) ist eine Orientierung, z. B. fuer den GEQ auf den Haupt-Bussen der M32.\n";
    t << "Beim GEQ genuegt meist 3 bis 6 dB Absenkung; ein schmales Parametrisches PEQ-Band trifft genauer.\n\n";

    int total = 0;
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        const auto& e = engines[(size_t) ch];
        juce::String block;
        for (int i = 0; i < fg::kMaxNotches; ++i)
        {
            const float f = e.uiNotchFreq[(size_t) i].load();
            if (f <= 0.0f) continue;
            const float d = e.uiNotchDepth[(size_t) i].load();
            const bool pinned = e.uiNotchPinned[(size_t) i].load();
            const double band = nearestGeqBand (f);
            block << juce::String (f, 1).paddedLeft (' ', 9) << " Hz   " << juce::String (d, 1).paddedLeft (' ', 6) << " dB   "
                  << noteName (f).paddedRight (' ', 12) << "  GEQ-Band " << (band >= 1000.0 ? juce::String (band / 1000.0, band == 1250 || band == 2500 || band == 3150 || band == 6300 || band == 12500 ? 2 : 1) + " kHz"
                                                                                           : juce::String (band, band == 31.5 ? 1 : 0) + " Hz")
                  << (pinned ? "   fix" : "") << "\n";
            ++total;
        }
        if (block.isNotEmpty())
            t << "== " << channelNames[(size_t) ch] << " (Kanal " << (ch + 1) << ") ==\n" << block << "\n";
    }
    if (total == 0) t << "Zurzeit sind keine Filter gesetzt.\n";
    return file.replaceWithText (t);
}

//==============================================================================
// Protokoll (läuft auch bei geschlossenem Editor) und Diagnose
juce::var FeedbackGuardProcessor::notchesAsVar (int ch) const
{
    juce::Array<juce::var> arr;
    const auto& e = engines[(size_t) ch];
    for (int i = 0; i < fg::kMaxNotches; ++i)
    {
        const float f = e.uiNotchFreq[(size_t) i].load();
        if (f <= 0.0f) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty ("f", juce::roundToInt (f * 10.0f) / 10.0);
        o->setProperty ("d", juce::roundToInt (e.uiNotchDepth[(size_t) i].load() * 10.0f) / 10.0);
        o->setProperty ("pinned", e.uiNotchPinned[(size_t) i].load());
        arr.add (juce::var (o));
    }
    return arr;
}

juce::var FeedbackGuardProcessor::spectrumAsVar (int ch) const
{
    juce::Array<juce::var> arr;
    for (int d = 0; d < fg::kDisplayBins; ++d)
        arr.add (juce::roundToInt (engines[(size_t) ch].uiSpectrum[(size_t) d].load()));
    return arr;
}

void FeedbackGuardProcessor::writeDiagLine (const juce::var& v)
{
    if (diagStream == nullptr) return;
    diagStream->writeText (juce::JSON::toString (v, true) + "\n", false, false, nullptr);
    diagStream->flush();
}

void FeedbackGuardProcessor::openDiagnostics()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("FeedbackGuardV5");
    if (! dir.createDirectory().wasOk()) return;

    diagFile = dir.getChildFile ("diagnose-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".jsonl");
    auto stream = std::make_unique<juce::FileOutputStream> (diagFile);
    if (! stream->openedOk()) return;
    diagStream = std::move (stream);

    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "start");
    o->setProperty ("plugin", "Feedback Guard V5");
    o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("sampleRate", currentSampleRate);
    o->setProperty ("channels", getMainBusNumInputChannels());
    auto* params = new juce::DynamicObject();
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            params->setProperty (rp->paramID, rp->convertFrom0to1 (rp->getValue()));
    o->setProperty ("params", juce::var (params));
    writeDiagLine (juce::var (o));
    lastStatusMs = lastSpectrumMs = juce::Time::getMillisecondCounter();
}

void FeedbackGuardProcessor::closeDiagnostics()
{
    if (diagStream == nullptr) return;
    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "stop");
    o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
    writeDiagLine (juce::var (o));
    diagStream.reset();
}

void FeedbackGuardProcessor::timerCallback()
{
    const bool diagWanted = pDiag->load() > 0.5f;
    if (diagWanted && diagStream == nullptr) openDiagnostics();
    if (! diagWanted && diagStream != nullptr) closeDiagnostics();

    // Ereignisse aller Kanäle einsammeln
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        fg::Event ev;
        while (engines[(size_t) ch].popEvent (ev))
        {
            eventLog.push_back ({ juce::Time::getCurrentTime(), ch, ev });
            if (eventLog.size() > 400) { eventLog.pop_front(); ++logBase; }

            if (diagStream != nullptr)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("type", "event");
                o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
                o->setProperty ("channel", ch);
                o->setProperty ("freq", juce::roundToInt (ev.freq * 10.0f) / 10.0);
                o->setProperty ("prominenceDb", juce::roundToInt (ev.prominenceDb * 10.0f) / 10.0);
                o->setProperty ("slopeDbPerS", juce::roundToInt (ev.slopeDbPerS * 10.0f) / 10.0);
                o->setProperty ("rule", (int) ev.rule);
                o->setProperty ("depthDb", juce::roundToInt (ev.depthDb * 10.0f) / 10.0);
                o->setProperty ("deepened", ev.deepened);
                o->setProperty ("pinned", ev.pinned);
                o->setProperty ("monitorOnly", ev.monitorOnly);
                o->setProperty ("merged", ev.merged);
                if (ev.merged) { o->setProperty ("bwLo", juce::roundToInt (ev.bwLo)); o->setProperty ("bwHi", juce::roundToInt (ev.bwHi)); }
                o->setProperty ("spectrum", spectrumAsVar (ch));
                writeDiagLine (juce::var (o));
            }
        }
    }

    if (diagStream != nullptr)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const int numCh = juce::jmin (getMainBusNumInputChannels(), kMaxChannels);

        if (now - lastStatusMs >= 2000)
        {
            lastStatusMs = now;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto& e = engines[(size_t) ch];
                auto* o = new juce::DynamicObject();
                o->setProperty ("type", "status");
                o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
                o->setProperty ("channel", ch);
                o->setProperty ("levelDb", juce::roundToInt (e.uiLevelDb.load() * 10.0f) / 10.0);
                o->setProperty ("risk", juce::roundToInt (e.uiRisk.load() * 100.0f) / 100.0);
                o->setProperty ("duckDb", juce::roundToInt (e.uiDuckDb.load() * 10.0f) / 10.0);
                o->setProperty ("refState", e.uiRefState.load());
                o->setProperty ("outOfRangeHz", juce::roundToInt (e.uiOutOfRangeHz.load()));
                o->setProperty ("notches", notchesAsVar (ch));
                writeDiagLine (juce::var (o));
            }
        }

        if (now - lastSpectrumMs >= 10000)
        {
            lastSpectrumMs = now;
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("type", "spectrum");
                o->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
                o->setProperty ("channel", ch);
                o->setProperty ("spectrum", spectrumAsVar (ch));
                writeDiagLine (juce::var (o));
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FeedbackGuardProcessor();
}
