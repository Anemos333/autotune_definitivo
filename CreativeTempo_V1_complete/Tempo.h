#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Creative tempo-domain target scheduler.
//
// This module never touches audio samples and never changes plugin latency.
// It sits after pitch/scale decisions and before the existing TransitionManager.
// When disabled it is a strict pass-through.
namespace CreativeTempo
{
    enum class Mode : int
    {
        off = 0,
        tempoGlide,
        glideLock
    };

    enum class Division : int
    {
        note128 = 0,
        note64,
        note32,
        note16,
        note8
    };

    struct Settings
    {
        Mode mode = Mode::off;
        Division division = Division::note32;

        // Duration of the pitch glide as a fraction of the selected division.
        // 0.35 means 35% of a 1/32, 1/16, etc.
        float glideFraction = 0.35f; // 0.05..1.0

        // Glide Lock only. 0 releases immediately, 1 releases on the next grid.
        float lockStrength = 1.0f;   // 0..1

        // If a strong onset is already very close to a grid line, do not wait
        // for the following line. This avoids a full-division musical delay.
        bool smartOnset = true;
        float smartOnsetWindow = 0.18f; // fraction of one division

        // Used for Tempo Glide when the host exposes no BPM. Glide Lock falls
        // back to immediate Tempo Glide if PPQ/transport data are unavailable.
        double fallbackBpm = 120.0;
    };

    struct HostPosition
    {
        double bpm = 120.0;
        double ppqAtBlockStart = 0.0;
        std::int64_t timeInSamples = 0;
        int numberOfSamples = 0;

        bool hasBpm = false;
        bool hasPpq = false;
        bool hasTimeInSamples = false;
        bool isPlaying = false;
        bool isLooping = false;
    };

    struct Decision
    {
        double controllerCents = 0.0;
        double destinationCents = 0.0;
        std::uint64_t targetRevision = 0;
        float transitionTimeMs = 35.0f;
        float gridPhase = 0.0f;

        bool active = false;
        bool waitingForGrid = false;
        bool hostSyncValid = false;
        bool forceTransition = false;
    };

    struct Metering
    {
        float bpm = 120.0f;
        float gridPhase = 0.0f;
        float glideTimeMs = 0.0f;
        bool active = false;
        bool waitingForGrid = false;
        bool hostSyncValid = false;
        Mode mode = Mode::off;
    };

    [[nodiscard]] double divisionToPpq(Division division) noexcept;
    [[nodiscard]] Division divisionFromIndex(int index) noexcept;
    [[nodiscard]] int divisionToIndex(Division division) noexcept;

    class Controller final
    {
    public:
        void prepare(double sampleRate) noexcept;
        void reset() noexcept;

        // Call once at the beginning of every audio callback.
        void beginBlock(const HostPosition& host,
                        const Settings& settings,
                        int numberOfSamples) noexcept;

        // Call once per sample, after the normal CorrectionController and
        // before TransitionManager. When mode == off, the values are returned
        // unchanged so the existing V5 path remains bit-for-bit untouched.
        [[nodiscard]] Decision processSample(
            double controllerCorrectionCents,
            double destinationCorrectionCents,
            std::uint64_t inputTargetRevision,
            float onsetStrength,
            bool musicalState,
            int sampleIndex,
            const Settings& settings,
            float normalTransitionTimeMs) noexcept;

        [[nodiscard]] Metering getMetering() const noexcept { return metering_; }
        [[nodiscard]] float getGlideTimeMs() const noexcept { return glideTimeMs_; }
        [[nodiscard]] bool isActive() const noexcept { return mode_ != Mode::off; }

    private:
        [[nodiscard]] double ppqAtSample(int sampleIndex) const noexcept;
        [[nodiscard]] float gridPhaseAtSample(int sampleIndex) const noexcept;
        [[nodiscard]] bool transportIsUsable() const noexcept;
        [[nodiscard]] bool detectTransportJump(const HostPosition& host,
                                               int numberOfSamples) noexcept;
        void initialiseFromInput(double controllerCents,
                                 double destinationCents,
                                 std::uint64_t inputRevision) noexcept;
        void releasePendingTarget(bool forceTransition) noexcept;
        void clearPending() noexcept;

        double sampleRate_ = 48000.0;
        double bpm_ = 120.0;
        double ppqStart_ = 0.0;
        double ppqPerSample_ = 0.0;
        double gridPpq_ = 0.125;
        double scheduledReleasePpq_ = 0.0;
        float glideTimeMs_ = 35.0f;

        Mode mode_ = Mode::off;
        Division division_ = Division::note32;
        bool hostHasBpm_ = false;
        bool hostHasPpq_ = false;
        bool hostPlaying_ = false;
        bool hostLooping_ = false;
        bool transportJumped_ = false;

        bool initialised_ = false;
        bool pendingTarget_ = false;
        bool forceEventPending_ = false;
        double heldControllerCents_ = 0.0;
        double heldDestinationCents_ = 0.0;
        double pendingDestinationCents_ = 0.0;
        std::uint64_t seenInputRevision_ = 0;
        std::uint64_t publishedRevision_ = 0;

        bool haveTransportHistory_ = false;
        std::int64_t expectedNextSample_ = 0;
        double expectedNextPpq_ = 0.0;

        Metering metering_;
    };
}
