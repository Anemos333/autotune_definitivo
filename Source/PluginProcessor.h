#pragma once

#include <JuceHeader.h>
#include "ScaleDefinitions.h"
#include "CustomScalePresets.h"
#include "LivePitchProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class MicrotonalAutotuneAudioProcessor : public juce::AudioProcessor
{
public:
    MicrotonalAutotuneAudioProcessor();
    ~MicrotonalAutotuneAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

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

    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }
    CustomScalePresets& getCustomPresets() noexcept { return customPresets; }

    // Message-thread helpers. They publish one coherent, fixed-size snapshot to
    // the callback instead of exposing mutable vectors to the DSP.
    void selectBuiltInScale (int index) noexcept;
    void selectCustomPreset (int index) noexcept;
    void selectRootNote (int index) noexcept;
    void publishScaleSnapshot() noexcept;

    // Non-realtime convenience API retained for UI/state tooling.
    std::vector<double> getCurrentScaleRatios() const;

    std::atomic<int> currentScaleIndex { 0 };
    std::atomic<int> activeCustomPresetIndex { -1 };
    std::atomic<int> rootNoteIndex { 9 };
    std::atomic<int> processingMode { 0 };

    // Must be called outside the audio callback. All engines are prepared in
    // prepareToPlay; mode changes only reset and atomically publish a ready slot.
    void updateProcessingMode (int newMode);

    double getRootFrequency() const noexcept;
    [[nodiscard]] LivePitchProcessor::Metering getPitchMetering() const noexcept;

private:
    static constexpr int maximumScaleRatios = ModernPitchEngine::maxScaleRatios;
    static constexpr int maximumSlowChannels = 2;
    static constexpr int minimumPreparedBlockSize = 4096;

    struct ScaleSnapshot
    {
        std::array<double, maximumScaleRatios> ratios {};
        int count = 0;
        double rootFrequency = 440.0;
        std::uint64_t revision = 0;
    };

    struct SlowChannelState
    {
        std::vector<float> circularBuffer;
        int writePosition = 0;
        double readPosition = 0.0;
    };

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    CustomScalePresets customPresets;

    float detectPitchYIN (const float* buffer, int numSamples, double sampleRate) noexcept;
    double findNearestTarget (double detectedFreqHz,
                              const ScaleSnapshot& scale) const noexcept;

    [[nodiscard]] ScaleSnapshot readScaleSnapshot() noexcept;
    void resetSlowStateNoAllocation() noexcept;
    bool sanitiseInputBuffer (juce::AudioBuffer<float>& buffer) noexcept;
    bool sanitiseOutputBuffer (juce::AudioBuffer<float>& buffer) noexcept;
    void recoverActiveDSPState (int mode) noexcept;

    [[nodiscard]] LivePitchProcessor& liveProcessorForMode (int mode) noexcept;
    [[nodiscard]] const LivePitchProcessor& liveProcessorForMode (int mode) const noexcept;
    [[nodiscard]] static int liveIndexForMode (int mode) noexcept;
    static ModernPitchEngine::LatencyMode modeToLatency (int mode) noexcept;
    int getLatencyForMode (int mode) const noexcept;

    [[nodiscard]] CreativeTempo::Settings getTempoSettings() const noexcept;
    [[nodiscard]] CreativeTempo::HostPosition readHostTempoPosition (
        int numberOfSamples) const noexcept;

    double currentSampleRate = 44100.0;
    int lastSamplesPerBlock = 512;
    std::atomic<bool> prepared { false };

    double smoothedShiftRatio = 1.0;
    int circBufSize = 0;
    std::array<SlowChannelState, maximumSlowChannels> slowChannels;

    std::vector<float> yinBuffer;
    std::vector<float> yinAccumulator;
    int yinBufferPos = 0;
    static constexpr int yinWindowSize = 2048;
    float lastDetectedPitch = 0.0f;
    std::atomic<float> slowMeterPitchHz { 0.0f };
    std::atomic<float> slowMeterTargetHz { 0.0f };

    std::array<LivePitchProcessor, 3> livePitchProcessors;
    std::atomic<int> activeLiveProcessorIndex { 1 };

    // Race-free triple buffering. The message thread writes only a slot that is
    // neither published nor currently copied by the callback, then atomically
    // publishes that immutable slot.
    std::array<ScaleSnapshot, 3> scaleSnapshots;
    std::atomic<int> publishedScaleSlot { 0 };
    std::atomic<int> audioReadingScaleSlot { -1 };
    ScaleSnapshot audioScaleSnapshot;
    std::atomic<std::uint64_t> scaleSnapshotRevision { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicrotonalAutotuneAudioProcessor)
};
