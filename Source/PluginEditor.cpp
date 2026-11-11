#include "PluginEditor.h"

namespace
{
const juce::Colour kBg      { 0xff101317 };
const juce::Colour kPanel   { 0xff171b21 };
const juce::Colour kAccent  { 0xff2fc4b2 };
const juce::Colour kAlarm   { 0xffff5a4f };
const juce::Colour kText    { 0xffdfe5ec };
const juce::Colour kDim     { 0xff7d8794 };
const juce::Colour kPin     { 0xffffb347 };
const juce::Colour kWarn    { 0xffe8c14b };

juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }
} // namespace

//==============================================================================
ChannelView::ChannelView (FeedbackGuardProcessor& p, int idx) : proc (p), eng (p.engines[(size_t) idx]), index (idx)
{
    smooth.fill (-120.0f);
    lastDetections = eng.detections.load();

    nameLabel.setText (proc.getChannelName (idx), juce::dontSendNotification);
    nameLabel.setEditable (false, true, false);                    // Doppelklick zum Umbenennen
    nameLabel.setColour (juce::Label::textColourId, kText);
    nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    nameLabel.onTextChange = [this]
    {
        proc.setChannelName (index, nameLabel.getText());
        nameLabel.setText (proc.getChannelName (index), juce::dontSendNotification);
    };
    addAndMakeVisible (nameLabel);

    const juce::String n (idx + 1);
    onBtn.setButtonText ("Aktiv");
    addAndMakeVisible (onBtn);
    onAtt = std::make_unique<ButtonAtt> (proc.apvts, "ch" + n + "on", onBtn);

    offSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    offSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 84, 20);
    offSlider.setTextValueSuffix (" % Empf.");
    addAndMakeVisible (offSlider);
    offAtt = std::make_unique<SliderAtt> (proc.apvts, "ch" + n + "off", offSlider);
}

void ChannelView::resized()
{
    const int w = getWidth();
    nameLabel.setBounds (8, 4, 116, 22);
    onBtn.setBounds (w - 362, 4, 66, 22);
    offSlider.setBounds (w - 292, 4, 284, 22);
}

float ChannelView::xOf (double freq) const
{
    const double fLo = eng.displayLo(), fHi = eng.displayHi();
    return (float) juce::jlimit (0.0, 1.0, std::log (freq / fLo) / std::log (fHi / fLo));
}

void ChannelView::tick()
{
    for (size_t d = 0; d < smooth.size(); ++d)
    {
        const float v = eng.uiSpectrum[d].load (std::memory_order_relaxed);
        smooth[d] = v > smooth[d] ? v : smooth[d] - 1.2f;      // schnell hoch, langsam runter
    }

    const auto det = eng.detections.load();
    if (det != lastDetections)
    {
        lastDetections = det;
        flashUntil = juce::Time::getMillisecondCounter() + 1500;
    }

    if (! nameLabel.isBeingEdited() && nameLabel.getText() != proc.getChannelName (index))
        nameLabel.setText (proc.getChannelName (index), juce::dontSendNotification);

    repaint();
}

