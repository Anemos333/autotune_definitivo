// Standalone smoke test for ModernPitchEngine integration
// Verifies: latency reporting, pitch detection, scale quantization, finite output
#include "Source/ModernPitchEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
constexpr double pi = 3.1415926535897932384626433832795;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// ---- Test 1: Latency values ----
void testLatency(ModernPitchEngine::LatencyMode mode, int expectedAt48k, const char* label)
{
    constexpr int sampleRate = 48000;
    ModernPitchEngine engine;
    engine.prepare(sampleRate, 64, 1, mode);
    int actual = engine.getLatencySamples();

    std::cout << "  " << label << ": reported " << actual << " samples";
    require(actual == expectedAt48k, "unexpected latency");
    std::cout << " [OK]\n";

    // Verify bypass impulse position
    juce::AudioBuffer<float> block(1, actual + 128);
    block.clear();
    block.getWritePointer(0)[0] = 1.0f;
    engine.processBypassed(block);

    int impulseIndex = -1;
    for (int i = 0; i < block.getNumSamples(); ++i)
    {
        if (std::abs(block.getReadPointer(0)[i]) > 0.5f)
        {
            impulseIndex = i;
            break;
        }
    }
    require(impulseIndex == actual, "bypass impulse delay differs from reported latency");
    std::cout << "    bypass impulse at sample " << impulseIndex << " [OK]\n";
}

// ---- Test 2: Pitch tracking with scale quantization ----
void testPitchTrackingAndCorrection()
{
    std::cout << "  Pitch tracking + correction (233 Hz -> 220 Hz)... ";

    constexpr int sampleRate = 48000;
    constexpr int totalSamples = sampleRate * 2;
    constexpr int blockSize = 64;
    constexpr double inputFrequency = 233.081880759; // Bb3

    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, ModernPitchEngine::LatencyMode::live);

    ModernPitchEngine::Parameters params;
    params.amount = 1.0f;
    params.retuneTimeMs = 5.0f;
    params.transitionTimeMs = 25.0f;
    params.preserveVibrato = 0.0f;
    params.humanize = 0.0f;
    params.formantPreservation = 0.9f;
    params.transientProtection = 0.8f;
    params.minimumPitchHz = 70.0f;
    params.maximumPitchHz = 1000.0f;

    // Scale: A in every octave (root=440)
    const std::vector<double> scale { 1.0 };

    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - offset);
        juce::AudioBuffer<float> block(1, count);
        float* data = block.getWritePointer(0);
        for (int i = 0; i < count; ++i)
        {
            const int absolute = offset + i;
            const double fade = std::min(1.0, static_cast<double>(absolute) / 256.0);
            data[i] = static_cast<float>(0.25 * fade *
                std::sin(2.0 * pi * inputFrequency *
                         static_cast<double>(absolute) / sampleRate));
        }
        engine.process(block, scale, 440.0, params);

        // Check all output samples are finite
        for (int i = 0; i < count; ++i)
            require(std::isfinite(data[i]), "non-finite sample produced");
    }

    auto meter = engine.getMetering();
    require(std::abs(meter.detectedPitchHz - static_cast<float>(inputFrequency)) < 4.0f,
            "pitch tracker did not converge");
    require(meter.confidence > 0.55f, "pitch confidence too low");
    require(meter.targetPitchHz > 215.0f && meter.targetPitchHz < 225.0f,
            "scale quantizer did not select A3=220 Hz");

    std::cout << "[OK] detected=" << meter.detectedPitchHz
              << " Hz, target=" << meter.targetPitchHz
              << " Hz, confidence=" << meter.confidence << '\n';
}

