#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

//==============================================================================
MicrotonalAutotuneAudioProcessor::MicrotonalAutotuneAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::mono(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
}

MicrotonalAutotuneAudioProcessor::~MicrotonalAutotuneAudioProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout MicrotonalAutotuneAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "speed", 1 }, "Velocita (ms)",
        juce::NormalisableRange<float> (0.0f, 500.0f, 1.0f), 50.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "amount", 1 }, "Amount",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "tempoMode", 1 }, "Creative Tempo Mode",
        juce::StringArray { "Off", "Tempo Glide", "Glide Lock" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "tempoDivision", 1 }, "Tempo Division",
        juce::StringArray { "1/128", "1/64", "1/32", "1/16", "1/8" }, 2));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tempoGlidePercent", 1 }, "Tempo Glide Length",
        juce::NormalisableRange<float> (5.0f, 100.0f, 1.0f), 35.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tempoLockStrength", 1 }, "Glide Lock Strength",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "tempoSmartOnset", 1 }, "Smart Onset", true));

    return { params.begin(), params.end() };
}

//==============================================================================
const juce::String MicrotonalAutotuneAudioProcessor::getName() const { return "Microtonal Autotune"; }
bool MicrotonalAutotuneAudioProcessor::acceptsMidi() const { return false; }
bool MicrotonalAutotuneAudioProcessor::producesMidi() const { return false; }
bool MicrotonalAutotuneAudioProcessor::isMidiEffect() const { return false; }
double MicrotonalAutotuneAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int MicrotonalAutotuneAudioProcessor::getNumPrograms() { return 1; }
int MicrotonalAutotuneAudioProcessor::getCurrentProgram() { return 0; }
void MicrotonalAutotuneAudioProcessor::setCurrentProgram (int) {}
const juce::String MicrotonalAutotuneAudioProcessor::getProgramName (int) { return {}; }
void MicrotonalAutotuneAudioProcessor::changeProgramName (int, const juce::String&) {}

//==============================================================================
ModernPitchEngine::LatencyMode MicrotonalAutotuneAudioProcessor::modeToLatency (int mode) noexcept
{
    switch (mode)
    {
        case 1:  return ModernPitchEngine::LatencyMode::quality;
        case 2:  return ModernPitchEngine::LatencyMode::live;
        case 3:  return ModernPitchEngine::LatencyMode::ultraLive;
        default: return ModernPitchEngine::LatencyMode::live; // fallback
    }
}

int MicrotonalAutotuneAudioProcessor::getLatencyForMode (int mode) const
{
    if (mode == 0)
        return yinWindowSize; // Slow mode: 2048 samples

    // For ModernPitchEngine modes, query the engine
    // The frame size depends on sample rate and mode
    // We can compute it the same way the engine does
    return livePitchProcessor.getLatencySamples();
}

//==============================================================================
void MicrotonalAutotuneAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    lastSamplesPerBlock = samplesPerBlock;
    smoothedShiftRatio = 1.0;

    // Circular buffer: 4x YIN window for comfortable pitch shifting (Slow mode)
    circBufSize = yinWindowSize * 4;
    circularBuffer.assign (static_cast<size_t> (circBufSize), 0.0f);
    circBufWritePos = 0;
    circBufReadPos = static_cast<double> (circBufSize - yinWindowSize);

    // YIN accumulation buffer (Slow mode)
    yinBuffer.assign (yinWindowSize, 0.0f);
    yinAccumulator.assign (static_cast<size_t> (yinWindowSize / 2), 0.0f);
    yinBufferPos = 0;
    lastDetectedPitch = 0.0f;
    slowMeterPitchHz.store (0.0f, std::memory_order_relaxed);
    slowMeterTargetHz.store (0.0f, std::memory_order_relaxed);

    // Prepare ModernPitchEngine-based live pitch processor
    int mode = processingMode.load();
    if (mode > 0)
    {
        livePitchProcessor.prepare (sampleRate,
                                    samplesPerBlock,
                                    std::max (1, getTotalNumOutputChannels()),
                                    modeToLatency (mode));
        // Set sensible defaults for the advanced parameters
        livePitchProcessor.setAdvancedParameters (
            35.0f,   // transitionMs
            0.70f,   // preserveVibrato
            0.20f,   // humanize
            0.90f,   // formantPreservation
            0.85f,   // transientProtection
            0.70f,   // detectorSensitivity
            12.0f,   // maximumCorrectionSemitones
            45.0f,   // minimumPitchHz
            1600.0f, // maximumPitchHz
            LivePitchProcessor::StereoMode::linkedMidSide
        );
        setLatencySamples (livePitchProcessor.getLatencySamples());
    }
    else
    {
        // Slow mode: prepare engine with default for potential future switch
        livePitchProcessor.prepare (sampleRate,
                                    samplesPerBlock,
                                    std::max (1, getTotalNumOutputChannels()),
                                    ModernPitchEngine::LatencyMode::live);
        livePitchProcessor.setAdvancedParameters (
            35.0f, 0.70f, 0.20f, 0.90f, 0.85f, 0.70f, 12.0f, 45.0f, 1600.0f,
            LivePitchProcessor::StereoMode::linkedMidSide
        );
        setLatencySamples (yinWindowSize);
    }
}