void ChannelView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (b, 6.0f);

    const bool channelOn = proc.apvts.getRawParameterValue ("ch" + juce::String (index + 1) + "on")->load() > 0.5f;
    const auto plot = b.reduced (10.0f, 0.0f).withTrimmedTop (32.0f).withTrimmedBottom (16.0f);

    // Gitter
    g.setFont (10.0f);
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = plot.getX() + xOf (f) * plot.getWidth();
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        if (f == 50.0 || f == 100.0 || f == 500.0 || f == 1000.0 || f == 5000.0 || f == 10000.0)
        {
            g.setColour (kDim);
            g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + " kHz" : juce::String ((int) f) + " Hz",
                        (int) x - 30, (int) plot.getBottom() + 1, 60, 14, juce::Justification::centred);
        }
    }

    auto yOfDb = [&] (float db)
    {
        const float t = (juce::jlimit (-110.0f, -10.0f, db) + 110.0f) / 100.0f;
        return plot.getBottom() - t * plot.getHeight();
    };
    for (float db : { -90.0f, -70.0f, -50.0f, -30.0f })
    {
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine ((int) yOfDb (db), plot.getX(), plot.getRight());
    }

    // Spektrum
    juce::Path fill, line;
    fill.startNewSubPath (plot.getX(), plot.getBottom());
    for (size_t d = 0; d < smooth.size(); ++d)
    {
        const float x = plot.getX() + ((float) d + 0.5f) / (float) smooth.size() * plot.getWidth();
        const float y = yOfDb (smooth[d]);
        fill.lineTo (x, y);
        if (d == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
    }
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.closeSubPath();
    g.setColour ((channelOn ? kAccent : kDim).withAlpha (0.20f));
    g.fillPath (fill);
    g.setColour (channelOn ? kAccent : kDim);
    g.strokePath (line, juce::PathStrokeType (1.4f));

    // Gesetzte Filter
    g.setFont (11.0f);
    int shown = 0;
    for (int i = 0; i < fg::kMaxNotches; ++i)
    {
        const float f = eng.uiNotchFreq[(size_t) i].load (std::memory_order_relaxed);
        if (f <= 0.0f) continue;
        const float depth = eng.uiNotchDepth[(size_t) i].load (std::memory_order_relaxed);
        const float x = plot.getX() + xOf (f) * plot.getWidth();
        const bool pinned = eng.uiNotchPinned[(size_t) i].load (std::memory_order_relaxed);
        const auto col = pinned ? kPin : kAlarm;       // orange = fixiert, rot = automatisch
        const float alpha = juce::jlimit (0.1f, 0.45f, -depth / 30.0f);
        g.setColour (col.withAlpha (alpha));
        g.fillRect (x - 3.0f, plot.getY(), 6.0f, plot.getHeight());
        g.setColour (col);
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());

        const juce::String txt = juce::String ((int) std::round (f)) + " Hz  " + juce::String (depth, 0) + " dB" + (pinned ? "  fix" : "");
        const int ty = (int) plot.getY() + 2 + (shown++ % 4) * 12;
        const bool right = x < plot.getCentreX();
        g.drawText (txt, right ? (int) x + 5 : (int) x - 105, ty, 100, 12,
                    right ? juce::Justification::left : juce::Justification::right);
    }

    // Kopfzeile: LED + Status
    const bool alarm = channelOn && juce::Time::getMillisecondCounter() < flashUntil;
    const float duck = eng.uiDuckDb.load (std::memory_order_relaxed);
    const bool braking = channelOn && duck < -0.5f;
    g.setColour (alarm || braking ? kAlarm : juce::Colour (0xff2c4d3f));
    g.fillEllipse (130.0f, 9.0f, 11.0f, 11.0f);
    g.setColour (alarm || braking ? kAlarm : kDim);
    g.setFont (11.0f);
    const float oor = channelOn ? eng.uiOutOfRangeHz.load (std::memory_order_relaxed) : 0.0f;
    juce::String status = ! channelOn ? utf8 ("Kanal aus") : braking ? "Notbremse " + juce::String (duck, 1) + " dB" : alarm ? "Feedback erkannt" : "ruhig";
    if (oor > 0.0f && ! alarm && ! braking)
    {
        // starker Dauerton ausserhalb der Grenzen: deutlich warnen
        g.setColour (kWarn);
        g.fillEllipse (130.0f, 9.0f, 11.0f, 11.0f);
        g.setColour (kWarn);
        status = juce::String ((int) std::round (oor)) + " Hz ausserhalb!";
    }
    g.drawText (status, 146, 6, 124, 16, juce::Justification::left);

    // Feedback-Nähe: 0 = ruhig, 1 = an der Auslöseschwelle (gelb ab 0,5, rot ab 0,85)
    const float risk = channelOn ? juce::jlimit (0.0f, 1.0f, eng.uiRisk.load (std::memory_order_relaxed)) : 0.0f;
    g.setColour (kDim);
    g.drawText (utf8 ("Nähe"), 274, 6, 34, 16, juce::Justification::left);
    const juce::Rectangle<float> bar (310.0f, 10.0f, 100.0f, 9.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRoundedRectangle (bar, 3.0f);
    if (risk > 0.0f)
    {
        g.setColour (risk > 0.85f ? kAlarm : risk > 0.5f ? kWarn : kAccent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * risk), 3.0f);
    }

    const float lvl = eng.uiLevelDb.load (std::memory_order_relaxed);
    g.setColour (kDim);
    g.drawText (lvl <= -100.0f ? juce::String ("-inf dB") : juce::String (lvl, 1) + " dB", 420, 6, 84, 16, juce::Justification::left);
}

