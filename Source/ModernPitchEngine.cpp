#include "ModernPitchEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr double pi = 3.1415926535897932384626433832795;
constexpr double twoPi = 2.0 * pi;
constexpr float minimumDetectorRms = 0.0012f;

[[nodiscard]] double safeLog2(double value) noexcept
{
    return std::log2(std::max(value, 1.0e-12));
}

[[nodiscard]] float smoothStep(float edge0, float edge1, float value) noexcept
{
    if (edge1 <= edge0)
        return value >= edge1 ? 1.0f : 0.0f;

    const float x = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

[[nodiscard]] double wrapCorrectionToNearestOctave(double cents) noexcept
{
    if (!std::isfinite(cents))
        return 0.0;

    // The scale engine is octave-repeating. A correction that differs by a
    // whole octave addresses the same scale degree but the wrong register.
    // Keep the correction in the nearest octave: [-600, +600) cents.
    double wrapped = std::fmod(cents + 600.0, 1200.0);
    if (wrapped < 0.0)
        wrapped += 1200.0;
    return wrapped - 600.0;
}

[[nodiscard]] double alignTargetToNearestOctave(double targetLog2,
                                                 double referenceLog2) noexcept
{
    if (!std::isfinite(targetLog2) || !std::isfinite(referenceLog2))
        return referenceLog2;

    return targetLog2 + std::round(referenceLog2 - targetLog2);
}
} // namespace

//==============================================================================
// Utilities

int ModernPitchEngine::nextPowerOfTwo(int value) noexcept
{
    int result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

float ModernPitchEngine::clamp01(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

int ModernPitchEngine::frameSizeForMode(double sampleRate, LatencyMode mode) noexcept
{
    const double safeRate = std::max(8000.0, sampleRate);
    int baseAt48k = 256;

    switch (mode)
    {
        case LatencyMode::ultraLive: baseAt48k = 128; break;
        case LatencyMode::live:      baseAt48k = 256; break;
        case LatencyMode::quality:   baseAt48k = 512; break;
    }

    const int requested = std::max(64,
        static_cast<int>(std::lround(static_cast<double>(baseAt48k)
                                     * safeRate / 48000.0)));
    return nextPowerOfTwo(requested);
}

//==============================================================================
// BiquadLowPass

void ModernPitchEngine::BiquadLowPass::prepare(double sampleRate,
                                                double cutoffHz,
                                                double q) noexcept
{
    const double safeSampleRate = std::max(1.0, sampleRate);
    const double safeCutoff = std::clamp(cutoffHz, 10.0, safeSampleRate * 0.45);
    const double safeQ = std::max(0.05, q);

    const double omega = twoPi * safeCutoff / safeSampleRate;
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine / (2.0 * safeQ);
    const double a0 = 1.0 + alpha;

    b0_ = ((1.0 - cosine) * 0.5) / a0;
    b1_ = (1.0 - cosine) / a0;
    b2_ = b0_;
    a1_ = (-2.0 * cosine) / a0;
    a2_ = (1.0 - alpha) / a0;
    reset();
}

void ModernPitchEngine::BiquadLowPass::reset() noexcept
{
    z1_ = 0.0;
    z2_ = 0.0;
}

float ModernPitchEngine::BiquadLowPass::process(float input) noexcept
{
    const double x = static_cast<double>(input);
    const double output = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * output + z2_;
    z2_ = b2_ * x - a2_ * output;
    return static_cast<float>(output);
}

//==============================================================================
// MultiRatePitchTracker

void ModernPitchEngine::MultiRatePitchTracker::prepare(double sampleRate) noexcept
{
    sampleRate_ = std::max(8000.0, sampleRate);

    halfRateAntiAlias_.prepare(sampleRate_, std::min(5200.0, sampleRate_ * 0.20));
    quarterRateAntiAlias_.prepare(sampleRate_ * 0.5,
                                  std::min(2600.0, sampleRate_ * 0.10));
    eighthRateAntiAlias_.prepare(sampleRate_ * 0.25,
                                 std::min(1300.0, sampleRate_ * 0.05));

    dcBlockCoefficient_ = static_cast<float>(std::exp(-twoPi * 22.0 / sampleRate_));

    fastEnergyCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.0018 * sampleRate_)));
    slowEnergyCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.035 * sampleRate_)));

    reset();
}

void ModernPitchEngine::MultiRatePitchTracker::reset() noexcept
{
    fullRateRing_.fill(0.0f);
    halfRateRing_.fill(0.0f);
    quarterRateRing_.fill(0.0f);
    eighthRateRing_.fill(0.0f);
    frame_.fill(0.0f);
    difference_.fill(1.0f);

    fullRateWritePosition_ = 0;
    halfRateWritePosition_ = 0;
    quarterRateWritePosition_ = 0;
    eighthRateWritePosition_ = 0;
    fullRateAvailableSamples_ = 0;
    halfRateAvailableSamples_ = 0;
    quarterRateAvailableSamples_ = 0;
    eighthRateAvailableSamples_ = 0;
    halfRateDecimationCounter_ = 0;
    quarterRateDecimationCounter_ = 0;
    eighthRateDecimationCounter_ = 0;
    hopCounter_ = 0;
    analysisHopCounter_ = 0;

    halfRateAntiAlias_.reset();
    quarterRateAntiAlias_.reset();
    eighthRateAntiAlias_.reset();

    previousInput_ = 0.0f;
    previousDcOutput_ = 0.0f;
    fastEnergy_ = 0.0f;
    slowEnergy_ = 0.0f;
    onsetEnvelope_ = 0.0f;
    onsetCooldownSamples_ = 0;
    onsetPending_ = false;

    fullRateCandidate_ = {};
    halfRateCandidate_ = {};
    quarterRateCandidate_ = {};
    eighthRateCandidate_ = {};
    decoderBeam_.fill({});

    trackedPitchHz_ = 0.0f;
    trackedConfidence_ = 0.0f;
    trackedPeriodicity_ = 0.0f;
    trackedConsensus_ = 0.0f;
    trackedSupportCount_ = 0;
    invalidHopCount_ = 0;

    octaveState_ = 0;
    pendingOctaveDelta_ = 0;
    pendingOctaveCount_ = 0;
    pendingOctaveFrequencyHz_ = 0.0f;
}

void ModernPitchEngine::MultiRatePitchTracker::setRange(float minimumPitchHz,
                                                         float maximumPitchHz) noexcept
{
    minimumPitchHz_ = std::clamp(minimumPitchHz, 35.0f, 500.0f);
    maximumPitchHz_ = std::clamp(maximumPitchHz,
                                 minimumPitchHz_ + 20.0f,
                                 3000.0f);
}

void ModernPitchEngine::MultiRatePitchTracker::setSensitivity(float sensitivity) noexcept
{
    sensitivity_ = clamp01(sensitivity);
}

void ModernPitchEngine::MultiRatePitchTracker::push(
    std::array<float, ringSize>& ring,
    int& writePosition,
    int& availableSamples,
    float sample) noexcept
{
    ring[static_cast<std::size_t>(writePosition)] = sample;
    writePosition = (writePosition + 1) & ringMask;
    availableSamples = std::min(availableSamples + 1, ringSize);
}

ModernPitchEngine::MultiRatePitchTracker::PitchCandidate
ModernPitchEngine::MultiRatePitchTracker::analyse(
    const std::array<float, ringSize>& ring,
    int writePosition,
    int availableSamples,
    double effectiveSampleRate,
    float minimumFrequency,
    float maximumFrequency) noexcept
{
    PitchCandidate result;

    if (availableSamples < analysisSize || effectiveSampleRate <= 0.0
        || minimumFrequency >= maximumFrequency)
    {
        return result;
    }

    const int startPosition = (writePosition - analysisSize + ringSize) & ringMask;

    double mean = 0.0;
    for (int index = 0; index < analysisSize; ++index)
    {
        const float sample = ring[static_cast<std::size_t>((startPosition + index) & ringMask)];
        frame_[static_cast<std::size_t>(index)] = sample;
        mean += static_cast<double>(sample);
    }
    mean /= static_cast<double>(analysisSize);

    double squaredSum = 0.0;
    for (float& sample : frame_)
    {
        sample -= static_cast<float>(mean);
        squaredSum += static_cast<double>(sample) * static_cast<double>(sample);
    }

    const float rms = static_cast<float>(std::sqrt(
        squaredSum / static_cast<double>(analysisSize)));
    if (rms < minimumDetectorRms)
        return result;

    const int tauMinimum = std::clamp(
        static_cast<int>(std::floor(effectiveSampleRate
                                    / static_cast<double>(maximumFrequency))),
        2,
        analysisSize - 16);

    const int tauMaximum = std::clamp(
        static_cast<int>(std::ceil(effectiveSampleRate
                                   / static_cast<double>(minimumFrequency))),
        tauMinimum + 1,
        analysisSize - 16);

    difference_.fill(1.0f);
    difference_[0] = 1.0f;

    for (int tau = 1; tau <= tauMaximum; ++tau)
    {
        const int overlap = analysisSize - tau;
        float sum0 = 0.0f;
        float sum1 = 0.0f;
        float sum2 = 0.0f;
        float sum3 = 0.0f;

        int index = 0;
        const int vectorEnd = overlap & ~3;
        for (; index < vectorEnd; index += 4)
        {
            const float delta0 = frame_[static_cast<std::size_t>(index)]
                               - frame_[static_cast<std::size_t>(index + tau)];
            const float delta1 = frame_[static_cast<std::size_t>(index + 1)]
                               - frame_[static_cast<std::size_t>(index + tau + 1)];
            const float delta2 = frame_[static_cast<std::size_t>(index + 2)]
                               - frame_[static_cast<std::size_t>(index + tau + 2)];
            const float delta3 = frame_[static_cast<std::size_t>(index + 3)]
                               - frame_[static_cast<std::size_t>(index + tau + 3)];
            sum0 += delta0 * delta0;
            sum1 += delta1 * delta1;
            sum2 += delta2 * delta2;
            sum3 += delta3 * delta3;
        }

        float differenceSum = (sum0 + sum1) + (sum2 + sum3);
        for (; index < overlap; ++index)
        {
            const float delta = frame_[static_cast<std::size_t>(index)]
                              - frame_[static_cast<std::size_t>(index + tau)];
            differenceSum += delta * delta;
        }

        difference_[static_cast<std::size_t>(tau)] = differenceSum
            / static_cast<float>(std::max(1, overlap));
    }

    double cumulativeSum = 0.0;
    for (int tau = 1; tau <= tauMaximum; ++tau)
    {
        cumulativeSum += static_cast<double>(difference_[static_cast<std::size_t>(tau)]);
        difference_[static_cast<std::size_t>(tau)] = cumulativeSum > 1.0e-20
            ? static_cast<float>(static_cast<double>(difference_[static_cast<std::size_t>(tau)])
                                 * static_cast<double>(tau) / cumulativeSum)
            : 1.0f;
    }

    const float yinThreshold = 0.12f + 0.16f * sensitivity_;
    const float fallbackThreshold = 0.26f + 0.20f * sensitivity_;

    int thresholdTau = -1;
    int globalTau = tauMinimum;
    float globalValue = difference_[static_cast<std::size_t>(tauMinimum)];

    for (int tau = tauMinimum; tau <= tauMaximum; ++tau)
    {
        const float value = difference_[static_cast<std::size_t>(tau)];
        if (value < globalValue)
        {
            globalValue = value;
            globalTau = tau;
        }

        if (thresholdTau < 0 && value < yinThreshold)
        {
            int localTau = tau;
            while (localTau + 1 <= tauMaximum
                   && difference_[static_cast<std::size_t>(localTau + 1)]
                        < difference_[static_cast<std::size_t>(localTau)])
            {
                ++localTau;
            }
            thresholdTau = localTau;
        }
    }

    if (thresholdTau < 0 && globalValue > fallbackThreshold)
        return result;

    // Alternative periods are deliberately retained because a weak fundamental
    // can be recovered from its harmonics.  They are not equally trusted:
    // doubled periods (subharmonics) receive the strongest prior penalty and
    // must subsequently survive the cross-rate consensus and temporal decoder.
    std::array<int, 5> candidateTaus {
        thresholdTau >= 0 ? thresholdTau : globalTau,
        globalTau,
        std::max(tauMinimum, globalTau / 2),
        std::min(tauMaximum, globalTau * 2),
        std::min(tauMaximum, (globalTau * 3) / 2)
    };
    constexpr std::array<float, 5> candidatePriors {
        1.00f, 0.98f, 0.88f, 0.70f, 0.78f
    };

    float bestScore = -1.0f;
    int bestTau = -1;
    float bestPeriodicity = 0.0f;

    for (std::size_t candidateIndex = 0;
         candidateIndex < candidateTaus.size();
         ++candidateIndex)
    {
        int tau = std::clamp(candidateTaus[candidateIndex],
                             tauMinimum,
                             tauMaximum);

        double correlation = 0.0;
        double energyA = 0.0;
        double energyB = 0.0;
        const int overlap = analysisSize - tau;

        for (int index = 0; index < overlap; ++index)
        {
            const double a = frame_[static_cast<std::size_t>(index)];
            const double b = frame_[static_cast<std::size_t>(index + tau)];
            correlation += a * b;
            energyA += a * a;
            energyB += b * b;
        }

        const double denominator = std::sqrt(std::max(1.0e-20, energyA * energyB));
        const float normalisedCorrelation = denominator > 0.0
            ? static_cast<float>(correlation / denominator)
            : 0.0f;
        const float periodicity = clamp01(0.5f * (normalisedCorrelation + 1.0f));
        const float yinConfidence = clamp01(
            1.0f - difference_[static_cast<std::size_t>(tau)]);

        // Prefer candidates containing at least two periods, but do not reject
        // low notes whose fundamental is mainly inferred from their harmonics.
        const float periodsInWindow = static_cast<float>(analysisSize)
                                    / static_cast<float>(std::max(1, tau));
        const float periodSupport = std::clamp(periodsInWindow / 2.2f, 0.55f, 1.0f);
        const float score = (0.67f * yinConfidence + 0.33f * periodicity)
                          * periodSupport
                          * candidatePriors[candidateIndex];

        if (score > bestScore)
        {
            bestScore = score;
            bestTau = tau;
            bestPeriodicity = periodicity;
        }
    }

    if (bestTau < 2 || bestScore < 0.45f)
        return result;

    double refinedTau = static_cast<double>(bestTau);
    if (bestTau > tauMinimum && bestTau < tauMaximum)
    {
        const double left = difference_[static_cast<std::size_t>(bestTau - 1)];
        const double centre = difference_[static_cast<std::size_t>(bestTau)];
        const double right = difference_[static_cast<std::size_t>(bestTau + 1)];
        const double denominator = left - 2.0 * centre + right;

        if (std::abs(denominator) > 1.0e-12)
            refinedTau += 0.5 * (left - right) / denominator;
    }

    if (refinedTau <= 0.0)
        return result;

    const float frequency = static_cast<float>(effectiveSampleRate / refinedTau);
    if (!std::isfinite(frequency)
        || frequency < minimumFrequency * 0.82f
        || frequency > maximumFrequency * 1.18f)
    {
        return result;
    }

    result.frequencyHz = frequency;
    result.confidence = clamp01(bestScore);
    result.periodicity = bestPeriodicity;
    result.valid = true;
    return result;
}

float ModernPitchEngine::MultiRatePitchTracker::centsDistance(
    float frequencyA,
    float frequencyB) noexcept
{
    if (frequencyA <= 0.0f || frequencyB <= 0.0f)
        return 100000.0f;

    return std::abs(1200.0f * std::log2(frequencyA / frequencyB));
}

float ModernPitchEngine::MultiRatePitchTracker::candidateBaseScore(
    const PitchCandidate& candidate) const noexcept
{
    if (!candidate.valid || candidate.frequencyHz <= 0.0f)
        return 0.0f;

    const float ageWeight = std::exp(-0.22f
        * static_cast<float>(std::max(0, candidate.ageInHops)));
    return clamp01((0.70f * candidate.confidence
                  + 0.30f * candidate.periodicity) * ageWeight);
}

float ModernPitchEngine::MultiRatePitchTracker::pathReliability(
    int pathIndex,
    float frequencyHz) const noexcept
{
    const auto bandWeight = [](float frequency,
                               float lowerSoft,
                               float lowerFull,
                               float upperFull,
                               float upperSoft) noexcept
    {
        const float lower = smoothStep(lowerSoft, lowerFull, frequency);
        const float upper = 1.0f - smoothStep(upperFull, upperSoft, frequency);
        return std::clamp(lower * upper, 0.08f, 1.0f);
    };

    switch (pathIndex)
    {
        case 0: return bandWeight(frequencyHz, 125.0f, 185.0f, 1250.0f, 2300.0f);
        case 1: return bandWeight(frequencyHz, 62.0f, 92.0f, 650.0f, 980.0f);
        case 2: return bandWeight(frequencyHz, 30.0f, 48.0f, 330.0f, 500.0f);
        case 3: return bandWeight(frequencyHz, 22.0f, 36.0f, 165.0f, 250.0f);
        default: break;
    }

    return 0.0f;
}

int ModernPitchEngine::MultiRatePitchTracker::collectFreshCandidates(
    std::array<PitchCandidate, detectorPathCount>& candidates) const noexcept
{
    int count = 0;

    const auto append = [&candidates, &count](const CandidateSlot& slot,
                                              int pathIndex,
                                              int maximumAge)
    {
        if (!slot.candidate.valid || slot.ageInHops > maximumAge
            || count >= detectorPathCount)
        {
            return;
        }

        PitchCandidate candidate = slot.candidate;
        candidate.pathIndex = pathIndex;
        candidate.ageInHops = slot.ageInHops;
        candidates[static_cast<std::size_t>(count++)] = candidate;
    };

    append(fullRateCandidate_,    0, 2);
    append(halfRateCandidate_,    1, 3);
    append(quarterRateCandidate_, 2, 5);
    append(eighthRateCandidate_,  3, 9);
    return count;
}

