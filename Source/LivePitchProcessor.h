#pragma once

#include "ModernPitchEngine.h"

#include <algorithm>
#include <vector>

// Drop-in-oriented adapter for projects that already use the original
// LivePitchProcessor interface. It delegates all DSP to ModernPitchEngine.
class LivePitchProcessor final
{
public:
    using LatencyMode = ModernPitchEngine::LatencyMode;
    using StereoMode = ModernPitchEngine::StereoMode;
    using Metering = ModernPitchEngine::Metering;

    void prepare(double sampleRate, int maximumExpectedSamplesPerBlock)
    {
        prepare(sampleRate, maximumExpectedSamplesPerBlock, 1, latencyMode_);
    }

    void prepare(double sampleRate,
                 int maximumExpectedSamplesPerBlock,
                 int numberOfChannels,
                 LatencyMode latencyMode)
    {
        sampleRate_ = sampleRate;
        maximumBlockSize_ = maximumExpectedSamplesPerBlock;
        channelCount_ = numberOfChannels;
        latencyMode_ = latencyMode;
        engine_.prepare(sampleRate_, maximumBlockSize_, channelCount_, latencyMode_);
    }

    void reset() noexcept
    {
        engine_.reset();
    }

    // Call only outside the audio callback. The owning AudioProcessor must then
    // call setLatencySamples(getLatencySamples()).
    void setLatencyModeNonRealtime(LatencyMode mode)
    {
        if (mode == latencyMode_ || sampleRate_ <= 0.0)
            return;

        latencyMode_ = mode;
        engine_.prepare(sampleRate_, maximumBlockSize_, channelCount_, latencyMode_);
    }

    void setAdvancedParameters(float transitionMs,
                               float preserveVibrato,
                               float humanize,
                               float formantPreservation,
                               float transientProtection,
                               float detectorSensitivity,
                               float maximumCorrectionSemitones,
                               float minimumPitchHz,
                               float maximumPitchHz,
                               StereoMode stereoMode,
                               float breathReduction = 0.50f) noexcept
    {
        parameters_.transitionTimeMs = transitionMs;
        parameters_.preserveVibrato = preserveVibrato;
        parameters_.humanize = humanize;
        parameters_.formantPreservation = formantPreservation;
        parameters_.transientProtection = transientProtection;
        parameters_.detectorSensitivity = detectorSensitivity;
        parameters_.maximumCorrectionSemitones = maximumCorrectionSemitones;
        parameters_.minimumPitchHz = minimumPitchHz;
        parameters_.maximumPitchHz = maximumPitchHz;
        parameters_.stereoMode = stereoMode;
        parameters_.breathReduction = std::clamp(breathReduction, 0.0f, 1.0f);
    }

    void setTempoSettings(const CreativeTempo::Settings& settings) noexcept
    {
        parameters_.tempo = settings;
    }

    void setTempoHostPosition(const CreativeTempo::HostPosition& position) noexcept
    {
        tempoHostPosition_ = position;
    }

    void process(juce::AudioBuffer<float>& buffer,
                 const double* scaleRatios,
                 int numberOfScaleRatios,
                 double rootFrequency,
                 float speedMs,
                 float amount)
    {
        parameters_.retuneTimeMs = speedMs;
        parameters_.amount = amount;
        engine_.process(buffer,
                        scaleRatios,
                        numberOfScaleRatios,
                        rootFrequency,
                        parameters_,
                        tempoHostPosition_);
    }

    void process(juce::AudioBuffer<float>& buffer,
                 const std::vector<double>& scaleRatios,
                 double rootFrequency,
                 float speedMs,
                 float amount)
    {
        process(buffer,
                scaleRatios.empty() ? nullptr : scaleRatios.data(),
                static_cast<int>(scaleRatios.size()),
                rootFrequency,
                speedMs,
                amount);
    }

    void process(float* data,
                 int numberOfSamples,
                 const std::vector<double>& scaleRatios,
                 double rootFrequency,
                 float speedMs,
                 float amount)
    {
        parameters_.retuneTimeMs = speedMs;
        parameters_.amount = amount;
        engine_.process(data,
                        numberOfSamples,
                        scaleRatios,
                        rootFrequency,
                        parameters_);
    }

    void processBypassed(juce::AudioBuffer<float>& buffer)
    {
        engine_.processBypassed(buffer);
    }

    [[nodiscard]] int getLatencySamples() const noexcept
    {
        return engine_.getLatencySamples();
    }

    [[nodiscard]] float getDetectedPitchHz() const noexcept
    {
        return engine_.getMetering().detectedPitchHz;
    }

    [[nodiscard]] float getDetectionConfidence() const noexcept
    {
        return engine_.getMetering().confidence;
    }

    [[nodiscard]] Metering getMetering() const noexcept
    {
        return engine_.getMetering();
    }

private:
    ModernPitchEngine engine_;
    ModernPitchEngine::Parameters parameters_;
    double sampleRate_ = 0.0;
    int maximumBlockSize_ = 0;
    int channelCount_ = 1;
    LatencyMode latencyMode_ = LatencyMode::live;
    CreativeTempo::HostPosition tempoHostPosition_;
};
