#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CustomScaleEditor.h"

//==============================================================================
class MicrotonalAutotuneAudioProcessorEditor : public juce::AudioProcessorEditor,
                                                public CustomScaleEditorListener,
                                                private juce::Timer
{
public:
    explicit MicrotonalAutotuneAudioProcessorEditor (MicrotonalAutotuneAudioProcessor&);
    ~MicrotonalAutotuneAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // CustomScaleEditorListener
    void customScaleEditorClosed() override;

private:
    MicrotonalAutotuneAudioProcessor& processorRef;

    // Background image
    juce::Image bgImage;
    juce::Image bgImageScaleEditor;

    // Main page components
    juce::ComboBox scaleSelector;
    juce::Label scaleSelectorLabel;

    juce::ComboBox rootNoteSelector;
    juce::Label rootNoteSelectorLabel;

    juce::Slider speedKnob;
    juce::Label  speedLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttachment;

    juce::Slider amountKnob;
    juce::Label  amountLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> amountAttachment;

    // Custom scale editor (shown/hidden)
    std::unique_ptr<CustomScaleEditor> customScaleEditorPage;
    bool showingScaleEditor = false;

    // Processing mode selector (replaces old LIVE button)
    juce::ComboBox modeSelector;
    juce::Label modeSelectorLabel;

    // Third page: creative tempo controls. This is intentionally separate
    // from the main processing-mode selector.
    juce::TextButton tempoPageButton { "Tempo" };
    juce::TextButton tempoBackButton { "Indietro" };
    juce::TextButton tempoOffButton { "Off" };
    juce::TextButton tempoGlideButton { "Tempo Glide" };
    juce::TextButton glideLockButton { "Glide Lock" };
    juce::ComboBox tempoDivisionSelector;
    juce::Label tempoDivisionLabel;
    juce::Slider tempoGlideLength;
    juce::Label tempoGlideLengthLabel;
    juce::Slider tempoLockStrength;
    juce::Label tempoLockStrengthLabel;
    juce::ToggleButton tempoSmartOnset { "Smart onset" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        tempoDivisionAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        tempoGlideLengthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        tempoLockStrengthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        tempoSmartOnsetAttachment;
    bool showingTempoPage = false;

    // Methods
    void buildScaleMenu();
    void onScaleSelected();
    void showCustomScaleEditor();
    void showTempoPage();
    void closeTempoPage();
    void setMainControlsVisible(bool shouldBeVisible);
    void setTempoControlsVisible(bool shouldBeVisible);
    void setTempoModeParameter(int modeIndex);
    void updateTempoModeButtons();
    void onRootNoteSelected();
    void onModeSelected();

    void timerCallback() override;
    [[nodiscard]] static juce::String trackingStateToString (
        ModernPitchEngine::TrackingState state);
    void drawMeterPanel (juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawTempoPage (juce::Graphics& g, juce::Rectangle<int> bounds);

    LivePitchProcessor::Metering displayedMetering;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicrotonalAutotuneAudioProcessorEditor)
};
