#include <JuceHeader.h>
#include "../Source/ModernPitchEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace
{
constexpr double pi = 3.1415926535897932384626433832795;

std::vector<double> chromaticScale()
{
    std::vector<double> scale;
    for (int note = 0; note < 12; ++note)
        scale.push_back(std::exp2(static_cast<double>(note) / 12.0));
    return scale;
}

float percentile(std::vector<float> values, float quantile)
{
    if (values.empty())
        return 0.0f;
    quantile = juce::jlimit(0.0f, 1.0f, quantile);
    const std::size_t index = static_cast<std::size_t>(
        std::lround(quantile * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

struct Summary
{
    juce::String file;
    int mode = 0;
    int latencySamples = 0;
    double sampleRate = 0.0;
    double durationSeconds = 0.0;
    double realtimeFactor = 0.0;
    bool finite = true;
    float inputPeak = 0.0f;
    float outputPeak = 0.0f;
    float detectedPitchMedian = 0.0f;
    float confidenceMedian = 0.0f;
    float voicingMedian = 0.0f;
    float consensusMedian = 0.0f;
    float harmonicityMedian = 0.0f;
    float noiseMedian = 0.0f;
    float noiseP90 = 0.0f;
    float noiseAbove80Fraction = 0.0f;
    float noiseFrameDelta = 0.0f;
    float wetMedian = 0.0f;
    float wetFrameDelta = 0.0f;
    float polyphonyMedian = 0.0f;
    float polyphonyP90 = 0.0f;
    float reliabilityMedian = 0.0f;
    float maskStabilityMedian = 0.0f;
    float highBandEnvelopeDeltaDb = 0.0f;
    float blockInvarianceError = 0.0f;
};

struct MeterAccumulator
{
    std::vector<float> pitch;
    std::vector<float> confidence;
    std::vector<float> voicing;
    std::vector<float> consensus;
    std::vector<float> harmonicity;
    std::vector<float> noise;
    std::vector<float> wet;
    std::vector<float> polyphony;
    std::vector<float> reliability;
    std::vector<float> maskStability;

    void add(const ModernPitchEngine::Metering& meter)
    {
        if (meter.detectedPitchHz > 0.0f)
            pitch.push_back(meter.detectedPitchHz);
        confidence.push_back(meter.confidence);
        voicing.push_back(meter.voicing);
        consensus.push_back(meter.consensus);

        // Spectral quality metrics are meaningful only while the tracker has
        // a credible voiced observation.  Silence and unvoiced consonants
        // would otherwise dominate the medians with noisePath == 1.
        if (meter.voicing > 0.35f && meter.confidence > 0.35f)
        {
            harmonicity.push_back(meter.harmonicity);
            noise.push_back(meter.noisePath);
            wet.push_back(meter.wetMix);
            polyphony.push_back(meter.polyphony);
            reliability.push_back(meter.spectralReliability);
            maskStability.push_back(meter.maskStability);
        }
    }
};

float meanAbsoluteDelta(const std::vector<float>& values)
{
    if (values.size() < 2)
        return 0.0f;
    double sum = 0.0;
    for (std::size_t index = 1; index < values.size(); ++index)
        sum += std::abs(static_cast<double>(values[index] - values[index - 1]));
    return static_cast<float>(sum / static_cast<double>(values.size() - 1));
}

float peak(const juce::AudioBuffer<float>& buffer)
{
    float maximum = 0.0f;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* data = buffer.getReadPointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            maximum = std::max(maximum, std::abs(data[sample]));
    }
    return maximum;
}

bool isFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* data = buffer.getReadPointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (!std::isfinite(data[sample]))
                return false;
    }
    return true;
}

float highBandEnvelopeDeltaDb(const juce::AudioBuffer<float>& buffer,
                              double sampleRate)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return 0.0f;

    const int windowSamples = std::max(16, static_cast<int>(std::lround(0.010 * sampleRate)));
    const float coefficient = static_cast<float>(
        1.0 - std::exp(-2.0 * pi * 3000.0 / sampleRate));
    std::vector<float> envelope;
    float lowPass = 0.0f;
    double energy = 0.0;
    int samplesInWindow = 0;

    const float* data = buffer.getReadPointer(0);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        lowPass += coefficient * (data[sample] - lowPass);
        const float highPass = data[sample] - lowPass;
        energy += static_cast<double>(highPass) * highPass;
        if (++samplesInWindow >= windowSamples)
        {
            envelope.push_back(static_cast<float>(
                10.0 * std::log10(energy / samplesInWindow + 1.0e-12)));
            energy = 0.0;
            samplesInWindow = 0;
        }
    }
    return meanAbsoluteDelta(envelope);
}

