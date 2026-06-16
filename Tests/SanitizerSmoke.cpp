#include "../Source/ModernPitchEngine.h"
#include <cmath>
#include <random>
#include <vector>

int main()
{
    constexpr double sr = 48000.0;
    constexpr int block = 73;
    std::vector<double> scale;
    for (int i = 0; i < 12; ++i) scale.push_back(std::exp2(i / 12.0));
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.08f);

    for (int mode = 0; mode < 3; ++mode)
    {
        ModernPitchEngine engine;
        engine.prepare(sr, block, 1,
            static_cast<ModernPitchEngine::LatencyMode>(mode));
        ModernPitchEngine::Parameters parameters;
        parameters.amount = 1.0f;
        parameters.breathReduction = 0.6f;
        juce::AudioBuffer<float> buffer(1, block);
        double phase = 0.0;
        for (int frame = 0; frame < 180; ++frame)
        {
            float* data = buffer.getWritePointer(0);
            for (int i = 0; i < block; ++i)
            {
                data[i] = 0.2f * static_cast<float>(std::sin(phase)) + noise(rng);
                phase += 2.0 * 3.14159265358979323846 * (190.0 + 0.15 * frame) / sr;
            }
            engine.process(buffer, scale, 261.625565, parameters);
            for (int i = 0; i < block; ++i)
                if (!std::isfinite(data[i])) return 10 + mode;
        }
    }
    return 0;
}
