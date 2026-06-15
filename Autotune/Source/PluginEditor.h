#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CustomScaleEditor.h"

//==============================================================================
class MicrotonalAutotuneAudioProcessorEditor : public juce::AudioProcessorEditor,
                                                public CustomScaleEditorListener
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

    // Methods
    void buildScaleMenu();
    void onScaleSelected();
    void showCustomScaleEditor();
    void onRootNoteSelected();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicrotonalAutotuneAudioProcessorEditor)
};
