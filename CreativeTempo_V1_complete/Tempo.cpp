#include "Tempo.h"

namespace CreativeTempo
{
namespace
{
    template <typename T>
    [[nodiscard]] T clampValue(T value, T low, T high) noexcept
    {
        return std::max(low, std::min(high, value));
    }

    [[nodiscard]] bool finite(double value) noexcept
    {
        return std::isfinite(value);
    }
}

// Quarter note == 1 PPQ.
double divisionToPpq(Division division) noexcept
{
    switch (division)
    {
        case Division::note128: return 1.0 / 32.0;
        case Division::note64:  return 1.0 / 16.0;
        case Division::note32:  return 1.0 / 8.0;
        case Division::note16:  return 1.0 / 4.0;
        case Division::note8:   return 1.0 / 2.0;
    }

    return 1.0 / 8.0;
}

Division divisionFromIndex(int index) noexcept
{
    return static_cast<Division>(clampValue(index, 0, 4));
}

int divisionToIndex(Division division) noexcept
{
    return clampValue(static_cast<int>(division), 0, 4);
}

void Controller::prepare(double sampleRate) noexcept
{
    sampleRate_ = finite(sampleRate) && sampleRate > 1000.0
        ? sampleRate
        : 48000.0;
    reset();
}

void Controller::reset() noexcept
{
    bpm_ = 120.0;
    ppqStart_ = 0.0;
    ppqPerSample_ = 0.0;
    gridPpq_ = divisionToPpq(Division::note32);
    scheduledReleasePpq_ = 0.0;
    glideTimeMs_ = 35.0f;

    mode_ = Mode::off;
    division_ = Division::note32;
    hostHasBpm_ = false;
    hostHasPpq_ = false;
    hostPlaying_ = false;
    hostLooping_ = false;
    transportJumped_ = false;

    initialised_ = false;
    pendingTarget_ = false;
    forceEventPending_ = false;
    heldControllerCents_ = 0.0;
    heldDestinationCents_ = 0.0;
    pendingDestinationCents_ = 0.0;
    seenInputRevision_ = 0;
    publishedRevision_ = 0;

    haveTransportHistory_ = false;
    expectedNextSample_ = 0;
    expectedNextPpq_ = 0.0;
    metering_ = {};
}

bool Controller::detectTransportJump(const HostPosition& host,
                                     int numberOfSamples) noexcept
{
    bool jumped = false;

    if (haveTransportHistory_ && host.isPlaying)
    {
        if (host.hasTimeInSamples)
        {
            const auto error = std::llabs(host.timeInSamples
                                          - expectedNextSample_);
            const auto tolerance = static_cast<std::int64_t>(
                std::max(4, numberOfSamples * 2));
            jumped = jumped || error > tolerance;
        }

        if (host.hasPpq && finite(host.ppqAtBlockStart))
        {
            const double expectedTravel = ppqPerSample_
                * static_cast<double>(std::max(1, numberOfSamples));
            const double tolerance = std::max(gridPpq_ * 1.5,
                                              expectedTravel * 3.0 + 0.01);
            jumped = jumped
                || std::abs(host.ppqAtBlockStart - expectedNextPpq_) > tolerance;
        }
    }

    return jumped;
}

void Controller::beginBlock(const HostPosition& host,
                            const Settings& settings,
                            int numberOfSamples) noexcept
{
    const Mode requestedMode = settings.mode;
    const Division requestedDivision = divisionFromIndex(
        divisionToIndex(settings.division));
    const bool modeChanged = requestedMode != mode_;
    const bool divisionChanged = requestedDivision != division_;

    mode_ = requestedMode;
    division_ = requestedDivision;
    gridPpq_ = divisionToPpq(division_);

    hostHasBpm_ = host.hasBpm && finite(host.bpm) && host.bpm > 1.0;
    hostHasPpq_ = host.hasPpq && finite(host.ppqAtBlockStart);
    hostPlaying_ = host.isPlaying;
    hostLooping_ = host.isLooping;

    const double fallback = finite(settings.fallbackBpm)
        ? clampValue(settings.fallbackBpm, 20.0, 400.0)
        : 120.0;
    bpm_ = hostHasBpm_ ? clampValue(host.bpm, 20.0, 400.0) : fallback;
    ppqStart_ = hostHasPpq_ ? host.ppqAtBlockStart : 0.0;
    ppqPerSample_ = bpm_ / (60.0 * sampleRate_);

    const float safeGlideFraction = clampValue(settings.glideFraction,
                                               0.05f,
                                               1.0f);
    const double divisionMs = 60000.0 * gridPpq_ / bpm_;
    glideTimeMs_ = static_cast<float>(clampValue(
        divisionMs * static_cast<double>(safeGlideFraction),
        2.0,
        2000.0));

    transportJumped_ = detectTransportJump(host, numberOfSamples);

    if (modeChanged || divisionChanged || transportJumped_)
    {
        clearPending();
        forceEventPending_ = false;

        // Reacquire the current musical correction on the next sample. This
        // prevents a stale grid event from firing after a mode change, seek,
        // loop wrap, or division change.
        initialised_ = false;
    }

    if (host.hasTimeInSamples)
    {
        expectedNextSample_ = host.timeInSamples
            + static_cast<std::int64_t>(std::max(0, numberOfSamples));
    }
    if (hostHasPpq_)
    {
        expectedNextPpq_ = ppqStart_
            + ppqPerSample_ * static_cast<double>(std::max(0, numberOfSamples));
    }
    haveTransportHistory_ = host.isPlaying
        && (host.hasTimeInSamples || hostHasPpq_);

    metering_.mode = mode_;
    metering_.bpm = static_cast<float>(bpm_);
    metering_.glideTimeMs = glideTimeMs_;
    metering_.active = mode_ != Mode::off;
    metering_.hostSyncValid = transportIsUsable();
    metering_.waitingForGrid = pendingTarget_;
    metering_.gridPhase = hostHasPpq_ ? gridPhaseAtSample(0) : 0.0f;
}

double Controller::ppqAtSample(int sampleIndex) const noexcept
{
    return ppqStart_ + static_cast<double>(std::max(0, sampleIndex))
        * ppqPerSample_;
}

float Controller::gridPhaseAtSample(int sampleIndex) const noexcept
{
    if (!hostHasPpq_ || gridPpq_ <= 0.0)
        return 0.0f;

    const double position = ppqAtSample(sampleIndex);
    double phase = std::fmod(position, gridPpq_);
    if (phase < 0.0)
        phase += gridPpq_;
    return static_cast<float>(clampValue(phase / gridPpq_, 0.0, 1.0));
}

bool Controller::transportIsUsable() const noexcept
{
    return hostHasBpm_ && hostHasPpq_ && hostPlaying_ && !transportJumped_;
}

void Controller::initialiseFromInput(double controllerCents,
                                     double destinationCents,
                                     std::uint64_t inputRevision) noexcept
{
    initialised_ = true;
    heldControllerCents_ = finite(controllerCents) ? controllerCents : 0.0;
    heldDestinationCents_ = finite(destinationCents)
        ? destinationCents
        : heldControllerCents_;
    pendingDestinationCents_ = heldDestinationCents_;
    seenInputRevision_ = inputRevision;
    publishedRevision_ = inputRevision;
    clearPending();
}

void Controller::clearPending() noexcept
{
    pendingTarget_ = false;
    scheduledReleasePpq_ = 0.0;
}

void Controller::releasePendingTarget(bool forceTransition) noexcept
{
    if (!pendingTarget_)
        return;

    heldDestinationCents_ = pendingDestinationCents_;
    ++publishedRevision_;
    if (publishedRevision_ == 0)
        ++publishedRevision_;
    forceEventPending_ = forceTransition;
    clearPending();
}

Decision Controller::processSample(double controllerCorrectionCents,
                                   double destinationCorrectionCents,
                                   std::uint64_t inputTargetRevision,
                                   float onsetStrength,
                                   bool musicalState,
                                   int sampleIndex,
                                   const Settings& settings,
                                   float normalTransitionTimeMs) noexcept
{
    Decision result;
    controllerCorrectionCents = finite(controllerCorrectionCents)
        ? controllerCorrectionCents
        : 0.0;
    destinationCorrectionCents = finite(destinationCorrectionCents)
        ? destinationCorrectionCents
        : controllerCorrectionCents;

    if (!initialised_)
        initialiseFromInput(controllerCorrectionCents,
                            destinationCorrectionCents,
                            inputTargetRevision);

    const float gridPhase = hostHasPpq_ ? gridPhaseAtSample(sampleIndex) : 0.0f;

    if (mode_ == Mode::off)
    {
        clearPending();
        forceEventPending_ = false;
        seenInputRevision_ = inputTargetRevision;
        publishedRevision_ = inputTargetRevision;
        heldControllerCents_ = controllerCorrectionCents;
        heldDestinationCents_ = destinationCorrectionCents;

        result.controllerCents = controllerCorrectionCents;
        result.destinationCents = destinationCorrectionCents;
        result.targetRevision = inputTargetRevision;
        result.transitionTimeMs = normalTransitionTimeMs;
        result.gridPhase = gridPhase;
        return result;
    }

    const bool newTarget = inputTargetRevision != seenInputRevision_;
    if (newTarget)
    {
        seenInputRevision_ = inputTargetRevision;
        pendingDestinationCents_ = destinationCorrectionCents;
        pendingTarget_ = true;

        const bool canLock = mode_ == Mode::glideLock
            && transportIsUsable()
            && musicalState;

        if (!canLock)
        {
            // Tempo Glide, stopped transport, or a host without PPQ: release
            // immediately but retain beat-synchronised glide duration.
            releasePendingTarget(true);
        }
        else
        {
            const double currentPpq = ppqAtSample(sampleIndex);
            const double gridIndex = std::floor(currentPpq / gridPpq_);
            const double previousGrid = gridIndex * gridPpq_;
            double nextGrid = previousGrid + gridPpq_;

            const double epsilon = std::max(1.0e-9, ppqPerSample_ * 0.75);
            if (std::abs(currentPpq - previousGrid) <= epsilon)
                nextGrid = currentPpq;

            const double distanceToPrevious = std::abs(currentPpq - previousGrid);
            const double distanceToNext = std::abs(nextGrid - currentPpq);
            const double nearestDistance = std::min(distanceToPrevious,
                                                    distanceToNext);
            const float safeWindow = clampValue(settings.smartOnsetWindow,
                                                0.02f,
                                                0.45f);
            const bool smartRelease = settings.smartOnset
                && onsetStrength >= 0.52f
                && nearestDistance <= gridPpq_
                    * static_cast<double>(safeWindow);

            if (smartRelease)
            {
                releasePendingTarget(true);
            }
            else
            {
                const float strength = clampValue(settings.lockStrength,
                                                  0.0f,
                                                  1.0f);
                scheduledReleasePpq_ = currentPpq
                    + (nextGrid - currentPpq) * static_cast<double>(strength);

                if (strength <= 0.001f
                    || scheduledReleasePpq_ <= currentPpq + epsilon)
                {
                    releasePendingTarget(true);
                }
            }
        }
    }
    else if (pendingTarget_)
    {
        // The scale target may refine slightly while waiting. Keep the latest
        // destination but do not restart the grid timer.
        pendingDestinationCents_ = destinationCorrectionCents;
    }

    if (pendingTarget_ && transportIsUsable())
    {
        const double currentPpq = ppqAtSample(sampleIndex);
        const double halfSamplePpq = 0.5 * ppqPerSample_;
        if (currentPpq + halfSamplePpq >= scheduledReleasePpq_)
            releasePendingTarget(true);
    }
    else if (pendingTarget_ && !transportIsUsable())
    {
        // Defensive fallback if the host stops or drops PPQ mid-block.
        releasePendingTarget(true);
    }

    const bool justReleased = forceEventPending_;

    if (pendingTarget_)
    {
        // Freeze the audible correction until the scheduled release. The
        // detector and normal controller continue running in the background.
        result.controllerCents = heldControllerCents_;
        result.destinationCents = heldDestinationCents_;
        result.targetRevision = publishedRevision_;
    }
    else
    {
        result.controllerCents = justReleased
            ? heldControllerCents_
            : controllerCorrectionCents;
        result.destinationCents = justReleased
            ? heldDestinationCents_
            : destinationCorrectionCents;
        result.targetRevision = publishedRevision_;
        result.forceTransition = justReleased;

        if (!justReleased)
        {
            heldControllerCents_ = controllerCorrectionCents;
            heldDestinationCents_ = destinationCorrectionCents;
        }
    }

    forceEventPending_ = false;
    result.transitionTimeMs = glideTimeMs_;
    result.gridPhase = gridPhase;
    result.active = true;
    result.waitingForGrid = pendingTarget_;
    result.hostSyncValid = transportIsUsable();

    metering_.mode = mode_;
    metering_.bpm = static_cast<float>(bpm_);
    metering_.gridPhase = gridPhase;
    metering_.glideTimeMs = glideTimeMs_;
    metering_.active = true;
    metering_.waitingForGrid = pendingTarget_;
    metering_.hostSyncValid = transportIsUsable();

    return result;
}
}