int ModernPitchEngine::MultiRatePitchTracker::buildConsensusHypotheses(
    const std::array<PitchCandidate, detectorPathCount>& candidates,
    int candidateCount,
    std::array<ConsensusHypothesis, maxConsensusHypotheses>& hypotheses) const noexcept
{
    int seedCount = 0;

    // Every detector contributes octave-explicit seeds.  A detector can only
    // contribute once to a resulting cluster, so generated octave variants do
    // not create fake consensus by themselves.
    for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex)
    {
        const auto& candidate = candidates[static_cast<std::size_t>(candidateIndex)];
        if (!candidate.valid)
            continue;

        for (int octaveShift = -2; octaveShift <= 2; ++octaveShift)
        {
            const float frequency = std::ldexp(candidate.frequencyHz, octaveShift);
            if (frequency < minimumPitchHz_ || frequency > maximumPitchHz_)
                continue;

            bool duplicate = false;
            for (int seedIndex = 0; seedIndex < seedCount; ++seedIndex)
            {
                if (centsDistance(hypotheses[static_cast<std::size_t>(seedIndex)].frequencyHz,
                                  frequency) < 28.0f)
                {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate && seedCount < maxConsensusHypotheses)
            {
                auto& seed = hypotheses[static_cast<std::size_t>(seedCount++)];
                seed = {};
                seed.frequencyHz = frequency;
                seed.valid = true;
            }
        }
    }

    int validCount = 0;
    for (int seedIndex = 0; seedIndex < seedCount; ++seedIndex)
    {
        const float seedFrequency = hypotheses[static_cast<std::size_t>(seedIndex)].frequencyHz;
        double weightedLogFrequency = 0.0;
        float totalWeight = 0.0f;
        float confidenceSum = 0.0f;
        float periodicitySum = 0.0f;
        int supportCount = 0;
        int directSupportCount = 0;
        std::uint8_t supportMask = 0;
        std::uint8_t freshSupportMask = 0;

        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex)
        {
            const auto& candidate = candidates[static_cast<std::size_t>(candidateIndex)];
            int bestOctaveShift = 0;
            float bestFrequency = candidate.frequencyHz;
            float bestDistance = centsDistance(bestFrequency, seedFrequency);

            for (int octaveShift = -2; octaveShift <= 2; ++octaveShift)
            {
                const float shiftedFrequency = std::ldexp(candidate.frequencyHz, octaveShift);
                if (shiftedFrequency < minimumPitchHz_ || shiftedFrequency > maximumPitchHz_)
                    continue;

                const float distance = centsDistance(shiftedFrequency, seedFrequency);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestFrequency = shiftedFrequency;
                    bestOctaveShift = octaveShift;
                }
            }

            const bool direct = bestOctaveShift == 0;
            const float tolerance = direct ? 55.0f : 38.0f;
            if (bestDistance > tolerance)
                continue;

            const float octavePrior = direct ? 1.0f
                : (std::abs(bestOctaveShift) == 1 ? 0.52f : 0.25f);
            const float reliability = pathReliability(candidate.pathIndex,
                                                       candidate.frequencyHz);
            const float baseScore = candidateBaseScore(candidate);
            const float weight = baseScore * reliability * octavePrior;

            // Octave-transposed support is useful as harmonic evidence, but it
            // must be genuinely strong; otherwise it is ignored rather than
            // being allowed to manufacture a low subharmonic.
            if ((!direct && baseScore < 0.60f) || weight < 0.10f)
                continue;

            weightedLogFrequency += static_cast<double>(weight)
                                  * safeLog2(static_cast<double>(bestFrequency));
            totalWeight += weight;
            confidenceSum += weight * candidate.confidence;
            periodicitySum += weight * candidate.periodicity;
            ++supportCount;
            if (direct)
                ++directSupportCount;

            const auto bit = static_cast<std::uint8_t>(1u << candidate.pathIndex);
            supportMask = static_cast<std::uint8_t>(supportMask | bit);
            if (candidate.ageInHops == 0)
                freshSupportMask = static_cast<std::uint8_t>(freshSupportMask | bit);
        }

        if (supportCount <= 0 || totalWeight <= 1.0e-6f)
            continue;

        ConsensusHypothesis hypothesis;
        hypothesis.frequencyHz = static_cast<float>(std::exp2(
            weightedLogFrequency / static_cast<double>(totalWeight)));
        hypothesis.confidence = clamp01(confidenceSum / totalWeight);
        hypothesis.periodicity = clamp01(periodicitySum / totalWeight);
        hypothesis.supportCount = supportCount;
        hypothesis.directSupportCount = directSupportCount;
        hypothesis.supportMask = supportMask;
        hypothesis.freshSupportMask = freshSupportMask;

        const float pathConsensus = static_cast<float>(supportCount - 1)
                                  / static_cast<float>(detectorPathCount - 1);
        const float directConsensus = static_cast<float>(directSupportCount)
                                    / static_cast<float>(detectorPathCount);
        hypothesis.consensus = clamp01(0.12f
                                     + 0.58f * pathConsensus
                                     + 0.30f * directConsensus);

        const float meanEvidence = clamp01(totalWeight
            / static_cast<float>(std::max(1, supportCount)));
        const float directPenalty = directSupportCount == 0 ? 0.16f : 0.0f;
        hypothesis.evidenceScore = meanEvidence
                                 * (0.70f + 0.30f * hypothesis.consensus)
                                 + 0.045f * static_cast<float>(directSupportCount)
                                 - directPenalty;
        hypothesis.valid = hypothesis.evidenceScore > 0.20f;

        if (!hypothesis.valid)
            continue;

        // Merge clusters that converged after weighted refinement.
        int mergeIndex = -1;
        for (int existing = 0; existing < validCount; ++existing)
        {
            if (centsDistance(hypotheses[static_cast<std::size_t>(existing)].frequencyHz,
                              hypothesis.frequencyHz) < 24.0f)
            {
                mergeIndex = existing;
                break;
            }
        }

        if (mergeIndex >= 0)
        {
            if (hypothesis.evidenceScore
                > hypotheses[static_cast<std::size_t>(mergeIndex)].evidenceScore)
            {
                hypotheses[static_cast<std::size_t>(mergeIndex)] = hypothesis;
            }
        }
        else if (validCount < maxConsensusHypotheses)
        {
            hypotheses[static_cast<std::size_t>(validCount++)] = hypothesis;
        }
    }

    std::sort(hypotheses.begin(),
              hypotheses.begin() + validCount,
              [](const ConsensusHypothesis& left,
                 const ConsensusHypothesis& right)
              {
                  return left.evidenceScore > right.evidenceScore;
              });
    return validCount;
}

bool ModernPitchEngine::MultiRatePitchTracker::isOctaveLikeTransition(
    float fromFrequency,
    float toFrequency,
    int& octaveDelta,
    float& residualCents) noexcept
{
    octaveDelta = 0;
    residualCents = 100000.0f;
    if (fromFrequency <= 0.0f || toFrequency <= 0.0f)
        return false;

    const float octaveDistance = std::log2(toFrequency / fromFrequency);
    octaveDelta = static_cast<int>(std::lround(octaveDistance));
    residualCents = std::abs(1200.0f
        * (octaveDistance - static_cast<float>(octaveDelta)));
    return octaveDelta != 0 && std::abs(octaveDelta) <= 2
        && residualCents <= 85.0f;
}

void ModernPitchEngine::MultiRatePitchTracker::updateDecoderBeam(
    const std::array<ConsensusHypothesis, maxConsensusHypotheses>& hypotheses,
    int hypothesisCount,
    bool onsetPending) noexcept
{
    std::array<DecoderState, maxConsensusHypotheses + decoderBeamWidth> proposals {};
    int proposalCount = 0;

    for (int hypothesisIndex = 0;
         hypothesisIndex < hypothesisCount && proposalCount < maxConsensusHypotheses;
         ++hypothesisIndex)
    {
        const auto& hypothesis = hypotheses[static_cast<std::size_t>(hypothesisIndex)];
        if (!hypothesis.valid)
            continue;

        DecoderState proposal;
        proposal.valid = true;
        proposal.logFrequency = safeLog2(hypothesis.frequencyHz);
        proposal.score = hypothesis.evidenceScore + 0.26f * hypothesis.consensus;
        proposal.octaveIndex = octaveState_;

        float bestTransitionScore = -1000.0f;
        int bestOctaveIndex = octaveState_;
        bool foundPrevious = false;

        for (const auto& previous : decoderBeam_)
        {
            if (!previous.valid)
                continue;

            foundPrevious = true;
            const float deltaCents = static_cast<float>(1200.0
                * (proposal.logFrequency - previous.logFrequency));
            const float absoluteCents = std::abs(deltaCents);
            const float continuityBonus = 0.30f * std::exp(-absoluteCents / 85.0f);
            const float transitionPenalty = onsetPending
                ? 0.10f * std::min(1.0f, absoluteCents / 1800.0f)
                : 0.19f * std::min(2.0f, absoluteCents / 650.0f);

            int octaveDelta = 0;
            float residualCents = 0.0f;
            const bool octaveLike = isOctaveLikeTransition(
                static_cast<float>(std::exp2(previous.logFrequency)),
                hypothesis.frequencyHz,
                octaveDelta,
                residualCents);
            const float octavePenalty = octaveLike
                ? 0.24f * static_cast<float>(std::abs(octaveDelta))
                    * (1.0f - 0.70f * hypothesis.consensus)
                : 0.0f;

            const float historyWeight = onsetPending ? 0.24f : 0.72f;
            const float transitionScore = historyWeight * previous.score
                                        + proposal.score
                                        + continuityBonus
                                        - transitionPenalty
                                        - octavePenalty;
            if (transitionScore > bestTransitionScore)
            {
                bestTransitionScore = transitionScore;
                bestOctaveIndex = previous.octaveIndex
                    + (octaveLike ? octaveDelta : 0);
            }
        }

        if (foundPrevious)
            proposal.score = bestTransitionScore;
        proposal.octaveIndex = bestOctaveIndex;
        proposals[static_cast<std::size_t>(proposalCount++)] = proposal;
    }

    // A short hold branch prevents a single weak hop from forcing a jump.  It
    // decays quickly, so genuine new notes still win after fresh evidence.
    for (const auto& previous : decoderBeam_)
    {
        if (!previous.valid || proposalCount >= static_cast<int>(proposals.size()))
            continue;

        DecoderState held = previous;
        held.score = previous.score * (onsetPending ? 0.22f : 0.76f)
                   - (onsetPending ? 0.10f : 0.055f);
        ++held.ageInHops;
        if (held.ageInHops <= 4)
            proposals[static_cast<std::size_t>(proposalCount++)] = held;
    }

    std::sort(proposals.begin(),
              proposals.begin() + proposalCount,
              [](const DecoderState& left, const DecoderState& right)
              {
                  return left.score > right.score;
              });

    decoderBeam_.fill({});
    int accepted = 0;
    for (int proposalIndex = 0;
         proposalIndex < proposalCount && accepted < decoderBeamWidth;
         ++proposalIndex)
    {
        const auto& proposal = proposals[static_cast<std::size_t>(proposalIndex)];
        if (!proposal.valid)
            continue;

        bool duplicate = false;
        for (int existing = 0; existing < accepted; ++existing)
        {
            const float distance = static_cast<float>(1200.0
                * std::abs(proposal.logFrequency
                         - decoderBeam_[static_cast<std::size_t>(existing)].logFrequency));
            if (distance < 24.0f)
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            decoderBeam_[static_cast<std::size_t>(accepted++)] = proposal;
    }
}

ModernPitchEngine::MultiRatePitchTracker::DecoderDecision
ModernPitchEngine::MultiRatePitchTracker::decodeCandidate(bool onsetPending) noexcept
{
    std::array<PitchCandidate, detectorPathCount> candidates {};
    const int candidateCount = collectFreshCandidates(candidates);
    if (candidateCount <= 0)
        return {};

    std::array<ConsensusHypothesis, maxConsensusHypotheses> hypotheses {};
    const int hypothesisCount = buildConsensusHypotheses(candidates,
                                                         candidateCount,
                                                         hypotheses);
    if (hypothesisCount <= 0)
        return {};

    updateDecoderBeam(hypotheses, hypothesisCount, onsetPending);
    if (!decoderBeam_[0].valid)
        return {};

    const float decodedFrequency = static_cast<float>(
        std::exp2(decoderBeam_[0].logFrequency));

    int matchedHypothesis = -1;
    float matchedDistance = 100000.0f;
    for (int index = 0; index < hypothesisCount; ++index)
    {
        const float distance = centsDistance(
            hypotheses[static_cast<std::size_t>(index)].frequencyHz,
            decodedFrequency);
        if (distance < matchedDistance)
        {
            matchedDistance = distance;
            matchedHypothesis = index;
        }
    }

    if (matchedHypothesis < 0 || matchedDistance > 65.0f)
        return {}; // the winning branch is only a decaying hold state

    const auto& hypothesis = hypotheses[static_cast<std::size_t>(matchedHypothesis)];
    DecoderDecision decision;
    decision.candidate.frequencyHz = hypothesis.frequencyHz;
    decision.candidate.confidence = clamp01(hypothesis.confidence
        * (0.76f + 0.24f * hypothesis.consensus));
    decision.candidate.periodicity = hypothesis.periodicity;
    decision.candidate.valid = true;
    decision.consensus = hypothesis.consensus;
    decision.supportCount = hypothesis.supportCount;
    decision.directSupportCount = hypothesis.directSupportCount;
    decision.freshSupportMask = hypothesis.freshSupportMask;
    decision.decoderOctaveIndex = decoderBeam_[0].octaveIndex;

    const bool closeToTrack = trackedPitchHz_ > 0.0f
        && centsDistance(trackedPitchHz_, decision.candidate.frequencyHz) < 95.0f;
    const bool sufficientInitialEvidence = decision.supportCount >= 2
        || decision.candidate.confidence >= 0.78f;
    decision.valid = closeToTrack || sufficientInitialEvidence;
    return decision;
}

bool ModernPitchEngine::MultiRatePitchTracker::confirmOctaveTransition(
    DecoderDecision& decision,
    bool onsetPending) noexcept
{
    if (!decision.valid || trackedPitchHz_ <= 0.0f)
    {
        pendingOctaveDelta_ = 0;
        pendingOctaveCount_ = 0;
        pendingOctaveFrequencyHz_ = 0.0f;
        return decision.valid;
    }

    int octaveDelta = 0;
    float residualCents = 0.0f;
    if (!isOctaveLikeTransition(trackedPitchHz_,
                                decision.candidate.frequencyHz,
                                octaveDelta,
                                residualCents))
    {
        pendingOctaveDelta_ = 0;
        pendingOctaveCount_ = 0;
        pendingOctaveFrequencyHz_ = 0.0f;
        return true;
    }

    const bool samePending = pendingOctaveDelta_ == octaveDelta
        && pendingOctaveFrequencyHz_ > 0.0f
        && centsDistance(pendingOctaveFrequencyHz_,
                         decision.candidate.frequencyHz) < 70.0f;

    if (!samePending)
    {
        pendingOctaveDelta_ = octaveDelta;
        pendingOctaveCount_ = 0;
        pendingOctaveFrequencyHz_ = decision.candidate.frequencyHz;
    }

    // Count only genuinely refreshed evidence.  Reusing an old low-rate
    // candidate over several full-rate hops must not confirm a subharmonic.
    if (decision.freshSupportMask != 0)
        ++pendingOctaveCount_;

    int requiredObservations = octaveDelta < 0 ? 3 : 2;
    if (onsetPending && decision.supportCount >= 2
        && decision.directSupportCount >= 2
        && decision.consensus > 0.82f)
    {
        requiredObservations = 2;
    }

    const bool credibleConsensus = octaveDelta < 0
        ? (decision.directSupportCount >= 1
           && (decision.supportCount >= 2
               || (decision.candidate.confidence > 0.92f
                   && decision.consensus > 0.68f)))
        : (decision.supportCount >= 2
           || (decision.directSupportCount >= 1
               && decision.candidate.confidence > 0.90f
               && decision.consensus > 0.62f));

    if (!credibleConsensus || pendingOctaveCount_ < requiredObservations)
    {
        // Hold the committed octave while the challenger accumulates evidence.
        decision.candidate.frequencyHz = trackedPitchHz_;
        decision.candidate.confidence = trackedConfidence_ * 0.97f;
        decision.candidate.periodicity = trackedPeriodicity_;
        decision.consensus = trackedConsensus_;
        decision.supportCount = trackedSupportCount_;
        decision.decoderOctaveIndex = octaveState_;
        decision.valid = trackedPitchHz_ > 0.0f;
        return false;
    }

    octaveState_ += octaveDelta;
    pendingOctaveDelta_ = 0;
    pendingOctaveCount_ = 0;
    pendingOctaveFrequencyHz_ = 0.0f;
    decision.decoderOctaveIndex = octaveState_;
    return true;
}

bool ModernPitchEngine::MultiRatePitchTracker::processSample(
    float inputSample,
    PitchObservation& observation) noexcept
{
    observation = {};

    const float dcBlocked = inputSample - previousInput_
                          + dcBlockCoefficient_ * previousDcOutput_;
    previousInput_ = inputSample;
    previousDcOutput_ = dcBlocked;

    const float energy = dcBlocked * dcBlocked;
    fastEnergy_ += fastEnergyCoefficient_ * (energy - fastEnergy_);
    slowEnergy_ += slowEnergyCoefficient_ * (energy - slowEnergy_);

    if (onsetCooldownSamples_ > 0)
        --onsetCooldownSamples_;

    const float energyRatio = fastEnergy_ / std::max(1.0e-9f, slowEnergy_);
    const float energeticEnough = fastEnergy_ > minimumDetectorRms * minimumDetectorRms * 3.0f;
    const float onsetStrength = clamp01((energyRatio - 1.8f) / 3.2f);

    onsetEnvelope_ = std::max(onsetStrength, onsetEnvelope_ * 0.985f);

    if (energeticEnough && energyRatio > 3.1f && onsetCooldownSamples_ == 0)
    {
        onsetPending_ = true;
        onsetCooldownSamples_ = std::max(1,
            static_cast<int>(std::lround(sampleRate_ * 0.010)));
    }

    push(fullRateRing_, fullRateWritePosition_, fullRateAvailableSamples_, dcBlocked);

    const float halfFiltered = halfRateAntiAlias_.process(dcBlocked);
    if (++halfRateDecimationCounter_ >= 2)
    {
        halfRateDecimationCounter_ = 0;
        push(halfRateRing_, halfRateWritePosition_, halfRateAvailableSamples_, halfFiltered);

        const float quarterFiltered = quarterRateAntiAlias_.process(halfFiltered);
        if (++quarterRateDecimationCounter_ >= 2)
        {
            quarterRateDecimationCounter_ = 0;
            push(quarterRateRing_, quarterRateWritePosition_,
                 quarterRateAvailableSamples_, quarterFiltered);

            const float eighthFiltered = eighthRateAntiAlias_.process(quarterFiltered);
            if (++eighthRateDecimationCounter_ >= 2)
            {
                eighthRateDecimationCounter_ = 0;
                push(eighthRateRing_, eighthRateWritePosition_,
                     eighthRateAvailableSamples_, eighthFiltered);
            }
        }
    }

    if (++hopCounter_ < detectorHop)
        return false;

    hopCounter_ = 0;
    ++analysisHopCounter_;

    ++fullRateCandidate_.ageInHops;
    ++halfRateCandidate_.ageInHops;
    ++quarterRateCandidate_.ageInHops;
    ++eighthRateCandidate_.ageInHops;

    const float fullMinimum = std::max(160.0f, minimumPitchHz_);
    const float fullMaximum = std::min(maximumPitchHz_, 2600.0f);
    if (fullMinimum < fullMaximum)
    {
        fullRateCandidate_.candidate = analyse(fullRateRing_,
                                               fullRateWritePosition_,
                                               fullRateAvailableSamples_,
                                               sampleRate_,
                                               fullMinimum,
                                               fullMaximum);
        fullRateCandidate_.candidate.pathIndex = 0;
        fullRateCandidate_.candidate.ageInHops = 0;
        fullRateCandidate_.ageInHops = 0;
    }

    if ((analysisHopCounter_ & 1) == 0)
    {
        const float halfMinimum = std::max(78.0f, minimumPitchHz_);
        const float halfMaximum = std::min(maximumPitchHz_, 900.0f);
        if (halfMinimum < halfMaximum)
        {
            halfRateCandidate_.candidate = analyse(halfRateRing_,
                                                   halfRateWritePosition_,
                                                   halfRateAvailableSamples_,
                                                   sampleRate_ * 0.5,
                                                   halfMinimum,
                                                   halfMaximum);
            halfRateCandidate_.candidate.pathIndex = 1;
            halfRateCandidate_.candidate.ageInHops = 0;
            halfRateCandidate_.ageInHops = 0;
        }
    }

    if ((analysisHopCounter_ & 3) == 0)
    {
        const float quarterMinimum = std::max(35.0f, minimumPitchHz_);
        const float quarterMaximum = std::min(maximumPitchHz_, 460.0f);
        if (quarterMinimum < quarterMaximum)
        {
            quarterRateCandidate_.candidate = analyse(quarterRateRing_,
                                                      quarterRateWritePosition_,
                                                      quarterRateAvailableSamples_,
                                                      sampleRate_ * 0.25,
                                                      quarterMinimum,
                                                      quarterMaximum);
            quarterRateCandidate_.candidate.pathIndex = 2;
            quarterRateCandidate_.candidate.ageInHops = 0;
            quarterRateCandidate_.ageInHops = 0;
        }
    }

    if ((analysisHopCounter_ & 7) == 0)
    {
        const float eighthMinimum = std::max(25.0f, minimumPitchHz_);
        const float eighthMaximum = std::min(maximumPitchHz_, 230.0f);
        if (eighthMinimum < eighthMaximum)
        {
            eighthRateCandidate_.candidate = analyse(eighthRateRing_,
                                                     eighthRateWritePosition_,
                                                     eighthRateAvailableSamples_,
                                                     sampleRate_ * 0.125,
                                                     eighthMinimum,
                                                     eighthMaximum);
            eighthRateCandidate_.candidate.pathIndex = 3;
            eighthRateCandidate_.candidate.ageInHops = 0;
            eighthRateCandidate_.ageInHops = 0;
        }
    }

    DecoderDecision decision = decodeCandidate(onsetPending_);
    const int previousOctaveState = octaveState_;
    const bool decoderDecisionAccepted = confirmOctaveTransition(decision,
                                                                  onsetPending_);
    const bool committedOctaveChange = octaveState_ != previousOctaveState;

    if (decision.valid && decision.candidate.valid)
    {
        const bool firstLock = trackedPitchHz_ <= 0.0f;
        const float selectedLog = std::log2(decision.candidate.frequencyHz);
        const float trackedLog = firstLock ? selectedLog : std::log2(trackedPitchHz_);

        // An onset may move faster than a stable note, but it no longer bypasses
        // the decoder.  Confirmed octave changes remain intentionally smoother
        // to avoid a low-frequency burst when the decision is first committed.
        float smoothing = 0.32f;
        if (firstLock)
            smoothing = 1.0f;
        else if (committedOctaveChange)
            smoothing = 0.24f;
        else if (onsetPending_)
            smoothing = decoderDecisionAccepted ? 0.58f : 0.36f;

        trackedPitchHz_ = std::exp2(trackedLog
            + smoothing * (selectedLog - trackedLog));
        trackedConfidence_ += 0.38f
            * (decision.candidate.confidence - trackedConfidence_);
        trackedPeriodicity_ += 0.38f
            * (decision.candidate.periodicity - trackedPeriodicity_);
        trackedConsensus_ += 0.35f
            * (decision.consensus - trackedConsensus_);
        trackedSupportCount_ = decision.supportCount;
        invalidHopCount_ = 0;

        const float rmsGate = smoothStep(minimumDetectorRms,
                                         minimumDetectorRms * 4.0f,
                                         std::sqrt(std::max(0.0f, slowEnergy_)));
        const float confidenceGate = smoothStep(0.42f, 0.88f, trackedConfidence_);
        const float periodicityGate = smoothStep(0.48f, 0.90f, trackedPeriodicity_);
        const float consensusGate = smoothStep(0.10f, 0.78f, trackedConsensus_);

        observation.frequencyHz = trackedPitchHz_;
        observation.confidence = trackedConfidence_;
        observation.periodicity = trackedPeriodicity_;
        observation.consensus = trackedConsensus_;
        observation.detectorSupport = trackedSupportCount_;
        observation.octaveState = octaveState_;
        observation.pendingOctaveObservations = pendingOctaveCount_;
        observation.voicing = clamp01(rmsGate
            * (0.48f * confidenceGate
             + 0.30f * periodicityGate
             + 0.22f * consensusGate));
        observation.valid = observation.voicing > 0.08f;
    }
    else
    {
        ++invalidHopCount_;
        trackedConfidence_ *= 0.90f;
        trackedPeriodicity_ *= 0.90f;
        trackedConsensus_ *= 0.88f;

        if (invalidHopCount_ > 12)
        {
            trackedPitchHz_ = 0.0f;
            trackedConfidence_ = 0.0f;
            trackedPeriodicity_ = 0.0f;
            trackedConsensus_ = 0.0f;
            trackedSupportCount_ = 0;
            decoderBeam_.fill({});
            pendingOctaveDelta_ = 0;
            pendingOctaveCount_ = 0;
            pendingOctaveFrequencyHz_ = 0.0f;
        }

        observation.frequencyHz = trackedPitchHz_;
        observation.confidence = trackedConfidence_;
        observation.periodicity = trackedPeriodicity_;
        observation.consensus = trackedConsensus_;
        observation.detectorSupport = trackedSupportCount_;
        observation.octaveState = octaveState_;
        observation.pendingOctaveObservations = pendingOctaveCount_;
        observation.voicing = 0.0f;
        observation.valid = false;
    }

    observation.onset = onsetPending_;
    observation.onsetStrength = onsetPending_ ? std::max(0.65f, onsetEnvelope_)
                                             : onsetEnvelope_;
    onsetPending_ = false;
    return true;
}

//==============================================================================
// ScaleQuantizer

std::uint64_t ModernPitchEngine::ScaleQuantizer::hashScale(
    const double* scaleRatios,
    int numberOfScaleRatios,
    double rootFrequency) noexcept
{
    constexpr std::uint64_t offsetBasis = 1469598103934665603ull;
    std::uint64_t hash = offsetBasis;

    const auto mix = [&hash](std::uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };

    const int safeCount = std::clamp(numberOfScaleRatios, 0, maxScaleRatios);
    mix(static_cast<std::uint64_t>(safeCount));

    std::uint64_t rootBits = 0;
    std::memcpy(&rootBits, &rootFrequency, sizeof(rootBits));
    mix(rootBits);

    for (int index = 0; index < safeCount; ++index)
    {
        const double ratio = scaleRatios != nullptr ? scaleRatios[index] : 0.0;
        std::uint64_t ratioBits = 0;
        std::memcpy(&ratioBits, &ratio, sizeof(ratioBits));
        mix(ratioBits);
    }

    return hash;
}

bool ModernPitchEngine::ScaleQuantizer::update(const double* scaleRatios,
                                                int numberOfScaleRatios,
                                                double rootFrequency) noexcept
{
    if (scaleRatios == nullptr || numberOfScaleRatios <= 0
        || !std::isfinite(rootFrequency) || rootFrequency <= 0.0)
    {
        const bool changed = cachedScaleSize_ != 0;
        cachedScaleSize_ = 0;
        targetValid_ = false;
        scaleHash_ = 0;
        return changed;
    }

    const int safeCount = std::clamp(numberOfScaleRatios, 1, maxScaleRatios);
    const std::uint64_t newHash = hashScale(scaleRatios, safeCount, rootFrequency);
    if (newHash == scaleHash_)
        return false;

    scaleHash_ = newHash;
    rootLog2_ = safeLog2(rootFrequency);
    cachedScaleSize_ = 0;

    for (int index = 0; index < safeCount; ++index)
    {
        double ratio = scaleRatios[index];
        if (!std::isfinite(ratio) || ratio <= 0.0)
            continue;

        while (ratio < 1.0)
            ratio *= 2.0;
        while (ratio >= 2.0)
            ratio *= 0.5;

        cachedScaleLogRatios_[static_cast<std::size_t>(cachedScaleSize_++)]
            = std::log2(ratio);
    }

    std::sort(cachedScaleLogRatios_.begin(),
              cachedScaleLogRatios_.begin() + cachedScaleSize_);

    int uniqueCount = 0;
    for (int index = 0; index < cachedScaleSize_; ++index)
    {
        const double value = cachedScaleLogRatios_[static_cast<std::size_t>(index)];
        if (uniqueCount == 0
            || std::abs(value
                        - cachedScaleLogRatios_[static_cast<std::size_t>(uniqueCount - 1)])
                   > 1.0e-8)
        {
            cachedScaleLogRatios_[static_cast<std::size_t>(uniqueCount++)] = value;
        }
    }

    cachedScaleSize_ = uniqueCount;
    targetValid_ = false;
    return true;
}

void ModernPitchEngine::ScaleQuantizer::resetTarget() noexcept
{
    targetValid_ = false;
}

double ModernPitchEngine::ScaleQuantizer::chooseTargetLog2(double inputLog2,
                                                            float hysteresisCents) noexcept
{
    if (cachedScaleSize_ <= 0 || !std::isfinite(inputLog2))
        return inputLog2;

    const double relativeLog = inputLog2 - rootLog2_;
    const double baseOctave = std::floor(relativeLog);
    double bestTarget = inputLog2;
    double bestDistance = std::numeric_limits<double>::max();

    for (int index = 0; index < cachedScaleSize_; ++index)
    {
        const double scalePosition = cachedScaleLogRatios_[static_cast<std::size_t>(index)];
        for (int octaveOffset = -1; octaveOffset <= 1; ++octaveOffset)
        {
            const double target = rootLog2_ + baseOctave
                                + static_cast<double>(octaveOffset)
                                + scalePosition;
            const double distance = std::abs(target - inputLog2);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestTarget = target;
            }
        }
    }

    if (targetValid_)
    {
        const double previousDistance = std::abs(currentTargetLog2_ - inputLog2);
        const double hysteresisOctaves = std::max(0.0f, hysteresisCents) / 1200.0;
        if (previousDistance <= bestDistance + hysteresisOctaves)
            bestTarget = currentTargetLog2_;
    }

    currentTargetLog2_ = bestTarget;
    targetValid_ = true;
    return currentTargetLog2_;
}

