#pragma once

#include <JuceHeader.h>
#include "Tempo.h"

#include <array>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

class ModernPitchEngine final
{
public:
    static constexpr int maxSupportedChannels = 8;
    static constexpr int maxScaleRatios = 96;

    enum class LatencyMode : int
    {
        ultraLive = 0, // approximately 2.7-2.9 ms at 44.1/48 kHz
        live = 1,      // approximately 5.3-5.8 ms at 44.1/48 kHz
        quality = 2    // approximately 10.7-11.6 ms at 44.1/48 kHz
    };

    enum class StereoMode : int
    {
        linkedMidSide = 0,
        dualMono = 1
    };

    enum class TrackingState : int
    {
        unvoiced = 0,
        attack,
        acquire,
        stable,
        transition,
        release
    };

    struct Parameters
    {
        float amount = 1.0f;                 // 0..1
        float retuneTimeMs = 8.0f;           // stable-note correction time
        float transitionTimeMs = 35.0f;      // note-to-note transition time
        float preserveVibrato = 0.70f;       // 0..1
        float humanize = 0.20f;              // 0..1, creates a small dead-band
        float formantPreservation = 0.90f;   // 0..1
        float transientProtection = 0.85f;   // 0..1
        float detectorSensitivity = 0.70f;   // 0..1
        float maximumCorrectionSemitones = 12.0f;
        float minimumPitchHz = 45.0f;
        float maximumPitchHz = 1600.0f;
        StereoMode stereoMode = StereoMode::linkedMidSide;

        // Wind Fix V5: 0 keeps the V4 residual unchanged, 1 applies the
        // full soft de-breath curve (up to approximately 12 dB in the air band).
        float breathReduction = 0.50f;       // 0..1

        // Creative tempo layer. When mode == off, the V5 signal path is
        // unchanged. The controller acts only on confirmed target revisions.
        CreativeTempo::Settings tempo;
    };

    struct Metering
    {
        float detectedPitchHz = 0.0f;
        float targetPitchHz = 0.0f;
        float confidence = 0.0f;
        float voicing = 0.0f;
        float breathiness = 0.0f;
        float harmonicity = 0.0f;
        float noisePath = 0.0f;
        float noiseReductionDb = 0.0f;
        float consensus = 0.0f;
        float correctionCents = 0.0f;
        float wetMix = 0.0f;
        float transitionBlend = 0.0f;
        bool dualSynthesisActive = false;
        int detectorSupport = 0;
        int octaveState = 0;
        int pendingOctaveObservations = 0;
        TrackingState state = TrackingState::unvoiced;

        // Creative tempo diagnostics.
        float tempoBpm = 120.0f;
        float tempoGridPhase = 0.0f;
        float tempoGlideTimeMs = 0.0f;
        bool tempoActive = false;
        bool tempoWaitingForGrid = false;
        bool tempoHostSyncValid = false;
        CreativeTempo::Mode tempoMode = CreativeTempo::Mode::off;
    };

    ModernPitchEngine() = default;

    void prepare(double sampleRate,
                 int maximumExpectedSamplesPerBlock,
                 int numberOfChannels,
                 LatencyMode latencyMode);

    void reset() noexcept;

    void process(juce::AudioBuffer<float>& buffer,
                 const double* scaleRatios,
                 int numberOfScaleRatios,
                 double rootFrequency,
                 const Parameters& parameters);

    void process(juce::AudioBuffer<float>& buffer,
                 const double* scaleRatios,
                 int numberOfScaleRatios,
                 double rootFrequency,
                 const Parameters& parameters,
                 const CreativeTempo::HostPosition& hostTempoPosition);

    void process(juce::AudioBuffer<float>& buffer,
                 const std::vector<double>& scaleRatios,
                 double rootFrequency,
                 const Parameters& parameters);

    void process(float* monoData,
                 int numberOfSamples,
                 const std::vector<double>& scaleRatios,
                 double rootFrequency,
                 const Parameters& parameters);

    // Keeps the same fixed delay while disabling correction. Use this from
    // AudioProcessor::processBlockBypassed() so host bypass remains aligned.
    void processBypassed(juce::AudioBuffer<float>& buffer);

