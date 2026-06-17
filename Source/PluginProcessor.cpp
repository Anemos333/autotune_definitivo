#include "PluginProcessor.h"
#if !defined(AUTOTUNE_HEADLESS_TEST)
#include "PluginEditor.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double c4Frequency = 261.6255653005986;

float finiteClamped(float value, float minimum, float maximum, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}
} // namespace

//==============================================================================
MicrotonalAutotuneAudioProcessor::MicrotonalAutotuneAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::mono(), true)
                         .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    publishScaleSnapshot();
}

MicrotonalAutotuneAudioProcessor::~MicrotonalAutotuneAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout
MicrotonalAutotuneAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "speed", 1 }, "Velocita (ms)",
        juce::NormalisableRange<float>(0.0f, 500.0f, 1.0f), 50.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "amount", 1 }, "Amount",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "tempoMode", 1 }, "Creative Tempo Mode",
        juce::StringArray { "Off", "Tempo Glide", "Glide Lock" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "tempoDivision", 1 }, "Tempo Division",
        juce::StringArray { "1/128", "1/64", "1/32", "1/16", "1/8" }, 2));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "tempoGlidePercent", 1 }, "Tempo Glide Length",
        juce::NormalisableRange<float>(5.0f, 100.0f, 1.0f), 35.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "tempoLockStrength", 1 }, "Glide Lock Strength",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "tempoSmartOnset", 1 }, "Smart Onset", true));

    return { params.begin(), params.end() };
}

const juce::String MicrotonalAutotuneAudioProcessor::getName() const { return "Microtonal Autotune"; }
bool MicrotonalAutotuneAudioProcessor::acceptsMidi() const { return false; }
bool MicrotonalAutotuneAudioProcessor::producesMidi() const { return false; }
bool MicrotonalAutotuneAudioProcessor::isMidiEffect() const { return false; }
double MicrotonalAutotuneAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int MicrotonalAutotuneAudioProcessor::getNumPrograms() { return 1; }
int MicrotonalAutotuneAudioProcessor::getCurrentProgram() { return 0; }
void MicrotonalAutotuneAudioProcessor::setCurrentProgram(int) {}
const juce::String MicrotonalAutotuneAudioProcessor::getProgramName(int) { return {}; }
void MicrotonalAutotuneAudioProcessor::changeProgramName(int, const juce::String&) {}

//==============================================================================
int MicrotonalAutotuneAudioProcessor::liveIndexForMode(int mode) noexcept
{
    switch (mode)
    {
        case 3: return 0; // ultra-live
        case 2: return 1; // live
        case 1: return 2; // quality
        default: return 1;
    }
}

ModernPitchEngine::LatencyMode
MicrotonalAutotuneAudioProcessor::modeToLatency(int mode) noexcept
{
    switch (mode)
    {
        case 3: return ModernPitchEngine::LatencyMode::ultraLive;
        case 1: return ModernPitchEngine::LatencyMode::quality;
        case 2:
        default: return ModernPitchEngine::LatencyMode::live;
    }
}

LivePitchProcessor& MicrotonalAutotuneAudioProcessor::liveProcessorForMode(int mode) noexcept
{
    return livePitchProcessors[static_cast<std::size_t>(liveIndexForMode(mode))];
}

const LivePitchProcessor&
MicrotonalAutotuneAudioProcessor::liveProcessorForMode(int mode) const noexcept
{
    return livePitchProcessors[static_cast<std::size_t>(liveIndexForMode(mode))];
}

int MicrotonalAutotuneAudioProcessor::getLatencyForMode(int mode) const noexcept
{
    return mode == 0 ? yinWindowSize : liveProcessorForMode(mode).getLatencySamples();
}

void MicrotonalAutotuneAudioProcessor::resetSlowStateNoAllocation() noexcept
{
    smoothedShiftRatio = 1.0;
    for (auto& channel : slowChannels)
    {
        std::fill(channel.circularBuffer.begin(), channel.circularBuffer.end(), 0.0f);
        channel.writePosition = 0;
        channel.readPosition = static_cast<double>(std::max(0, circBufSize - yinWindowSize));
    }
    std::fill(yinBuffer.begin(), yinBuffer.end(), 0.0f);
    std::fill(yinAccumulator.begin(), yinAccumulator.end(), 0.0f);
    yinBufferPos = 0;
    lastDetectedPitch = 0.0f;
    slowMeterPitchHz.store(0.0f, std::memory_order_relaxed);
    slowMeterTargetHz.store(0.0f, std::memory_order_relaxed);
}