//==============================================================================
juce::File FeedbackGuardEditor::defaultDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("FeedbackGuardV5");
    d.createDirectory();
    return d;
}

FeedbackGuardEditor::FeedbackGuardEditor (FeedbackGuardProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    laf.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
    laf.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
    laf.setColour (juce::ToggleButton::tickColourId, kAccent);
    setLookAndFeel (&laf);

    numChannels = juce::jlimit (1, FeedbackGuardProcessor::kMaxChannels, p.getMainBusNumInputChannels());
    numShown    = juce::jmin (numChannels, 4);

    for (int i = 0; i < numShown; ++i)
    {
        views.push_back (std::make_unique<ChannelView> (proc, i));
        addAndMakeVisible (*views.back());
    }

    auto setupToggle = [this] (juce::ToggleButton& b, const juce::String& text)
    {
        b.setButtonText (text);
        addAndMakeVisible (b);
    };
    setupToggle (activeBtn, "Aktiv");
    setupToggle (monitorBtn, "Nur beobachten");
    setupToggle (duckBtn, "Notbremse");
    setupToggle (useRefBtn, "PA-Referenz");
    setupToggle (diagBtn, "Diagnose aufzeichnen");
    setupToggle (mergeBtn, utf8 ("Wandernde Töne bündeln (breiteres Filter)"));
    mergeBtn.setTooltip (utf8 ("Liegen mehrere neue Filter dicht beieinander, wird ein bestehendes verbreitert statt immer neue zu setzen. Hilft bei Pfeifen, das in der Frequenz wandert."));
    activeAtt  = std::make_unique<ButtonAtt> (proc.apvts, "active", activeBtn);
    monitorAtt = std::make_unique<ButtonAtt> (proc.apvts, "monitor", monitorBtn);
    duckAtt    = std::make_unique<ButtonAtt> (proc.apvts, "duck", duckBtn);
    useRefAtt  = std::make_unique<ButtonAtt> (proc.apvts, "useref", useRefBtn);
    diagAtt    = std::make_unique<ButtonAtt> (proc.apvts, "diag", diagBtn);
    mergeAtt   = std::make_unique<ButtonAtt> (proc.apvts, "merge", mergeBtn);

    resetBtn.setButtonText (utf8 ("Filter zurücksetzen"));
    resetBtn.onClick = [this]
    {
        proc.requestReset();
        appendLog (juce::Time::getCurrentTime().toString (false, true) + utf8 ("  Alle Filter zurückgesetzt\n"));
    };
    addAndMakeVisible (resetBtn);

    pinBtn.setButtonText ("Filter fixieren");
    pinBtn.onClick = [this]
    {
        proc.requestPinAll();
        appendLog (juce::Time::getCurrentTime().toString (false, true) + "  Aktuelle Filter fixiert (bleiben dauerhaft, werden im Projekt gespeichert)\n");
    };
    addAndMakeVisible (pinBtn);

    saveBtn.setButtonText ("Raum speichern...");
    saveBtn.onClick = [this] { savePresetDialog(); };
    addAndMakeVisible (saveBtn);
    loadBtn.setButtonText ("Raum laden...");
    loadBtn.onClick = [this] { loadPresetDialog(); };
    addAndMakeVisible (loadBtn);
    exportBtn.setButtonText ("Liste exportieren...");
    exportBtn.onClick = [this] { exportDialog(); };
    addAndMakeVisible (exportBtn);

    modeBox.addItemList ({ "Live", "Einmessen" }, 1);
    addAndMakeVisible (modeBox);
    modeAtt = std::make_unique<ComboAtt> (proc.apvts, "mode", modeBox);
    modeLabel.setText ("Betrieb", juce::dontSendNotification);
    modeLabel.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (modeLabel);

    addKnob (knobs[0], "sens",       "Empfindlichkeit");
    addKnob (knobs[1], "depth",      "Max. Absenkung");
    addKnob (knobs[2], "hold",       "Haltezeit");
    addKnob (knobs[3], "maxnotches", "Max. Filter");
    addKnob (knobs[4], "minf",       "Untere Grenze");
    addKnob (knobs[5], "maxf",       "Obere Grenze");
    addKnob (knobs[6], "duckdb",     "Notbremse");

    speedBox.addItemList ({ "Vorsichtig", "Normal", "Schnell" }, 1);
    addAndMakeVisible (speedBox);
    speedAtt = std::make_unique<ComboAtt> (proc.apvts, "speed", speedBox);
    speedLabel.setText ("Reaktion", juce::dontSendNotification);
    speedLabel.setJustificationType (juce::Justification::centred);
    speedLabel.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (speedLabel);

    statusLabel.setColour (juce::Label::textColourId, kDim);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    log.setMultiLine (true);
    log.setReadOnly (true);
    log.setScrollbarsShown (true);
    log.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    log.setColour (juce::TextEditor::backgroundColourId, kPanel);
    log.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (log);

    // Bisherige Ereignisse (auch aus der Zeit, in der der Editor geschlossen war) anzeigen
    const auto counter = proc.getLogCounter();
    shownLog = juce::jmax (proc.getLogBase(), counter > 30 ? counter - 30 : (uint64_t) 0);

    const int h = 118 + numShown * 150 + 8 + 2 * 126 + 8 + 96 + 18;
    setSize (900, h);
    startTimerHz (30);
}