// ---- Test 3: Microtonal scale support ----
void testMicrotonalScale()
{
    std::cout << "  Microtonal scale (24-EDO, quarter tones)... ";

    constexpr int sampleRate = 48000;
    constexpr int blockSize = 64;
    constexpr int totalSamples = sampleRate;

    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, ModernPitchEngine::LatencyMode::quality);

    ModernPitchEngine::Parameters params;
    params.amount = 1.0f;
    params.retuneTimeMs = 3.0f;

    // 24-EDO: all 24 quarter-tone steps
    std::vector<double> scale24;
    for (int k = 0; k < 24; ++k)
        scale24.push_back(std::pow(2.0, static_cast<double>(k) / 24.0));

    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - offset);
        juce::AudioBuffer<float> block(1, count);
        float* data = block.getWritePointer(0);
        for (int i = 0; i < count; ++i)
        {
            const int absolute = offset + i;
            data[i] = static_cast<float>(0.2 *
                std::sin(2.0 * pi * 450.0 * absolute / sampleRate));
        }
        engine.process(block, scale24, 440.0, params);

        for (int i = 0; i < count; ++i)
            require(std::isfinite(data[i]), "non-finite sample in microtonal mode");
    }

    auto meter = engine.getMetering();
    require(meter.detectedPitchHz > 420.0f && meter.detectedPitchHz < 480.0f,
            "pitch tracker failed on microtonal test");

    std::cout << "[OK] detected=" << meter.detectedPitchHz
              << " Hz, target=" << meter.targetPitchHz << " Hz\n";
}

// ---- Test 4: White noise resilience ----
void testWhiteNoiseResilience()
{
    std::cout << "  White noise resilience... ";

    constexpr int sampleRate = 44100;
    constexpr int blockSize = 128;
    constexpr int totalSamples = sampleRate;

    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, ModernPitchEngine::LatencyMode::ultraLive);

    ModernPitchEngine::Parameters params;
    params.amount = 1.0f;
    params.retuneTimeMs = 10.0f;

    std::vector<double> chromatic;
    for (int k = 0; k < 12; ++k)
        chromatic.push_back(std::pow(2.0, k / 12.0));

    unsigned int seed = 42;
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - offset);
        juce::AudioBuffer<float> block(1, count);
        float* data = block.getWritePointer(0);
        for (int i = 0; i < count; ++i)
        {
            // Simple LCG for deterministic pseudo-random noise
            seed = seed * 1664525u + 1013904223u;
            data[i] = static_cast<float>(static_cast<int>(seed) / 2147483648.0) * 0.3f;
        }
        engine.process(block, chromatic, 440.0, params);

        for (int i = 0; i < count; ++i)
            require(std::isfinite(data[i]), "non-finite output on white noise");
    }

    std::cout << "[OK] no crashes, all output finite\n";
}

// ---- Test 5: Low pitch tracking (55 Hz, eighth-rate) ----
void testLowPitchTracking()
{
    std::cout << "  Low pitch tracking (55 Hz via eighth-rate)... ";

    constexpr int sampleRate = 48000;
    constexpr int blockSize = 64;
    constexpr int totalSamples = sampleRate;
    constexpr double frequency = 55.0;

    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, ModernPitchEngine::LatencyMode::live);

    ModernPitchEngine::Parameters params;
    params.amount = 0.0f; // don't correct, just track
    params.minimumPitchHz = 35.0f;
    params.maximumPitchHz = 500.0f;

    std::vector<double> chromatic;
    for (int k = 0; k < 12; ++k)
        chromatic.push_back(std::pow(2.0, k / 12.0));

    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - offset);
        juce::AudioBuffer<float> block(1, count);
        float* data = block.getWritePointer(0);
        for (int i = 0; i < count; ++i)
        {
            const int absolute = offset + i;
            data[i] = static_cast<float>(0.3 *
                std::sin(2.0 * pi * frequency * absolute / sampleRate));
        }
        engine.process(block, chromatic, 440.0, params);
    }

    auto meter = engine.getMetering();
    require(std::abs(meter.detectedPitchHz - 55.0f) < 2.0f,
            "eighth-rate tracker did not converge on 55 Hz");

    std::cout << "[OK] detected=" << meter.detectedPitchHz << " Hz\n";
}

