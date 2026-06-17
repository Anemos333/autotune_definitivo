#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <vector>

namespace allocation_probe
{
std::atomic<bool> enabled { false };
std::atomic<std::uint64_t> count { 0 };
}

void* operator new(std::size_t size)
{
    if (allocation_probe::enabled.load(std::memory_order_relaxed))
        allocation_probe::count.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
constexpr double pi = 3.1415926535897932384626433832795;

struct Result
{
    int failures = 0;

    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    }
};

void setParameter(MicrotonalAutotuneAudioProcessor& processor,
                  const char* id,
                  float plainValue)
{
    if (auto* parameter = processor.getAPVTS().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
}

bool allFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (!std::isfinite(buffer.getSample(channel, sample)))
                return false;
    return true;
}

void fillSignal(juce::AudioBuffer<float>& buffer,
                double sampleRate,
                std::int64_t startSample,
                double frequency,
                float amplitude = 0.25f)
{
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const double time = static_cast<double>(startSample + sample) / sampleRate;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const double channelFrequency = frequency * (1.0 + 0.017 * channel);
            const double phase = channel == 0 ? 0.0 : 0.63;
            buffer.setSample(channel, sample,
                amplitude * static_cast<float>(std::sin(2.0 * pi * channelFrequency * time + phase)));
        }
    }
}

float correlation(const std::vector<float>& left,
                  const std::vector<float>& right)
{
    if (left.size() != right.size() || left.empty())
        return 0.0f;
    double sumL = 0.0, sumR = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        sumL += left[index];
        sumR += right[index];
    }
    const double meanL = sumL / left.size();
    const double meanR = sumR / right.size();
    double cross = 0.0, energyL = 0.0, energyR = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const double l = left[index] - meanL;
        const double r = right[index] - meanR;
        cross += l * r;
        energyL += l * l;
        energyR += r * r;
    }
    return static_cast<float>(cross / std::sqrt(std::max(1.0e-30, energyL * energyR)));
}

float stereoWidth(const std::vector<float>& left,
                  const std::vector<float>& right)
{
    double mid = 0.0, side = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const double m = 0.5 * (left[index] + right[index]);
        const double s = 0.5 * (left[index] - right[index]);
        mid += m * m;
        side += s * s;
    }
    return static_cast<float>(std::sqrt(side / std::max(1.0e-30, mid)));
}

void prepareStereo(MicrotonalAutotuneAudioProcessor& processor,
                   double sampleRate,
                   int blockSize)
{
    const auto layout = juce::AudioProcessor::BusesLayout {
        { juce::AudioChannelSet::stereo() },
        { juce::AudioChannelSet::stereo() }
    };
    if (!processor.setBusesLayout(layout))
        throw std::runtime_error("cannot configure stereo layout");
    processor.prepareToPlay(sampleRate, blockSize);
}