void MicrotonalAutotuneAudioProcessor::prepareToPlay(double sampleRate,
                                                       int samplesPerBlock)
{
    prepared.store(false, std::memory_order_release);
    currentSampleRate = std::isfinite(sampleRate) && sampleRate > 1000.0
        ? sampleRate : 44100.0;
    lastSamplesPerBlock = std::max(1, samplesPerBlock);

    circBufSize = yinWindowSize * 4;
    for (auto& channel : slowChannels)
        channel.circularBuffer.assign(static_cast<std::size_t>(circBufSize), 0.0f);
    yinBuffer.assign(static_cast<std::size_t>(yinWindowSize), 0.0f);
    yinAccumulator.assign(static_cast<std::size_t>(yinWindowSize / 2), 0.0f);
    resetSlowStateNoAllocation();

    const int preparedBlockSize = std::max(lastSamplesPerBlock,
                                           minimumPreparedBlockSize);
    const int channels = std::clamp(getTotalNumOutputChannels(),
                                    1,
                                    ModernPitchEngine::maxSupportedChannels);
    constexpr std::array<ModernPitchEngine::LatencyMode, 3> modes {
        ModernPitchEngine::LatencyMode::ultraLive,
        ModernPitchEngine::LatencyMode::live,
        ModernPitchEngine::LatencyMode::quality
    };

    for (std::size_t index = 0; index < livePitchProcessors.size(); ++index)
    {
        auto& processor = livePitchProcessors[index];
        processor.prepare(currentSampleRate, preparedBlockSize, channels, modes[index]);
        processor.setAdvancedParameters(
            35.0f, 0.70f, 0.20f, 0.90f, 0.85f, 0.70f,
            12.0f, 35.0f, 1600.0f,
            LivePitchProcessor::StereoMode::linkedMidSide);
    }

    publishScaleSnapshot();
    audioScaleSnapshot = readScaleSnapshot();

    const int mode = std::clamp(processingMode.load(std::memory_order_acquire), 0, 3);
    activeLiveProcessorIndex.store(liveIndexForMode(mode), std::memory_order_release);
    setLatencySamples(getLatencyForMode(mode));
    prepared.store(true, std::memory_order_release);
}

void MicrotonalAutotuneAudioProcessor::releaseResources()
{
    prepared.store(false, std::memory_order_release);
    for (auto& processor : livePitchProcessors)
        processor.reset();
    for (auto& channel : slowChannels)
        channel.circularBuffer.clear();
    yinBuffer.clear();
    yinAccumulator.clear();
}

bool MicrotonalAutotuneAudioProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const
{
    const auto& input = layouts.getChannelSet(true, 0);
    const auto& output = layouts.getChannelSet(false, 0);
    return input == output
        && (input == juce::AudioChannelSet::mono()
            || input == juce::AudioChannelSet::stereo());
}

void MicrotonalAutotuneAudioProcessor::updateProcessingMode(int newMode)
{
    newMode = std::clamp(newMode, 0, 3);
    if (processingMode.load(std::memory_order_acquire) == newMode)
        return;

    suspendProcessing(true);
    if (newMode == 0)
        resetSlowStateNoAllocation();
    else
    {
        auto& processor = liveProcessorForMode(newMode);
        processor.reset();
        activeLiveProcessorIndex.store(liveIndexForMode(newMode),
                                       std::memory_order_release);
    }
    processingMode.store(newMode, std::memory_order_release);
    setLatencySamples(getLatencyForMode(newMode));
    suspendProcessing(false);
}