    [[nodiscard]] int getLatencySamples() const noexcept { return latencySamples_; }
    [[nodiscard]] LatencyMode getLatencyMode() const noexcept { return latencyMode_; }
    [[nodiscard]] Metering getMetering() const noexcept;

private:
    struct PitchObservation
    {
        float frequencyHz = 0.0f;
        float confidence = 0.0f;
        float periodicity = 0.0f;
        float voicing = 0.0f;
        float consensus = 0.0f;
        float onsetStrength = 0.0f;
        int detectorSupport = 0;
        int octaveState = 0;
        int pendingOctaveObservations = 0;
        bool valid = false;
        bool onset = false;
    };

    struct HarmonicNoiseContext
    {
        float detectedPitchHz = 0.0f;
        float confidence = 0.0f;
        float voicing = 0.0f;
        float consensus = 0.0f;
        float onsetStrength = 0.0f;
        float breathReduction = 0.50f;
    };

    class BiquadLowPass
    {
    public:
        void prepare(double sampleRate, double cutoffHz, double q = 0.7071067811865476) noexcept;
        void reset() noexcept;
        [[nodiscard]] float process(float input) noexcept;

    private:
        double b0_ = 1.0;
        double b1_ = 0.0;
        double b2_ = 0.0;
        double a1_ = 0.0;
        double a2_ = 0.0;
        double z1_ = 0.0;
        double z2_ = 0.0;
    };

    class MultiRatePitchTracker
    {
    public:
        void prepare(double sampleRate) noexcept;
        void reset() noexcept;
        void setRange(float minimumPitchHz, float maximumPitchHz) noexcept;
        void setSensitivity(float sensitivity) noexcept;

        // Returns true when a new temporally decoded observation is available.
        bool processSample(float inputSample, PitchObservation& observation) noexcept;

    private:
        static constexpr int ringSize = 512;
        static constexpr int ringMask = ringSize - 1;
        static constexpr int analysisSize = 256;
        static constexpr int detectorHop = 32;
        static constexpr int detectorPathCount = 4;
        static constexpr int maxConsensusHypotheses = 20;
        static constexpr int decoderBeamWidth = 6;

        struct PitchCandidate
        {
            float frequencyHz = 0.0f;
            float confidence = 0.0f;
            float periodicity = 0.0f;
            int pathIndex = -1;
            int ageInHops = 1000;
            bool valid = false;
        };

        struct CandidateSlot
        {
            PitchCandidate candidate;
            int ageInHops = 1000;
        };

        struct ConsensusHypothesis
        {
            float frequencyHz = 0.0f;
            float confidence = 0.0f;
            float periodicity = 0.0f;
            float consensus = 0.0f;
            float evidenceScore = -1000.0f;
            int supportCount = 0;
            int directSupportCount = 0;
            std::uint8_t supportMask = 0;
            std::uint8_t freshSupportMask = 0;
            bool valid = false;
        };

        struct DecoderState
        {
            double logFrequency = 0.0;
            float score = -1000.0f;
            int ageInHops = 0;
            int octaveIndex = 0;
            bool valid = false;
        };

        struct DecoderDecision
        {
            PitchCandidate candidate;
            float consensus = 0.0f;
            int supportCount = 0;
            int directSupportCount = 0;
            std::uint8_t freshSupportMask = 0;
            int decoderOctaveIndex = 0;
            bool valid = false;
        };

        static_assert((ringSize & (ringSize - 1)) == 0,
                      "Pitch tracker ring size must be a power of two");

        void push(std::array<float, ringSize>& ring,
                  int& writePosition,
                  int& availableSamples,
                  float sample) noexcept;

        [[nodiscard]] PitchCandidate analyse(const std::array<float, ringSize>& ring,
                                             int writePosition,
                                             int availableSamples,
                                             double effectiveSampleRate,
                                             float minimumFrequency,
                                             float maximumFrequency) noexcept;

        [[nodiscard]] int collectFreshCandidates(
            std::array<PitchCandidate, detectorPathCount>& candidates) const noexcept;