FeedbackGuardEditor::~FeedbackGuardEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void FeedbackGuardEditor::addKnob (Knob& k, const juce::String& paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    addAndMakeVisible (k.slider);
    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (k.label);
    k.attachment = std::make_unique<SliderAtt> (proc.apvts, paramId, k.slider);
}

void FeedbackGuardEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("Feedback Guard V5", 16, 10, 300, 28, juce::Justification::left);
    g.setColour (kDim);
    g.setFont (11.0f);
    g.drawText (utf8 ("erkennt Rückkopplung und setzt schmale Filter, Sprache bleibt unberührt"), 16, 36, 380, 14, juce::Justification::left);
}

void FeedbackGuardEditor::resized()
{
    auto r = getLocalBounds().reduced (10, 0);

    auto top = r.removeFromTop (118);

    // Titelzeile rechts: Diagnose, Raum-Presets, Export
    auto head = top.removeFromTop (38).withTrimmedTop (10).withTrimmedBottom (4);
    exportBtn.setBounds (head.removeFromRight (130));
    head.removeFromRight (8);
    loadBtn.setBounds (head.removeFromRight (110));
    head.removeFromRight (8);
    saveBtn.setBounds (head.removeFromRight (120));
    head.removeFromRight (8);
    diagBtn.setBounds (head.removeFromRight (160));

    mergeBtn.setBounds (top.removeFromBottom (26).reduced (0, 2).removeFromLeft (330));
    auto bar = top.removeFromBottom (34).reduced (0, 3);
    activeBtn.setBounds (bar.removeFromLeft (70));
    monitorBtn.setBounds (bar.removeFromLeft (125));
    duckBtn.setBounds (bar.removeFromLeft (100));
    useRefBtn.setBounds (bar.removeFromLeft (115));
    bar.removeFromLeft (8);
    modeLabel.setBounds (bar.removeFromLeft (52));
    modeBox.setBounds (bar.removeFromLeft (100).reduced (0, 2));
    resetBtn.setBounds (bar.removeFromRight (160));
    bar.removeFromRight (8);
    pinBtn.setBounds (bar.removeFromRight (130));

    for (auto& v : views)
    {
        v->setBounds (r.removeFromTop (146));
        r.removeFromTop (4);
    }
    r.removeFromTop (4);

    // 2 Reihen mit je 4 Zellen (7 Regler + Auswahlfeld)
    const int cellW = r.getWidth() / 4;
    for (int row = 0; row < 2; ++row)
    {
        auto rowArea = r.removeFromTop (126);
        for (int col = 0; col < 4; ++col)
        {
            auto cell = rowArea.removeFromLeft (cellW).reduced (6, 0);
            const int idx = row * 4 + col;
            if (idx < 7)
            {
                knobs[(size_t) idx].label.setBounds (cell.removeFromTop (16));
                knobs[(size_t) idx].slider.setBounds (cell);
            }
            else
            {
                speedLabel.setBounds (cell.removeFromTop (16));
                speedBox.setBounds (cell.removeFromTop (46).reduced (10, 10));
                statusLabel.setBounds (cell);
            }
        }
    }

    r.removeFromTop (8);
    log.setBounds (r.removeFromTop (96));
}

//==============================================================================
void FeedbackGuardEditor::appendLog (const juce::String& line)
{
    log.moveCaretToEnd();
    log.insertTextAtCaret (line);
}