void MicrotonalAutotuneAudioProcessor::releaseResources()
{
    circularBuffer.clear();
    yinBuffer.clear();
    yinAccumulator.clear();
    livePitchProcessor.reset();
}

bool MicrotonalAutotuneAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainInput  = layouts.getChannelSet (true,  0);
    const auto& mainOutput = layouts.getChannelSet (false, 0);

    // Support mono or stereo, input and output must match
    if (mainInput != mainOutput)
        return false;

    if (mainInput != juce::AudioChannelSet::mono() &&
        mainInput != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

//==============================================================================
void MicrotonalAutotuneAudioProcessor::updateProcessingMode (int newMode)
{
    int oldMode = processingMode.load();
    if (newMode == oldMode)
        return;

    processingMode.store (newMode);

    // Non-real-time reconfiguration: suspend processing, rebuild engine, update latency
    suspendProcessing (true);

    if (newMode > 0)
    {
        livePitchProcessor.setLatencyModeNonRealtime (modeToLatency (newMode));
        setLatencySamples (livePitchProcessor.getLatencySamples());
    }
    else
    {
        // Switching back to Slow mode
        setLatencySamples (yinWindowSize);

        // Reset Slow-mode state
        smoothedShiftRatio = 1.0;
        circularBuffer.assign (static_cast<size_t> (circBufSize), 0.0f);
        circBufWritePos = 0;
        circBufReadPos = static_cast<double> (circBufSize - yinWindowSize);
        yinBuffer.assign (yinWindowSize, 0.0f);
        yinBufferPos = 0;
        lastDetectedPitch = 0.0f;
        slowMeterPitchHz.store (0.0f, std::memory_order_relaxed);
        slowMeterTargetHz.store (0.0f, std::memory_order_relaxed);
    }

    suspendProcessing (false);
}

//==============================================================================
// YIN Pitch Detection Algorithm (Slow mode — unchanged from original)
float MicrotonalAutotuneAudioProcessor::detectPitchYIN (const float* buffer, int numSamples, double sampleRate) const
{
    if (numSamples < 2)
        return 0.0f;

    const int halfWindow = numSamples / 2;
    std::vector<float> diff (static_cast<size_t> (halfWindow), 0.0f);

    // Step 1: Difference function
    for (int tau = 0; tau < halfWindow; ++tau)
    {
        float sum = 0.0f;
        for (int j = 0; j < halfWindow; ++j)
        {
            float delta = buffer[j] - buffer[j + tau];
            sum += delta * delta;
        }
        diff[static_cast<size_t> (tau)] = sum;
    }

    // Step 2: Cumulative Mean Normalized Difference Function (CMNDF)
    diff[0] = 1.0f;
    float runningSum = 0.0f;
    for (int tau = 1; tau < halfWindow; ++tau)
    {
        runningSum += diff[static_cast<size_t> (tau)];
        if (runningSum > 0.0f)
            diff[static_cast<size_t> (tau)] = diff[static_cast<size_t> (tau)] * static_cast<float> (tau) / runningSum;
        else
            diff[static_cast<size_t> (tau)] = 1.0f;
    }

    // Step 3: Absolute threshold
    constexpr float threshold = 0.15f;
    int tauEstimate = -1;

    // Skip tau=0 and tau=1 (too high frequency / not meaningful)
    for (int tau = 2; tau < halfWindow; ++tau)
    {
        if (diff[static_cast<size_t> (tau)] < threshold)
        {
            // Find the local minimum
            while (tau + 1 < halfWindow &&
                   diff[static_cast<size_t> (tau + 1)] < diff[static_cast<size_t> (tau)])
            {
                ++tau;
            }
            tauEstimate = tau;
            break;
        }
    }

    if (tauEstimate < 2)
        return 0.0f;

    // Step 4: Parabolic interpolation for sub-sample accuracy
    double betterTau = static_cast<double> (tauEstimate);

    if (tauEstimate > 0 && tauEstimate < halfWindow - 1)
    {
        float s0 = diff[static_cast<size_t> (tauEstimate - 1)];
        float s1 = diff[static_cast<size_t> (tauEstimate)];
        float s2 = diff[static_cast<size_t> (tauEstimate + 1)];

        float denom = 2.0f * (2.0f * s1 - s0 - s2);
        if (std::abs (denom) > 1e-10f)
            betterTau = static_cast<double> (tauEstimate) + static_cast<double> ((s0 - s2) / denom);
    }

    if (betterTau <= 0.0)
        return 0.0f;

    return static_cast<float> (sampleRate / betterTau);
}

//==============================================================================
std::vector<double> MicrotonalAutotuneAudioProcessor::getCurrentScaleRatios() const
{
    int customIdx = activeCustomPresetIndex.load();
    if (customIdx >= 0 && customIdx < customPresets.getNumPresets())
        return customPresets.getPreset (customIdx).ratios;

    int scaleIdx = currentScaleIndex.load();
    if (scaleIdx >= 0 && scaleIdx < ScaleDefinitions::getScaleCount())
        return ScaleDefinitions::getScale (scaleIdx).ratios;

    // Fallback: chromatic 12-EDO
    return ScaleDefinitions::getScale (0).ratios;
}

double MicrotonalAutotuneAudioProcessor::getRootFrequency() const
{
    // Root note frequencies in octave 4
    // 0-11: C4, C#4, D4, D#4, E4, F4, F#4, G4, G#4, A4, A#4, B4
    // 12-18: Byzantine Ni, Pa, Vu, Ga, Di, Ke, Zo
    static const double rootFreqs[] = {
        261.6256, // C4
        277.1826, // C#4
        293.6648, // D4
        311.1270, // D#4
        329.6276, // E4
        349.2282, // F4
        369.9944, // F#4
        391.9954, // G4
        415.3047, // G#4
        440.0000, // A4
        466.1638, // A#4
        493.8833, // B4
        // Byzantine notes (Ni=C, then Byzantine diatonic intervals in 72-moria system)
        261.6256,                                   // Ni  = C4
        261.6256 * std::pow (2.0, 12.0 / 72.0),     // Pa  (12 moria from Ni)
        261.6256 * std::pow (2.0, 22.0 / 72.0),     // Vu  (22 moria from Ni)
        261.6256 * std::pow (2.0, 30.0 / 72.0),     // Ga  (30 moria from Ni)
        261.6256 * std::pow (2.0, 42.0 / 72.0),     // Di  (42 moria from Ni)
        261.6256 * std::pow (2.0, 54.0 / 72.0),     // Ke  (54 moria from Ni)
        261.6256 * std::pow (2.0, 64.0 / 72.0)      // Zo  (64 moria from Ni)
    };

    int idx = rootNoteIndex.load();
    if (idx >= 0 && idx < 19)
        return rootFreqs[idx];

    return 440.0; // fallback A4
}

double MicrotonalAutotuneAudioProcessor::findNearestTarget (double detectedFreqHz) const
{
    if (detectedFreqHz <= 0.0)
        return detectedFreqHz;

    auto scaleRatios = getCurrentScaleRatios();
    if (scaleRatios.empty())
        return detectedFreqHz;

    // Reference: use selected root note frequency
    // Work in log2 space relative to the root.
    double refFreq = getRootFrequency();
    double logRatio = std::log2 (detectedFreqHz / refFreq);

    // Octave number (floor)
    double octave = std::floor (logRatio);
    double fractionalOctave = logRatio - octave;  // [0, 1)

    // Convert scale ratios to log2 positions within the octave
    double bestDist = 1e9;
    double bestLogTarget = 0.0;

    for (double ratio : scaleRatios)
    {
        double logPos = std::log2 (ratio); // [0, ~1)
        double dist = std::abs (fractionalOctave - logPos);

        // Also check wrap-around (near octave boundary)
        double distWrapUp   = std::abs (fractionalOctave - (logPos + 1.0));
        double distWrapDown = std::abs (fractionalOctave - (logPos - 1.0));

        double minDist = std::min ({ dist, distWrapUp, distWrapDown });

        if (minDist < bestDist)
        {
            bestDist = minDist;
            if (distWrapUp < dist && distWrapUp < distWrapDown)
                bestLogTarget = octave + logPos + 1.0;
            else if (distWrapDown < dist && distWrapDown < distWrapUp)
                bestLogTarget = octave + logPos - 1.0;
            else
                bestLogTarget = octave + logPos;
        }
    }

    return refFreq * std::pow (2.0, bestLogTarget);
}

//==============================================================================
CreativeTempo::Settings
MicrotonalAutotuneAudioProcessor::getTempoSettings() const noexcept
{
    CreativeTempo::Settings settings;

    const int mode = static_cast<int>(std::lround(
        apvts.getRawParameterValue("tempoMode")->load()));
    settings.mode = static_cast<CreativeTempo::Mode>(
        juce::jlimit(0, 2, mode));

    const int division = static_cast<int>(std::lround(
        apvts.getRawParameterValue("tempoDivision")->load()));
    settings.division = CreativeTempo::divisionFromIndex(division);
    settings.glideFraction = juce::jlimit(0.05f, 1.0f,
        apvts.getRawParameterValue("tempoGlidePercent")->load() / 100.0f);
    settings.lockStrength = juce::jlimit(0.0f, 1.0f,
        apvts.getRawParameterValue("tempoLockStrength")->load() / 100.0f);
    settings.smartOnset =
        apvts.getRawParameterValue("tempoSmartOnset")->load() >= 0.5f;
    settings.smartOnsetWindow = 0.18f;
    settings.fallbackBpm = 120.0;
    return settings;
}

CreativeTempo::HostPosition
MicrotonalAutotuneAudioProcessor::readHostTempoPosition(
    int numberOfSamples) const noexcept
{
    CreativeTempo::HostPosition result;
    result.numberOfSamples = std::max(0, numberOfSamples);

    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
            {
                result.bpm = *bpm;
                result.hasBpm = std::isfinite(result.bpm)
                             && result.bpm > 1.0;
            }

            if (const auto ppq = position->getPpqPosition())
            {
                result.ppqAtBlockStart = *ppq;
                result.hasPpq = std::isfinite(result.ppqAtBlockStart);
            }

            if (const auto sampleTime = position->getTimeInSamples())
            {
                result.timeInSamples = *sampleTime;
                result.hasTimeInSamples = true;
            }

            result.isPlaying = position->getIsPlaying();
            result.isLooping = position->getIsLooping();
        }
    }

    return result;
}

