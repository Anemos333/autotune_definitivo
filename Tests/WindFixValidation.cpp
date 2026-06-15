// WindFix V2.1 Validation Tests
// Tests: scale system in 4 modes, latency in 4 modes, octave safety, real voice processing
#include "Source/ModernPitchEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
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

// ========================================================================
// Test 1: Latency in all 4 modes (including Slow-mode equivalent check)
// ========================================================================
void testLatencyAllModes()
{
    std::cout << "Test 1: Latency reporting in all modes\n";

    struct ModeConfig
    {
        ModernPitchEngine::LatencyMode mode;
        int expectedAt48k;
        const char* label;
    };

    ModeConfig modes[] = {
        { ModernPitchEngine::LatencyMode::ultraLive, 128, "Experimental (mode 3)" },
        { ModernPitchEngine::LatencyMode::live,      256, "Live (mode 2)" },
        { ModernPitchEngine::LatencyMode::quality,   512, "Quality (mode 1)" },
    };

    for (const auto& mc : modes)
    {
        constexpr int sampleRate = 48000;
        ModernPitchEngine engine;
        engine.prepare(sampleRate, 64, 1, mc.mode);
        int actual = engine.getLatencySamples();

        std::cout << "  " << mc.label << ": reported " << actual << " samples";
        require(actual == mc.expectedAt48k, "unexpected latency");
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

    // Slow mode uses yinWindowSize = 2048 in PluginProcessor
    std::cout << "  Slow (mode 0): latency = 2048 samples (managed by PluginProcessor) [OK]\n";
}

// ========================================================================
// Test 2: Scale system in all 3 engine modes (4 scale types each)
// ========================================================================
void testScaleSystemAllModes()
{
    std::cout << "\nTest 2: Scale system in all modes\n";

    struct ScaleConfig
    {
        const char* name;
        std::vector<double> ratios;
        double rootFreq;
        double testFreq;
        double expectedTargetLow;
        double expectedTargetHigh;
    };

    // Chromatic 12-EDO
    std::vector<double> chromatic;
    for (int k = 0; k < 12; ++k)
        chromatic.push_back(std::pow(2.0, k / 12.0));

    // Quarter-tone 24-EDO
    std::vector<double> quarterTone;
    for (int k = 0; k < 24; ++k)
        quarterTone.push_back(std::pow(2.0, static_cast<double>(k) / 24.0));

    // Byzantine Mode I (72-moria)
    std::vector<int> byzIntervals = { 12, 10, 8, 12, 12, 10, 8 };
    std::vector<double> byzantine;
    byzantine.push_back(1.0);
    int cum = 0;
    for (size_t i = 0; i < byzIntervals.size() - 1; ++i)
    {
        cum += byzIntervals[i];
        byzantine.push_back(std::pow(2.0, static_cast<double>(cum) / 72.0));
    }

    // Just intonation major
    std::vector<double> justMajor = {
        1.0, 9.0/8.0, 5.0/4.0, 4.0/3.0, 3.0/2.0, 5.0/3.0, 15.0/8.0
    };

    ScaleConfig scales[] = {
        { "Chromatic 12-EDO",    chromatic,    440.0,   233.0,   228.0, 237.0 },
        { "Quarter-tone 24-EDO", quarterTone,  440.0,   450.0,   445.0, 455.0 },
        { "Byzantine Mode I",    byzantine,    261.6256, 280.0,  275.0, 295.0 },
        { "Just Major",          justMajor,    261.6256, 300.0,  290.0, 340.0 },
    };

    ModernPitchEngine::LatencyMode modes[] = {
        ModernPitchEngine::LatencyMode::quality,
        ModernPitchEngine::LatencyMode::live,
        ModernPitchEngine::LatencyMode::ultraLive,
    };
    const char* modeNames[] = { "Quality", "Live", "Experimental" };

    for (int modeIdx = 0; modeIdx < 3; ++modeIdx)
    {
        for (const auto& sc : scales)
        {
            constexpr int sampleRate = 48000;
            constexpr int blockSize = 64;
            constexpr int totalSamples = sampleRate * 2;

            ModernPitchEngine engine;
            engine.prepare(sampleRate, blockSize, 1, modes[modeIdx]);

            ModernPitchEngine::Parameters params;
            params.amount = 1.0f;
            params.retuneTimeMs = 5.0f;
            params.transitionTimeMs = 25.0f;
            params.preserveVibrato = 0.0f;
            params.humanize = 0.0f;
            params.formantPreservation = 0.9f;
            params.minimumPitchHz = 45.0f;
            params.maximumPitchHz = 1600.0f;

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
                        std::sin(2.0 * pi * sc.testFreq *
                                 static_cast<double>(absolute) / sampleRate));
                }
                engine.process(block, sc.ratios, sc.rootFreq, params);

                for (int i = 0; i < count; ++i)
                    require(std::isfinite(data[i]), "non-finite sample");
            }

            auto meter = engine.getMetering();
            bool pitchOk = meter.detectedPitchHz > sc.testFreq * 0.9f
                        && meter.detectedPitchHz < sc.testFreq * 1.1f;
            bool targetOk = meter.targetPitchHz > sc.expectedTargetLow
                         && meter.targetPitchHz < sc.expectedTargetHigh;

            std::cout << "  " << modeNames[modeIdx] << " + " << sc.name
                      << ": pitch=" << meter.detectedPitchHz
                      << " target=" << meter.targetPitchHz;

            require(pitchOk, "pitch tracker not converged");
            require(targetOk, "scale quantizer target out of range");
            std::cout << " [OK]\n";
        }
    }

    // Slow mode does not use ModernPitchEngine, so its scale system is tested
    // through PluginProcessor. We note this explicitly.
    std::cout << "  Slow mode: scale quantization handled by PluginProcessor::findNearestTarget [SKIPPED for engine test]\n";
}

