#pragma once

#include <array>
#include <deque>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "FeedbackEngine.h"

struct LogEntry
{
    juce::Time time;
    int channel = 0;
    fg::Event event;
};

struct PinnedFilter
{
    int   channel = 0;
    float freq = 0.0f;
    float depthDb = 0.0f;
};

class FeedbackGuardProcessor : public juce::AudioProcessor,
                               private juce::Timer
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kChunk = 1024;

    FeedbackGuardProcessor();
    ~FeedbackGuardProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- von der Oberfläche aufgerufen (Message-Thread) --------------------
    void requestReset() { resetRequested.store (true); }
    void requestPinAll() { for (auto& e : engines) e.pinAll(); }

    juce::String getChannelName (int ch) const;
    void setChannelName (int ch, const juce::String& name);

    // Ereignisprotokoll (läuft auch bei geschlossenem Editor weiter)
    uint64_t getLogBase() const    { return logBase; }
    uint64_t getLogCounter() const { return logBase + (uint64_t) eventLog.size(); }
    const LogEntry& getLogEntry (uint64_t absoluteIndex) const { return eventLog[(size_t) (absoluteIndex - logBase)]; }

    // Raum-Presets und Export
    std::vector<PinnedFilter> collectPinned() const;
    bool savePreset (const juce::File& file, const juce::String& name) const;
    int  loadPreset (const juce::File& file);                    // Anzahl geladener Filter, -1 bei Fehler
    bool exportProblemList (const juce::File& file) const;

    // Diagnose-Aufzeichnung
    juce::File getDiagnosticFile() const { return diagFile; }
    bool isDiagnosticsRunning() const    { return diagStream != nullptr; }

    juce::AudioProcessorValueTreeState apvts;
    std::array<fg::ChannelEngine, kMaxChannels> engines;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    fg::Settings readSettings (int ch) const;
    void queuePinned (const std::vector<PinnedFilter>& list, bool replace);
    void timerCallback() override;
    void openDiagnostics();
    void closeDiagnostics();
    void writeDiagLine (const juce::var& v);
    juce::var notchesAsVar (int ch) const;
    juce::var spectrumAsVar (int ch) const;

    // Fixierte Filter aus einem geladenen Projekt/Preset; der Audio-Thread übernimmt sie im nächsten Block
    juce::SpinLock pendingLock;
    std::array<PinnedFilter, kMaxChannels * fg::kMaxNotches> pending {};
    int numPending = 0;
    bool pendingPresent = false;

    std::atomic<bool> resetRequested { false };
    double currentSampleRate = 48000.0;

    // Überblendung beim Ein-/Ausschalten (pro Kanal) und Arbeitspuffer
    std::array<float, kMaxChannels> mixGain {};
    bool mixInitialised = false;
    std::array<std::array<float, kChunk>, kMaxChannels> dryScratch {};
    std::array<float, kChunk> refScratch {};

    // Protokoll und Diagnose (nur Message-Thread)
    std::deque<LogEntry> eventLog;
    uint64_t logBase = 0;
    std::array<juce::String, kMaxChannels> channelNames;
    std::unique_ptr<juce::FileOutputStream> diagStream;
    juce::File diagFile;
    juce::uint32 lastStatusMs = 0, lastSpectrumMs = 0;

    std::atomic<float>* pActive = nullptr;
    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pMonitor = nullptr;
    std::atomic<float>* pSens = nullptr;
    std::atomic<float>* pSpeed = nullptr;
    std::atomic<float>* pMaxNotches = nullptr;
    std::atomic<float>* pDepth = nullptr;
    std::atomic<float>* pHold = nullptr;
    std::atomic<float>* pMinF = nullptr;
    std::atomic<float>* pMaxF = nullptr;
    std::atomic<float>* pDuck = nullptr;
    std::atomic<float>* pDuckDb = nullptr;
    std::atomic<float>* pUseRef = nullptr;
    std::atomic<float>* pMerge = nullptr;
    std::atomic<float>* pDiag = nullptr;
    std::array<std::atomic<float>*, kMaxChannels> pChOn {}, pChOff {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackGuardProcessor)
};