//==============================================================================
float MicrotonalAutotuneAudioProcessor::detectPitchYIN(const float* buffer,
                                                        int numSamples,
                                                        double sampleRate) noexcept
{
    if (buffer == nullptr || numSamples < 4
        || yinAccumulator.size() < static_cast<std::size_t>(numSamples / 2))
        return 0.0f;

    const int halfWindow = numSamples / 2;
    std::fill_n(yinAccumulator.begin(), halfWindow, 0.0f);

    for (int tau = 0; tau < halfWindow; ++tau)
    {
        double sum = 0.0;
        for (int sample = 0; sample < halfWindow; ++sample)
        {
            const double delta = static_cast<double>(buffer[sample])
                               - static_cast<double>(buffer[sample + tau]);
            sum += delta * delta;
        }
        yinAccumulator[static_cast<std::size_t>(tau)] =
            static_cast<float>(std::min(sum,
                static_cast<double>(std::numeric_limits<float>::max())));
    }

    yinAccumulator[0] = 1.0f;
    double runningSum = 0.0;
    for (int tau = 1; tau < halfWindow; ++tau)
    {
        runningSum += yinAccumulator[static_cast<std::size_t>(tau)];
        yinAccumulator[static_cast<std::size_t>(tau)] = runningSum > 1.0e-20
            ? static_cast<float>(yinAccumulator[static_cast<std::size_t>(tau)]
                                 * static_cast<double>(tau) / runningSum)
            : 1.0f;
    }

    constexpr float threshold = 0.15f;
    int tauEstimate = -1;
    for (int tau = 2; tau < halfWindow; ++tau)
    {
        if (yinAccumulator[static_cast<std::size_t>(tau)] < threshold)
        {
            while (tau + 1 < halfWindow
                   && yinAccumulator[static_cast<std::size_t>(tau + 1)]
                        < yinAccumulator[static_cast<std::size_t>(tau)])
                ++tau;
            tauEstimate = tau;
            break;
        }
    }

    if (tauEstimate < 2)
        return 0.0f;

    double refinedTau = static_cast<double>(tauEstimate);
    if (tauEstimate < halfWindow - 1)
    {
        const double left = yinAccumulator[static_cast<std::size_t>(tauEstimate - 1)];
        const double centre = yinAccumulator[static_cast<std::size_t>(tauEstimate)];
        const double right = yinAccumulator[static_cast<std::size_t>(tauEstimate + 1)];
        const double denominator = 2.0 * (2.0 * centre - left - right);
        if (std::abs(denominator) > 1.0e-12)
            refinedTau += (left - right) / denominator;
    }

    const double frequency = refinedTau > 0.0 ? sampleRate / refinedTau : 0.0;
    return std::isfinite(frequency) ? static_cast<float>(frequency) : 0.0f;
}

//==============================================================================
double MicrotonalAutotuneAudioProcessor::getRootFrequency() const noexcept
{
    static const std::array<double, 19> frequencies {
        261.6256, 277.1826, 293.6648, 311.1270, 329.6276, 349.2282,
        369.9944, 391.9954, 415.3047, 440.0000, 466.1638, 493.8833,
        c4Frequency,
        c4Frequency * 1.122462048309373,
        c4Frequency * 1.236067977499790,
        c4Frequency * 1.334839854170034,
        c4Frequency * 1.498307076876682,
        c4Frequency * 1.681792830507429,
        c4Frequency * 1.851749424574581
    };
    const int index = std::clamp(rootNoteIndex.load(std::memory_order_acquire), 0, 18);
    return frequencies[static_cast<std::size_t>(index)];
}

void MicrotonalAutotuneAudioProcessor::selectBuiltInScale(int index) noexcept
{
    currentScaleIndex.store(std::clamp(index, 0,
        std::max(0, ScaleDefinitions::getScaleCount() - 1)),
        std::memory_order_release);
    activeCustomPresetIndex.store(-1, std::memory_order_release);
    publishScaleSnapshot();
}

void MicrotonalAutotuneAudioProcessor::selectCustomPreset(int index) noexcept
{
    if (index >= 0 && index < customPresets.getNumPresets())
        activeCustomPresetIndex.store(index, std::memory_order_release);
    else
        activeCustomPresetIndex.store(-1, std::memory_order_release);
    publishScaleSnapshot();
}

void MicrotonalAutotuneAudioProcessor::selectRootNote(int index) noexcept
{
    rootNoteIndex.store(std::clamp(index, 0, 18), std::memory_order_release);
    publishScaleSnapshot();
}