//==============================================================================
// CorrectionController

void ModernPitchEngine::CorrectionController::prepare(double sampleRate) noexcept
{
    sampleRate_ = std::max(8000.0, sampleRate);

    authorityAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.006 * sampleRate_)));
    authorityReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.035 * sampleRate_)));
    wetAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.010 * sampleRate_)));
    wetReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.055 * sampleRate_)));

    reset();
}

void ModernPitchEngine::CorrectionController::reset() noexcept
{
    state_ = TrackingState::unvoiced;
    stateSamplesRemaining_ = 0;
    stableObservationCount_ = 0;
    invalidObservationCount_ = 0;
    observedLog2_ = 0.0;
    pitchCentreLog2_ = 0.0;
    targetLog2_ = 0.0;
    pitchCentreValid_ = false;
    targetValid_ = false;
    desiredCorrectionCents_ = 0.0;
    synthesisTargetCorrectionCents_ = 0.0;
    currentCorrectionCents_ = 0.0;
    correctionVelocityCentsPerSecond_ = 0.0;
    targetRevision_ = 0;
    currentConfidence_ = 0.0f;
    currentVoicing_ = 0.0f;
    currentOnsetStrength_ = 0.0f;
    spectralBreathiness_ = 0.0f;
    spectralHarmonicity_ = 1.0f;
    authority_ = 0.0f;
    authorityTarget_ = 0.0f;
    wetMix_ = 0.0f;
    wetMixTarget_ = 0.0f;
    smoothedVoicing_ = 0.0f;
    voicedLatched_ = false;
    voicedEnterCount_ = 0;
    voicedExitCount_ = 0;
}

void ModernPitchEngine::CorrectionController::setSpectralReliability(
    float breathiness,
    float harmonicity) noexcept
{
    spectralBreathiness_ = clamp01(breathiness);
    spectralHarmonicity_ = clamp01(harmonicity);
}

void ModernPitchEngine::CorrectionController::enterState(TrackingState newState,
                                                          int durationSamples) noexcept
{
    state_ = newState;
    stateSamplesRemaining_ = std::max(0, durationSamples);
}

void ModernPitchEngine::CorrectionController::updateVoicingLatch(
    bool observationUsable,
    float voicing,
    float sensitivity) noexcept
{
    const float safeSensitivity = clamp01(sensitivity);
    const float targetVoicing = observationUsable ? clamp01(voicing) : 0.0f;

    // This is deliberately evaluated only when a new pitch observation arrives.
    // A relatively fast attack and a slower release prevent noisy vowels from
    // repeatedly switching the corrected path on and off.
    const float smoothing = targetVoicing > smoothedVoicing_ ? 0.42f : 0.18f;
    smoothedVoicing_ += smoothing * (targetVoicing - smoothedVoicing_);

    const float enterThreshold = 0.60f - 0.10f * safeSensitivity;
    const float exitThreshold = 0.38f - 0.06f * safeSensitivity;

    if (!voicedLatched_)
    {
        voicedExitCount_ = 0;
        if (observationUsable && smoothedVoicing_ >= enterThreshold)
            ++voicedEnterCount_;
        else
            voicedEnterCount_ = 0;

        if (voicedEnterCount_ >= 2)
        {
            voicedLatched_ = true;
            voicedEnterCount_ = 0;
        }
    }
    else
    {
        voicedEnterCount_ = 0;
        if (!observationUsable || smoothedVoicing_ <= exitThreshold)
            ++voicedExitCount_;
        else
            voicedExitCount_ = 0;

        if (voicedExitCount_ >= 4)
        {
            voicedLatched_ = false;
            voicedExitCount_ = 0;
        }
    }
}

float ModernPitchEngine::CorrectionController::confidenceAuthority(
    float confidence,
    float sensitivity) const noexcept
{
    const float safeSensitivity = clamp01(sensitivity);
    const float low = 0.62f - 0.20f * safeSensitivity;
    const float high = 0.90f - 0.08f * safeSensitivity;
    return smoothStep(low, high, confidence);
}

float ModernPitchEngine::CorrectionController::getFormantStability() const noexcept
{
    // During attacks and note changes the spectral envelope is less reliable.
    // Reducing, rather than disabling, formant compensation avoids a timbral
    // discontinuity while the new trajectory settles.
    switch (state_)
    {
        case TrackingState::unvoiced:   return 0.45f;
        case TrackingState::attack:     return 0.35f;
        case TrackingState::acquire:    return 0.68f;
        case TrackingState::stable:     return 1.00f;
        case TrackingState::transition: return 0.58f;
        case TrackingState::release:    return 0.65f;
    }

    return 1.0f;
}

void ModernPitchEngine::CorrectionController::acceptObservation(
    const PitchObservation& observation,
    ScaleQuantizer& quantizer,
    const Parameters& parameters) noexcept
{
    currentConfidence_ = clamp01(observation.confidence);
    currentVoicing_ = clamp01(observation.voicing);
    currentOnsetStrength_ = clamp01(observation.onsetStrength);

    const float minimumUsableVoicing = 0.07f + 0.12f
        * (1.0f - clamp01(parameters.detectorSensitivity));
    const bool observationUsable = observation.valid
                                && observation.frequencyHz > 0.0f
                                && currentVoicing_ >= minimumUsableVoicing
                                && quantizer.hasScale();

    updateVoicingLatch(observationUsable,
                       currentVoicing_,
                       parameters.detectorSensitivity);

    if (observation.onset)
    {
        const float protection = clamp01(parameters.transientProtection);
        const double attackMs = 1.5 + 8.5 * static_cast<double>(protection);
        enterState(TrackingState::attack,
                   std::max(1, static_cast<int>(std::lround(
                       attackMs * 0.001 * sampleRate_))));
        stableObservationCount_ = 0;
        invalidObservationCount_ = 0;
        pitchCentreValid_ = false;
        targetValid_ = false;
        quantizer.resetTarget();

        // Do not hard-mute the wet path here. The state machine and the
        // asymmetric release below fade it smoothly, preserving continuity.
    }

    if (!observationUsable)
    {
        ++invalidObservationCount_;
        stableObservationCount_ = 0;

        // Short confidence drop-outs are common inside vowels. Hold the last
        // musical decision for a few observations instead of immediately
        // returning the shifter to ratio 1, which sounds like wind/pumping.
        if (invalidObservationCount_ <= 3)
        {
            // Keep the musical target through short detector drop-outs, but do
            // not preserve noisy pitch modulation when the spectral path says
            // that the tail is predominantly breath/residual.
            const float breathRelease = 1.0f - 0.20f * spectralBreathiness_;
            authorityTarget_ *= 0.94f * breathRelease;
            wetMixTarget_ *= 0.96f;
            return;
        }

        authorityTarget_ = 0.0f;
        wetMixTarget_ = 0.0f;
        desiredCorrectionCents_ = 0.0;

        if (invalidObservationCount_ > 3 && state_ != TrackingState::attack)
            enterState(TrackingState::release,
                       std::max(1, static_cast<int>(std::lround(0.040 * sampleRate_))));
        if (invalidObservationCount_ > 18)
        {
            enterState(TrackingState::unvoiced);
            pitchCentreValid_ = false;
            targetValid_ = false;
        }
        return;
    }

    invalidObservationCount_ = 0;
    observedLog2_ = safeLog2(observation.frequencyHz);

    if (!pitchCentreValid_ || observation.onset)
    {
        pitchCentreLog2_ = observedLog2_;
        pitchCentreValid_ = true;
    }
    else
    {
        const double distanceCents = std::abs((observedLog2_ - pitchCentreLog2_) * 1200.0);
        const double baseCentreAlpha = distanceCents > 90.0 ? 0.32 : 0.10;
        const double breathStability = std::clamp(
            1.0 - 0.72 * static_cast<double>(spectralBreathiness_),
            0.20,
            1.0);
        const double harmonicStability = 0.55
            + 0.45 * static_cast<double>(spectralHarmonicity_);
        const double centreAlpha = baseCentreAlpha
            * breathStability
            * harmonicStability;
        pitchCentreLog2_ += centreAlpha * (observedLog2_ - pitchCentreLog2_);
    }

    const float hysteresisCents = 18.0f + 38.0f * clamp01(parameters.humanize);
    double newTargetLog2 = quantizer.chooseTargetLog2(pitchCentreLog2_,
                                                       hysteresisCents);

    // Defensive register lock. Scale degrees repeat every octave, therefore
    // the selected target must always be the octave-equivalent target nearest
    // to the tracked pitch centre. This prevents a stale octave state or a
    // custom-scale edge case from publishing an accidental -/+1200-cent move.
    newTargetLog2 = alignTargetToNearestOctave(newTargetLog2,
                                               pitchCentreLog2_);

    const double targetJumpCents = targetValid_
        ? std::abs((newTargetLog2 - targetLog2_) * 1200.0)
        : std::numeric_limits<double>::infinity();

    // A revision marks a real musical target change, independently from the
    // per-sample correction trajectory. The downstream TransitionManager uses
    // it to arm the second synthesis layer exactly once per note decision.
    // 18 cents keeps microtonal steps eligible while rejecting detector jitter.
    if (!targetValid_ || targetJumpCents > 18.0)
        ++targetRevision_;

    if (targetValid_ && targetJumpCents > 48.0
        && state_ != TrackingState::attack)
    {
        const double transitionMs = std::max(1.0f, parameters.transitionTimeMs);
        enterState(TrackingState::transition,
                   std::max(1, static_cast<int>(std::lround(
                       transitionMs * 0.001 * sampleRate_))));
        stableObservationCount_ = 0;
    }

    targetLog2_ = newTargetLog2;
    targetValid_ = true;

    const double vibratoComponent = observedLog2_ - pitchCentreLog2_;

    // Breath-aware vibrato gate.  Aperiodic pitch jitter must not be promoted
    // to a sustained musical modulation.  Clean, harmonic vowels retain the
    // user's full preserveVibrato setting; aspirated tails progressively
    // collapse toward the tracked pitch centre.
    const float cleanBreath = 1.0f - spectralBreathiness_;
    const float vibratoReliability = clamp01(
        spectralHarmonicity_ * cleanBreath * cleanBreath);
    const float effectivePreserveVibrato =
        clamp01(parameters.preserveVibrato) * vibratoReliability;
    const double correctedLog2 = targetLog2_
        + static_cast<double>(effectivePreserveVibrato) * vibratoComponent;
    double errorCents = (correctedLog2 - observedLog2_) * 1200.0;
    errorCents = wrapCorrectionToNearestOctave(errorCents);

    const double deadBandCents = 1.5 + 20.0 * static_cast<double>(clamp01(parameters.humanize));
    if (std::abs(errorCents) <= deadBandCents)
    {
        errorCents = 0.0;
    }
    else
    {
        errorCents = std::copysign(std::abs(errorCents) - deadBandCents,
                                   errorCents);
    }

    const double maxCorrectionCents = 1200.0 * std::clamp(
        static_cast<double>(parameters.maximumCorrectionSemitones), 0.0, 24.0);
    errorCents = std::clamp(errorCents,
                            -maxCorrectionCents,
                            maxCorrectionCents);
    desiredCorrectionCents_ = errorCents
        * static_cast<double>(clamp01(parameters.amount));

    const float confidenceGate = confidenceAuthority(currentConfidence_,
                                                      parameters.detectorSensitivity);
    authorityTarget_ = clamp01(confidenceGate
                               * (voicedLatched_ ? smoothedVoicing_ : 0.0f));

    const float correctionNeed = smoothStep(
        1.5f, 10.0f, static_cast<float>(std::abs(desiredCorrectionCents_)));
    const float transientAttenuation = 1.0f
        - clamp01(parameters.transientProtection) * currentOnsetStrength_;
    wetMixTarget_ = clamp01((voicedLatched_ ? smoothedVoicing_ : 0.0f)
                            * correctionNeed
                            * transientAttenuation);

    ++stableObservationCount_;
    if (state_ == TrackingState::unvoiced || state_ == TrackingState::release)
        enterState(TrackingState::acquire,
                   std::max(1, static_cast<int>(std::lround(0.008 * sampleRate_))));
    else if (state_ == TrackingState::acquire && stableObservationCount_ >= 3)
        enterState(TrackingState::stable);
}