        [[nodiscard]] int buildConsensusHypotheses(
            const std::array<PitchCandidate, detectorPathCount>& candidates,
            int candidateCount,
            std::array<ConsensusHypothesis, maxConsensusHypotheses>& hypotheses) const noexcept;

        [[nodiscard]] DecoderDecision decodeCandidate(bool onsetPending) noexcept;
        [[nodiscard]] float pathReliability(int pathIndex, float frequencyHz) const noexcept;
        [[nodiscard]] float candidateBaseScore(const PitchCandidate& candidate) const noexcept;
        [[nodiscard]] static float centsDistance(float frequencyA, float frequencyB) noexcept;
        [[nodiscard]] static bool isOctaveLikeTransition(float fromFrequency,
                                                         float toFrequency,
                                                         int& octaveDelta,
                                                         float& residualCents) noexcept;
        [[nodiscard]] bool confirmOctaveTransition(DecoderDecision& decision,
                                                   bool onsetPending) noexcept;
        void updateDecoderBeam(
            const std::array<ConsensusHypothesis, maxConsensusHypotheses>& hypotheses,
            int hypothesisCount,
            bool onsetPending) noexcept;

        double sampleRate_ = 48000.0;
        float minimumPitchHz_ = 45.0f;
        float maximumPitchHz_ = 1600.0f;
        float sensitivity_ = 0.70f;

        std::array<float, ringSize> fullRateRing_ {};
        std::array<float, ringSize> halfRateRing_ {};
        std::array<float, ringSize> quarterRateRing_ {};
        std::array<float, ringSize> eighthRateRing_ {};

        int fullRateWritePosition_ = 0;
        int halfRateWritePosition_ = 0;
        int quarterRateWritePosition_ = 0;
        int eighthRateWritePosition_ = 0;
        int fullRateAvailableSamples_ = 0;
        int halfRateAvailableSamples_ = 0;
        int quarterRateAvailableSamples_ = 0;
        int eighthRateAvailableSamples_ = 0;

        int halfRateDecimationCounter_ = 0;
        int quarterRateDecimationCounter_ = 0;
        int eighthRateDecimationCounter_ = 0;
        int hopCounter_ = 0;
        int analysisHopCounter_ = 0;

        BiquadLowPass halfRateAntiAlias_;
        BiquadLowPass quarterRateAntiAlias_;
        BiquadLowPass eighthRateAntiAlias_;

        float previousInput_ = 0.0f;
        float previousDcOutput_ = 0.0f;
        float dcBlockCoefficient_ = 0.995f;

        float fastEnergy_ = 0.0f;
        float slowEnergy_ = 0.0f;
        float fastEnergyCoefficient_ = 0.0f;
        float slowEnergyCoefficient_ = 0.0f;
        float onsetEnvelope_ = 0.0f;
        int onsetCooldownSamples_ = 0;
        bool onsetPending_ = false;

        CandidateSlot fullRateCandidate_;
        CandidateSlot halfRateCandidate_;
        CandidateSlot quarterRateCandidate_;
        CandidateSlot eighthRateCandidate_;

        std::array<float, analysisSize> frame_ {};
        std::array<float, analysisSize> difference_ {};
        std::array<DecoderState, decoderBeamWidth> decoderBeam_ {};

        float trackedPitchHz_ = 0.0f;
        float trackedConfidence_ = 0.0f;
        float trackedPeriodicity_ = 0.0f;
        float trackedConsensus_ = 0.0f;
        int trackedSupportCount_ = 0;
        int invalidHopCount_ = 0;

        int octaveState_ = 0;
        int pendingOctaveDelta_ = 0;
        int pendingOctaveCount_ = 0;
        float pendingOctaveFrequencyHz_ = 0.0f;
    };

    class ScaleQuantizer
    {
    public:
        bool update(const double* scaleRatios,
                    int numberOfScaleRatios,
                    double rootFrequency) noexcept;

        void resetTarget() noexcept;

        [[nodiscard]] double chooseTargetLog2(double inputLog2,
                                              float hysteresisCents) noexcept;

        [[nodiscard]] double getCurrentTargetLog2() const noexcept
        {
            return currentTargetLog2_;
        }