void testAmountZeroStereo(Result& result)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 257;
    constexpr int totalSamples = 48000;

    for (int mode = 0; mode < 4; ++mode)
    {
        MicrotonalAutotuneAudioProcessor processor;
        prepareStereo(processor, sampleRate, blockSize);
        processor.updateProcessingMode(mode);
        setParameter(processor, "amount", 0.0f);

        std::vector<float> inputL(totalSamples), inputR(totalSamples);
        std::vector<float> outputL(totalSamples), outputR(totalSamples);
        juce::MidiBuffer midi;
        std::int64_t position = 0;
        while (position < totalSamples)
        {
            const int count = std::min(blockSize,
                totalSamples - static_cast<int>(position));
            juce::AudioBuffer<float> block(2, count);
            fillSignal(block, sampleRate, position, 146.832);
            for (int sample = 0; sample < count; ++sample)
            {
                inputL[static_cast<std::size_t>(position + sample)] = block.getSample(0, sample);
                inputR[static_cast<std::size_t>(position + sample)] = block.getSample(1, sample);
            }
            processor.processBlock(block, midi);
            for (int sample = 0; sample < count; ++sample)
            {
                outputL[static_cast<std::size_t>(position + sample)] = block.getSample(0, sample);
                outputR[static_cast<std::size_t>(position + sample)] = block.getSample(1, sample);
            }
            position += count;
        }

        const int latency = processor.getLatencySamples();
        std::vector<float> expectedL, expectedR, actualL, actualR;
        for (int sample = latency + 1024; sample < totalSamples; ++sample)
        {
            expectedL.push_back(inputL[static_cast<std::size_t>(sample - latency)]);
            expectedR.push_back(inputR[static_cast<std::size_t>(sample - latency)]);
            actualL.push_back(outputL[static_cast<std::size_t>(sample)]);
            actualR.push_back(outputR[static_cast<std::size_t>(sample)]);
        }

        float maximumError = 0.0f;
        for (std::size_t index = 0; index < actualL.size(); ++index)
        {
            maximumError = std::max(maximumError,
                std::abs(actualL[index] - expectedL[index]));
            maximumError = std::max(maximumError,
                std::abs(actualR[index] - expectedR[index]));
        }
        const float correlationDelta = std::abs(
            correlation(actualL, actualR) - correlation(expectedL, expectedR));
        const float widthDelta = std::abs(
            stereoWidth(actualL, actualR) - stereoWidth(expectedL, expectedR));

        result.require(maximumError < 2.0e-5f,
            "Amount 0 sample error mode " + std::to_string(mode));
        result.require(correlationDelta < 2.0e-4f,
            "Amount 0 correlation changed mode " + std::to_string(mode));
        result.require(widthDelta < 2.0e-4f,
            "Amount 0 width changed mode " + std::to_string(mode));
    }
}

void testNonFiniteAndImpulses(Result& result)
{
    MicrotonalAutotuneAudioProcessor processor;
    prepareStereo(processor, 48000.0, 4096);
    juce::MidiBuffer midi;

    for (int mode = 0; mode < 4; ++mode)
    {
        processor.updateProcessingMode(mode);
        juce::AudioBuffer<float> block(2, 4096);
        block.clear();
        block.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
        block.setSample(1, 1, std::numeric_limits<float>::infinity());
        block.setSample(0, 2, -std::numeric_limits<float>::infinity());
        block.setSample(1, 3, std::numeric_limits<float>::denorm_min());
        block.setSample(0, 4, 1.0f);
        block.setSample(1, 4, -1.0f);
        processor.processBlock(block, midi);
        result.require(allFinite(block),
            "non-finite output mode " + std::to_string(mode));
    }
}

void testNoAudioThreadAllocations(Result& result)
{
    MicrotonalAutotuneAudioProcessor processor;
    prepareStereo(processor, 48000.0, 4096);
    processor.updateProcessingMode(2);
    setParameter(processor, "amount", 100.0f);
    juce::AudioBuffer<float> block(2, 4096);
    juce::MidiBuffer midi;
    fillSignal(block, 48000.0, 0, 220.0);
    processor.processBlock(block, midi); // warm all lazy paths

    allocation_probe::count.store(0, std::memory_order_relaxed);
    allocation_probe::enabled.store(true, std::memory_order_release);
    for (int iteration = 0; iteration < 200; ++iteration)
    {
        fillSignal(block, 48000.0,
                   static_cast<std::int64_t>(iteration) * block.getNumSamples(),
                   220.0 + (iteration % 13));
        processor.processBlock(block, midi);
    }
    allocation_probe::enabled.store(false, std::memory_order_release);
    result.require(allocation_probe::count.load(std::memory_order_relaxed) == 0,
                   "allocation detected after prepareToPlay");
}