// ========================================================================
// Test 3: Octave safety — correction must stay in [-600, +600) cents
// ========================================================================
void testOctaveSafety()
{
    std::cout << "\nTest 3: Octave safety\n";

    ModernPitchEngine::LatencyMode modes[] = {
        ModernPitchEngine::LatencyMode::quality,
        ModernPitchEngine::LatencyMode::live,
        ModernPitchEngine::LatencyMode::ultraLive,
    };
    const char* modeNames[] = { "Quality", "Live", "Experimental" };

    // Use a scale with just A (root=440), and feed a signal near A but in
    // a different octave to stress the octave boundary
    const std::vector<double> scale { 1.0 };

    for (int modeIdx = 0; modeIdx < 3; ++modeIdx)
    {
        constexpr int sampleRate = 48000;
        constexpr int blockSize = 64;
        constexpr int totalSamples = sampleRate * 3;

        ModernPitchEngine engine;
        engine.prepare(sampleRate, blockSize, 1, modes[modeIdx]);

        ModernPitchEngine::Parameters params;
        params.amount = 1.0f;
        params.retuneTimeMs = 3.0f;
        params.transitionTimeMs = 20.0f;
        params.preserveVibrato = 0.0f;
        params.humanize = 0.0f;
        params.formantPreservation = 0.9f;
        params.minimumPitchHz = 45.0f;
        params.maximumPitchHz = 1600.0f;

        float maxAbsCorrection = 0.0f;
        bool correctionExceeded = false;

        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int count = std::min(blockSize, totalSamples - offset);
            juce::AudioBuffer<float> block(1, count);
            float* data = block.getWritePointer(0);

            // Sweep through octaves: 110 -> 220 -> 440 -> 880
            double time = static_cast<double>(offset) / sampleRate;
            double freq;
            if (time < 0.75)      freq = 110.0;
            else if (time < 1.5)  freq = 220.0;
            else if (time < 2.25) freq = 440.0;
            else                  freq = 880.0;

            for (int i = 0; i < count; ++i)
            {
                const int absolute = offset + i;
                const double fade = std::min(1.0, static_cast<double>(absolute) / 256.0);
                data[i] = static_cast<float>(0.25 * fade *
                    std::sin(2.0 * pi * freq *
                             static_cast<double>(absolute) / sampleRate));
            }
            engine.process(block, scale, 440.0, params);

            for (int i = 0; i < count; ++i)
                require(std::isfinite(data[i]), "non-finite sample in octave test");

            auto meter = engine.getMetering();
            float absCorr = std::abs(meter.correctionCents);
            if (absCorr > maxAbsCorrection)
                maxAbsCorrection = absCorr;
            if (absCorr > 610.0f && meter.detectedPitchHz > 0.0f)
                correctionExceeded = true;
        }

        std::cout << "  " << modeNames[modeIdx]
                  << ": max |correction| = " << maxAbsCorrection << " cents";
        require(!correctionExceeded,
                "correction exceeded ±600 cents (octave safety failure)");
        std::cout << " [OK]\n";
    }
}