void ModernPitchEngine::CorrectionController::advanceOneSample(
    const Parameters& parameters) noexcept
{
    if (stateSamplesRemaining_ > 0)
    {
        --stateSamplesRemaining_;
        if (stateSamplesRemaining_ == 0)
        {
            if (state_ == TrackingState::attack)
                enterState(TrackingState::acquire,
                           std::max(1, static_cast<int>(std::lround(0.007 * sampleRate_))));
            else if (state_ == TrackingState::transition)
                enterState(TrackingState::stable);
            else if (state_ == TrackingState::release)
                enterState(TrackingState::unvoiced);
        }
    }

    float stateAuthorityScale = 1.0f;
    switch (state_)
    {
        case TrackingState::unvoiced:   stateAuthorityScale = 0.0f; break;
        case TrackingState::attack:     stateAuthorityScale = 0.10f; break;
        case TrackingState::acquire:    stateAuthorityScale = 0.68f; break;
        case TrackingState::stable:     stateAuthorityScale = 1.0f; break;
        case TrackingState::transition: stateAuthorityScale = 0.82f; break;
        case TrackingState::release:    stateAuthorityScale = 0.20f; break;
    }

    const float effectiveAuthorityTarget = authorityTarget_ * stateAuthorityScale;
    const float authorityCoefficient = effectiveAuthorityTarget > authority_
        ? authorityAttackCoefficient_
        : authorityReleaseCoefficient_;
    authority_ += authorityCoefficient * (effectiveAuthorityTarget - authority_);

    const float effectiveWetTarget = wetMixTarget_ * stateAuthorityScale;
    const float wetCoefficient = effectiveWetTarget > wetMix_
        ? wetAttackCoefficient_
        : wetReleaseCoefficient_;
    wetMix_ += wetCoefficient * (effectiveWetTarget - wetMix_);

    const double targetCorrectionCents = desiredCorrectionCents_
                                       * static_cast<double>(authority_);
    synthesisTargetCorrectionCents_ = targetCorrectionCents;

    double responseMs = std::max(0.35, static_cast<double>(parameters.retuneTimeMs));
    if (state_ == TrackingState::transition)
        responseMs = std::max(responseMs,
                              static_cast<double>(parameters.transitionTimeMs));
    else if (state_ == TrackingState::acquire)
        responseMs = std::max(responseMs, 4.0);

    const double dt = 1.0 / sampleRate_;
    const double responseSeconds = responseMs * 0.001;
    const double omega = std::min(0.22 / dt, 4.6 / std::max(0.00035, responseSeconds));

    double acceleration = omega * omega
                            * (targetCorrectionCents - currentCorrectionCents_)
                        - 2.0 * omega * correctionVelocityCentsPerSecond_;

    const double maximumVelocity = std::max(2400.0,
        8.0 * std::max(1200.0, std::abs(targetCorrectionCents))
            / std::max(0.001, responseSeconds));
    const double maximumAcceleration = maximumVelocity
                                     / std::max(0.0005, responseSeconds * 0.35);
    acceleration = std::clamp(acceleration,
                              -maximumAcceleration,
                              maximumAcceleration);

    correctionVelocityCentsPerSecond_ += acceleration * dt;
    correctionVelocityCentsPerSecond_ = std::clamp(
        correctionVelocityCentsPerSecond_,
        -maximumVelocity,
        maximumVelocity);
    currentCorrectionCents_ += correctionVelocityCentsPerSecond_ * dt;

    if (std::abs(targetCorrectionCents - currentCorrectionCents_) < 0.002
        && std::abs(correctionVelocityCentsPerSecond_) < 0.02)
    {
        currentCorrectionCents_ = targetCorrectionCents;
        correctionVelocityCentsPerSecond_ = 0.0;
    }
}

double ModernPitchEngine::CorrectionController::getPitchRatio() const noexcept
{
    return std::exp2(currentCorrectionCents_ / 1200.0);
}

float ModernPitchEngine::CorrectionController::getTargetPitchHz() const noexcept
{
    if (!targetValid_)
        return 0.0f;
    return static_cast<float>(std::exp2(targetLog2_));
}

//==============================================================================
// TransitionManager

void ModernPitchEngine::TransitionManager::prepare(
    double sampleRate,
    int synthesisFrameSize,
    LatencyMode latencyMode) noexcept
{
    sampleRate_ = std::max(8000.0, sampleRate);
    synthesisFrameSize_ = std::max(64, synthesisFrameSize);
    synthesisHopSize_ = std::max(1, synthesisFrameSize_ / 4);
    latencyMode_ = latencyMode;
    reset();
}

void ModernPitchEngine::TransitionManager::reset() noexcept
{
    phase_ = Phase::idle;
    initialised_ = false;
    pendingTarget_ = false;
    pendingForceTransition_ = false;
    beginEventPending_ = false;
    lastSeenRevision_ = 0;
    pendingRevision_ = 0;
    transitionRevision_ = 0;
    idleCents_ = 0.0;
    primaryCents_ = 0.0;
    secondaryCents_ = 0.0;
    secondaryVelocityCentsPerSecond_ = 0.0;
    transitionTargetCents_ = 0.0;
    pendingTargetCents_ = 0.0;
    preRollSamplesRemaining_ = 0;
    crossfadeSamplesTotal_ = 1;
    crossfadeSampleIndex_ = 0;
    transitionCooldownSamples_ = 0;
    publishedBlend_ = 0.0f;
}

double ModernPitchEngine::TransitionManager::transitionThresholdCents() const noexcept
{
    // Adjacent microtonal targets must remain eligible, while fluctuations
    // smaller than a quarter tone are better handled by the primary trajectory.
    switch (latencyMode_)
    {
        case LatencyMode::ultraLive: return 24.0;
        case LatencyMode::live:      return 28.0;
        case LatencyMode::quality:   return 32.0;
    }

    return 28.0;
}

int ModernPitchEngine::TransitionManager::crossfadeLengthSamples(
    const Parameters& parameters) const noexcept
{
    double minimumMs = 10.0;
    switch (latencyMode_)
    {
        case LatencyMode::ultraLive: minimumMs = 7.0; break;
        case LatencyMode::live:      minimumMs = 10.0; break;
        case LatencyMode::quality:   minimumMs = 14.0; break;
    }

    const double requestedMs = std::clamp(
        0.30 * static_cast<double>(parameters.transitionTimeMs),
        minimumMs,
        24.0);
    return std::max(1, static_cast<int>(std::lround(
        requestedMs * 0.001 * sampleRate_)));
}

void ModernPitchEngine::TransitionManager::startTransition(
    double currentCents,
    double targetCents,
    const Parameters& parameters) noexcept
{
    primaryCents_ = currentCents;
    secondaryCents_ = currentCents;
    secondaryVelocityCentsPerSecond_ = 0.0;
    transitionTargetCents_ = targetCents;

    // The second layer is generated immediately but remains inaudible until a
    // complete overlap-add history exists. This is synthesis pre-roll, not
    // added plugin latency: the primary layer continues to produce audio.
    preRollSamplesRemaining_ = synthesisFrameSize_ + synthesisHopSize_;
    crossfadeSamplesTotal_ = crossfadeLengthSamples(parameters);
    crossfadeSampleIndex_ = 0;
    phase_ = Phase::preRoll;
    beginEventPending_ = true;
    publishedBlend_ = 0.0f;
}

void ModernPitchEngine::TransitionManager::updateSecondaryTrajectory(
    double targetCents,
    const Parameters& parameters) noexcept
{
    transitionTargetCents_ = std::clamp(targetCents, -2400.0, 2400.0);

    const double dt = 1.0 / sampleRate_;
    double minimumResponseMs = 7.0;
    switch (latencyMode_)
    {
        case LatencyMode::ultraLive: minimumResponseMs = 7.0; break;
        case LatencyMode::live:      minimumResponseMs = 10.0; break;
        case LatencyMode::quality:   minimumResponseMs = 14.0; break;
    }

    const double responseMs = std::clamp(
        std::max(minimumResponseMs,
                 static_cast<double>(parameters.transitionTimeMs)),
        minimumResponseMs,
        85.0);
    const double responseSeconds = responseMs * 0.001;
    const double omega = std::min(0.20 / dt,
                                  4.6 / std::max(0.001, responseSeconds));
    const double error = transitionTargetCents_ - secondaryCents_;

    double acceleration = omega * omega * error
                        - 2.0 * omega * secondaryVelocityCentsPerSecond_;
    const double maximumVelocity = std::max(
        4800.0,
        9.0 * std::max(100.0, std::abs(error))
            / std::max(0.001, responseSeconds));
    const double maximumAcceleration = maximumVelocity
        / std::max(0.0005, responseSeconds * 0.30);

    acceleration = std::clamp(acceleration,
                              -maximumAcceleration,
                              maximumAcceleration);
    secondaryVelocityCentsPerSecond_ += acceleration * dt;
    secondaryVelocityCentsPerSecond_ = std::clamp(
        secondaryVelocityCentsPerSecond_,
        -maximumVelocity,
        maximumVelocity);
    secondaryCents_ += secondaryVelocityCentsPerSecond_ * dt;

    if (std::abs(error) < 0.003
        && std::abs(secondaryVelocityCentsPerSecond_) < 0.03)
    {
        secondaryCents_ = transitionTargetCents_;
        secondaryVelocityCentsPerSecond_ = 0.0;
    }
}

ModernPitchEngine::TransitionManager::Command
ModernPitchEngine::TransitionManager::processSample(
    double controllerCorrectionCents,
    double destinationCorrectionCents,
    std::uint64_t targetRevision,
    TrackingState trackingState,
    float wetMix,
    const Parameters& parameters,
    bool forceTransition) noexcept
{
    Command command;
    controllerCorrectionCents = wrapCorrectionToNearestOctave(
        controllerCorrectionCents);
    destinationCorrectionCents = wrapCorrectionToNearestOctave(
        destinationCorrectionCents);

    if (!initialised_)
    {
        initialised_ = true;
        lastSeenRevision_ = targetRevision;
        idleCents_ = controllerCorrectionCents;
        primaryCents_ = idleCents_;
        secondaryCents_ = idleCents_;
    }

    if (targetRevision != lastSeenRevision_)
    {
        lastSeenRevision_ = targetRevision;
        pendingRevision_ = targetRevision;
        pendingTargetCents_ = destinationCorrectionCents;
        pendingTarget_ = true;
        pendingForceTransition_ = forceTransition;
    }
    else if (pendingTarget_)
    {
        // Authority and transient protection may still be settling after the
        // note decision; retain the latest effective destination.
        pendingTargetCents_ = destinationCorrectionCents;
        pendingForceTransition_ = pendingForceTransition_ || forceTransition;
    }

    if (phase_ == Phase::idle)
    {
        if (transitionCooldownSamples_ > 0)
            --transitionCooldownSamples_;

        // Follow the already-smoothed controller with a very high slew limit.
        // This keeps normal vibrato continuous without reintroducing a second
        // audible low-pass stage after CorrectionController.
        const double maximumStep = 24000.0 / sampleRate_;
        idleCents_ += std::clamp(controllerCorrectionCents - idleCents_,
                                 -maximumStep,
                                 maximumStep);

        if (pendingTarget_)
        {
            const double jumpCents = pendingTargetCents_ - idleCents_;
            const bool musicalState = trackingState != TrackingState::unvoiced
                                   && trackingState != TrackingState::release;

            const double requiredJump = pendingForceTransition_
                ? 1.0
                : transitionThresholdCents();

            if (musicalState
                && transitionCooldownSamples_ <= 0
                && std::abs(jumpCents) >= requiredJump)
            {
                startTransition(idleCents_, pendingTargetCents_, parameters);
                transitionRevision_ = pendingRevision_;
                pendingTarget_ = false;
                pendingForceTransition_ = false;
            }
            else if ((!pendingForceTransition_
                      && std::abs(jumpCents) < transitionThresholdCents())
                     || (trackingState == TrackingState::unvoiced
                         && wetMix < 0.01f))
            {
                // Small pitch motion stays on the primary path. This is useful
                // for vibrato and very fine scale steps where dual synthesis
                // would be more expensive than beneficial.
                pendingTarget_ = false;
                pendingForceTransition_ = false;
            }
        }

        if (phase_ == Phase::idle)
        {
            command.primaryCents = wrapCorrectionToNearestOctave(idleCents_);
            command.secondaryCents = command.primaryCents;
            publishedBlend_ = 0.0f;
            return command;
        }
    }

    // A new decision received early in the transition can safely retarget the
    // secondary path. Later decisions are queued for the next transition so a
    // crossfade is never reversed halfway through.
    if (pendingTarget_)
    {
        const bool earlyEnough = phase_ == Phase::preRoll
                              || publishedBlend_ < 0.35f;
        if (earlyEnough)
        {
            transitionTargetCents_ = pendingTargetCents_;
            transitionRevision_ = pendingRevision_;
            pendingTarget_ = false;
        }
    }

    if (targetRevision == transitionRevision_)
        transitionTargetCents_ = destinationCorrectionCents;

    updateSecondaryTrajectory(transitionTargetCents_, parameters);

    command.primaryCents = wrapCorrectionToNearestOctave(primaryCents_);
    command.secondaryCents = wrapCorrectionToNearestOctave(secondaryCents_);
    command.dualSynthesis = true;
    command.beginSecondary = beginEventPending_;
    beginEventPending_ = false;

    if (phase_ == Phase::preRoll)
    {
        command.blend = 0.0f;
        if (preRollSamplesRemaining_ > 0)
            --preRollSamplesRemaining_;
        if (preRollSamplesRemaining_ <= 0)
        {
            phase_ = Phase::crossfade;
            crossfadeSampleIndex_ = 0;
        }
    }
    else
    {
        const float linearPhase = crossfadeSamplesTotal_ > 1
            ? static_cast<float>(crossfadeSampleIndex_)
                / static_cast<float>(crossfadeSamplesTotal_ - 1)
            : 1.0f;
        command.blend = smoothStep(0.0f, 1.0f, linearPhase);
        ++crossfadeSampleIndex_;

        if (crossfadeSampleIndex_ >= crossfadeSamplesTotal_)
        {
            command.blend = 1.0f;
            command.commitSecondary = true;
            idleCents_ = secondaryCents_;
            primaryCents_ = secondaryCents_;
            phase_ = Phase::idle;
            crossfadeSampleIndex_ = 0;
            transitionCooldownSamples_ = std::max(
                synthesisFrameSize_ / 2,
                static_cast<int>(std::lround(0.006 * sampleRate_)));
        }
    }

    publishedBlend_ = command.blend;
    return command;
}

//==============================================================================
// SpectralVoiceShifter