void MicrotonalAutotuneAudioProcessor::publishScaleSnapshot() noexcept
{
    ScaleSnapshot snapshot;
    snapshot.rootFrequency = getRootFrequency();
    snapshot.revision = scaleSnapshotRevision.fetch_add(1, std::memory_order_relaxed) + 1;

    const std::vector<double>* source = nullptr;
    const int customIndex = activeCustomPresetIndex.load(std::memory_order_acquire);
    if (customIndex >= 0 && customIndex < customPresets.getNumPresets())
        source = &customPresets.getPreset(customIndex).ratios;
    else
    {
        const int builtInIndex = currentScaleIndex.load(std::memory_order_acquire);
        if (builtInIndex >= 0 && builtInIndex < ScaleDefinitions::getScaleCount())
            source = &ScaleDefinitions::getScale(builtInIndex).ratios;
    }

    if (source == nullptr || source->empty())
        source = &ScaleDefinitions::getScale(0).ratios;

    for (const double ratio : *source)
    {
        if (snapshot.count >= maximumScaleRatios)
            break;
        if (std::isfinite(ratio) && ratio >= 1.0 && ratio < 2.0)
            snapshot.ratios[static_cast<std::size_t>(snapshot.count++)] = ratio;
    }

    if (snapshot.count == 0)
    {
        snapshot.ratios[0] = 1.0;
        snapshot.count = 1;
    }

    const int published = publishedScaleSlot.load(std::memory_order_acquire);
    const int reading = audioReadingScaleSlot.load(std::memory_order_acquire);
    int destination = 0;
    for (int candidate = 0; candidate < 3; ++candidate)
    {
        if (candidate != published && candidate != reading)
        {
            destination = candidate;
            break;
        }
    }
    scaleSnapshots[static_cast<std::size_t>(destination)] = snapshot;
    publishedScaleSlot.store(destination, std::memory_order_release);
}

MicrotonalAutotuneAudioProcessor::ScaleSnapshot
MicrotonalAutotuneAudioProcessor::readScaleSnapshot() noexcept
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const int slot = publishedScaleSlot.load(std::memory_order_acquire);
        audioReadingScaleSlot.store(slot, std::memory_order_release);
        if (slot == publishedScaleSlot.load(std::memory_order_acquire))
        {
            ScaleSnapshot snapshot = scaleSnapshots[static_cast<std::size_t>(slot)];
            audioReadingScaleSlot.store(-1, std::memory_order_release);
            audioScaleSnapshot = snapshot;
            return snapshot;
        }
        audioReadingScaleSlot.store(-1, std::memory_order_release);
    }
    return audioScaleSnapshot;
}

std::vector<double> MicrotonalAutotuneAudioProcessor::getCurrentScaleRatios() const
{
    const int customIndex = activeCustomPresetIndex.load(std::memory_order_acquire);
    if (customIndex >= 0 && customIndex < customPresets.getNumPresets())
        return customPresets.getPreset(customIndex).ratios;
    const int builtInIndex = currentScaleIndex.load(std::memory_order_acquire);
    if (builtInIndex >= 0 && builtInIndex < ScaleDefinitions::getScaleCount())
        return ScaleDefinitions::getScale(builtInIndex).ratios;
    return ScaleDefinitions::getScale(0).ratios;
}

double MicrotonalAutotuneAudioProcessor::findNearestTarget(
    double detectedFrequency,
    const ScaleSnapshot& scale) const noexcept
{
    if (!std::isfinite(detectedFrequency) || detectedFrequency <= 0.0
        || scale.count <= 0 || !std::isfinite(scale.rootFrequency)
        || scale.rootFrequency <= 0.0)
        return detectedFrequency;

    const double logRatio = std::log2(detectedFrequency / scale.rootFrequency);
    const double octave = std::floor(logRatio);
    const double fractionalOctave = logRatio - octave;
    double bestDistance = std::numeric_limits<double>::max();
    double bestTarget = logRatio;

    for (int index = 0; index < scale.count; ++index)
    {
        const double ratio = scale.ratios[static_cast<std::size_t>(index)];
        if (!std::isfinite(ratio) || ratio <= 0.0)
            continue;
        const double position = std::log2(ratio);
        for (int wrap = -1; wrap <= 1; ++wrap)
        {
            const double candidate = position + static_cast<double>(wrap);
            const double distance = std::abs(fractionalOctave - candidate);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestTarget = octave + candidate;
            }
        }
    }

    const double target = scale.rootFrequency * std::exp2(bestTarget);
    return std::isfinite(target) ? target : detectedFrequency;
}