        [[nodiscard]] bool hasScale() const noexcept { return cachedScaleSize_ > 0; }

    private:
        [[nodiscard]] static std::uint64_t hashScale(const double* scaleRatios,
                                                     int numberOfScaleRatios,
                                                     double rootFrequency) noexcept;

        std::array<double, maxScaleRatios> cachedScaleLogRatios_ {};
        int cachedScaleSize_ = 0;
        double rootLog2_ = 0.0;
        double currentTargetLog2_ = 0.0;
        bool targetValid_ = false;
        std::uint64_t scaleHash_ = 0;
    };

    class CorrectionController
    {
    public:
        void prepare(double sampleRate) noexcept;
        void reset() noexcept;

        void acceptObservation(const PitchObservation& observation,
                               ScaleQuantizer& quantizer,
                               const Parameters& parameters) noexcept;

        void setSpectralReliability(float breathiness,
                                    float harmonicity) noexcept;
        void advanceOneSample(const Parameters& parameters) noexcept;

        [[nodiscard]] double getPitchRatio() const noexcept;
        [[nodiscard]] double getCurrentCorrectionCents() const noexcept
        {
            return currentCorrectionCents_;
        }
        [[nodiscard]] double getSynthesisTargetCorrectionCents() const noexcept
        {
            return synthesisTargetCorrectionCents_;
        }
        [[nodiscard]] double getDesiredCorrectionCents() const noexcept
        {
            return desiredCorrectionCents_;
        }
        [[nodiscard]] std::uint64_t getTargetRevision() const noexcept
        {
            return targetRevision_;
        }
        [[nodiscard]] float getWetMix() const noexcept { return wetMix_; }
        [[nodiscard]] float getCorrectionCents() const noexcept
        {
            return static_cast<float>(currentCorrectionCents_);
        }
        [[nodiscard]] float getTargetPitchHz() const noexcept;
        [[nodiscard]] TrackingState getState() const noexcept { return state_; }
        [[nodiscard]] float getVoicing() const noexcept { return currentVoicing_; }
        [[nodiscard]] float getFormantStability() const noexcept;

    private:
        void enterState(TrackingState newState, int durationSamples = 0) noexcept;
        void updateVoicingLatch(bool observationUsable,
                                float voicing,
                                float sensitivity) noexcept;
        [[nodiscard]] float confidenceAuthority(float confidence,
                                                float sensitivity) const noexcept;

        double sampleRate_ = 48000.0;
        TrackingState state_ = TrackingState::unvoiced;
        int stateSamplesRemaining_ = 0;
        int stableObservationCount_ = 0;
        int invalidObservationCount_ = 0;

        double observedLog2_ = 0.0;
        double pitchCentreLog2_ = 0.0;
        double targetLog2_ = 0.0;
        bool pitchCentreValid_ = false;
        bool targetValid_ = false;

        double desiredCorrectionCents_ = 0.0;
        double synthesisTargetCorrectionCents_ = 0.0;
        double currentCorrectionCents_ = 0.0;
        double correctionVelocityCentsPerSecond_ = 0.0;
        std::uint64_t targetRevision_ = 0;

        float currentConfidence_ = 0.0f;
        float currentVoicing_ = 0.0f;
        float currentOnsetStrength_ = 0.0f;
        float spectralBreathiness_ = 0.0f;
        float spectralHarmonicity_ = 1.0f;
        float authority_ = 0.0f;
        float authorityTarget_ = 0.0f;
        float wetMix_ = 0.0f;
        float wetMixTarget_ = 0.0f;
        float smoothedVoicing_ = 0.0f;
        float authorityAttackCoefficient_ = 1.0f;
        float authorityReleaseCoefficient_ = 1.0f;
        float wetAttackCoefficient_ = 1.0f;
        float wetReleaseCoefficient_ = 1.0f;
        bool voicedLatched_ = false;
        int voicedEnterCount_ = 0;
        int voicedExitCount_ = 0;
    };