//==============================================================================
void MicrotonalAutotuneAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    // Clear unused output channels
    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, numSamples);

    if (numSamples == 0 || totalNumInputChannels == 0)
        return;

    // Get parameters
    float speedMs = apvts.getRawParameterValue ("speed")->load();
    float amountPct = apvts.getRawParameterValue ("amount")->load();
    float amount = amountPct / 100.0f;

    int mode = processingMode.load (std::memory_order_relaxed);

    // ==================== MODERN ENGINE MODES (Quality / Live / Experimental) ====================
    if (mode > 0)
    {
        auto scaleRatios = getCurrentScaleRatios();
        double rootFreq  = getRootFrequency();

        // Creative tempo is an optional target scheduler. Off is a strict
        // pass-through, so the V5 correction path is unchanged.
        livePitchProcessor.setTempoSettings (getTempoSettings());
        livePitchProcessor.setTempoHostPosition (
            readHostTempoPosition (numSamples));

        // Use the AudioBuffer overload so the engine handles mono/stereo properly
        livePitchProcessor.process (buffer, scaleRatios, rootFreq, speedMs, amount);

        return;
    }

    // ==================== SLOW MODE (original YIN processing — unchanged) ====================

    // Speed smoothing coefficient (one-pole filter)
    // speedMs = time to reach ~63% of target
    double speedCoeff = 1.0;
    if (speedMs > 0.0f)
    {
        double speedSamples = (static_cast<double> (speedMs) / 1000.0) * currentSampleRate;
        speedCoeff = 1.0 - std::exp (-1.0 / speedSamples);
    }

    // Process first channel, then copy to others (mono processing for pitch)
    const float* inputData = buffer.getReadPointer (0);

    // Accumulate samples into YIN buffer for pitch detection
    for (int i = 0; i < numSamples; ++i)
    {
        yinBuffer[static_cast<size_t> (yinBufferPos)] = inputData[i];
        yinBufferPos++;

        if (yinBufferPos >= yinWindowSize)
        {
            // Run YIN pitch detection
            float detectedPitch = detectPitchYIN (yinBuffer.data(), yinWindowSize, currentSampleRate);

            // Only update if we got a valid pitch (20 Hz to 5000 Hz range)
            if (detectedPitch > 20.0f && detectedPitch < 5000.0f)
            {
                lastDetectedPitch = detectedPitch;
                slowMeterPitchHz.store (detectedPitch,
                                        std::memory_order_relaxed);
            }

            yinBufferPos = 0;
        }
    }

    // Compute target shift ratio
    double targetShiftRatio = 1.0;
    if (lastDetectedPitch > 20.0f)
    {
        double targetFreq = findNearestTarget (static_cast<double> (lastDetectedPitch));
        if (targetFreq > 0.0)
        {
            targetShiftRatio = targetFreq / static_cast<double> (lastDetectedPitch);
            slowMeterTargetHz.store (static_cast<float> (targetFreq),
                                     std::memory_order_relaxed);
        }
    }
    else
    {
        slowMeterTargetHz.store (0.0f, std::memory_order_relaxed);
    }

    // Process each sample with pitch shifting via variable-rate read from circular buffer
    float* outputData = buffer.getWritePointer (0);

    for (int i = 0; i < numSamples; ++i)
    {
        // Write input to circular buffer
        circularBuffer[static_cast<size_t> (circBufWritePos)] = inputData[i];
        circBufWritePos = (circBufWritePos + 1) % circBufSize;

        // Smooth the shift ratio
        smoothedShiftRatio += speedCoeff * (targetShiftRatio - smoothedShiftRatio);

        // Apply amount: blend between 1.0 (no correction) and smoothedShiftRatio
        double effectiveRatio = 1.0 + (smoothedShiftRatio - 1.0) * static_cast<double> (amount);

        // Read from circular buffer at variable rate
        circBufReadPos += effectiveRatio;
        while (circBufReadPos >= static_cast<double> (circBufSize))
            circBufReadPos -= static_cast<double> (circBufSize);
        while (circBufReadPos < 0.0)
            circBufReadPos += static_cast<double> (circBufSize);

        // Linear interpolation
        int readIdx0 = static_cast<int> (circBufReadPos);
        int readIdx1 = (readIdx0 + 1) % circBufSize;
        double frac = circBufReadPos - static_cast<double> (readIdx0);

        float sample = static_cast<float> (
            circularBuffer[static_cast<size_t> (readIdx0)] * (1.0 - frac) +
            circularBuffer[static_cast<size_t> (readIdx1)] * frac);

        outputData[i] = sample;
    }

    // Copy mono result to all output channels
    for (int ch = 1; ch < totalNumOutputChannels; ++ch)
        buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
}