void ModernPitchEngine::SpectralVoiceShifter::prepare(double sampleRate,
                                                        int frameSize)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    frameSize_ = std::max(64, nextPowerOfTwo(frameSize));
    hopSize_ = std::max(1, frameSize_ / 4);

    const int inputRingSize = nextPowerOfTwo(frameSize_ * 4);
    inputRing_.assign(static_cast<std::size_t>(inputRingSize), 0.0f);
    inputRingMask_ = inputRingSize - 1;

    const int outputRingSize = nextPowerOfTwo(frameSize_ * 8);
    outputRingMask_ = outputRingSize - 1;

    window_.resize(static_cast<std::size_t>(frameSize_));
    for (int index = 0; index < frameSize_; ++index)
    {
        const double periodicHann = 0.5 - 0.5 * std::cos(
            twoPi * static_cast<double>(index)
            / static_cast<double>(frameSize_));
        window_[static_cast<std::size_t>(index)] = static_cast<float>(
            std::sqrt(std::max(0.0, periodicHann)));
    }

    // Precompute the FFT permutation and roots. The old implementation rebuilt
    // both for every forward and inverse transform.
    fftBitReversal_.resize(static_cast<std::size_t>(frameSize_));
    int fftBits = 0;
    while ((1 << fftBits) < frameSize_)
        ++fftBits;
    for (int index = 0; index < frameSize_; ++index)
    {
        unsigned value = static_cast<unsigned>(index);
        unsigned reversed = 0;
        for (int bit = 0; bit < fftBits; ++bit)
        {
            reversed = (reversed << 1u) | (value & 1u);
            value >>= 1u;
        }
        fftBitReversal_[static_cast<std::size_t>(index)] =
            static_cast<int>(reversed);
    }

    fftTwiddles_.resize(static_cast<std::size_t>(frameSize_ / 2));
    for (int index = 0; index < frameSize_ / 2; ++index)
    {
        const double angle = -twoPi * static_cast<double>(index)
                           / static_cast<double>(frameSize_);
        fftTwiddles_[static_cast<std::size_t>(index)] = Complex(
            static_cast<float>(std::cos(angle)),
            static_cast<float>(std::sin(angle)));
    }

    sineTable_.resize(static_cast<std::size_t>(sineTableSize + 1));
    for (int index = 0; index <= sineTableSize; ++index)
    {
        sineTable_[static_cast<std::size_t>(index)] = static_cast<float>(
            std::sin(twoPi * static_cast<double>(index)
                     / static_cast<double>(sineTableSize)));
    }

    formantGainTable_.resize(static_cast<std::size_t>(
        (formantLevelCount + 1) * (formantRatioTableSize + 1)));
    for (int level = 0; level <= formantLevelCount; ++level)
    {
        const double amount = static_cast<double>(level)
                            / static_cast<double>(formantLevelCount);
        for (int ratioIndex = 0; ratioIndex <= formantRatioTableSize; ++ratioIndex)
        {
            const double ratio = 0.56 + (1.78 - 0.56)
                * static_cast<double>(ratioIndex)
                / static_cast<double>(formantRatioTableSize);
            formantGainTable_[static_cast<std::size_t>(
                level * (formantRatioTableSize + 1) + ratioIndex)] =
                static_cast<float>(std::pow(ratio, amount));
        }
    }

    // sqrt-Hann with 75% overlap has a constant sum of squared windows.
    double overlapNormalisation = 0.0;
    const int overlapCount = std::max(1, frameSize_ / hopSize_);
    for (int overlap = 0; overlap < overlapCount; ++overlap)
    {
        const int index = (overlap * hopSize_) % frameSize_;
        const double value = window_[static_cast<std::size_t>(index)];
        overlapNormalisation += value * value;
    }
    synthesisGain_ = static_cast<float>(1.0
        / std::max(1.0e-9, overlapNormalisation));
    envelopeUpdateInterval_ = 2;

    const int positiveBinCount = frameSize_ / 2 + 1;
    fftBuffer_.assign(static_cast<std::size_t>(frameSize_), Complex {});
    magnitudes_.assign(static_cast<std::size_t>(positiveBinCount), 0.0f);
    analysisPhases_.assign(static_cast<std::size_t>(positiveBinCount), 0.0f);
    previousMagnitudes_.assign(static_cast<std::size_t>(positiveBinCount), 0.0f);
    previousAnalysisPhases_.assign(static_cast<std::size_t>(positiveBinCount), 0.0f);
    trueSourceBins_.assign(static_cast<std::size_t>(positiveBinCount), 0.0);
    propagatedPhases_.assign(static_cast<std::size_t>(positiveBinCount), 0.0);
    logMagnitudes_.assign(static_cast<std::size_t>(positiveBinCount), 0.0f);
    rawSpectralEnvelope_.assign(static_cast<std::size_t>(positiveBinCount), 1.0f);
    spectralEnvelope_.assign(static_cast<std::size_t>(positiveBinCount), 1.0f);
    rawHarmonicMask_.assign(static_cast<std::size_t>(positiveBinCount), 1.0f);
    harmonicMask_.assign(static_cast<std::size_t>(positiveBinCount), 1.0f);
    harmonicMaskScratch_.assign(static_cast<std::size_t>(positiveBinCount), 1.0f);
    prefixSum_.assign(static_cast<std::size_t>(positiveBinCount + 1), 0.0);
    nearestPeak_.assign(static_cast<std::size_t>(positiveBinCount), 0);
    peakBins_.clear();
    peakBins_.reserve(static_cast<std::size_t>(positiveBinCount));

    for (auto& layer : layers_)
    {
        layer.spectrum.assign(static_cast<std::size_t>(frameSize_), Complex {});
        layer.synthesisPhases.assign(static_cast<std::size_t>(positiveBinCount), 0.0);
        layer.outputAccumulationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.phaseInitialised = false;
    }

    const double wetAttackMs = frameSize_ <= 128 ? 10.0
                             : frameSize_ <= 256 ? 8.0
                                                 : 6.0;
    const double wetReleaseMs = frameSize_ <= 128 ? 55.0
                              : frameSize_ <= 256 ? 45.0
                                                  : 35.0;

    wetAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (wetAttackMs * 0.001 * sampleRate_)));
    wetReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (wetReleaseMs * 0.001 * sampleRate_)));

    const double envelopeUpdateSeconds = static_cast<double>(
        hopSize_ * envelopeUpdateInterval_) / sampleRate_;
    envelopeAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-envelopeUpdateSeconds / 0.008));
    envelopeReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-envelopeUpdateSeconds / 0.035));

    formantReductionCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.004 * sampleRate_)));
    formantRecoveryCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.028 * sampleRate_)));

    transientReleaseCoefficient_ = static_cast<float>(
        std::exp(-1.0 / (0.012 * sampleRate_)));

    const double frameSeconds = static_cast<double>(hopSize_) / sampleRate_;
    breathAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.018));
    breathReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.110));
    maskAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.006));
    maskReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.024));
    metricAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.012));
    metricReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.070));

    // Wind Fix V5: the residual attenuation is frame-rate controlled, so it
    // adds no per-sample transcendental work.  Reduction enters smoothly,
    // releases slowly to avoid pumping, but restores rapidly on transients.
    noiseReductionAttackCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.028));
    noiseReductionReleaseCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.180));
    transientNoiseRestoreCoefficient_ = static_cast<float>(
        1.0 - std::exp(-frameSeconds / 0.006));
    dryBreathLowPassCoefficient_ = static_cast<float>(
        1.0 - std::exp(-twoPi * 2800.0 / sampleRate_));
    reset();
}

void ModernPitchEngine::SpectralVoiceShifter::clearLayerOutput(
    SynthesisLayer& layer) noexcept
{
    std::fill(layer.outputAccumulationRing.begin(),
              layer.outputAccumulationRing.end(),
              0.0f);
}

void ModernPitchEngine::SpectralVoiceShifter::reset() noexcept
{
    std::fill(inputRing_.begin(), inputRing_.end(), 0.0f);
    std::fill(fftBuffer_.begin(), fftBuffer_.end(), Complex {});
    std::fill(magnitudes_.begin(), magnitudes_.end(), 0.0f);
    std::fill(analysisPhases_.begin(), analysisPhases_.end(), 0.0f);
    std::fill(previousMagnitudes_.begin(), previousMagnitudes_.end(), 0.0f);
    std::fill(previousAnalysisPhases_.begin(), previousAnalysisPhases_.end(), 0.0f);
    std::fill(trueSourceBins_.begin(), trueSourceBins_.end(), 0.0);
    std::fill(propagatedPhases_.begin(), propagatedPhases_.end(), 0.0);
    std::fill(logMagnitudes_.begin(), logMagnitudes_.end(), 0.0f);
    std::fill(rawSpectralEnvelope_.begin(), rawSpectralEnvelope_.end(), 1.0f);
    std::fill(spectralEnvelope_.begin(), spectralEnvelope_.end(), 1.0f);
    std::fill(rawHarmonicMask_.begin(), rawHarmonicMask_.end(), 1.0f);
    std::fill(harmonicMask_.begin(), harmonicMask_.end(), 1.0f);
    std::fill(harmonicMaskScratch_.begin(), harmonicMaskScratch_.end(), 1.0f);
    std::fill(prefixSum_.begin(), prefixSum_.end(), 0.0);
    std::fill(nearestPeak_.begin(), nearestPeak_.end(), 0);
    peakBins_.clear();

    for (auto& layer : layers_)
    {
        std::fill(layer.spectrum.begin(), layer.spectrum.end(), Complex {});
        std::fill(layer.synthesisPhases.begin(),
                  layer.synthesisPhases.end(),
                  0.0);
        clearLayerOutput(layer);
        layer.phaseInitialised = false;
    }

    inputSampleCounter_ = 0;
    analysisPhaseInitialised_ = false;
    phaseResetPending_ = false;
    envelopeInitialised_ = false;
    wetGateOpen_ = false;
    dualTransitionActive_ = false;
    secondaryStartPending_ = false;
    activeLayerIndex_ = 0;
    secondaryLayerIndex_ = 1;
    wetMix_ = 0.0f;
    smoothedFormantPreservation_ = 0.0f;
    transientSuppression_ = 0.0f;
    smoothedBreathiness_ = 0.0f;
    smoothedHarmonicity_ = 1.0f;
    smoothedNoisePathAmount_ = 0.0f;
    smoothedNoiseGain_ = 1.0f;
    currentNoiseReductionDb_ = 0.0f;
    dryBreathLowPass_ = 0.0f;
    breathProtection_ = 0.0f;
    breathPersistenceFrames_ = 0;
    envelopeFrameCounter_ = 0;
}

void ModernPitchEngine::SpectralVoiceShifter::beginSecondaryTransition() noexcept
{
    secondaryLayerIndex_ = 1 - activeLayerIndex_;
    auto& primary = layers_[static_cast<std::size_t>(activeLayerIndex_)];
    auto& secondary = layers_[static_cast<std::size_t>(secondaryLayerIndex_)];

    clearLayerOutput(secondary);
    std::copy(primary.synthesisPhases.begin(),
              primary.synthesisPhases.end(),
              secondary.synthesisPhases.begin());
    secondary.phaseInitialised = primary.phaseInitialised;

    dualTransitionActive_ = true;
    secondaryStartPending_ = true;
}

double ModernPitchEngine::SpectralVoiceShifter::wrapPhase(double phase) noexcept
{
    while (phase > pi)
        phase -= twoPi;
    while (phase < -pi)
        phase += twoPi;
    return phase;
}

float ModernPitchEngine::SpectralVoiceShifter::readInputSample(
    std::int64_t absoluteSample) const noexcept
{
    if (absoluteSample < 0 || inputRing_.empty())
        return 0.0f;

    const int index = static_cast<int>(absoluteSample & inputRingMask_);
    return inputRing_[static_cast<std::size_t>(index)];
}

void ModernPitchEngine::SpectralVoiceShifter::fft(std::vector<Complex>& data,
                                                   bool inverse) noexcept
{
    const int size = static_cast<int>(data.size());
    if (size != frameSize_ || fftBitReversal_.size() != data.size())
        return;

    for (int index = 0; index < size; ++index)
    {
        const int reversed = fftBitReversal_[static_cast<std::size_t>(index)];
        if (index < reversed)
            std::swap(data[static_cast<std::size_t>(index)],
                      data[static_cast<std::size_t>(reversed)]);
    }

    for (int length = 2; length <= size; length <<= 1)
    {
        const int halfLength = length / 2;
        const int twiddleStride = size / length;

        for (int startIndex = 0; startIndex < size; startIndex += length)
        {
            for (int offset = 0; offset < halfLength; ++offset)
            {
                Complex twiddle = fftTwiddles_[static_cast<std::size_t>(
                    offset * twiddleStride)];
                if (inverse)
                    twiddle = std::conj(twiddle);

                const Complex even = data[static_cast<std::size_t>(startIndex + offset)];
                const Complex odd = data[static_cast<std::size_t>(
                    startIndex + offset + halfLength)] * twiddle;
                data[static_cast<std::size_t>(startIndex + offset)] = even + odd;
                data[static_cast<std::size_t>(startIndex + offset + halfLength)] = even - odd;
            }
        }
    }

    if (inverse)
    {
        const float scale = 1.0f / static_cast<float>(size);
        for (Complex& value : data)
            value *= scale;
    }
}

void ModernPitchEngine::SpectralVoiceShifter::fastSinCos(
    double phase,
    float& sine,
    float& cosine) const noexcept
{
    // synthesis phases are kept close to [-pi, pi], so a single addition is
    // sufficient before table lookup.
    double wrapped = phase;
    if (wrapped < 0.0)
        wrapped += twoPi;
    else if (wrapped >= twoPi)
        wrapped -= twoPi;

    const double tablePosition = wrapped
        * (static_cast<double>(sineTableSize) / twoPi);
    const int baseIndex = static_cast<int>(tablePosition) & (sineTableSize - 1);
    const float fraction = static_cast<float>(
        tablePosition - static_cast<double>(static_cast<int>(tablePosition)));

    const int nextIndex = baseIndex + 1;
    const float sin0 = sineTable_[static_cast<std::size_t>(baseIndex)];
    const float sin1 = sineTable_[static_cast<std::size_t>(nextIndex)];
    sine = sin0 + fraction * (sin1 - sin0);

    const double cosinePosition = tablePosition
        + static_cast<double>(sineTableSize / 4);
    const int cosineInteger = static_cast<int>(cosinePosition);
    const int cosineIndex = cosineInteger & (sineTableSize - 1);
    const float cosineFraction = static_cast<float>(
        cosinePosition - static_cast<double>(cosineInteger));
    const float cos0 = sineTable_[static_cast<std::size_t>(cosineIndex)];
    const float cos1 = sineTable_[static_cast<std::size_t>(cosineIndex + 1)];
    cosine = cos0 + cosineFraction * (cos1 - cos0);
}

float ModernPitchEngine::SpectralVoiceShifter::lookupFormantGain(
    float envelopeRatio,
    float formantAmount) const noexcept
{
    const float ratio = std::clamp(envelopeRatio, 0.56f, 1.78f);
    const float amount = clamp01(formantAmount);

    if (amount <= 1.0e-5f)
        return 1.0f;
    if (amount >= 0.99999f)
        return ratio;

    const float ratioPosition = (ratio - 0.56f)
        * static_cast<float>(formantRatioTableSize) / (1.78f - 0.56f);
    const int ratioIndex = std::clamp(static_cast<int>(ratioPosition),
                                      0,
                                      formantRatioTableSize - 1);
    const float ratioFraction = ratioPosition - static_cast<float>(ratioIndex);

    const float levelPosition = amount * static_cast<float>(formantLevelCount);
    const int levelIndex = std::clamp(static_cast<int>(levelPosition),
                                      0,
                                      formantLevelCount - 1);
    const float levelFraction = levelPosition - static_cast<float>(levelIndex);
    const int rowSize = formantRatioTableSize + 1;

    const auto sampleRow = [&](int level) noexcept
    {
        const std::size_t offset = static_cast<std::size_t>(level * rowSize + ratioIndex);
        const float a = formantGainTable_[offset];
        const float b = formantGainTable_[offset + 1];
        return a + ratioFraction * (b - a);
    };

    const float lower = sampleRow(levelIndex);
    const float upper = sampleRow(levelIndex + 1);
    return lower + levelFraction * (upper - lower);
}

void ModernPitchEngine::SpectralVoiceShifter::calculateEnvelope(
    int positiveBins) noexcept
{
    const double binWidthHz = sampleRate_ / static_cast<double>(frameSize_);
    const int smoothingRadius = std::clamp(
        static_cast<int>(std::lround(420.0 / std::max(1.0, binWidthHz))),
        2,
        std::max(2, positiveBins / 12));

    prefixSum_[0] = 0.0;
    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        logMagnitudes_[static_cast<std::size_t>(bin)] = static_cast<float>(
            std::log(std::max(1.0e-9f,
                              magnitudes_[static_cast<std::size_t>(bin)])));
        prefixSum_[static_cast<std::size_t>(bin + 1)] =
            prefixSum_[static_cast<std::size_t>(bin)]
            + static_cast<double>(logMagnitudes_[static_cast<std::size_t>(bin)]);
    }

    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        const int first = std::max(0, bin - smoothingRadius);
        const int last = std::min(positiveBins, bin + smoothingRadius);
        const double sum = prefixSum_[static_cast<std::size_t>(last + 1)]
                         - prefixSum_[static_cast<std::size_t>(first)];
        const double average = sum / static_cast<double>(last - first + 1);
        rawSpectralEnvelope_[static_cast<std::size_t>(bin)] =
            static_cast<float>(std::exp(average));
    }

    if (!envelopeInitialised_)
    {
        for (int bin = 0; bin <= positiveBins; ++bin)
            spectralEnvelope_[static_cast<std::size_t>(bin)] =
                rawSpectralEnvelope_[static_cast<std::size_t>(bin)];
        envelopeInitialised_ = true;
        return;
    }

    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        const std::size_t index = static_cast<std::size_t>(bin);
        const float target = rawSpectralEnvelope_[index];
        const float coefficient = target > spectralEnvelope_[index]
            ? envelopeAttackCoefficient_
            : envelopeReleaseCoefficient_;
        spectralEnvelope_[index] += coefficient
            * (target - spectralEnvelope_[index]);
    }
}

void ModernPitchEngine::SpectralVoiceShifter::calculatePeakRegions(
    int positiveBins) noexcept
{
    peakBins_.clear();

    float maximumMagnitude = 0.0f;
    int maximumBin = 0;
    for (int bin = 1; bin < positiveBins; ++bin)
    {
        const float magnitude = magnitudes_[static_cast<std::size_t>(bin)];
        if (magnitude > maximumMagnitude)
        {
            maximumMagnitude = magnitude;
            maximumBin = bin;
        }
    }

    const float threshold = maximumMagnitude * 0.012f;
    for (int bin = 1; bin < positiveBins; ++bin)
    {
        const float centre = magnitudes_[static_cast<std::size_t>(bin)];
        if (centre >= threshold
            && centre >= magnitudes_[static_cast<std::size_t>(bin - 1)]
            && centre > magnitudes_[static_cast<std::size_t>(bin + 1)])
        {
            peakBins_.push_back(bin);
        }
    }

    if (peakBins_.empty())
        peakBins_.push_back(maximumBin);

    int peakIndex = 0;
    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        while (peakIndex + 1 < static_cast<int>(peakBins_.size()))
        {
            const int currentPeak = peakBins_[static_cast<std::size_t>(peakIndex)];
            const int nextPeak = peakBins_[static_cast<std::size_t>(peakIndex + 1)];
            if (bin <= (currentPeak + nextPeak) / 2)
                break;
            ++peakIndex;
        }

        nearestPeak_[static_cast<std::size_t>(bin)] =
            peakBins_[static_cast<std::size_t>(peakIndex)];
    }
}

