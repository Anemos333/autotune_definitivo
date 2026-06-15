#include <iostream>
#include <cmath>
#include <vector>
#include <random>
#include "../Source/ModernPitchEngine.h"

// Simple mock for JUCE's MathConstants
#ifndef juce_MathConstants_h
constexpr double pi = 3.14159265358979323846;
#endif

void testEngine(double testFreq, double sampleRate, int blockSize, ModernPitchEngine::LatencyMode latencyMode, const std::string& label) {
    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, latencyMode);

    juce::AudioBuffer<float> buffer(1, blockSize);
    ModernPitchEngine::Parameters params;
    params.retuneTimeMs = 5.0f;
    params.amount = 1.0f;

    // 12-ET scale (C major)
    std::vector<double> scaleRatios = {
        1.0, 9.0/8.0, 5.0/4.0, 4.0/3.0, 3.0/2.0, 5.0/3.0, 15.0/8.0
    };
    double rootFreq = 261.625565; // C4

    std::cout << "--- Test: " << label << " ---" << std::endl;

    double phase = 0.0;
    double phaseInc = 2.0 * pi * testFreq / sampleRate;

    // Run for a few blocks
    for (int block = 0; block < 10; ++block) {
        float* channelData = buffer.getWritePointer(0);
        for (int i = 0; i < blockSize; ++i) {
            channelData[i] = static_cast<float>(std::sin(phase));
            phase += phaseInc;
            if (phase >= 2.0 * pi) phase -= 2.0 * pi;
        }

        engine.process(buffer, scaleRatios.data(), static_cast<int>(scaleRatios.size()), rootFreq, params);
    }

    auto meter = engine.getMetering();
    std::cout << "Detected Pitch: " << meter.detectedPitchHz << " Hz" << std::endl;
    std::cout << "Confidence: " << meter.confidence << std::endl;
    std::cout << "Target Pitch: " << meter.targetPitchHz << " Hz" << std::endl;
    std::cout << std::endl;
}

void testWhiteNoise(double sampleRate, int blockSize, ModernPitchEngine::LatencyMode latencyMode) {
    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, 1, latencyMode);

    juce::AudioBuffer<float> buffer(1, blockSize);
    ModernPitchEngine::Parameters params;
    
    std::vector<double> scaleRatios = {1.0, 9.0/8.0, 5.0/4.0, 4.0/3.0, 3.0/2.0, 5.0/3.0, 15.0/8.0};
    double rootFreq = 261.625565;

    std::cout << "--- Test: White Noise ---" << std::endl;

    std::mt19937 rng;
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    for (int block = 0; block < 10; ++block) {
        float* channelData = buffer.getWritePointer(0);
        for (int i = 0; i < blockSize; ++i) {
            channelData[i] = dist(rng);
        }
        engine.process(buffer, scaleRatios.data(), static_cast<int>(scaleRatios.size()), rootFreq, params);
    }

    auto meter = engine.getMetering();
    std::cout << "Detected Pitch: " << meter.detectedPitchHz << " Hz" << std::endl;
    std::cout << "Confidence: " << meter.confidence << std::endl;
    std::cout << "Voicing: " << meter.voicing << std::endl;
    std::cout << std::endl;
}

int main() {
    double sampleRate = 48000.0;
    int blockSize = 512;

    testEngine(220.0, sampleRate, blockSize, ModernPitchEngine::LatencyMode::quality, "220 Hz Quality");
    testEngine(440.0, sampleRate, blockSize, ModernPitchEngine::LatencyMode::live, "440 Hz Live");
    testEngine(110.0, sampleRate, blockSize, ModernPitchEngine::LatencyMode::ultraLive, "110 Hz Experimental");
    
    testWhiteNoise(sampleRate, blockSize, ModernPitchEngine::LatencyMode::quality);

    return 0;
}