    class TransitionManager
    {
    public:
        struct Command
        {
            double primaryCents = 0.0;
            double secondaryCents = 0.0;
            float blend = 0.0f;
            bool dualSynthesis = false;
            bool beginSecondary = false;
            bool commitSecondary = false;
        };

        void prepare(double sampleRate,
                     int synthesisFrameSize,
                     LatencyMode latencyMode) noexcept;
        void reset() noexcept;

        [[nodiscard]] Command processSample(
            double controllerCorrectionCents,
            double destinationCorrectionCents,
            std::uint64_t targetRevision,
            TrackingState trackingState,
            float wetMix,
            const Parameters& parameters,
            bool forceTransition = false) noexcept;

        [[nodiscard]] float getBlend() const noexcept { return publishedBlend_; }
        [[nodiscard]] bool isDualSynthesisActive() const noexcept
        {
            return phase_ != Phase::idle;
        }

    private:
        enum class Phase : std::uint8_t
        {
            idle = 0,
            preRoll,
            crossfade
        };

        void startTransition(double currentCents,
                             double targetCents,
                             const Parameters& parameters) noexcept;
        void updateSecondaryTrajectory(double targetCents,
                                       const Parameters& parameters) noexcept;
        [[nodiscard]] double transitionThresholdCents() const noexcept;
        [[nodiscard]] int crossfadeLengthSamples(
            const Parameters& parameters) const noexcept;

        double sampleRate_ = 48000.0;
        int synthesisFrameSize_ = 256;
        int synthesisHopSize_ = 64;
        LatencyMode latencyMode_ = LatencyMode::live;
        Phase phase_ = Phase::idle;

        bool initialised_ = false;
        bool pendingTarget_ = false;
        bool pendingForceTransition_ = false;
        bool beginEventPending_ = false;
        std::uint64_t lastSeenRevision_ = 0;
        std::uint64_t pendingRevision_ = 0;
        std::uint64_t transitionRevision_ = 0;

        double idleCents_ = 0.0;
        double primaryCents_ = 0.0;
        double secondaryCents_ = 0.0;
        double secondaryVelocityCentsPerSecond_ = 0.0;
        double transitionTargetCents_ = 0.0;
        double pendingTargetCents_ = 0.0;

        int preRollSamplesRemaining_ = 0;
        int crossfadeSamplesTotal_ = 1;
        int crossfadeSampleIndex_ = 0;
        int transitionCooldownSamples_ = 0;
        float publishedBlend_ = 0.0f;
    };

    class SpectralVoiceShifter
    {
    public:
        void prepare(double sampleRate, int frameSize);
        void reset() noexcept;

        [[nodiscard]] float processSample(
            float inputSample,
            const TransitionManager::Command& transition,
            float desiredWetMix,
            float formantPreservation,
            const HarmonicNoiseContext& harmonicNoiseContext,
            bool forcePhaseReset) noexcept;
        [[nodiscard]] float processBypassedSample(float inputSample) noexcept;

        [[nodiscard]] int getLatencySamples() const noexcept { return frameSize_; }
        [[nodiscard]] float getBreathiness() const noexcept { return smoothedBreathiness_; }
        [[nodiscard]] float getHarmonicity() const noexcept { return smoothedHarmonicity_; }
        [[nodiscard]] float getNoisePathAmount() const noexcept { return smoothedNoisePathAmount_; }
        [[nodiscard]] float getNoiseReductionDb() const noexcept { return currentNoiseReductionDb_; }

    private:
        using Complex = std::complex<float>;
        static constexpr int sineTableSize = 4096;
        static constexpr int formantRatioTableSize = 256;
        static constexpr int formantLevelCount = 32;

        struct SynthesisLayer
        {
            std::vector<Complex> spectrum;
            std::vector<double> synthesisPhases;
            std::vector<float> outputAccumulationRing;
            bool phaseInitialised = false;
        };