float ModernPitchEngine::SpectralVoiceShifter::interpolateEnvelope(
    double binPosition) const noexcept
{
    if (spectralEnvelope_.empty())
        return 1.0f;

    const int maximumBin = static_cast<int>(spectralEnvelope_.size()) - 1;
    const double clamped = std::clamp(binPosition,
                                      0.0,
                                      static_cast<double>(maximumBin));
    const int lower = static_cast<int>(std::floor(clamped));
    const int upper = std::min(maximumBin, lower + 1);
    const float fraction = static_cast<float>(
        clamped - static_cast<double>(lower));
    return spectralEnvelope_[static_cast<std::size_t>(lower)]
         + fraction * (spectralEnvelope_[static_cast<std::size_t>(upper)]
                       - spectralEnvelope_[static_cast<std::size_t>(lower)]);
}

float ModernPitchEngine::SpectralVoiceShifter::binFrequency(int bin) const noexcept
{
    if (frameSize_ <= 0)
        return 0.0f;

    return static_cast<float>(sampleRate_
        * static_cast<double>(std::max(0, bin))
        / static_cast<double>(frameSize_));
}

float ModernPitchEngine::SpectralVoiceShifter::calculateHighBandFlatness(
    int firstBin,
    int lastBin) const noexcept
{
    if (magnitudes_.empty())
        return 0.0f;

    const int maximumBin = static_cast<int>(magnitudes_.size()) - 1;
    firstBin = std::clamp(firstBin, 0, maximumBin);
    lastBin = std::clamp(lastBin, firstBin, maximumBin);

    double logSum = 0.0;
    double linearSum = 0.0;
    int count = 0;

    for (int bin = firstBin; bin <= lastBin; ++bin)
    {
        const double magnitude = std::max(
            1.0e-12,
            static_cast<double>(magnitudes_[static_cast<std::size_t>(bin)]));
        logSum += std::log(magnitude);
        linearSum += magnitude;
        ++count;
    }

    if (count <= 0 || linearSum <= 1.0e-12)
        return 0.0f;

    const double geometricMean = std::exp(logSum / static_cast<double>(count));
    const double arithmeticMean = linearSum / static_cast<double>(count);
    return clamp01(static_cast<float>(
        geometricMean / std::max(1.0e-12, arithmeticMean)));
}

void ModernPitchEngine::SpectralVoiceShifter::updateHarmonicNoiseAnalysis(
    int positiveBins,
    float spectralFlux,
    const HarmonicNoiseContext& context) noexcept
{
    if (positiveBins <= 3 || magnitudes_.empty())
    {
        smoothedBreathiness_ += breathReleaseCoefficient_
            * (0.0f - smoothedBreathiness_);
        smoothedHarmonicity_ += metricReleaseCoefficient_
            * (0.0f - smoothedHarmonicity_);
        smoothedNoisePathAmount_ += metricReleaseCoefficient_
            * (1.0f - smoothedNoisePathAmount_);
        breathProtection_ = smoothStep(0.24f, 0.74f, smoothedBreathiness_);
        return;
    }

    const float binWidthHz = static_cast<float>(sampleRate_
        / static_cast<double>(frameSize_));
    const float f0 = context.detectedPitchHz;
    const bool reliableF0 = f0 >= 42.0f
                         && f0 <= static_cast<float>(sampleRate_ * 0.22)
                         && context.confidence >= 0.20f;
    const float periodicEvidence = clamp01(
        0.42f * context.confidence
        + 0.32f * context.voicing
        + 0.26f * context.consensus);

    double totalEnergy = 0.0;
    double highEnergy = 0.0;
    double airEnergy = 0.0;
    double baseHarmonicEnergy = 0.0;

    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        const std::size_t index = static_cast<std::size_t>(bin);
        const float magnitude = magnitudes_[index];
        const double energy = static_cast<double>(magnitude) * magnitude;
        const float frequencyHz = static_cast<float>(bin) * binWidthHz;
        totalEnergy += energy;

        if (frequencyHz >= 2800.0f)
            highEnergy += energy;
        if (frequencyHz >= 5200.0f)
            airEnergy += energy;

        const int previousBin = std::max(0, bin - 2);
        const int nextBin = std::min(positiveBins, bin + 2);
        float neighbourSum = 0.0f;
        int neighbourCount = 0;
        for (int neighbour = previousBin; neighbour <= nextBin; ++neighbour)
        {
            if (neighbour == bin)
                continue;
            neighbourSum += magnitudes_[static_cast<std::size_t>(neighbour)];
            ++neighbourCount;
        }
        const float neighbourMean = neighbourCount > 0
            ? neighbourSum / static_cast<float>(neighbourCount)
            : magnitude;
        const float localCrest = magnitude
            / std::max(1.0e-9f, neighbourMean);
        const float peakEvidence = smoothStep(1.10f, 3.40f, localCrest);

        const int nearestPeak = nearestPeak_[index];
        const float peakDistance = static_cast<float>(std::abs(nearestPeak - bin));
        const float peakSpread = 1.0f - smoothStep(0.65f, 3.25f, peakDistance);
        const float peakMagnitude = magnitudes_[static_cast<std::size_t>(
            std::clamp(nearestPeak, 0, positiveBins))];
        const float peakProminence = smoothStep(
            1.20f,
            5.50f,
            peakMagnitude / std::max(1.0e-9f, neighbourMean));
        const float localPeakEvidence = clamp01(
            0.46f * peakEvidence + 0.54f * peakSpread * peakProminence);

        const float phaseDeviation = static_cast<float>(std::abs(
            trueSourceBins_[index] - static_cast<double>(bin)));
        const float phaseCoherence = 1.0f
            - smoothStep(0.22f, 1.35f, phaseDeviation);

        float combEvidence = 0.0f;
        if (reliableF0 && frequencyHz >= 0.60f * f0)
        {
            const int harmonicNumber = std::max(
                1,
                static_cast<int>(std::lround(frequencyHz / f0)));
            const float expectedFrequency = static_cast<float>(harmonicNumber) * f0;
            const float distanceHz = std::abs(frequencyHz - expectedFrequency);
            const float toleranceHz = std::min(
                0.23f * f0,
                std::max(0.78f * binWidthHz,
                         0.030f * f0 + 0.0045f * frequencyHz));
            combEvidence = 1.0f
                - smoothStep(toleranceHz,
                             std::max(toleranceHz + binWidthHz,
                                      2.55f * toleranceHz),
                             distanceHz);
        }

        const float lowBandPrior = 1.0f
            - smoothStep(2500.0f, 8500.0f, frequencyHz);
        float rawMask = reliableF0
            ? (0.46f * combEvidence
               + 0.25f * localPeakEvidence
               + 0.21f * phaseCoherence
               + 0.08f * periodicEvidence * lowBandPrior)
            : (0.55f * localPeakEvidence
               + 0.35f * phaseCoherence
               + 0.10f * periodicEvidence * lowBandPrior);

        // Coarse Live/Experimental FFTs cannot resolve individual low-order
        // harmonics reliably.  In the body band, trust the independent F0
        // tracker and apply a resolution-dependent voiced floor.  Breath is
        // still separated progressively above the formant/body region.
        if (frequencyHz >= 70.0f && frequencyHz <= 4600.0f)
        {
            const float bodyWeight = 1.0f
                - smoothStep(1900.0f, 4600.0f, frequencyHz);
            const float resolutionBase = frameSize_ <= 128 ? 0.18f
                                       : frameSize_ <= 256 ? 0.14f
                                                           : 0.10f;
            const float resolutionTracking = frameSize_ <= 128 ? 0.88f
                                           : frameSize_ <= 256 ? 0.88f
                                                               : 0.88f;
            const float voicedFloor = bodyWeight
                * (resolutionBase + resolutionTracking * periodicEvidence);
            rawMask = std::max(rawMask, voicedFloor);
        }
        if (bin == 0 || bin == positiveBins)
            rawMask = 0.0f;

        rawHarmonicMask_[index] = clamp01(rawMask);
        baseHarmonicEnergy += energy
            * static_cast<double>(rawHarmonicMask_[index]);
    }

    // Smooth the mask across frequency before temporal hysteresis.  A soft
    // mask avoids musical-noise islands and keeps adjacent sidebands together.
    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        const int left = std::max(0, bin - 1);
        const int right = std::min(positiveBins, bin + 1);
        harmonicMaskScratch_[static_cast<std::size_t>(bin)] = clamp01(
            0.22f * rawHarmonicMask_[static_cast<std::size_t>(left)]
            + 0.56f * rawHarmonicMask_[static_cast<std::size_t>(bin)]
            + 0.22f * rawHarmonicMask_[static_cast<std::size_t>(right)]);
    }

    const float highRatio = totalEnergy > 1.0e-14
        ? static_cast<float>(highEnergy / totalEnergy)
        : 0.0f;
    const float airRatio = totalEnergy > 1.0e-14
        ? static_cast<float>(airEnergy / totalEnergy)
        : 0.0f;
    const float baseHarmonicity = totalEnergy > 1.0e-14
        ? clamp01(static_cast<float>(baseHarmonicEnergy / totalEnergy))
        : 0.0f;

    const int highFirstBin = std::clamp(
        static_cast<int>(std::ceil(2800.0f / std::max(1.0f, binWidthHz))),
        1,
        positiveBins);
    const float highFlatness = calculateHighBandFlatness(highFirstBin,
                                                          positiveBins);
    const float highScore = smoothStep(0.16f, 0.55f, highRatio);
    const float airScore = smoothStep(0.055f, 0.30f, airRatio);
    const float flatScore = smoothStep(0.20f, 0.67f, highFlatness);
    const float noiseScore = smoothStep(0.28f, 0.78f, 1.0f - baseHarmonicity);
    const float weakPeriodicScore = 1.0f - periodicEvidence;
    const float transientPenalty = 0.44f
        * smoothStep(0.20f, 0.72f, spectralFlux)
        + 0.24f * clamp01(context.onsetStrength);

    float rawBreathiness = 0.28f * highScore
                         + 0.18f * airScore
                         + 0.20f * flatScore
                         + 0.24f * noiseScore
                         + 0.16f * weakPeriodicScore
                         - transientPenalty;
    rawBreathiness = clamp01(rawBreathiness);

    if (rawBreathiness > 0.34f && spectralFlux < 0.48f)
        breathPersistenceFrames_ = std::min(24, breathPersistenceFrames_ + 1);
    else
        breathPersistenceFrames_ = std::max(0, breathPersistenceFrames_ - 2);

    const float persistence = smoothStep(
        1.0f,
        8.0f,
        static_cast<float>(breathPersistenceFrames_));
    rawBreathiness *= 0.58f + 0.42f * persistence;

    const float frameLevel = totalEnergy > 0.0
        ? static_cast<float>(std::sqrt(totalEnergy
            / static_cast<double>(std::max(1, positiveBins))))
        : 0.0f;
    rawBreathiness *= smoothStep(0.00008f, 0.00110f, frameLevel);

    const float breathCoefficient = rawBreathiness > smoothedBreathiness_
        ? breathAttackCoefficient_
        : breathReleaseCoefficient_;
    smoothedBreathiness_ += breathCoefficient
        * (rawBreathiness - smoothedBreathiness_);
    breathProtection_ = smoothStep(0.24f, 0.74f, smoothedBreathiness_);

    double finalHarmonicEnergy = 0.0;
    double finalNoiseEnergy = 0.0;
    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        const std::size_t index = static_cast<std::size_t>(bin);
        const float frequencyHz = static_cast<float>(bin) * binWidthHz;
        const float highBandProtection = smoothStep(
            2400.0f,
            7800.0f,
            frequencyHz);
        const float breathMaskScale = 1.0f
            - breathProtection_ * (0.18f + 0.62f * highBandProtection);
        const float targetMask = clamp01(
            harmonicMaskScratch_[index]
            * std::clamp(breathMaskScale, 0.20f, 1.0f));
        const float coefficient = targetMask > harmonicMask_[index]
            ? maskAttackCoefficient_
            : maskReleaseCoefficient_;
        harmonicMask_[index] += coefficient
            * (targetMask - harmonicMask_[index]);
        harmonicMask_[index] = clamp01(harmonicMask_[index]);

        const double energy = static_cast<double>(magnitudes_[index])
                            * magnitudes_[index];
        finalHarmonicEnergy += energy
            * static_cast<double>(harmonicMask_[index]);
        finalNoiseEnergy += energy
            * static_cast<double>(1.0f - harmonicMask_[index]);
    }

    const double classifiedEnergy = finalHarmonicEnergy + finalNoiseEnergy;
    const float harmonicityTarget = classifiedEnergy > 1.0e-14
        ? clamp01(static_cast<float>(finalHarmonicEnergy / classifiedEnergy))
        : 0.0f;
    const float noisePathTarget = classifiedEnergy > 1.0e-14
        ? clamp01(static_cast<float>(finalNoiseEnergy / classifiedEnergy))
        : 1.0f;

    // Wind Fix V5: attenuate only the sustained aperiodic residual.  The
    // harmonic body remains fully corrected; consonant/transient evidence
    // forces a quick restoration toward unity so articulation is preserved.
    const float reductionAmount = clamp01(context.breathReduction);
    const float persistenceGate = smoothStep(2.0f, 9.0f,
        static_cast<float>(breathPersistenceFrames_));
    const float transientEvidence = clamp01(
        0.68f * smoothStep(0.18f, 0.62f, spectralFlux)
        + 0.32f * clamp01(context.onsetStrength));
    const float softNoiseEvidence = smoothStep(0.04f, 0.60f, noisePathTarget);
    const float breathEvidence = std::max(
        breathProtection_ * persistenceGate,
        0.80f * smoothStep(0.18f, 0.72f, smoothedBreathiness_));

    // A gentle baseline acts on every confidently separated residual; the
    // sustained-breath term increases attenuation on long airy tails.  The
    // transient factor prevents the same rule from swallowing consonants.
    const float reductionDrive = clamp01(
        softNoiseEvidence * (0.40f + 0.60f * breathEvidence)
        * (1.0f - transientEvidence));

    const float maximumReductionDb = 12.0f * reductionAmount;
    const float targetReductionDb = maximumReductionDb * reductionDrive;
    float targetNoiseGain = std::pow(10.0f, -targetReductionDb / 20.0f);

    // Never bury a transient or sibilant merely because it follows a breathy
    // tail.  This branch changes only a frame-rate control value.
    const bool transientRestore = transientEvidence > 0.38f;
    if (transientRestore)
        targetNoiseGain = std::max(targetNoiseGain, 0.92f);

    const float noiseGainCoefficient = targetNoiseGain < smoothedNoiseGain_
        ? noiseReductionAttackCoefficient_
        : (transientRestore ? transientNoiseRestoreCoefficient_
                            : noiseReductionReleaseCoefficient_);
    smoothedNoiseGain_ += noiseGainCoefficient
        * (targetNoiseGain - smoothedNoiseGain_);
    smoothedNoiseGain_ = std::clamp(smoothedNoiseGain_, 0.20f, 1.0f);
    currentNoiseReductionDb_ = std::max(0.0f,
        -20.0f * std::log10(std::max(1.0e-6f, smoothedNoiseGain_)));

    const float harmonicCoefficient = harmonicityTarget > smoothedHarmonicity_
        ? metricAttackCoefficient_
        : metricReleaseCoefficient_;
    smoothedHarmonicity_ += harmonicCoefficient
        * (harmonicityTarget - smoothedHarmonicity_);

    const float noiseCoefficient = noisePathTarget > smoothedNoisePathAmount_
        ? metricAttackCoefficient_
        : metricReleaseCoefficient_;
    smoothedNoisePathAmount_ += noiseCoefficient
        * (noisePathTarget - smoothedNoisePathAmount_);
}

void ModernPitchEngine::SpectralVoiceShifter::synthesiseLayer(
    SynthesisLayer& layer,
    std::int64_t frameEndSample,
    double correctionCents,
    float formantPreservation,
    bool resetPhases,
    float phaseAnchor,
    int positiveBins) noexcept
{
    std::fill(layer.spectrum.begin(), layer.spectrum.end(), Complex {});

    const double safeCents = wrapCorrectionToNearestOctave(correctionCents);
    const double safeRatio = std::exp2(safeCents / 1200.0);
    const double expectedPhaseScale = twoPi * static_cast<double>(hopSize_)
                                    / static_cast<double>(frameSize_);

    const bool initialiseLayer = resetPhases || !layer.phaseInitialised;
    for (int sourceBin = 0; sourceBin <= positiveBins; ++sourceBin)
    {
        const double analysisPhase =
            analysisPhases_[static_cast<std::size_t>(sourceBin)];

        if (initialiseLayer)
        {
            layer.synthesisPhases[static_cast<std::size_t>(sourceBin)] =
                analysisPhase;
        }
        else
        {
            double& synthesisPhase =
                layer.synthesisPhases[static_cast<std::size_t>(sourceBin)];
            synthesisPhase += expectedPhaseScale
                * trueSourceBins_[static_cast<std::size_t>(sourceBin)]
                * safeRatio;

            // Keep phases bounded. This improves numerical stability and makes
            // the lookup-table oscillator independent of song duration.
            synthesisPhase -= twoPi * std::nearbyint(synthesisPhase / twoPi);

            if (phaseAnchor > 0.0f)
            {
                const double phaseError = wrapPhase(
                    analysisPhase - synthesisPhase);
                synthesisPhase += static_cast<double>(phaseAnchor) * phaseError;
                synthesisPhase -= twoPi * std::nearbyint(synthesisPhase / twoPi);
            }
        }

        propagatedPhases_[static_cast<std::size_t>(sourceBin)] =
            layer.synthesisPhases[static_cast<std::size_t>(sourceBin)];
    }

    const float safeFormant = clamp01(formantPreservation);
    const float energyScale = static_cast<float>(1.0 / std::sqrt(safeRatio));

    for (int sourceBin = 0; sourceBin <= positiveBins; ++sourceBin)
    {
        const std::size_t sourceIndex = static_cast<std::size_t>(sourceBin);
        const float magnitude = magnitudes_[sourceIndex];
        if (magnitude <= 1.0e-12f)
            continue;

        const float harmonicWeight = clamp01(harmonicMask_[sourceIndex]);
        const float noiseWeight = 1.0f - harmonicWeight;

        // The aperiodic residual stays at its original bin and analysis phase.
        // Both dual-synthesis layers receive exactly the same residual, so a
        // note transition cannot stretch or repitch the singer's breath.
        // V5 applies a smooth, frequency-shaped attenuation only here: low
        // bands retain body while the air band receives the full de-breath
        // gain.  No extra FFT/IFFT or audio delay is introduced.
        if (noiseWeight > 1.0e-5f)
        {
            const float frequencyHz = binFrequency(sourceBin);
            const float bandStrength = 0.16f
                + 0.84f * smoothStep(850.0f, 6200.0f, frequencyHz);
            const float residualGain = 1.0f
                - bandStrength * (1.0f - smoothedNoiseGain_);
            layer.spectrum[sourceIndex] += fftBuffer_[sourceIndex]
                * (noiseWeight * residualGain);
        }

        const float harmonicMagnitude = magnitude * harmonicWeight;
        if (harmonicMagnitude <= 1.0e-12f)
            continue;

        const double targetPosition = static_cast<double>(sourceBin) * safeRatio;
        if (targetPosition > static_cast<double>(positiveBins) + 1.0)
            continue;

        const int peak = nearestPeak_[sourceIndex];
        const double relativeAnalysisPhase = wrapPhase(
            static_cast<double>(analysisPhases_[sourceIndex])
            - static_cast<double>(analysisPhases_[static_cast<std::size_t>(peak)]));
        const double outputPhase = initialiseLayer
            ? static_cast<double>(analysisPhases_[sourceIndex])
            : propagatedPhases_[static_cast<std::size_t>(peak)]
                + relativeAnalysisPhase;

        const float sourceEnvelope = std::max(
            1.0e-8f,
            spectralEnvelope_[sourceIndex]);
        const float targetEnvelope = std::max(
            1.0e-8f,
            interpolateEnvelope(targetPosition));
        const float envelopeRatio = std::clamp(
            targetEnvelope / sourceEnvelope,
            0.56f,
            1.78f);
        const float formantGain = lookupFormantGain(envelopeRatio, safeFormant);
        const float outputMagnitude = harmonicMagnitude
                                    * formantGain
                                    * energyScale;
        float phaseSine = 0.0f;
        float phaseCosine = 1.0f;
        fastSinCos(outputPhase, phaseSine, phaseCosine);
        const Complex polar(outputMagnitude * phaseCosine,
                            outputMagnitude * phaseSine);

        const int targetBin0 = static_cast<int>(std::floor(targetPosition));
        const float fraction = static_cast<float>(
            targetPosition - static_cast<double>(targetBin0));

        if (targetBin0 >= 0 && targetBin0 <= positiveBins)
        {
            layer.spectrum[static_cast<std::size_t>(targetBin0)] +=
                polar * (1.0f - fraction);
        }

        const int targetBin1 = targetBin0 + 1;
        if (targetBin1 >= 0 && targetBin1 <= positiveBins)
            layer.spectrum[static_cast<std::size_t>(targetBin1)] +=
                polar * fraction;
    }

    layer.phaseInitialised = true;
    layer.spectrum[0] = Complex(layer.spectrum[0].real(), 0.0f);
    layer.spectrum[static_cast<std::size_t>(positiveBins)] =
        Complex(layer.spectrum[static_cast<std::size_t>(positiveBins)].real(),
                0.0f);

    for (int bin = 1; bin < positiveBins; ++bin)
    {
        layer.spectrum[static_cast<std::size_t>(frameSize_ - bin)] =
            std::conj(layer.spectrum[static_cast<std::size_t>(bin)]);
    }

    fft(layer.spectrum, true);

    const std::int64_t outputStartSample = frameEndSample + 1;
    for (int index = 0; index < frameSize_; ++index)
    {
        const float synthesisWindow = window_[static_cast<std::size_t>(index)];
        const float output = layer.spectrum[static_cast<std::size_t>(index)].real()
                           * synthesisWindow;
        const int outputIndex = static_cast<int>((outputStartSample + index)
                                                  & outputRingMask_);
        layer.outputAccumulationRing[static_cast<std::size_t>(outputIndex)] +=
            output;
    }
}