juce::AudioBuffer<float> renderBuffer(const juce::AudioBuffer<float>& input,
                                      double sampleRate,
                                      int blockSize,
                                      ModernPitchEngine::LatencyMode mode,
                                      MeterAccumulator* meters,
                                      double& elapsedSeconds)
{
    juce::AudioBuffer<float> output;
    output.makeCopyOf(input);
    ModernPitchEngine engine;
    engine.prepare(sampleRate, blockSize, output.getNumChannels(), mode);
    ModernPitchEngine::Parameters parameters;
    parameters.amount = 1.0f;
    parameters.retuneTimeMs = 50.0f;
    parameters.transitionTimeMs = 35.0f;
    parameters.breathReduction = 0.50f;
    parameters.stereoMode = ModernPitchEngine::StereoMode::linkedMidSide;
    const auto scale = chromaticScale();

    const auto start = std::chrono::steady_clock::now();
    for (int offset = 0; offset < output.getNumSamples(); offset += blockSize)
    {
        const int count = std::min(blockSize, output.getNumSamples() - offset);
        juce::AudioBuffer<float> view(output.getArrayOfWritePointers(),
                                      output.getNumChannels(), offset, count);
        engine.process(view, scale, 261.625565, parameters);
        if (meters != nullptr)
            meters->add(engine.getMetering());
    }
    elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    return output;
}

float blockInvariance(const juce::AudioBuffer<float>& input,
                      double sampleRate,
                      ModernPitchEngine::LatencyMode mode)
{
    const int maximumSamples = std::min(
        input.getNumSamples(), static_cast<int>(std::lround(8.0 * sampleRate)));
    juce::AudioBuffer<float> excerpt(input.getNumChannels(), maximumSamples);
    for (int channel = 0; channel < input.getNumChannels(); ++channel)
        excerpt.copyFrom(channel, 0, input, channel, 0, maximumSamples);

    double ignored = 0.0;
    const auto a = renderBuffer(excerpt, sampleRate, 32, mode, nullptr, ignored);
    const auto b = renderBuffer(excerpt, sampleRate, 257, mode, nullptr, ignored);
    float maximum = 0.0f;
    for (int channel = 0; channel < a.getNumChannels(); ++channel)
    {
        const float* x = a.getReadPointer(channel);
        const float* y = b.getReadPointer(channel);
        for (int sample = 0; sample < a.getNumSamples(); ++sample)
            maximum = std::max(maximum, std::abs(x[sample] - y[sample]));
    }
    return maximum;
}

Summary analyseFile(const juce::File& file,
                    juce::AudioFormatReader& reader,
                    int modeValue,
                    double maximumSeconds)
{
    const int channels = std::clamp(static_cast<int>(reader.numChannels), 1, 2);
    const auto requested = static_cast<juce::int64>(std::lround(
        maximumSeconds * reader.sampleRate));
    const int samples = static_cast<int>(std::min(reader.lengthInSamples, requested));
    juce::AudioBuffer<float> input(channels, samples);
    reader.read(&input, 0, samples, 0, true, true);

    MeterAccumulator meters;
    double elapsed = 0.0;
    const auto mode = static_cast<ModernPitchEngine::LatencyMode>(modeValue);
    const auto output = renderBuffer(input, reader.sampleRate, 64, mode,
                                     &meters, elapsed);

    Summary summary;
    summary.file = file.getFileName();
    summary.mode = modeValue;
    summary.latencySamples = modeValue == 0 ? 128 : modeValue == 1 ? 256 : 512;
    summary.sampleRate = reader.sampleRate;
    summary.durationSeconds = samples / reader.sampleRate;
    summary.realtimeFactor = elapsed > 0.0 ? summary.durationSeconds / elapsed : 0.0;
    summary.finite = isFinite(output);
    summary.inputPeak = peak(input);
    summary.outputPeak = peak(output);
    summary.detectedPitchMedian = percentile(meters.pitch, 0.5f);
    summary.confidenceMedian = percentile(meters.confidence, 0.5f);
    summary.voicingMedian = percentile(meters.voicing, 0.5f);
    summary.consensusMedian = percentile(meters.consensus, 0.5f);
    summary.harmonicityMedian = percentile(meters.harmonicity, 0.5f);
    summary.noiseMedian = percentile(meters.noise, 0.5f);
    summary.noiseP90 = percentile(meters.noise, 0.9f);
    summary.noiseAbove80Fraction = meters.noise.empty() ? 0.0f
        : static_cast<float>(std::count_if(meters.noise.begin(), meters.noise.end(),
            [](float value) { return value > 0.80f; }))
            / static_cast<float>(meters.noise.size());
    summary.noiseFrameDelta = meanAbsoluteDelta(meters.noise);
    summary.wetMedian = percentile(meters.wet, 0.5f);
    summary.wetFrameDelta = meanAbsoluteDelta(meters.wet);
    summary.polyphonyMedian = percentile(meters.polyphony, 0.5f);
    summary.polyphonyP90 = percentile(meters.polyphony, 0.9f);
    summary.reliabilityMedian = percentile(meters.reliability, 0.5f);
    summary.maskStabilityMedian = percentile(meters.maskStability, 0.5f);
    summary.highBandEnvelopeDeltaDb = highBandEnvelopeDeltaDb(output,
                                                               reader.sampleRate);
    summary.blockInvarianceError = blockInvariance(input, reader.sampleRate, mode);
    return summary;
}