//==============================================================================
void MicrotonalAutotuneAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                              juce::MidiBuffer&)
{
    int mode = processingMode.load (std::memory_order_relaxed);

    if (mode > 0)
    {
        // Use the engine's bypassed processing to maintain aligned latency
        livePitchProcessor.processBypassed (buffer);
    }
    else
    {
        // Slow mode: apply delay manually to maintain latency alignment
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = buffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                circularBuffer[static_cast<size_t> (circBufWritePos)] = data[i];
                circBufWritePos = (circBufWritePos + 1) % circBufSize;

                circBufReadPos += 1.0;
                while (circBufReadPos >= static_cast<double> (circBufSize))
                    circBufReadPos -= static_cast<double> (circBufSize);

                int readIdx = static_cast<int> (circBufReadPos);
                data[i] = circularBuffer[static_cast<size_t> (readIdx)];
            }
        }
    }
}

//==============================================================================
LivePitchProcessor::Metering
MicrotonalAutotuneAudioProcessor::getPitchMetering() const noexcept
{
    if (processingMode.load (std::memory_order_relaxed) > 0)
        return livePitchProcessor.getMetering();

    LivePitchProcessor::Metering meter;
    meter.detectedPitchHz = slowMeterPitchHz.load (std::memory_order_relaxed);
    meter.targetPitchHz = slowMeterTargetHz.load (std::memory_order_relaxed);
    // The legacy Slow detector does not expose calibrated confidence or
    // voicing values, so report them as unavailable rather than inventing a
    // percentage.  Pitch and target remain useful in this mode.
    meter.confidence = 0.0f;
    meter.voicing = 0.0f;
    meter.consensus = 0.0f;
    meter.detectorSupport = meter.detectedPitchHz > 0.0f ? 1 : 0;
    if (meter.detectedPitchHz > 0.0f && meter.targetPitchHz > 0.0f)
    {
        meter.correctionCents = 1200.0f * std::log2(
            meter.targetPitchHz / meter.detectedPitchHz);
    }
    meter.state = meter.detectedPitchHz > 0.0f
        ? ModernPitchEngine::TrackingState::stable
        : ModernPitchEngine::TrackingState::unvoiced;
    return meter;
}