void ModernPitchEngine::SpectralVoiceShifter::processFrame(
    std::int64_t frameEndSample,
    const TransitionManager::Command& transition,
    float formantPreservation,
    const HarmonicNoiseContext& harmonicNoiseContext,
    bool forcePhaseReset) noexcept
{
    const std::int64_t frameStartSample = frameEndSample - frameSize_ + 1;

    for (int index = 0; index < frameSize_; ++index)
    {
        const float input = readInputSample(frameStartSample + index);
        fftBuffer_[static_cast<std::size_t>(index)] = Complex(
            input * window_[static_cast<std::size_t>(index)], 0.0f);
    }

    fft(fftBuffer_, false);

    const int positiveBins = frameSize_ / 2;
    double positiveFlux = 0.0;
    double magnitudeSum = 0.0;

    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        const Complex value = fftBuffer_[static_cast<std::size_t>(bin)];
        const float magnitude = std::abs(value);
        magnitudes_[static_cast<std::size_t>(bin)] = magnitude;
        analysisPhases_[static_cast<std::size_t>(bin)] =
            std::atan2(value.imag(), value.real());
        positiveFlux += std::max(
            0.0f,
            magnitude - previousMagnitudes_[static_cast<std::size_t>(bin)]);
        magnitudeSum += magnitude;
    }

    if (!envelopeInitialised_
        || ++envelopeFrameCounter_ >= envelopeUpdateInterval_)
    {
        envelopeFrameCounter_ = 0;
        calculateEnvelope(positiveBins);
    }
    calculatePeakRegions(positiveBins);

    const float spectralFlux = magnitudeSum > 1.0e-12
        ? static_cast<float>(positiveFlux / magnitudeSum)
        : 0.0f;
    if (spectralFlux > 0.20f)
    {
        transientSuppression_ = std::max(
            transientSuppression_,
            clamp01((spectralFlux - 0.20f) / 0.36f));
    }

    const bool resetAnalysis = forcePhaseReset
                            || phaseResetPending_
                            || !analysisPhaseInitialised_;
    phaseResetPending_ = false;

    const float phaseAnchor = resetAnalysis ? 0.0f
        : 0.32f * smoothStep(0.24f, 0.72f, spectralFlux);
    const double expectedPhaseScale = twoPi * static_cast<double>(hopSize_)
                                    / static_cast<double>(frameSize_);
    const double binFromPhaseScale = static_cast<double>(frameSize_)
                                   / (twoPi * static_cast<double>(hopSize_));

    for (int sourceBin = 0; sourceBin <= positiveBins; ++sourceBin)
    {
        const double analysisPhase =
            analysisPhases_[static_cast<std::size_t>(sourceBin)];
        double trueSourceBin = static_cast<double>(sourceBin);

        if (!resetAnalysis)
        {
            const double expectedAdvance = expectedPhaseScale
                                         * static_cast<double>(sourceBin);
            const double phaseDeviation = wrapPhase(
                analysisPhase
                - static_cast<double>(previousAnalysisPhases_[
                    static_cast<std::size_t>(sourceBin)])
                - expectedAdvance);
            trueSourceBin += phaseDeviation * binFromPhaseScale;
        }

        trueSourceBins_[static_cast<std::size_t>(sourceBin)] = trueSourceBin;
    }

    updateHarmonicNoiseAnalysis(positiveBins,
                                spectralFlux,
                                harmonicNoiseContext);

    auto& primary = layers_[static_cast<std::size_t>(activeLayerIndex_)];
    synthesiseLayer(primary,
                    frameEndSample,
                    transition.primaryCents,
                    formantPreservation,
                    resetAnalysis,
                    phaseAnchor,
                    positiveBins);

    if (dualTransitionActive_ && transition.dualSynthesis)
    {
        auto& secondary = layers_[static_cast<std::size_t>(secondaryLayerIndex_)];
        synthesiseLayer(secondary,
                        frameEndSample,
                        transition.secondaryCents,
                        formantPreservation,
                        resetAnalysis,
                        phaseAnchor,
                        positiveBins);
    }

    for (int bin = 0; bin <= positiveBins; ++bin)
    {
        previousMagnitudes_[static_cast<std::size_t>(bin)] =
            magnitudes_[static_cast<std::size_t>(bin)];
        previousAnalysisPhases_[static_cast<std::size_t>(bin)] =
            analysisPhases_[static_cast<std::size_t>(bin)];
    }

    analysisPhaseInitialised_ = true;
    secondaryStartPending_ = false;
}

float ModernPitchEngine::SpectralVoiceShifter::consumeLayerOutput(
    SynthesisLayer& layer,
    std::int64_t sample) noexcept
{
    const int outputIndex = static_cast<int>(sample & outputRingMask_);
    const std::size_t index = static_cast<std::size_t>(outputIndex);
    const float accumulated = layer.outputAccumulationRing[index];
    layer.outputAccumulationRing[index] = 0.0f;
    return accumulated * synthesisGain_;
}

float ModernPitchEngine::SpectralVoiceShifter::blendLayers(
    float primary,
    float secondary,
    float transitionBlend) noexcept
{
    // TransitionManager already publishes a smoothstep trajectory. A direct
    // complementary crossfade is both phase-stable and much cheaper than
    // evaluating sine/cosine for every audio sample.
    const float mix = clamp01(transitionBlend);
    return primary + mix * (secondary - primary);
}

float ModernPitchEngine::SpectralVoiceShifter::processSample(
    float inputSample,
    const TransitionManager::Command& transition,
    float desiredWetMix,
    float formantPreservation,
    const HarmonicNoiseContext& harmonicNoiseContext,
    bool forcePhaseReset) noexcept
{
    if (frameSize_ <= 0 || inputRing_.empty())
        return inputSample;

    const std::int64_t currentSample = inputSampleCounter_;
    const int inputIndex = static_cast<int>(currentSample & inputRingMask_);
    inputRing_[static_cast<std::size_t>(inputIndex)] = inputSample;

    if (forcePhaseReset)
        phaseResetPending_ = true;

    if (transition.beginSecondary
        || (transition.dualSynthesis && !dualTransitionActive_))
    {
        beginSecondaryTransition();
    }

    const float formantTarget = clamp01(formantPreservation);
    const float formantCoefficient = formantTarget < smoothedFormantPreservation_
        ? formantReductionCoefficient_
        : formantRecoveryCoefficient_;
    smoothedFormantPreservation_ += formantCoefficient
        * (formantTarget - smoothedFormantPreservation_);

    if (((currentSample + 1) % hopSize_) == 0)
    {
        processFrame(currentSample,
                     transition,
                     smoothedFormantPreservation_,
                     harmonicNoiseContext,
                     forcePhaseReset);
    }

    auto& primaryLayer = layers_[static_cast<std::size_t>(activeLayerIndex_)];
    const float primaryShifted = consumeLayerOutput(primaryLayer, currentSample);

    float secondaryShifted = 0.0f;
    if (dualTransitionActive_)
    {
        auto& secondaryLayer =
            layers_[static_cast<std::size_t>(secondaryLayerIndex_)];
        secondaryShifted = consumeLayerOutput(secondaryLayer, currentSample);
    }
    else
    {
        // Consume stale samples from the inactive layer so an old transition
        // can never leak into a later one.
        auto& inactiveLayer =
            layers_[static_cast<std::size_t>(1 - activeLayerIndex_)];
        static_cast<void>(consumeLayerOutput(inactiveLayer, currentSample));
    }

    const float shifted = dualTransitionActive_
        ? blendLayers(primaryShifted, secondaryShifted, transition.blend)
        : primaryShifted;

    const float delayedDry = readInputSample(currentSample - frameSize_);

    // The ordinary wet/dry interpolation would otherwise re-introduce the
    // unattenuated breath from delayedDry whenever correction authority is
    // below 100%.  A very cheap analysis-only high-shelf proxy applies the
    // same sustained-breath control to the dry contribution.  The harmonic
    // body below the crossover is untouched, and transient restoration in
    // smoothedNoiseGain_ protects consonants.
    dryBreathLowPass_ += dryBreathLowPassCoefficient_
        * (delayedDry - dryBreathLowPass_);
    const float dryAir = delayedDry - dryBreathLowPass_;
    const float dryBreathGate = smoothStep(0.18f, 0.68f,
                                            smoothedBreathiness_);
    const float dryAirGain = 1.0f
        - dryBreathGate * (1.0f - smoothedNoiseGain_);
    const float breathManagedDry = delayedDry
        - dryAir * (1.0f - dryAirGain);

    float wetTarget = clamp01(desiredWetMix);
    const double audibleCents = dualTransitionActive_
        ? transition.primaryCents
            + static_cast<double>(transition.blend)
                * (transition.secondaryCents - transition.primaryCents)
        : transition.primaryCents;
    const float correctionCents = static_cast<float>(std::abs(audibleCents));
    wetTarget *= smoothStep(0.8f, 5.0f, correctionCents);
    wetTarget *= 1.0f - 0.88f * clamp01(transientSuppression_);
    transientSuppression_ *= transientReleaseCoefficient_;

    if (!wetGateOpen_ && wetTarget >= 0.10f)
        wetGateOpen_ = true;
    else if (wetGateOpen_ && wetTarget <= 0.025f)
        wetGateOpen_ = false;

    if (!wetGateOpen_)
        wetTarget = 0.0f;

    const float wetCoefficient = wetTarget > wetMix_
        ? wetAttackCoefficient_
        : wetReleaseCoefficient_;
    wetMix_ += wetCoefficient * (wetTarget - wetMix_);

    if (std::abs(wetMix_) < 1.0e-7f && wetTarget == 0.0f)
        wetMix_ = 0.0f;

    const float output = breathManagedDry
        + wetMix_ * (shifted - breathManagedDry);

    if (transition.commitSecondary && dualTransitionActive_)
    {
        activeLayerIndex_ = secondaryLayerIndex_;
        secondaryLayerIndex_ = 1 - activeLayerIndex_;
        dualTransitionActive_ = false;
        secondaryStartPending_ = false;
    }

    ++inputSampleCounter_;
    return output;
}

float ModernPitchEngine::SpectralVoiceShifter::processBypassedSample(
    float inputSample) noexcept
{
    if (frameSize_ <= 0 || inputRing_.empty())
        return inputSample;

    const std::int64_t currentSample = inputSampleCounter_;
    const int inputIndex = static_cast<int>(currentSample & inputRingMask_);
    inputRing_[static_cast<std::size_t>(inputIndex)] = inputSample;

    // Clear only the slots that pass the read head. This keeps bypass O(1) per
    // sample and prevents synthesis left over from before bypass from leaking
    // when processing resumes.
    const int outputIndex = static_cast<int>(currentSample & outputRingMask_);
    for (auto& layer : layers_)
        layer.outputAccumulationRing[static_cast<std::size_t>(outputIndex)] = 0.0f;

    const float delayedDry = readInputSample(currentSample - frameSize_);
    ++inputSampleCounter_;

    analysisPhaseInitialised_ = false;
    phaseResetPending_ = true;
    envelopeInitialised_ = false;
    dualTransitionActive_ = false;
    secondaryStartPending_ = false;
    wetGateOpen_ = false;
    wetMix_ = 0.0f;
    transientSuppression_ = 0.0f;
    smoothedBreathiness_ = 0.0f;
    smoothedHarmonicity_ = 1.0f;
    smoothedNoisePathAmount_ = 0.0f;
    smoothedNoiseGain_ = 1.0f;
    currentNoiseReductionDb_ = 0.0f;
    dryBreathLowPass_ = 0.0f;
    breathProtection_ = 0.0f;
    breathPersistenceFrames_ = 0;
    return delayedDry;
}

//==============================================================================
// FixedDelay

void ModernPitchEngine::FixedDelay::prepare(int delaySamples)
{
    delaySamples_ = std::max(0, delaySamples);
    const int requiredSize = std::max(2, delaySamples_ + 2);
    const int bufferSize = nextPowerOfTwo(requiredSize);
    buffer_.assign(static_cast<std::size_t>(bufferSize), 0.0f);
    mask_ = bufferSize - 1;
    reset();
}

void ModernPitchEngine::FixedDelay::reset() noexcept
{
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    sampleCounter_ = 0;
}

float ModernPitchEngine::FixedDelay::process(float inputSample) noexcept
{
    if (buffer_.empty())
        return inputSample;

    const int writeIndex = static_cast<int>(sampleCounter_ & mask_);
    buffer_[static_cast<std::size_t>(writeIndex)] = inputSample;

    float output = 0.0f;
    if (sampleCounter_ >= delaySamples_)
    {
        const int readIndex = static_cast<int>(
            (sampleCounter_ - delaySamples_) & mask_);
        output = buffer_[static_cast<std::size_t>(readIndex)];
    }

    ++sampleCounter_;
    return output;
}

//==============================================================================
// ModernPitchEngine

void ModernPitchEngine::prepare(double sampleRate,
                                int /*maximumExpectedSamplesPerBlock*/,
                                int numberOfChannels,
                                LatencyMode latencyMode)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    preparedChannels_ = std::clamp(numberOfChannels, 1, maxSupportedChannels);
    latencyMode_ = latencyMode;
    latencySamples_ = frameSizeForMode(sampleRate_, latencyMode_);

    pitchTracker_.prepare(sampleRate_);
    // Analysis-only conditioning: reduce high-frequency air noise before YIN
    // without touching the audible path or adding output latency.
    detectorConditioner_.prepare(sampleRate_,
        std::min(4600.0, sampleRate_ * 0.20));
    correctionController_.prepare(sampleRate_);
    transitionManager_.prepare(sampleRate_, latencySamples_, latencyMode_);
    tempoController_.prepare(sampleRate_);

    // Allocate FFT/LUT state only for channels the host actually exposes.
    // The previous code prepared all eight possible channels even for a mono
    // vocal track, multiplying setup time and memory without any runtime value.
    for (int channel = 0; channel < preparedChannels_; ++channel)
    {
        shifters_[static_cast<std::size_t>(channel)].prepare(sampleRate_,
                                                             latencySamples_);
        auxiliaryDelays_[static_cast<std::size_t>(channel)].prepare(latencySamples_);
    }

    reset();
}

void ModernPitchEngine::reset() noexcept
{
    pitchTracker_.reset();
    detectorConditioner_.reset();
    scaleQuantizer_.resetTarget();
    correctionController_.reset();
    transitionManager_.reset();
    tempoController_.reset();

    for (auto& shifter : shifters_)
        shifter.reset();
    for (auto& delay : auxiliaryDelays_)
        delay.reset();

    meterSequence_.store(1u, std::memory_order_release);
    meterPitchHz_.store(0.0f, std::memory_order_relaxed);
    meterTargetHz_.store(0.0f, std::memory_order_relaxed);
    meterConfidence_.store(0.0f, std::memory_order_relaxed);
    meterVoicing_.store(0.0f, std::memory_order_relaxed);
    meterBreathiness_.store(0.0f, std::memory_order_relaxed);
    meterHarmonicity_.store(0.0f, std::memory_order_relaxed);
    meterNoisePath_.store(0.0f, std::memory_order_relaxed);
    meterNoiseReductionDb_.store(0.0f, std::memory_order_relaxed);
    meterConsensus_.store(0.0f, std::memory_order_relaxed);
    meterCorrectionCents_.store(0.0f, std::memory_order_relaxed);
    meterWetMix_.store(0.0f, std::memory_order_relaxed);
    meterTransitionBlend_.store(0.0f, std::memory_order_relaxed);
    meterDualSynthesisActive_.store(false, std::memory_order_relaxed);
    meterDetectorSupport_.store(0, std::memory_order_relaxed);
    meterOctaveState_.store(0, std::memory_order_relaxed);
    meterPendingOctaveObservations_.store(0, std::memory_order_relaxed);
    meterState_.store(static_cast<int>(TrackingState::unvoiced),
                      std::memory_order_relaxed);
    meterTempoBpm_.store(120.0f, std::memory_order_relaxed);
    meterTempoGridPhase_.store(0.0f, std::memory_order_relaxed);
    meterTempoGlideTimeMs_.store(0.0f, std::memory_order_relaxed);
    meterTempoActive_.store(false, std::memory_order_relaxed);
    meterTempoWaiting_.store(false, std::memory_order_relaxed);
    meterTempoHostSync_.store(false, std::memory_order_relaxed);
    meterTempoMode_.store(static_cast<int>(CreativeTempo::Mode::off),
                          std::memory_order_relaxed);
    meterSequence_.store(2u, std::memory_order_release);
}