void writeHeader(std::ostream& stream)
{
    stream << "file,mode,latencySamples,sampleRate,durationSeconds,realtimeFactor,finite,"
              "inputPeak,outputPeak,pitchMedian,confidenceMedian,voicingMedian,"
              "consensusMedian,harmonicityMedian,noiseMedian,noiseP90,noiseAbove80,"
              "noiseFrameDelta,wetMedian,wetFrameDelta,polyphonyMedian,polyphonyP90,"
              "reliabilityMedian,maskStabilityMedian,highBandEnvelopeDeltaDb,"
              "blockInvarianceError\n";
}

void writeSummary(std::ostream& stream, const Summary& value)
{
    const std::string escapedFile = value.file.replace("\"", "\"\"").toStdString();
    stream << '"' << escapedFile << '"' << ',' << value.mode << ','
           << value.latencySamples << ','
           << value.sampleRate << ',' << value.durationSeconds << ','
           << value.realtimeFactor << ',' << (value.finite ? 1 : 0) << ','
           << value.inputPeak << ',' << value.outputPeak << ','
           << value.detectedPitchMedian << ',' << value.confidenceMedian << ','
           << value.voicingMedian << ',' << value.consensusMedian << ','
           << value.harmonicityMedian << ',' << value.noiseMedian << ','
           << value.noiseP90 << ',' << value.noiseAbove80Fraction << ','
           << value.noiseFrameDelta << ',' << value.wetMedian << ','
           << value.wetFrameDelta << ',' << value.polyphonyMedian << ','
           << value.polyphonyP90 << ',' << value.reliabilityMedian << ','
           << value.maskStabilityMedian << ',' << value.highBandEnvelopeDeltaDb << ','
           << value.blockInvarianceError << '\n';
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: AutotuneRealWorldRegression summary.csv audio1 [audio2 ...]\n";
        return 2;
    }

    std::ofstream csv(argv[1]);
    if (!csv)
        return 3;
    writeHeader(csv);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    bool hardFailure = false;

    for (int argument = 2; argument < argc; ++argument)
    {
        const juce::File file(juce::String::fromUTF8(argv[argument]));
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr)
        {
            std::cerr << "Cannot read " << file.getFullPathName() << '\n';
            hardFailure = true;
            continue;
        }

        for (int mode = 0; mode < 3; ++mode)
        {
            const Summary summary = analyseFile(file, *reader, mode, 60.0);
            writeSummary(csv, summary);
            std::cout << file.getFileName().toStdString() << " mode " << mode
                      << ": noise=" << summary.noiseMedian
                      << " rel=" << summary.reliabilityMedian
                      << " poly=" << summary.polyphonyMedian
                      << " mask=" << summary.maskStabilityMedian
                      << " blockErr=" << summary.blockInvarianceError << '\n';

            hardFailure = hardFailure
                || !summary.finite
                || summary.outputPeak > 4.0f
                || summary.blockInvarianceError > 2.0e-5f;
        }
    }

    return hardFailure ? 1 : 0;
}
