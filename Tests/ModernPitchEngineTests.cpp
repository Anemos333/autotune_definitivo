#include "../Source/ModernPitchEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr double pi = 3.1415926535897932384626433832795;

struct TestContext
{
    int failures = 0;

    void expect(bool condition, const std::string& message)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "[FAIL] " << message << '\n';
        }
        else
        {
            std::cout << "[PASS] " << message << '\n';
        }
    }
};

std::vector<double> chromaticScale()
{
    std::vector<double> scale;
    for (int note = 0; note < 12; ++note)
        scale.push_back(std::exp2(static_cast<double>(note) / 12.0));
    return scale;
}

struct RenderResult
{
    std::vector<float> audio;
    std::vector<ModernPitchEngine::Metering> meters;
};

RenderResult render(const std::vector<float>& input,
                    int blockSize,
                    ModernPitchEngine::LatencyMode mode,
                    const ModernPitchEngine::Parameters& parameters,
                    int meterEveryBlocks = 1)
{
    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, mode);
    auto scale = chromaticScale();

    RenderResult result;
    result.audio = input;
    juce::AudioBuffer<float> block(1, blockSize);
    int blockCounter = 0;

    for (std::size_t offset = 0; offset < input.size(); offset += blockSize)
    {
        const int count = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(blockSize), input.size() - offset));
        auto* data = block.getWritePointer(0);
        std::copy_n(result.audio.data() + offset, count, data);
        std::fill(data + count, data + blockSize, 0.0f);
        engine.process(data, count, scale, 261.625565, parameters);
        std::copy_n(data, count, result.audio.data() + offset);

        if ((blockCounter++ % std::max(1, meterEveryBlocks)) == 0)
            result.meters.push_back(engine.getMetering());
    }

    result.meters.push_back(engine.getMetering());
    return result;
}

std::vector<float> sineSignal(double frequency,
                              double seconds,
                              float amplitude = 0.25f)
{
    const int count = static_cast<int>(std::lround(seconds * sampleRate));
    std::vector<float> signal(static_cast<std::size_t>(count));
    double phase = 0.0;
    const double increment = 2.0 * pi * frequency / sampleRate;
    for (float& sample : signal)
    {
        sample = amplitude * static_cast<float>(std::sin(phase));
        phase += increment;
        if (phase >= 2.0 * pi)
            phase -= 2.0 * pi;
    }
    return signal;
}

ModernPitchEngine::Parameters defaultParameters()
{
    ModernPitchEngine::Parameters parameters;
    parameters.amount = 1.0f;
    parameters.retuneTimeMs = 8.0f;
    parameters.transitionTimeMs = 35.0f;
    parameters.breathReduction = 0.50f;
    return parameters;
}

float maxAbsoluteDifference(const std::vector<float>& a,
                            const std::vector<float>& b)
{
    if (a.size() != b.size())
        return std::numeric_limits<float>::infinity();
    float maximum = 0.0f;
    for (std::size_t index = 0; index < a.size(); ++index)
        maximum = std::max(maximum, std::abs(a[index] - b[index]));
    return maximum;
}

bool allFinite(const std::vector<float>& audio)
{
    return std::all_of(audio.begin(), audio.end(), [](float value)
    {
        return std::isfinite(value);
    });
}

float maximumAbsolute(const std::vector<float>& audio)
{
    float maximum = 0.0f;
    for (float sample : audio)
        maximum = std::max(maximum, std::abs(sample));
    return maximum;
}

void testLatency(TestContext& test)
{
    for (int modeValue = 0; modeValue < 3; ++modeValue)
    {
        const auto mode = static_cast<ModernPitchEngine::LatencyMode>(modeValue);
        const int expected = modeValue == 0 ? 128 : modeValue == 1 ? 256 : 512;
        std::vector<float> impulse(4096, 0.0f);
        impulse[0] = 1.0f;
        auto parameters = defaultParameters();
        parameters.amount = 0.0f;
        const auto rendered = render(impulse, 37, mode, parameters);

        int first = -1;
        for (int index = 0; index < static_cast<int>(rendered.audio.size()); ++index)
        {
            if (std::abs(rendered.audio[static_cast<std::size_t>(index)]) > 0.5f)
            {
                first = index;
                break;
            }
        }
        test.expect(first == expected,
                    "exact impulse latency for mode " + std::to_string(modeValue));
    }
}

void testBlockSizeInvariance(TestContext& test)
{
    std::vector<float> input(static_cast<std::size_t>(sampleRate * 2.0));
    std::mt19937 generator(42);
    std::normal_distribution<float> noise(0.0f, 0.015f);
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const double time = static_cast<double>(index) / sampleRate;
        input[index] = static_cast<float>(
            0.24 * std::sin(2.0 * pi * 231.0 * time)
            + 0.09 * std::sin(2.0 * pi * 462.0 * time)) + noise(generator);
    }

    const auto parameters = defaultParameters();
    const auto small = render(input, 32, ModernPitchEngine::LatencyMode::live,
                              parameters);
    const auto irregular = render(input, 257, ModernPitchEngine::LatencyMode::live,
                                  parameters);
    test.expect(maxAbsoluteDifference(small.audio, irregular.audio) < 2.0e-6f,
                "output is independent from DAW block size");
}