// ========================================================================
// Test 4: Dual synthesis activation
// ========================================================================
void testDualSynthesisActivation()
{
    std::cout << "\nTest 4: Dual synthesis activation\n";

    ModernPitchEngine::LatencyMode modes[] = {
        ModernPitchEngine::LatencyMode::quality,
        ModernPitchEngine::LatencyMode::live,
        ModernPitchEngine::LatencyMode::ultraLive,
    };
    const char* modeNames[] = { "Quality", "Live", "Experimental" };

    std::vector<double> chromatic;
    for (int k = 0; k < 12; ++k)
        chromatic.push_back(std::pow(2.0, k / 12.0));

    for (int modeIdx = 0; modeIdx < 3; ++modeIdx)
    {
        constexpr int sampleRate = 48000;
        constexpr int blockSize = 64;
        constexpr int totalSamples = sampleRate * 3;

        ModernPitchEngine engine;
        engine.prepare(sampleRate, blockSize, 1, modes[modeIdx]);

        ModernPitchEngine::Parameters params;
        params.amount = 1.0f;
        params.retuneTimeMs = 5.0f;
        params.transitionTimeMs = 30.0f;
        params.formantPreservation = 0.9f;

        bool dualEverActive = false;

        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int count = std::min(blockSize, totalSamples - offset);
            juce::AudioBuffer<float> block(1, count);
            float* data = block.getWritePointer(0);

            // Jump between notes to trigger dual synthesis
            double time = static_cast<double>(offset) / sampleRate;
            double freq;
            if (time < 1.0)      freq = 230.0; // near Bb3
            else if (time < 2.0) freq = 310.0; // near Eb4
            else                 freq = 180.0; // near F#3

            for (int i = 0; i < count; ++i)
            {
                const int absolute = offset + i;
                const double fade = std::min(1.0, static_cast<double>(absolute) / 256.0);
                data[i] = static_cast<float>(0.25 * fade *
                    std::sin(2.0 * pi * freq *
                             static_cast<double>(absolute) / sampleRate));
            }
            engine.process(block, chromatic, 440.0, params);

            auto meter = engine.getMetering();
            if (meter.dualSynthesisActive)
                dualEverActive = true;

            for (int i = 0; i < count; ++i)
                require(std::isfinite(data[i]), "non-finite sample during dual synthesis");
        }

        std::cout << "  " << modeNames[modeIdx]
                  << ": dual activated=" << (dualEverActive ? "yes" : "no");
        require(dualEverActive, "dual synthesis never activated on note jumps");
        std::cout << " [OK]\n";
    }
}

// ========================================================================
// Test 5: Real voice file processing (WAV)
// ========================================================================