        void processFrame(std::int64_t frameEndSample,
                          const TransitionManager::Command& transition,
                          float formantPreservation,
                          const HarmonicNoiseContext& harmonicNoiseContext,
                          bool forcePhaseReset) noexcept;
        void synthesiseLayer(SynthesisLayer& layer,
                             std::int64_t frameEndSample,
                             double correctionCents,
                             float formantPreservation,
                             bool resetPhases,
                             float phaseAnchor,
                             int positiveBins) noexcept;
        void beginSecondaryTransition() noexcept;
        void clearLayerOutput(SynthesisLayer& layer) noexcept;
        [[nodiscard]] float consumeLayerOutput(SynthesisLayer& layer,
                                               std::int64_t sample) noexcept;
        [[nodiscard]] float blendLayers(float primary,
                                        float secondary,
                                        float transitionBlend) noexcept;

        void fft(std::vector<Complex>& data, bool inverse) noexcept;
        [[nodiscard]] static double wrapPhase(double phase) noexcept;
        void fastSinCos(double phase, float& sine, float& cosine) const noexcept;
        [[nodiscard]] float lookupFormantGain(float envelopeRatio,
                                              float formantAmount) const noexcept;
        [[nodiscard]] float readInputSample(std::int64_t absoluteSample) const noexcept;
        [[nodiscard]] float interpolateEnvelope(double binPosition) const noexcept;
        void calculateEnvelope(int positiveBins) noexcept;
        void calculatePeakRegions(int positiveBins) noexcept;
        void updateHarmonicNoiseAnalysis(
            int positiveBins,
            float spectralFlux,
            const HarmonicNoiseContext& context) noexcept;
        [[nodiscard]] float binFrequency(int bin) const noexcept;
        [[nodiscard]] float calculateHighBandFlatness(
            int firstBin,
            int lastBin) const noexcept;

        double sampleRate_ = 48000.0;
        int frameSize_ = 0;
        int hopSize_ = 0;

        std::vector<float> inputRing_;
        int inputRingMask_ = 0;
        int outputRingMask_ = 0;

        std::vector<float> window_;
        std::vector<int> fftBitReversal_;
        std::vector<Complex> fftTwiddles_;
        std::vector<float> sineTable_;
        std::vector<float> formantGainTable_;
        std::vector<Complex> fftBuffer_;
        std::vector<float> magnitudes_;
        std::vector<float> analysisPhases_;
        std::vector<float> previousMagnitudes_;
        std::vector<float> previousAnalysisPhases_;
        std::vector<double> trueSourceBins_;
        std::vector<double> propagatedPhases_;
        std::vector<float> logMagnitudes_;
        std::vector<float> rawSpectralEnvelope_;
        std::vector<float> spectralEnvelope_;
        std::vector<float> rawHarmonicMask_;
        std::vector<float> harmonicMask_;
        std::vector<float> harmonicMaskScratch_;
        std::vector<double> prefixSum_;
        std::vector<int> nearestPeak_;
        std::vector<int> peakBins_;
        std::array<SynthesisLayer, 2> layers_;

        std::int64_t inputSampleCounter_ = 0;
        bool analysisPhaseInitialised_ = false;
        bool phaseResetPending_ = false;
        bool envelopeInitialised_ = false;
        bool wetGateOpen_ = false;
        bool dualTransitionActive_ = false;
        bool secondaryStartPending_ = false;
        int activeLayerIndex_ = 0;
        int secondaryLayerIndex_ = 1;
        int envelopeFrameCounter_ = 0;
        int envelopeUpdateInterval_ = 2;
        float synthesisGain_ = 0.5f;
        float wetMix_ = 0.0f;
        float wetAttackCoefficient_ = 1.0f;
        float wetReleaseCoefficient_ = 1.0f;
        float envelopeAttackCoefficient_ = 1.0f;
        float envelopeReleaseCoefficient_ = 1.0f;
        float smoothedFormantPreservation_ = 0.0f;
        float formantReductionCoefficient_ = 1.0f;
        float formantRecoveryCoefficient_ = 1.0f;
        float transientSuppression_ = 0.0f;
        float transientReleaseCoefficient_ = 0.99f;