//==============================================================================
CreativeTempo::Settings MicrotonalAutotuneAudioProcessor::getTempoSettings() const noexcept
{
    CreativeTempo::Settings settings;
    const auto read = [this](const char* id, float fallback) noexcept
    {
        if (const auto* value = apvts.getRawParameterValue(id))
            return std::isfinite(value->load()) ? value->load() : fallback;
        return fallback;
    };

    settings.mode = static_cast<CreativeTempo::Mode>(std::clamp(
        static_cast<int>(std::lround(read("tempoMode", 0.0f))), 0, 2));
    settings.division = CreativeTempo::divisionFromIndex(std::clamp(
        static_cast<int>(std::lround(read("tempoDivision", 2.0f))), 0, 4));
    settings.glideFraction = std::clamp(read("tempoGlidePercent", 35.0f) / 100.0f,
                                        0.05f, 1.0f);
    settings.lockStrength = std::clamp(read("tempoLockStrength", 100.0f) / 100.0f,
                                       0.0f, 1.0f);
    settings.smartOnset = read("tempoSmartOnset", 1.0f) >= 0.5f;
    settings.smartOnsetWindow = 0.18f;
    settings.fallbackBpm = 120.0;
    return settings;
}

CreativeTempo::HostPosition
MicrotonalAutotuneAudioProcessor::readHostTempoPosition(int numberOfSamples) const noexcept
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
                result.hasBpm = std::isfinite(result.bpm) && result.bpm > 1.0;
            }
            if (const auto ppq = position->getPpqPosition())
            {
                result.ppqAtBlockStart = *ppq;
                result.hasPpq = std::isfinite(result.ppqAtBlockStart);
            }
            if (const auto time = position->getTimeInSamples())
            {
                result.timeInSamples = *time;
                result.hasTimeInSamples = true;
            }
            result.isPlaying = position->getIsPlaying();
            result.isLooping = position->getIsLooping();
        }
    }
    return result;
}

bool MicrotonalAutotuneAudioProcessor::sanitiseInputBuffer(
    juce::AudioBuffer<float>& buffer) noexcept
{
    bool changed = false;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        float* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float value = samples[sample];
            if (!std::isfinite(value)
                || (value != 0.0f && std::abs(value) < std::numeric_limits<float>::min()))
            {
                samples[sample] = 0.0f;
                changed = true;
            }
        }
    }
    return changed;
}

bool MicrotonalAutotuneAudioProcessor::sanitiseOutputBuffer(
    juce::AudioBuffer<float>& buffer) noexcept
{
    bool corrupted = false;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        float* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            if (!std::isfinite(samples[sample]))
            {
                samples[sample] = 0.0f;
                corrupted = true;
            }
            else if (samples[sample] != 0.0f
                     && std::abs(samples[sample]) < std::numeric_limits<float>::min())
                samples[sample] = 0.0f;
        }
    }
    return corrupted;
}

void MicrotonalAutotuneAudioProcessor::recoverActiveDSPState(int mode) noexcept
{
    if (mode == 0)
        resetSlowStateNoAllocation();
    else
        liveProcessorForMode(mode).reset();
}