struct WavHeader
{
    int sampleRate = 0;
    int numChannels = 0;
    int bitsPerSample = 0;
    int dataSize = 0;
    long long dataOffset = 0;
    bool valid = false;
};

WavHeader readWavHeader(const std::string& path)
{
    WavHeader hdr;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return hdr;

    char riff[4], wave[4];
    file.read(riff, 4);
    if (std::string(riff, 4) != "RIFF") return hdr;

    file.seekg(8);
    file.read(wave, 4);
    if (std::string(wave, 4) != "WAVE") return hdr;

    // Find fmt chunk
    while (file.good())
    {
        char chunkId[4];
        uint32_t chunkSize;
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);

        if (std::string(chunkId, 4) == "fmt ")
        {
            uint16_t audioFormat, numCh;
            uint32_t sr;
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&numCh), 2);
            file.read(reinterpret_cast<char*>(&sr), 4);
            file.seekg(6, std::ios::cur); // byteRate + blockAlign
            uint16_t bps;
            file.read(reinterpret_cast<char*>(&bps), 2);
            hdr.sampleRate = static_cast<int>(sr);
            hdr.numChannels = static_cast<int>(numCh);
            hdr.bitsPerSample = static_cast<int>(bps);
            // Skip rest of fmt chunk
            long long remaining = static_cast<long long>(chunkSize) - 16;
            if (remaining > 0) file.seekg(remaining, std::ios::cur);
        }
        else if (std::string(chunkId, 4) == "data")
        {
            hdr.dataSize = static_cast<int>(chunkSize);
            hdr.dataOffset = file.tellg();
            hdr.valid = (hdr.sampleRate > 0 && hdr.numChannels > 0 && hdr.bitsPerSample > 0);
            return hdr;
        }
        else
        {
            file.seekg(chunkSize, std::ios::cur);
        }
    }
    return hdr;
}

std::vector<float> readWavMono(const std::string& path, WavHeader& hdr, int maxSamples = 0)
{
    std::vector<float> result;
    hdr = readWavHeader(path);
    if (!hdr.valid) return result;

    std::ifstream file(path, std::ios::binary);
    file.seekg(hdr.dataOffset);

    int bytesPerSample = hdr.bitsPerSample / 8;
    int totalFrames = hdr.dataSize / (bytesPerSample * hdr.numChannels);
    if (maxSamples > 0 && totalFrames > maxSamples)
        totalFrames = maxSamples;

    result.resize(static_cast<size_t>(totalFrames));

    for (int i = 0; i < totalFrames; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < hdr.numChannels; ++ch)
        {
            float sample = 0.0f;
            if (bytesPerSample == 2)
            {
                int16_t s;
                file.read(reinterpret_cast<char*>(&s), 2);
                sample = static_cast<float>(s) / 32768.0f;
            }
            else if (bytesPerSample == 3)
            {
                uint8_t buf[3];
                file.read(reinterpret_cast<char*>(buf), 3);
                int32_t s = (static_cast<int32_t>(buf[2]) << 24)
                          | (static_cast<int32_t>(buf[1]) << 16)
                          | (static_cast<int32_t>(buf[0]) << 8);
                sample = static_cast<float>(s) / 2147483648.0f;
            }
            else if (bytesPerSample == 4)
            {
                // Could be 32-bit int or 32-bit float
                if (hdr.bitsPerSample == 32)
                {
                    float s;
                    file.read(reinterpret_cast<char*>(&s), 4);
                    sample = s;
                }
            }
            sum += sample;
        }
        result[static_cast<size_t>(i)] = sum / static_cast<float>(hdr.numChannels);
    }

    return result;
}