void ModernPitchEngine::process(juce::AudioBuffer<float>& buffer,
                                const double* scaleRatios,
                                int numberOfScaleRatios,
                                double rootFrequency,
                                const Parameters& parameters)
{
    process(buffer,
            scaleRatios,
            numberOfScaleRatios,
            rootFrequency,
            parameters,
            CreativeTempo::HostPosition {});
}

void ModernPitchEngine::process(juce::AudioBuffer<float>& buffer,
                                const double* scaleRatios,
                                int numberOfScaleRatios,
                                double rootFrequency,
                                const Parameters& parameters,
                                const CreativeTempo::HostPosition& hostTempoPosition)
{
    if (scaleQuantizer_.update(scaleRatios,
                               numberOfScaleRatios,
                               rootFrequency))
    {
        correctionController_.reset();
        transitionManager_.reset();
        tempoController_.reset();
    }

    pitchTracker_.setRange(parameters.minimumPitchHz,
                           parameters.maximumPitchHz);
    pitchTracker_.setSensitivity(parameters.detectorSensitivity);

    const int numberOfSamples = buffer.getNumSamples();
    const int numberOfChannels = std::min({ buffer.getNumChannels(),
                                            preparedChannels_,
                                            maxSupportedChannels });
    if (numberOfSamples <= 0 || numberOfChannels <= 0)
        return;

    tempoController_.beginBlock(hostTempoPosition,
                                parameters.tempo,
                                numberOfSamples);
    Parameters transitionParameters = parameters;
    if (tempoController_.isActive())
        transitionParameters.transitionTimeMs = tempoController_.getGlideTimeMs();

    std::array<float*, maxSupportedChannels> channelData {};
    for (int channel = 0; channel < numberOfChannels; ++channel)
        channelData[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);

    currentStereoMode_ = parameters.stereoMode;
    const bool useMidSide = currentStereoMode_ == StereoMode::linkedMidSide
                         && numberOfChannels >= 2;

    PitchObservation observation;
    bool forcePhaseReset = false;
    float latestPitchHz = meterPitchHz_.load(std::memory_order_relaxed);
    float latestConfidence = meterConfidence_.load(std::memory_order_relaxed);
    float latestVoicing = meterVoicing_.load(std::memory_order_relaxed);
    float latestBreathiness = meterBreathiness_.load(std::memory_order_relaxed);
    float latestHarmonicity = meterHarmonicity_.load(std::memory_order_relaxed);
    float latestNoisePath = meterNoisePath_.load(std::memory_order_relaxed);
    float latestNoiseReductionDb = meterNoiseReductionDb_.load(std::memory_order_relaxed);
    float latestConsensus = meterConsensus_.load(std::memory_order_relaxed);
    float latestOnsetStrength = 0.0f;
    int latestDetectorSupport = meterDetectorSupport_.load(std::memory_order_relaxed);
    int latestOctaveState = meterOctaveState_.load(std::memory_order_relaxed);
    int latestPendingOctave = meterPendingOctaveObservations_.load(std::memory_order_relaxed);

    for (int sampleIndex = 0; sampleIndex < numberOfSamples; ++sampleIndex)
    {
        float detectorInput = 0.0f;
        if (useMidSide)
        {
            detectorInput = 0.5f
                * (channelData[0][sampleIndex] + channelData[1][sampleIndex]);
        }
        else
        {
            for (int channel = 0; channel < numberOfChannels; ++channel)
                detectorInput += channelData[static_cast<std::size_t>(channel)][sampleIndex];
            detectorInput /= static_cast<float>(numberOfChannels);
        }

        detectorInput = detectorConditioner_.process(detectorInput);

        forcePhaseReset = false;
        if (pitchTracker_.processSample(detectorInput, observation))
        {
            float spectralBreath = shifters_[0].getBreathiness();
            float spectralHarmonicity = shifters_[0].getHarmonicity();
            if (!useMidSide && numberOfChannels > 1)
            {
                spectralBreath = 0.0f;
                spectralHarmonicity = 0.0f;
                for (int channel = 0; channel < numberOfChannels; ++channel)
                {
                    spectralBreath += shifters_[static_cast<std::size_t>(channel)]
                        .getBreathiness();
                    spectralHarmonicity += shifters_[static_cast<std::size_t>(channel)]
                        .getHarmonicity();
                }
                const float inverseChannels = 1.0f
                    / static_cast<float>(numberOfChannels);
                spectralBreath *= inverseChannels;
                spectralHarmonicity *= inverseChannels;
            }
            correctionController_.setSpectralReliability(spectralBreath,
                                                         spectralHarmonicity);
            correctionController_.acceptObservation(observation,
                                                    scaleQuantizer_,
                                                    parameters);
            // Do not reset spectral phase on musical onsets. The renderer
            // handles transients with soft phase anchoring and wet suppression.
            forcePhaseReset = false;

            latestPitchHz = observation.frequencyHz;
            latestConfidence = observation.confidence;
            latestVoicing = observation.voicing;
            latestConsensus = observation.consensus;
            latestDetectorSupport = observation.detectorSupport;
            latestOctaveState = observation.octaveState;
            latestPendingOctave = observation.pendingOctaveObservations;
            latestOnsetStrength = observation.onsetStrength;
        }

        correctionController_.advanceOneSample(parameters);
        const float wetMix = correctionController_.getWetMix();
        const auto trackingState = correctionController_.getState();
        const bool musicalState = trackingState != TrackingState::unvoiced
                               && trackingState != TrackingState::release;
        const auto tempoDecision = tempoController_.processSample(
            correctionController_.getCurrentCorrectionCents(),
            correctionController_.getDesiredCorrectionCents(),
            correctionController_.getTargetRevision(),
            latestOnsetStrength,
            musicalState,
            sampleIndex,
            parameters.tempo,
            parameters.transitionTimeMs);
        const auto transition = transitionManager_.processSample(
            tempoDecision.controllerCents,
            tempoDecision.destinationCents,
            tempoDecision.targetRevision,
            trackingState,
            wetMix,
            transitionParameters,
            tempoDecision.forceTransition);
        const float formant = clamp01(parameters.formantPreservation
            * correctionController_.getFormantStability());
        const HarmonicNoiseContext harmonicNoiseContext {
            latestPitchHz,
            latestConfidence,
            latestVoicing,
            latestConsensus,
            latestOnsetStrength,
            clamp01(parameters.breathReduction)
        };

        if (useMidSide)
        {
            const float left = channelData[0][sampleIndex];
            const float right = channelData[1][sampleIndex];
            const float mid = 0.5f * (left + right);
            const float side = 0.5f * (left - right);

            const float processedMid = shifters_[0].processSample(mid,
                                                                  transition,
                                                                  wetMix,
                                                                  formant,
                                                                  harmonicNoiseContext,
                                                                  forcePhaseReset);
            const float delayedSide = auxiliaryDelays_[0].process(side);
            channelData[0][sampleIndex] = processedMid + delayedSide;
            channelData[1][sampleIndex] = processedMid - delayedSide;

            for (int channel = 2; channel < numberOfChannels; ++channel)
            {
                float& sample = channelData[static_cast<std::size_t>(channel)][sampleIndex];
                sample = shifters_[static_cast<std::size_t>(channel)].processSample(
                    sample, transition, wetMix, formant,
                    harmonicNoiseContext, forcePhaseReset);
            }
        }
        else
        {
            for (int channel = 0; channel < numberOfChannels; ++channel)
            {
                float& sample = channelData[static_cast<std::size_t>(channel)][sampleIndex];
                sample = shifters_[static_cast<std::size_t>(channel)].processSample(
                    sample, transition, wetMix, formant,
                    harmonicNoiseContext, forcePhaseReset);
            }
        }
    }

    latestBreathiness = 0.0f;
    latestHarmonicity = 0.0f;
    latestNoisePath = 0.0f;
    latestNoiseReductionDb = 0.0f;
    const int meteredShifters = useMidSide ? 1 : numberOfChannels;
    for (int channel = 0; channel < meteredShifters; ++channel)
    {
        const auto& shifter = shifters_[static_cast<std::size_t>(channel)];
        latestBreathiness += shifter.getBreathiness();
        latestHarmonicity += shifter.getHarmonicity();
        latestNoisePath += shifter.getNoisePathAmount();
        latestNoiseReductionDb += shifter.getNoiseReductionDb();
    }
    const float inverseMeteredShifters = 1.0f
        / static_cast<float>(std::max(1, meteredShifters));
    latestBreathiness *= inverseMeteredShifters;
    latestHarmonicity *= inverseMeteredShifters;
    latestNoisePath *= inverseMeteredShifters;
    latestNoiseReductionDb *= inverseMeteredShifters;
    const auto tempoMeter = tempoController_.getMetering();

    meterSequence_.fetch_add(1u, std::memory_order_acq_rel); // odd: publishing
    meterPitchHz_.store(latestPitchHz, std::memory_order_relaxed);
    meterTargetHz_.store(correctionController_.getTargetPitchHz(),
                         std::memory_order_relaxed);
    meterConfidence_.store(latestConfidence, std::memory_order_relaxed);
    meterVoicing_.store(latestVoicing, std::memory_order_relaxed);
    meterBreathiness_.store(latestBreathiness, std::memory_order_relaxed);
    meterHarmonicity_.store(latestHarmonicity, std::memory_order_relaxed);
    meterNoisePath_.store(latestNoisePath, std::memory_order_relaxed);
    meterNoiseReductionDb_.store(latestNoiseReductionDb, std::memory_order_relaxed);
    meterConsensus_.store(latestConsensus, std::memory_order_relaxed);
    meterCorrectionCents_.store(correctionController_.getCorrectionCents(),
                                std::memory_order_relaxed);
    meterWetMix_.store(correctionController_.getWetMix(),
                       std::memory_order_relaxed);
    meterTransitionBlend_.store(transitionManager_.getBlend(),
                                std::memory_order_relaxed);
    meterDualSynthesisActive_.store(
        transitionManager_.isDualSynthesisActive(),
        std::memory_order_relaxed);
    meterDetectorSupport_.store(latestDetectorSupport,
                                std::memory_order_relaxed);
    meterOctaveState_.store(latestOctaveState,
                            std::memory_order_relaxed);
    meterPendingOctaveObservations_.store(latestPendingOctave,
                                          std::memory_order_relaxed);
    meterState_.store(static_cast<int>(correctionController_.getState()),
                      std::memory_order_relaxed);
    meterTempoBpm_.store(tempoMeter.bpm, std::memory_order_relaxed);
    meterTempoGridPhase_.store(tempoMeter.gridPhase, std::memory_order_relaxed);
    meterTempoGlideTimeMs_.store(tempoMeter.glideTimeMs, std::memory_order_relaxed);
    meterTempoActive_.store(tempoMeter.active, std::memory_order_relaxed);
    meterTempoWaiting_.store(tempoMeter.waitingForGrid, std::memory_order_relaxed);
    meterTempoHostSync_.store(tempoMeter.hostSyncValid, std::memory_order_relaxed);
    meterTempoMode_.store(static_cast<int>(tempoMeter.mode),
                          std::memory_order_relaxed);
    meterSequence_.fetch_add(1u, std::memory_order_release); // even: complete
}

void ModernPitchEngine::process(juce::AudioBuffer<float>& buffer,
                                const std::vector<double>& scaleRatios,
                                double rootFrequency,
                                const Parameters& parameters)
{
    process(buffer,
            scaleRatios.empty() ? nullptr : scaleRatios.data(),
            static_cast<int>(scaleRatios.size()),
            rootFrequency,
            parameters);
}

void ModernPitchEngine::process(float* monoData,
                                int numberOfSamples,
                                const std::vector<double>& scaleRatios,
                                double rootFrequency,
                                const Parameters& parameters)
{
    if (monoData == nullptr || numberOfSamples <= 0)
        return;

    float* channels[] { monoData };
    juce::AudioBuffer<float> view(channels, 1, numberOfSamples);
    process(view, scaleRatios, rootFrequency, parameters);
}

void ModernPitchEngine::processBypassed(juce::AudioBuffer<float>& buffer)
{
    const int numberOfSamples = buffer.getNumSamples();
    const int numberOfChannels = std::min({ buffer.getNumChannels(),
                                            preparedChannels_,
                                            maxSupportedChannels });
    if (numberOfSamples <= 0 || numberOfChannels <= 0)
        return;

    std::array<float*, maxSupportedChannels> channelData {};
    for (int channel = 0; channel < numberOfChannels; ++channel)
        channelData[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);

    correctionController_.reset();
    transitionManager_.reset();
    tempoController_.reset();

    const bool useMidSide = currentStereoMode_ == StereoMode::linkedMidSide
                         && numberOfChannels >= 2;

    for (int sampleIndex = 0; sampleIndex < numberOfSamples; ++sampleIndex)
    {
        if (useMidSide)
        {
            const float left = channelData[0][sampleIndex];
            const float right = channelData[1][sampleIndex];
            const float mid = 0.5f * (left + right);
            const float side = 0.5f * (left - right);
            const float delayedMid = shifters_[0].processBypassedSample(mid);
            const float delayedSide = auxiliaryDelays_[0].process(side);
            channelData[0][sampleIndex] = delayedMid + delayedSide;
            channelData[1][sampleIndex] = delayedMid - delayedSide;

            for (int channel = 2; channel < numberOfChannels; ++channel)
            {
                float& sample = channelData[static_cast<std::size_t>(channel)][sampleIndex];
                sample = shifters_[static_cast<std::size_t>(channel)]
                    .processBypassedSample(sample);
            }
        }
        else
        {
            for (int channel = 0; channel < numberOfChannels; ++channel)
            {
                float& sample = channelData[static_cast<std::size_t>(channel)][sampleIndex];
                sample = shifters_[static_cast<std::size_t>(channel)]
                    .processBypassedSample(sample);
            }
        }
    }

    meterSequence_.fetch_add(1u, std::memory_order_acq_rel);
    meterPitchHz_.store(0.0f, std::memory_order_relaxed);
    meterTargetHz_.store(0.0f, std::memory_order_relaxed);
    meterConfidence_.store(0.0f, std::memory_order_relaxed);
    meterVoicing_.store(0.0f, std::memory_order_relaxed);
    meterBreathiness_.store(0.0f, std::memory_order_relaxed);
    meterHarmonicity_.store(0.0f, std::memory_order_relaxed);
    meterNoisePath_.store(0.0f, std::memory_order_relaxed);
    meterNoiseReductionDb_.store(0.0f, std::memory_order_relaxed);
    meterConsensus_.store(0.0f, std::memory_order_relaxed);
    meterCorrectionCents_.store(0.0f, std::memory_order_relaxed);
    meterWetMix_.store(0.0f, std::memory_order_relaxed);
    meterTransitionBlend_.store(0.0f, std::memory_order_relaxed);
    meterDualSynthesisActive_.store(false, std::memory_order_relaxed);
    meterDetectorSupport_.store(0, std::memory_order_relaxed);
    meterOctaveState_.store(0, std::memory_order_relaxed);
    meterPendingOctaveObservations_.store(0, std::memory_order_relaxed);
    meterState_.store(static_cast<int>(TrackingState::unvoiced),
                      std::memory_order_relaxed);
    meterTempoBpm_.store(120.0f, std::memory_order_relaxed);
    meterTempoGridPhase_.store(0.0f, std::memory_order_relaxed);
    meterTempoGlideTimeMs_.store(0.0f, std::memory_order_relaxed);
    meterTempoActive_.store(false, std::memory_order_relaxed);
    meterTempoWaiting_.store(false, std::memory_order_relaxed);
    meterTempoHostSync_.store(false, std::memory_order_relaxed);
    meterTempoMode_.store(static_cast<int>(CreativeTempo::Mode::off),
                          std::memory_order_relaxed);
    meterSequence_.fetch_add(1u, std::memory_order_release);
}

ModernPitchEngine::Metering ModernPitchEngine::getMetering() const noexcept
{
    Metering result;

    // A bounded seqlock read gives the GUI a coherent snapshot without ever
    // blocking the audio thread.  In the extremely unlikely case of repeated
    // contention, the last read is still safe because every field is atomic.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const std::uint32_t before = meterSequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
            continue;

        result.detectedPitchHz = meterPitchHz_.load(std::memory_order_relaxed);
        result.targetPitchHz = meterTargetHz_.load(std::memory_order_relaxed);
        result.confidence = meterConfidence_.load(std::memory_order_relaxed);
        result.voicing = meterVoicing_.load(std::memory_order_relaxed);
        result.breathiness = meterBreathiness_.load(std::memory_order_relaxed);
        result.harmonicity = meterHarmonicity_.load(std::memory_order_relaxed);
        result.noisePath = meterNoisePath_.load(std::memory_order_relaxed);
        result.noiseReductionDb = meterNoiseReductionDb_.load(std::memory_order_relaxed);
        result.consensus = meterConsensus_.load(std::memory_order_relaxed);
        result.correctionCents = meterCorrectionCents_.load(std::memory_order_relaxed);
        result.wetMix = meterWetMix_.load(std::memory_order_relaxed);
        result.transitionBlend = meterTransitionBlend_.load(
            std::memory_order_relaxed);
        result.dualSynthesisActive = meterDualSynthesisActive_.load(
            std::memory_order_relaxed);
        result.detectorSupport = meterDetectorSupport_.load(std::memory_order_relaxed);
        result.octaveState = meterOctaveState_.load(std::memory_order_relaxed);
        result.pendingOctaveObservations = meterPendingOctaveObservations_.load(
            std::memory_order_relaxed);
        result.state = static_cast<TrackingState>(
            meterState_.load(std::memory_order_relaxed));
        result.tempoBpm = meterTempoBpm_.load(std::memory_order_relaxed);
        result.tempoGridPhase = meterTempoGridPhase_.load(std::memory_order_relaxed);
        result.tempoGlideTimeMs = meterTempoGlideTimeMs_.load(std::memory_order_relaxed);
        result.tempoActive = meterTempoActive_.load(std::memory_order_relaxed);
        result.tempoWaitingForGrid = meterTempoWaiting_.load(std::memory_order_relaxed);
        result.tempoHostSyncValid = meterTempoHostSync_.load(std::memory_order_relaxed);
        result.tempoMode = static_cast<CreativeTempo::Mode>(
            meterTempoMode_.load(std::memory_order_relaxed));

        const std::uint32_t after = meterSequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0u)
            return result;
    }

    return result;
}