//==============================================================================
void MicrotonalAutotuneAudioProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int inputChannels = std::min(getTotalNumInputChannels(),
                                      buffer.getNumChannels());
    const int outputChannels = std::min(getTotalNumOutputChannels(),
                                       buffer.getNumChannels());
    const int sampleCount = buffer.getNumSamples();

    for (int channel = inputChannels; channel < outputChannels; ++channel)
        buffer.clear(channel, 0, sampleCount);
    if (sampleCount <= 0 || inputChannels <= 0
        || !prepared.load(std::memory_order_acquire))
        return;

    sanitiseInputBuffer(buffer);
    const float speed = finiteClamped(
        apvts.getRawParameterValue("speed")->load(), 0.0f, 500.0f, 50.0f);
    const float amount = finiteClamped(
        apvts.getRawParameterValue("amount")->load() / 100.0f,
        0.0f, 1.0f, 1.0f);
    const int mode = std::clamp(processingMode.load(std::memory_order_acquire), 0, 3);

    if (mode > 0)
    {
        const ScaleSnapshot scale = readScaleSnapshot();
        auto& processor = liveProcessorForMode(mode);
        processor.setTempoSettings(getTempoSettings());
        processor.setTempoHostPosition(readHostTempoPosition(sampleCount));

        // Amount 0 is a strict latency-aligned bypass. It never enters the
        // resynthesis/crossfade path, preserving stereo width and correlation.
        if (amount <= 1.0e-7f)
            processor.processBypassed(buffer);
        else
            processor.process(buffer,
                              scale.ratios.data(),
                              scale.count,
                              scale.rootFrequency,
                              speed,
                              amount);

        if (sanitiseOutputBuffer(buffer))
            recoverActiveDSPState(mode);
        return;
    }

    const ScaleSnapshot scale = readScaleSnapshot();
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        double detectorSample = 0.0;
        for (int channel = 0; channel < inputChannels; ++channel)
            detectorSample += buffer.getSample(channel, sample);
        detectorSample /= static_cast<double>(inputChannels);
        yinBuffer[static_cast<std::size_t>(yinBufferPos++)] =
            static_cast<float>(detectorSample);

        if (yinBufferPos >= yinWindowSize)
        {
            const float detected = detectPitchYIN(yinBuffer.data(), yinWindowSize,
                                                  currentSampleRate);
            if (std::isfinite(detected) && detected > 20.0f && detected < 5000.0f)
            {
                lastDetectedPitch = detected;
                slowMeterPitchHz.store(detected, std::memory_order_relaxed);
            }
            yinBufferPos = 0;
        }
    }

    double targetRatio = 1.0;
    if (lastDetectedPitch > 20.0f)
    {
        const double target = findNearestTarget(lastDetectedPitch, scale);
        if (std::isfinite(target) && target > 0.0)
        {
            targetRatio = target / static_cast<double>(lastDetectedPitch);
            slowMeterTargetHz.store(static_cast<float>(target),
                                    std::memory_order_relaxed);
        }
    }
    else
        slowMeterTargetHz.store(0.0f, std::memory_order_relaxed);

    const double smoothing = speed > 0.0f
        ? 1.0 - std::exp(-1.0 / (currentSampleRate * speed * 0.001))
        : 1.0;

    const int processedChannels = std::min(outputChannels, maximumSlowChannels);
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        smoothedShiftRatio += smoothing * (targetRatio - smoothedShiftRatio);
        const double effectiveRatio = 1.0
            + (smoothedShiftRatio - 1.0) * static_cast<double>(amount);

        for (int channel = 0; channel < processedChannels; ++channel)
        {
            auto& state = slowChannels[static_cast<std::size_t>(channel)];
            const float input = buffer.getSample(channel, sample);
            state.circularBuffer[static_cast<std::size_t>(state.writePosition)] = input;
            state.writePosition = (state.writePosition + 1) % circBufSize;

            state.readPosition += effectiveRatio;
            while (state.readPosition >= static_cast<double>(circBufSize))
                state.readPosition -= static_cast<double>(circBufSize);
            while (state.readPosition < 0.0)
                state.readPosition += static_cast<double>(circBufSize);

            const int index0 = static_cast<int>(state.readPosition);
            const int index1 = (index0 + 1) % circBufSize;
            const double fraction = state.readPosition - static_cast<double>(index0);
            const float output = static_cast<float>(
                state.circularBuffer[static_cast<std::size_t>(index0)] * (1.0 - fraction)
                + state.circularBuffer[static_cast<std::size_t>(index1)] * fraction);
            buffer.setSample(channel, sample, output);
        }
    }

    if (sanitiseOutputBuffer(buffer))
        recoverActiveDSPState(0);
}

void MicrotonalAutotuneAudioProcessor::processBlockBypassed(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (!prepared.load(std::memory_order_acquire))
        return;
    sanitiseInputBuffer(buffer);

    const int mode = std::clamp(processingMode.load(std::memory_order_acquire), 0, 3);
    if (mode > 0)
        liveProcessorForMode(mode).processBypassed(buffer);
    else
    {
        const int channels = std::min({ buffer.getNumChannels(),
                                        getTotalNumOutputChannels(),
                                        maximumSlowChannels });
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                auto& state = slowChannels[static_cast<std::size_t>(channel)];
                const float input = buffer.getSample(channel, sample);
                state.circularBuffer[static_cast<std::size_t>(state.writePosition)] = input;
                state.writePosition = (state.writePosition + 1) % circBufSize;
                state.readPosition += 1.0;
                if (state.readPosition >= static_cast<double>(circBufSize))
                    state.readPosition -= static_cast<double>(circBufSize);
                buffer.setSample(channel, sample,
                    state.circularBuffer[static_cast<std::size_t>(
                        static_cast<int>(state.readPosition))]);
            }
        }
    }

    if (sanitiseOutputBuffer(buffer))
        recoverActiveDSPState(mode);
}