void FeedbackGuardEditor::showEntry (const LogEntry& le)
{
    static const char* rules[] = { "schnelles Aufschaukeln", "wachsender Ton", "stehender Ton" };
    const auto& e = le.event;
    juce::String line = le.time.toString (false, true) + "  " + proc.getChannelName (le.channel)
                      + "  " + juce::String ((int) std::round (e.freq)) + " Hz  (" + rules[juce::jlimit (0, 2, (int) e.rule)] + ", +"
                      + juce::String (e.prominenceDb, 0) + " dB)";
    if (e.monitorOnly)    line += "  -> nur beobachtet";
    else if (e.merged)    line += "  -> Filter verbreitert auf " + juce::String ((int) std::round (e.bwLo)) + "-" + juce::String ((int) std::round (e.bwHi)) + " Hz, " + juce::String (e.depthDb, 0) + " dB";
    else if (e.deepened)  line += "  -> Filter vertieft auf " + juce::String (e.depthDb, 0) + " dB";
    else                  line += "  -> Filter gesetzt " + juce::String (e.depthDb, 0) + " dB";
    if (e.pinned && ! e.monitorOnly) line += " (fixiert)";
    appendLog (line + "\n");
}

void FeedbackGuardEditor::savePresetDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Raum-Preset speichern", defaultDir().getChildFile ("Raum.fgroom"), "*.fgroom");
    juce::Component::SafePointer<FeedbackGuardEditor> safe (this);
    auto* p = &proc;
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, p] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              const auto file = f.withFileExtension ("fgroom");
                              const bool ok = p->savePreset (file, file.getFileNameWithoutExtension());
                              if (safe != nullptr)
                                  safe->appendLog (juce::Time::getCurrentTime().toString (false, true)
                                                   + (ok ? "  Raum gespeichert: " + file.getFullPathName() : juce::String ("  Raum konnte nicht gespeichert werden")) + "\n");
                          });
}

void FeedbackGuardEditor::loadPresetDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Raum-Preset laden", defaultDir(), "*.fgroom");
    juce::Component::SafePointer<FeedbackGuardEditor> safe (this);
    auto* p = &proc;
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe, p] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              const int n = p->loadPreset (f);
                              if (safe != nullptr)
                                  safe->appendLog (juce::Time::getCurrentTime().toString (false, true)
                                                   + (n >= 0 ? "  Raum geladen: " + juce::String (n) + " Filter aus " + f.getFileName()
                                                             : juce::String ("  Datei ist kein Raum-Preset")) + "\n");
                          });
}

void FeedbackGuardEditor::exportDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Problemfrequenzen exportieren", defaultDir().getChildFile ("Problemfrequenzen.txt"), "*.txt");
    juce::Component::SafePointer<FeedbackGuardEditor> safe (this);
    auto* p = &proc;
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, p] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              const bool ok = p->exportProblemList (f.withFileExtension ("txt"));
                              if (safe != nullptr)
                                  safe->appendLog (juce::Time::getCurrentTime().toString (false, true)
                                                   + (ok ? "  Liste exportiert: " + f.withFileExtension ("txt").getFullPathName() : juce::String ("  Export fehlgeschlagen")) + "\n");
                          });
}

void FeedbackGuardEditor::timerCallback()
{
    for (auto& v : views) v->tick();

    // neue Ereignisse aus dem Prozessor-Protokoll anzeigen
    if (shownLog < proc.getLogBase()) shownLog = proc.getLogBase();
    while (shownLog < proc.getLogCounter())
        showEntry (proc.getLogEntry (shownLog++));

    const bool ringOut = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;
    juce::String hint = ringOut ? utf8 ("Einmessen: neue Filter bleiben dauerhaft stehen.")
                                : utf8 ("Live: Filter werden nach der Haltezeit freigegeben.");

    if (proc.apvts.getRawParameterValue ("useref")->load() > 0.5f)
    {
        const int st = proc.engines[0].uiRefState.load();
        hint += st == 2 ? " PA-Referenz: aktiv." : st == 1 ? " PA-Referenz: PA still." : " PA-Referenz: kein Signal am Sidechain.";
    }
    if (proc.isDiagnosticsRunning())
        hint += " Diagnose: " + proc.getDiagnosticFile().getFileName();
    if (numChannels > numShown)
        hint += " " + juce::String (numChannels - numShown) + utf8 (" weitere Kan\xc3\xa4le werden mitverarbeitet.");

    if (statusLabel.getText() != hint)
        statusLabel.setText (hint, juce::dontSendNotification);
}