// ---- Test 6: Byzantine scale (72 moria) ----
void testByzantineScale()
{
    std::cout << "  Byzantine scale (72-moria Diatonic)... ";

    constexpr int sampleRate = 48000;
    constexpr int blockSize = 64;
    constexpr int totalSamples = sampleRate;

    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, ModernPitchEngine::LatencyMode::live);

    ModernPitchEngine::Parameters params;
    params.amount = 1.0f;
    params.retuneTimeMs = 5.0f;

    // Byzantine Mode I: intervals 12-10-8-12-12-10-8 moria
    std::vector<int> intervals = { 12, 10, 8, 12, 12, 10, 8 };
    std::vector<double> ratios;
    ratios.push_back(1.0);
    int cum = 0;
    for (size_t i = 0; i < intervals.size() - 1; ++i)
    {
        cum += intervals[i];
        ratios.push_back(std::pow(2.0, static_cast<double>(cum) / 72.0));
    }

    // Byzantine root Ni = C4 = 261.6256
    double rootFreq = 261.6256;

    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - offset);
        juce::AudioBuffer<float> block(1, count);
        float* data = block.getWritePointer(0);
        for (int i = 0; i < count; ++i)
        {
            const int absolute = offset + i;
            data[i] = static_cast<float>(0.25 *
                std::sin(2.0 * pi * 280.0 * absolute / sampleRate));
        }
        engine.process(block, ratios, rootFreq, params);

        for (int i = 0; i < count; ++i)
            require(std::isfinite(data[i]), "non-finite output on Byzantine scale");
    }

    auto meter = engine.getMetering();
    require(meter.detectedPitchHz > 260.0f && meter.detectedPitchHz < 300.0f,
            "pitch tracker failed on Byzantine test");

    std::cout << "[OK] detected=" << meter.detectedPitchHz
              << " Hz, target=" << meter.targetPitchHz << " Hz\n";
}

// ---- Test 7: Block-size invariance ----
void testBlockSizeInvariance()
{
    std::cout << "  Block-size invariance (17 vs 113)... ";

    auto render = [](int blockSize) -> std::vector<float>
    {
        constexpr int sampleRate = 48000;
        constexpr int totalSamples = 24000;
        ModernPitchEngine engine;
        engine.prepare(sampleRate, blockSize, 1, ModernPitchEngine::LatencyMode::ultraLive);

        ModernPitchEngine::Parameters params;
        params.retuneTimeMs = 7.0f;
        params.transitionTimeMs = 28.0f;
        params.preserveVibrato = 0.5f;

        std::vector<double> scale {
            1.0, 1.122462048309373, 1.2599210498948732,
            1.3348398541700344, 1.4983070768766815,
            1.681792830507429, 1.887748625363387
        };

        std::vector<float> output(static_cast<size_t>(totalSamples), 0.0f);
        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int count = std::min(blockSize, totalSamples - offset);
            juce::AudioBuffer<float> block(1, count);
            float* data = block.getWritePointer(0);
            for (int i = 0; i < count; ++i)
            {
                const int absolute = offset + i;
                const double f = absolute < totalSamples / 2 ? 228.0 : 252.0;
                data[i] = static_cast<float>(
                    0.22 * std::sin(2.0 * pi * f * absolute / sampleRate)
                    + 0.08 * std::sin(4.0 * pi * f * absolute / sampleRate));
            }
            engine.process(block, scale, 440.0, params);
            std::copy_n(data, count, output.begin() + offset);
        }
        return output;
    };

    const auto a = render(17);
    const auto b = render(113);
    require(a.size() == b.size(), "block-size render lengths differ");

    double squaredError = 0.0;
    double squaredSignal = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double delta = static_cast<double>(a[i]) - b[i];
        squaredError += delta * delta;
        squaredSignal += static_cast<double>(a[i]) * a[i];
    }

    double relativeError = std::sqrt(squaredError / std::max(1.0e-20, squaredSignal));
    require(relativeError < 1.0e-6, "DSP output depends on host block size");
    std::cout << "[OK] relative error=" << relativeError << '\n';
}

} // namespace

int main()
{
    std::cout << "=== ModernPitchEngine Integration Tests ===\n\n";

    std::cout << "Test 1: Latency reporting\n";
    testLatency(ModernPitchEngine::LatencyMode::ultraLive, 128, "Ultra Live");
    testLatency(ModernPitchEngine::LatencyMode::live,      256, "Live");
    testLatency(ModernPitchEngine::LatencyMode::quality,   512, "Quality");

    std::cout << "\nTest 2: Pitch tracking + correction\n";
    testPitchTrackingAndCorrection();

    std::cout << "\nTest 3: Microtonal scale support\n";
    testMicrotonalScale();

    std::cout << "\nTest 4: White noise resilience\n";
    testWhiteNoiseResilience();

    std::cout << "\nTest 5: Low pitch tracking\n";
    testLowPitchTracking();

    std::cout << "\nTest 6: Byzantine scale\n";
    testByzantineScale();

    std::cout << "\nTest 7: Block-size invariance\n";
    testBlockSizeInvariance();

    std::cout << "\n=== All integration tests passed! ===\n";
    return 0;
}
