#pragma once

#include <JuceHeader.h>
#include "ScaleDefinitions.h"
#include "CustomScalePresets.h"
#include <vector>
#include <atomic>

class MicrotonalAutotuneAudioProcessor : public juce::AudioProcessor
{
public:
    MicrotonalAutotuneAudioProcessor();
    ~MicrotonalAutotuneAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Access to APVTS
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // Custom presets access (thread-safe via message thread only for GUI)
    CustomScalePresets& getCustomPresets() { return customPresets; }

    // Get current active scale ratios
    std::vector<double> getCurrentScaleRatios() const;

    // Scale index parameter
    std::atomic<int> currentScaleIndex { 0 };

    // For custom scale: store which custom preset is active (-1 = none, using built-in)
    std::atomic<int> activeCustomPresetIndex { -1 };

    // Root note index: 0-11 = C,C#,D,...,B (12-ET), 12-18 = Ni,Pa,Vu,Ga,Di,Ke,Zo (Byzantine)
    std::atomic<int> rootNoteIndex { 9 }; // default A

    // Get the reference frequency for the selected root note
    double getRootFrequency() const;

private:
    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    CustomScalePresets customPresets;

    // YIN pitch detection
    float detectPitchYIN (const float* buffer, int numSamples, double sampleRate) const;

    // Find nearest note in scale
    double findNearestTarget (double detectedFreqHz) const;

    // Pitch shifting state
    double currentSampleRate = 44100.0;

    // Smoothed pitch shift ratio
    double smoothedShiftRatio = 1.0;

    // Circular buffer for pitch shifting
    std::vector<float> circularBuffer;
    int circBufSize = 0;
    int circBufWritePos = 0;
    double circBufReadPos = 0.0;

    // YIN internal buffer
    std::vector<float> yinBuffer;
    std::vector<float> yinAccumulator;
    int yinBufferPos = 0;
    static constexpr int yinWindowSize = 2048;
    float lastDetectedPitch = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicrotonalAutotuneAudioProcessor)
};