void testPitchAndOctave(TestContext& test)
{
    auto parameters = defaultParameters();
    const auto clean = render(sineSignal(230.0, 2.0), 64,
                              ModernPitchEngine::LatencyMode::live, parameters);
    const auto cleanMeter = clean.meters.back();
    test.expect(std::abs(cleanMeter.detectedPitchHz - 230.0f) < 2.0f,
                "clean pitch detection stays within 2 Hz");

    std::vector<float> harmonicDominant(static_cast<std::size_t>(sampleRate * 2.5));
    for (std::size_t index = 0; index < harmonicDominant.size(); ++index)
    {
        const double time = static_cast<double>(index) / sampleRate;
        harmonicDominant[index] = static_cast<float>(
            0.08 * std::sin(2.0 * pi * 110.0 * time)
            + 0.65 * std::sin(2.0 * pi * 220.0 * time)
            + 0.25 * std::sin(2.0 * pi * 330.0 * time));
    }
    const auto dominant = render(harmonicDominant, 64,
                                 ModernPitchEngine::LatencyMode::live, parameters);
    test.expect(std::abs(dominant.meters.back().detectedPitchHz - 110.0f) < 4.0f,
                "weak fundamental survives a dominant second harmonic");
}

void testNoiseAndPolyphonySafety(TestContext& test)
{
    auto parameters = defaultParameters();
    std::mt19937 generator(1);
    std::normal_distribution<float> white(0.0f, 0.18f);
    std::vector<float> noise(static_cast<std::size_t>(sampleRate * 2.0));
    for (float& sample : noise)
        sample = white(generator);
    const auto noiseResult = render(noise, 64,
                                    ModernPitchEngine::LatencyMode::live,
                                    parameters);
    const auto noiseMeter = noiseResult.meters.back();
    test.expect(noiseMeter.voicing < 0.25f && noiseMeter.wetMix < 0.05f,
                "white noise is not forced through pitch correction");
    test.expect(noiseMeter.spectralReliability < 0.30f,
                "noise-dominant input publishes low spectral reliability");

    const auto clean = render(sineSignal(220.0, 2.0), 64,
                              ModernPitchEngine::LatencyMode::live, parameters);
    std::vector<float> polyphonic(static_cast<std::size_t>(sampleRate * 2.0));
    for (std::size_t index = 0; index < polyphonic.size(); ++index)
    {
        const double time = static_cast<double>(index) / sampleRate;
        polyphonic[index] = static_cast<float>(
            0.30 * std::sin(2.0 * pi * 220.0 * time)
            + 0.30 * std::sin(2.0 * pi * 277.18 * time));
    }
    const auto poly = render(polyphonic, 64,
                             ModernPitchEngine::LatencyMode::live, parameters);
    test.expect(poly.meters.back().polyphony
                    > clean.meters.back().polyphony + 0.05f,
                "competing pitch family raises the polyphony meter");
    test.expect(poly.meters.back().spectralReliability
                    < clean.meters.back().spectralReliability,
                "polyphony reduces correction reliability");
}

void testDynamicAirAndMaskStability(TestContext& test)
{
    auto parameters = defaultParameters();
    std::mt19937 generator(9);
    std::normal_distribution<float> air(0.0f, 0.055f);
    const int count = static_cast<int>(sampleRate * 4.0);
    std::vector<float> sustained(static_cast<std::size_t>(count));
    float previousNoise = 0.0f;
    for (int index = 0; index < count; ++index)
    {
        const double time = static_cast<double>(index) / sampleRate;
        const float rawNoise = air(generator);
        const float highPassed = rawNoise - 0.92f * previousNoise;
        previousNoise = rawNoise;
        sustained[static_cast<std::size_t>(index)] = static_cast<float>(
            0.19 * std::sin(2.0 * pi * 231.0 * time)
            + 0.08 * std::sin(2.0 * pi * 462.0 * time))
            + highPassed;
    }

    const auto rendered = render(sustained, 64,
                                 ModernPitchEngine::LatencyMode::ultraLive,
                                 parameters);
    const auto meter = rendered.meters.back();
    test.expect(meter.sustainedNoteSeconds > 1.0f,
                "stable note age is tracked in seconds");
    test.expect(meter.maskStability > 0.92f,
                "short-window harmonic mask remains temporally stable");
    test.expect(meter.noiseReductionDb < 7.0f,
                "long airy note keeps a natural residual floor");
}

void testFiniteAndBounded(TestContext& test)
{
    std::mt19937 generator(123);
    std::uniform_real_distribution<float> random(-0.6f, 0.6f);
    std::vector<float> input(static_cast<std::size_t>(sampleRate));
    for (float& sample : input)
        sample = random(generator);

    for (int modeValue = 0; modeValue < 3; ++modeValue)
    {
        const auto rendered = render(
            input,
            113,
            static_cast<ModernPitchEngine::LatencyMode>(modeValue),
            defaultParameters());
        test.expect(allFinite(rendered.audio),
                    "finite output for mode " + std::to_string(modeValue));
        test.expect(maximumAbsolute(rendered.audio) < 4.0f,
                    "bounded output for mode " + std::to_string(modeValue));
    }
}
} // namespace

int main()
{
    TestContext test;
    testLatency(test);
    testBlockSizeInvariance(test);
    testPitchAndOctave(test);
    testNoiseAndPolyphonySafety(test);
    testDynamicAirAndMaskStability(test);
    testFiniteAndBounded(test);

    if (test.failures != 0)
    {
        std::cerr << test.failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All ModernPitchEngine regression tests passed.\n";
    return 0;
}