//==============================================================================
bool MicrotonalAutotuneAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* MicrotonalAutotuneAudioProcessor::createEditor()
{
    return new MicrotonalAutotuneAudioProcessorEditor (*this);
}

//==============================================================================
void MicrotonalAutotuneAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Save APVTS parameters
    auto state = apvts.copyState();

    // Save scale selection
    state.setProperty ("scaleIndex", currentScaleIndex.load(), nullptr);
    state.setProperty ("customPresetIndex", activeCustomPresetIndex.load(), nullptr);
    state.setProperty ("rootNoteIndex", rootNoteIndex.load(), nullptr);
    state.setProperty ("processingMode", processingMode.load(), nullptr);

    // Save custom presets
    auto customTree = customPresets.toValueTree();
    state.addChild (customTree, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    if (xml != nullptr)
        copyXmlToBinary (*xml, destData);
}

void MicrotonalAutotuneAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr)
    {
        auto tree = juce::ValueTree::fromXml (*xmlState);

        if (tree.isValid())
        {
            apvts.replaceState (tree);

            // Restore scale selection
            if (tree.hasProperty ("scaleIndex"))
                currentScaleIndex.store (static_cast<int> (tree.getProperty ("scaleIndex")));

            if (tree.hasProperty ("customPresetIndex"))
                activeCustomPresetIndex.store (static_cast<int> (tree.getProperty ("customPresetIndex")));

            if (tree.hasProperty ("rootNoteIndex"))
                rootNoteIndex.store (static_cast<int> (tree.getProperty ("rootNoteIndex")));

            // Restore processing mode (with backward compatibility for liveModeEnabled)
            if (tree.hasProperty ("processingMode"))
            {
                int mode = static_cast<int> (tree.getProperty ("processingMode"));
                processingMode.store (mode);

                if (mode > 0)
                {
                    livePitchProcessor.setLatencyModeNonRealtime (modeToLatency (mode));
                    setLatencySamples (livePitchProcessor.getLatencySamples());
                }
                else
                {
                    setLatencySamples (yinWindowSize);
                }
            }
            else if (tree.hasProperty ("liveModeEnabled"))
            {
                // Backward compatibility: old sessions with liveModeEnabled bool
                bool wasLive = static_cast<int> (tree.getProperty ("liveModeEnabled")) != 0;
                int mode = wasLive ? 2 : 0; // map old Live to new Live mode
                processingMode.store (mode);

                if (mode > 0)
                {
                    livePitchProcessor.setLatencyModeNonRealtime (modeToLatency (mode));
                    setLatencySamples (livePitchProcessor.getLatencySamples());
                }
                else
                {
                    setLatencySamples (yinWindowSize);
                }
            }

            // Restore custom presets
            auto customTree = tree.getChildWithName ("CustomScales");
            if (customTree.isValid())
                customPresets.fromValueTree (customTree);
        }
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MicrotonalAutotuneAudioProcessor();
}