void testRandomParameters(Result& result, bool longProfile)
{
    const int iterations = longProfile ? 100000 : 5000;
    std::mt19937 rng(0xA17E5EEDu);
    std::uniform_real_distribution<float> amount(0.0f, 100.0f);
    std::uniform_real_distribution<float> speed(0.0f, 500.0f);
    std::uniform_int_distribution<int> mode(0, 3);
    std::uniform_int_distribution<int> blockSize(1, 4096);
    std::uniform_int_distribution<int> root(0, 18);

    MicrotonalAutotuneAudioProcessor processor;
    prepareStereo(processor, 48000.0, 4096);
    juce::MidiBuffer midi;
    std::int64_t position = 0;
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        if ((iteration % 100) == 0)
            processor.updateProcessingMode(mode(rng));
        processor.selectRootNote(root(rng));
        processor.selectBuiltInScale(iteration % ScaleDefinitions::getScaleCount());
        setParameter(processor, "amount", amount(rng));
        setParameter(processor, "speed", speed(rng));

        const int count = blockSize(rng);
        juce::AudioBuffer<float> block(2, count);
        fillSignal(block, 48000.0, position, 55.0 + (iteration % 1000));
        processor.processBlock(block, midi);
        if (!allFinite(block))
        {
            result.require(false, "random parameter fuzz produced non-finite output");
            break;
        }
        position += count;
    }
}

void testStateRoundTrips(Result& result, bool longProfile)
{
    const int iterations = longProfile ? 10000 : 500;
    MicrotonalAutotuneAudioProcessor source;
    MicrotonalAutotuneAudioProcessor restored;
    prepareStereo(source, 44100.0, 512);
    prepareStereo(restored, 44100.0, 512);

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        source.selectBuiltInScale(iteration % ScaleDefinitions::getScaleCount());
        source.selectRootNote(iteration % 19);
        source.updateProcessingMode(iteration % 4);
        setParameter(source, "amount", static_cast<float>(iteration % 101));
        setParameter(source, "speed", static_cast<float>(iteration % 501));

        juce::MemoryBlock first;
        source.getStateInformation(first);
        restored.setStateInformation(first.getData(), static_cast<int>(first.getSize()));
        juce::MemoryBlock second;
        restored.getStateInformation(second);
        result.require(first == second,
            "state round-trip is not deterministic at iteration "
            + std::to_string(iteration));
        if (first != second)
            break;
    }
}

void testLifecycleMatrix(Result& result, bool longProfile)
{
    const std::vector<double> sampleRates = longProfile
        ? std::vector<double> { 22050.0, 32000.0, 44100.0, 48000.0,
                                88200.0, 96000.0, 176400.0, 192000.0 }
        : std::vector<double> { 44100.0, 48000.0, 96000.0 };
    const std::vector<int> blockSizes = longProfile
        ? std::vector<int> { 1, 2, 3, 7, 16, 31, 64, 127, 256, 511,
                             1024, 2048, 4096 }
        : std::vector<int> { 1, 64, 257, 4096 };

    MicrotonalAutotuneAudioProcessor processor;
    const auto layout = juce::AudioProcessor::BusesLayout {
        { juce::AudioChannelSet::stereo() },
        { juce::AudioChannelSet::stereo() }
    };
    result.require(processor.setBusesLayout(layout), "stereo layout setup");
    juce::MidiBuffer midi;

    int modeChanges = 0;
    for (double sampleRate : sampleRates)
    {
        for (int blockSize : blockSizes)
        {
            processor.prepareToPlay(sampleRate, blockSize);
            for (int mode = 0; mode < 4; ++mode)
            {
                processor.updateProcessingMode(mode);
                ++modeChanges;
                juce::AudioBuffer<float> block(2, blockSize);
                fillSignal(block, sampleRate, 0, 82.407);
                processor.processBlock(block, midi);
                result.require(allFinite(block), "lifecycle matrix finite output");
            }
            processor.releaseResources();
        }
    }

    const int requestedChanges = longProfile ? 1000 : 100;
    processor.prepareToPlay(48000.0, 512);
    for (; modeChanges < requestedChanges; ++modeChanges)
        processor.updateProcessingMode(modeChanges % 4);
}
} // namespace

int main(int argc, char* argv[])
{
    const bool longProfile = argc > 1 && std::string(argv[1]) == "--long";
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    Result result;

    testAmountZeroStereo(result);
    testNonFiniteAndImpulses(result);
    testNoAudioThreadAllocations(result);
    testRandomParameters(result, longProfile);
    testStateRoundTrips(result, longProfile);
    testLifecycleMatrix(result, longProfile);

    std::cout << "profile=" << (longProfile ? "long" : "ci")
              << " failures=" << result.failures << '\n';
    return result.failures == 0 ? 0 : 1;
}