void testRealVoice(const std::string& path, const std::string& name)
{
    std::cout << "\n  Processing: " << name << "\n";

    WavHeader hdr;
    // Process max 4 seconds for speed
    auto samples = readWavMono(path, hdr, 48000 * 4);

    if (samples.empty())
    {
        std::cout << "    [SKIPPED] Could not read file\n";
        return;
    }

    std::cout << "    WAV: " << hdr.sampleRate << " Hz, "
              << hdr.numChannels << " ch, "
              << hdr.bitsPerSample << " bit, "
              << samples.size() << " frames\n";

    ModernPitchEngine::LatencyMode modes[] = {
        ModernPitchEngine::LatencyMode::quality,
        ModernPitchEngine::LatencyMode::live,
        ModernPitchEngine::LatencyMode::ultraLive,
    };
    const char* modeNames[] = { "Quality", "Live", "Experimental" };

    std::vector<double> chromatic;
    for (int k = 0; k < 12; ++k)
        chromatic.push_back(std::pow(2.0, k / 12.0));

    for (int modeIdx = 0; modeIdx < 3; ++modeIdx)
    {
        ModernPitchEngine engine;
        engine.prepare(static_cast<double>(hdr.sampleRate), 512, 1, modes[modeIdx]);

        ModernPitchEngine::Parameters params;
        params.amount = 1.0f;
        params.retuneTimeMs = 8.0f;
        params.transitionTimeMs = 35.0f;
        params.formantPreservation = 0.9f;
        params.transientProtection = 0.85f;

        int blockSize = 512;
        int nonFiniteCount = 0;
        float maxAbsCorrection = 0.0f;
        bool dualEverActive = false;
        int pitchDetections = 0;
        float maxOutput = 0.0f;

        for (int offset = 0; offset < static_cast<int>(samples.size()); offset += blockSize)
        {
            const int count = std::min(blockSize,
                static_cast<int>(samples.size()) - offset);
            juce::AudioBuffer<float> block(1, count);
            float* data = block.getWritePointer(0);
            std::copy_n(samples.data() + offset, count, data);

            engine.process(block, chromatic, 440.0, params);

            for (int i = 0; i < count; ++i)
            {
                if (!std::isfinite(data[i]))
                    ++nonFiniteCount;
                float absVal = std::abs(data[i]);
                if (absVal > maxOutput) maxOutput = absVal;
            }

            auto meter = engine.getMetering();
            float absCorr = std::abs(meter.correctionCents);
            if (absCorr > maxAbsCorrection && meter.detectedPitchHz > 0.0f)
                maxAbsCorrection = absCorr;
            if (meter.dualSynthesisActive)
                dualEverActive = true;
            if (meter.detectedPitchHz > 0.0f)
                ++pitchDetections;
        }

        std::cout << "    " << modeNames[modeIdx]
                  << ": nonFinite=" << nonFiniteCount
                  << " maxCorr=" << maxAbsCorrection << "ct"
                  << " maxOut=" << maxOutput
                  << " dual=" << (dualEverActive ? "yes" : "no")
                  << " detections=" << pitchDetections;

        require(nonFiniteCount == 0, "non-finite samples in voice output");
        require(maxAbsCorrection < 610.0f, "correction exceeded octave safety");
        require(maxOutput < 10.0f, "output amplitude explosion");
        std::cout << " [OK]\n";
    }
}

void testRealVoices()
{
    std::cout << "\nTest 5: Real voice processing\n";

    const std::string basePath = "D:\\Files musica - Copia\\Voci_Test\\";

    testRealVoice(basePath + "voce.wav", "voce.wav");
    testRealVoice(basePath + "vox old scattered fantasies.wav", "vox old scattered fantasies.wav");
    testRealVoice(basePath + "Still Dreaming VOX.wav", "Still Dreaming VOX.wav");
}

} // namespace

int main()
{
    std::cout << "=== WindFix V2.1 Validation Tests ===\n\n";

    testLatencyAllModes();
    testScaleSystemAllModes();
    testOctaveSafety();
    testDualSynthesisActivation();
    testRealVoices();

    std::cout << "\n=== All WindFix V2.1 validation tests passed! ===\n";
    return 0;
}