        // Wind Fix V4: causal harmonic/noise decomposition. The mask is
        // smoothed in frequency and time; the residual is reconstructed at
        // its original bins/phases inside the same IFFT as the shifted voice.
        float smoothedBreathiness_ = 0.0f;
        float smoothedHarmonicity_ = 1.0f;
        float smoothedNoisePathAmount_ = 0.0f;
        float smoothedNoiseGain_ = 1.0f;
        float currentNoiseReductionDb_ = 0.0f;
        float breathProtection_ = 0.0f;
        float breathAttackCoefficient_ = 1.0f;
        float breathReleaseCoefficient_ = 1.0f;
        float maskAttackCoefficient_ = 1.0f;
        float maskReleaseCoefficient_ = 1.0f;
        float metricAttackCoefficient_ = 1.0f;
        float metricReleaseCoefficient_ = 1.0f;
        float noiseReductionAttackCoefficient_ = 1.0f;
        float noiseReductionReleaseCoefficient_ = 1.0f;
        float transientNoiseRestoreCoefficient_ = 1.0f;
        float dryBreathLowPass_ = 0.0f;
        float dryBreathLowPassCoefficient_ = 1.0f;
        int breathPersistenceFrames_ = 0;

    };

    class FixedDelay
    {
    public:
        void prepare(int delaySamples);
        void reset() noexcept;
        [[nodiscard]] float process(float inputSample) noexcept;

    private:
        std::vector<float> buffer_;
        int mask_ = 0;
        int delaySamples_ = 0;
        std::int64_t sampleCounter_ = 0;
    };

    [[nodiscard]] static int frameSizeForMode(double sampleRate, LatencyMode mode) noexcept;
    [[nodiscard]] static int nextPowerOfTwo(int value) noexcept;
    [[nodiscard]] static float clamp01(float value) noexcept;

    double sampleRate_ = 48000.0;
    int preparedChannels_ = 1;
    int latencySamples_ = 256;
    LatencyMode latencyMode_ = LatencyMode::live;
    StereoMode currentStereoMode_ = StereoMode::linkedMidSide;

    MultiRatePitchTracker pitchTracker_;
    BiquadLowPass detectorConditioner_;
    ScaleQuantizer scaleQuantizer_;
    CorrectionController correctionController_;
    TransitionManager transitionManager_;
    CreativeTempo::Controller tempoController_;

    std::array<SpectralVoiceShifter, maxSupportedChannels> shifters_;
    std::array<FixedDelay, maxSupportedChannels> auxiliaryDelays_;

    // Seqlock-style snapshot: the audio thread publishes a coherent metering
    // frame, while the GUI can read without locks or blocking the callback.
    std::atomic<std::uint32_t> meterSequence_ { 0 };
    std::atomic<float> meterPitchHz_ { 0.0f };
    std::atomic<float> meterTargetHz_ { 0.0f };
    std::atomic<float> meterConfidence_ { 0.0f };
    std::atomic<float> meterVoicing_ { 0.0f };
    std::atomic<float> meterBreathiness_ { 0.0f };
    std::atomic<float> meterHarmonicity_ { 0.0f };
    std::atomic<float> meterNoisePath_ { 0.0f };
    std::atomic<float> meterNoiseReductionDb_ { 0.0f };
    std::atomic<float> meterConsensus_ { 0.0f };
    std::atomic<float> meterCorrectionCents_ { 0.0f };
    std::atomic<float> meterWetMix_ { 0.0f };
    std::atomic<float> meterTransitionBlend_ { 0.0f };
    std::atomic<bool> meterDualSynthesisActive_ { false };
    std::atomic<int> meterDetectorSupport_ { 0 };
    std::atomic<int> meterOctaveState_ { 0 };
    std::atomic<int> meterPendingOctaveObservations_ { 0 };
    std::atomic<int> meterState_ { static_cast<int>(TrackingState::unvoiced) };
    std::atomic<float> meterTempoBpm_ { 120.0f };
    std::atomic<float> meterTempoGridPhase_ { 0.0f };
    std::atomic<float> meterTempoGlideTimeMs_ { 0.0f };
    std::atomic<bool> meterTempoActive_ { false };
    std::atomic<bool> meterTempoWaiting_ { false };
    std::atomic<bool> meterTempoHostSync_ { false };
    std::atomic<int> meterTempoMode_ { static_cast<int>(CreativeTempo::Mode::off) };
};
