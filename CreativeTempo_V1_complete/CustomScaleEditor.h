#pragma once

#include <JuceHeader.h>
#include "CustomScalePresets.h"
#include <vector>

//==============================================================================
// Forward declaration
class MicrotonalAutotuneAudioProcessor;

// Callback interface for when the editor should close
class CustomScaleEditorListener
{
public:
    virtual ~CustomScaleEditorListener() = default;
    virtual void customScaleEditorClosed() = 0;
};

//==============================================================================
class CustomScaleEditor : public juce::Component
{
public:
    CustomScaleEditor (MicrotonalAutotuneAudioProcessor& processor,
                       CustomScaleEditorListener& listener,
                       juce::Image backgroundImage);
    ~CustomScaleEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;

private:
    MicrotonalAutotuneAudioProcessor& processorRef;
    CustomScaleEditorListener& listenerRef;
    juce::Image bgImage;

    // The octave rectangle area (set in resized)
    juce::Rectangle<int> octaveRect;

    // Division positions as ratios [0, 1] in log space
    // Each value is a position in [0, 1] representing log2(ratio)
    std::vector<double> divisions;

    static constexpr int minDivisions = 3;
    static constexpr int maxDivisions = 33;

    // UI components
    juce::TextEditor nameEditor;
    juce::TextButton saveButton   { "Salva" };
    juce::TextButton backButton   { "Indietro" };
    juce::Label titleLabel;
    juce::Label nameLabel;
    juce::Label octaveLabel;
    juce::Label infoLabel;

    void updateInfoLabel();
    void onSave();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomScaleEditor)
};