//==============================================================================
LivePitchProcessor::Metering
MicrotonalAutotuneAudioProcessor::getPitchMetering() const noexcept
{
    const int mode = std::clamp(processingMode.load(std::memory_order_acquire), 0, 3);
    if (mode > 0)
        return liveProcessorForMode(mode).getMetering();

    LivePitchProcessor::Metering meter;
    meter.detectedPitchHz = slowMeterPitchHz.load(std::memory_order_relaxed);
    meter.targetPitchHz = slowMeterTargetHz.load(std::memory_order_relaxed);
    meter.detectorSupport = meter.detectedPitchHz > 0.0f ? 1 : 0;
    if (meter.detectedPitchHz > 0.0f && meter.targetPitchHz > 0.0f)
        meter.correctionCents = 1200.0f
            * std::log2(meter.targetPitchHz / meter.detectedPitchHz);
    meter.state = meter.detectedPitchHz > 0.0f
        ? ModernPitchEngine::TrackingState::stable
        : ModernPitchEngine::TrackingState::unvoiced;
    return meter;
}

#if defined(AUTOTUNE_HEADLESS_TEST)
bool MicrotonalAutotuneAudioProcessor::hasEditor() const { return false; }
juce::AudioProcessorEditor* MicrotonalAutotuneAudioProcessor::createEditor() { return nullptr; }
#else
bool MicrotonalAutotuneAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* MicrotonalAutotuneAudioProcessor::createEditor()
{
    return new MicrotonalAutotuneAudioProcessorEditor(*this);
}
#endif

//==============================================================================
void MicrotonalAutotuneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty("scaleIndex", currentScaleIndex.load(std::memory_order_acquire), nullptr);
    state.setProperty("customPresetIndex",
                      activeCustomPresetIndex.load(std::memory_order_acquire), nullptr);
    state.setProperty("rootNoteIndex", rootNoteIndex.load(std::memory_order_acquire), nullptr);
    state.setProperty("processingMode", processingMode.load(std::memory_order_acquire), nullptr);
    state.removeChild(state.getChildWithName("CustomScales"), nullptr);
    state.addChild(customPresets.toValueTree(), -1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void MicrotonalAutotuneAudioProcessor::setStateInformation(const void* data,
                                                            int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;
    auto state = juce::ValueTree::fromXml(*xml);
    if (!state.isValid())
        return;

    apvts.replaceState(state);
    const auto customTree = state.getChildWithName("CustomScales");
    if (customTree.isValid())
        customPresets.fromValueTree(customTree);

    const int scale = std::clamp(static_cast<int>(state.getProperty("scaleIndex", 0)),
                                 0, std::max(0, ScaleDefinitions::getScaleCount() - 1));
    const int custom = std::clamp(static_cast<int>(state.getProperty(
                                      "customPresetIndex", -1)),
                                  -1, customPresets.getNumPresets() - 1);
    const int root = std::clamp(static_cast<int>(state.getProperty("rootNoteIndex", 9)),
                                0, 18);
    int mode = 0;
    if (state.hasProperty("processingMode"))
        mode = std::clamp(static_cast<int>(state.getProperty("processingMode")), 0, 3);
    else if (state.hasProperty("liveModeEnabled"))
        mode = static_cast<int>(state.getProperty("liveModeEnabled")) != 0 ? 2 : 0;

    currentScaleIndex.store(scale, std::memory_order_release);
    activeCustomPresetIndex.store(custom, std::memory_order_release);
    rootNoteIndex.store(root, std::memory_order_release);
    publishScaleSnapshot();

    if (prepared.load(std::memory_order_acquire))
    {
        const int previous = processingMode.load(std::memory_order_acquire);
        if (previous != mode)
            updateProcessingMode(mode);
        else
            setLatencySamples(getLatencyForMode(mode));
    }
    else
        processingMode.store(mode, std::memory_order_release);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MicrotonalAutotuneAudioProcessor();
}
