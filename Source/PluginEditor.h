#pragma once

#include <array>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

//==============================================================================
// Ein Kanal: Name, Alarm-LED, Feedback-Nähe, Pegel, Kanal-Schalter, Empfindlichkeits-Versatz, Spektrum, Filter
class ChannelView : public juce::Component
{
public:
    ChannelView (FeedbackGuardProcessor& proc, int channelIndex);

    void tick();                              // vom Timer des Editors aufgerufen
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    float xOf (double freq) const;            // 0..1 auf logarithmischer Achse

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    FeedbackGuardProcessor& proc;
    fg::ChannelEngine& eng;
    int index;
    std::array<float, fg::kDisplayBins> smooth {};
    uint32_t lastDetections = 0;
    juce::uint32 flashUntil = 0;

    juce::Label nameLabel;
    juce::ToggleButton onBtn;
    juce::Slider offSlider;
    std::unique_ptr<ButtonAtt> onAtt;
    std::unique_ptr<SliderAtt> offAtt;
};

//==============================================================================
class FeedbackGuardEditor : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit FeedbackGuardEditor (FeedbackGuardProcessor&);
    ~FeedbackGuardEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAtt> attachment;
    };

    void timerCallback() override;
    void addKnob (Knob& k, const juce::String& paramId, const juce::String& text);
    void appendLog (const juce::String& line);
    void showEntry (const LogEntry& e);
    void savePresetDialog();
    void loadPresetDialog();
    void exportDialog();
    static juce::File defaultDir();

    FeedbackGuardProcessor& proc;
    juce::LookAndFeel_V4 laf;

    int numChannels = 2;
    int numShown = 2;
    std::vector<std::unique_ptr<ChannelView>> views;

    juce::ToggleButton activeBtn, monitorBtn, duckBtn, useRefBtn, diagBtn, mergeBtn;
    juce::TextButton resetBtn, pinBtn, saveBtn, loadBtn, exportBtn;
    juce::ComboBox modeBox;
    juce::Label modeLabel;
    std::unique_ptr<ComboAtt> modeAtt;
    std::unique_ptr<ButtonAtt> activeAtt, monitorAtt, duckAtt, useRefAtt, diagAtt, mergeAtt;

    std::array<Knob, 7> knobs;
    juce::ComboBox speedBox;
    juce::Label speedLabel;
    std::unique_ptr<ComboAtt> speedAtt;

    juce::Label statusLabel;
    juce::TextEditor log;
    uint64_t shownLog = 0;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackGuardEditor)
};
