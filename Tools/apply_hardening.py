#!/usr/bin/env python3
"""Apply deterministic source edits that are awkward to express through Contents API.

The script is idempotent and deliberately fails when an expected anchor changes,
so CI cannot silently build a partially hardened DSP path.
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def patch(path: str, transforms) -> bool:
    file = ROOT / path
    original = file.read_text(encoding="utf-8")
    updated = original
    for label, old, new in transforms:
        updated = replace_once(updated, old, new, f"{path}: {label}")
    if updated != original:
        file.write_text(updated, encoding="utf-8", newline="\n")
        print(f"patched {path}")
        return True
    print(f"already patched {path}")
    return False


def main() -> int:
    changed = False

    changed |= patch("Source/ModernPitchEngine.h", [
        ("normalisation ring",
         """        struct SynthesisLayer
        {
            std::vector<Complex> spectrum;
            std::vector<double> synthesisPhases;
            std::vector<float> outputAccumulationRing;
            bool phaseInitialised = false;
        };""",
         """        struct SynthesisLayer
        {
            std::vector<Complex> spectrum;
            std::vector<double> synthesisPhases;
            std::vector<float> outputAccumulationRing;
            std::vector<float> outputNormalisationRing;
            bool phaseInitialised = false;
        };"""),
        ("correlation aware crossfade declaration",
         """        [[nodiscard]] float blendLayers(float primary,
                                         float secondary,
                                         float transitionBlend) noexcept;

        void fft""",
         """        struct CrossfadeEnergyState
        {
            float primaryEnergy = 1.0e-6f;
            float secondaryEnergy = 1.0e-6f;
            float crossEnergy = 0.0f;
        };

        [[nodiscard]] float energyPreservingCrossfade(
            float primary,
            float secondary,
            float mix,
            CrossfadeEnergyState& state) noexcept;
        [[nodiscard]] float blendLayers(float primary,
                                         float secondary,
                                         float transitionBlend) noexcept;

        void fft"""),
        ("crossfade state members",
         """        float synthesisGain_ = 0.5f;
        float wetMix_ = 0.0f;""",
         """        float synthesisGain_ = 0.5f;
        CrossfadeEnergyState transitionMixState_ {};
        CrossfadeEnergyState dryWetMixState_ {};
        float crossfadeEnergyCoefficient_ = 0.01f;
        float wetMix_ = 0.0f;"""),
    ])

    changed |= patch("Source/ModernPitchEngine.cpp", [
        ("allocate OLA normalisation",
         """        layer.outputAccumulationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.phaseInitialised = false;""",
         """        layer.outputAccumulationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.outputNormalisationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.phaseInitialised = false;"""),
        ("prepare crossfade smoothing",
         """    dryBreathLowPassCoefficient_ = static_cast<float>(
        1.0 - std::exp(-twoPi * 2800.0 / sampleRate_));
    reset();""",
         """    dryBreathLowPassCoefficient_ = static_cast<float>(
        1.0 - std::exp(-twoPi * 2800.0 / sampleRate_));
    crossfadeEnergyCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / (0.004 * sampleRate_)));
    reset();"""),
        ("clear OLA normalisation",
         """    std::fill(layer.outputAccumulationRing.begin(),
              layer.outputAccumulationRing.end(),
              0.0f);
}""",
         """    std::fill(layer.outputAccumulationRing.begin(),
              layer.outputAccumulationRing.end(),
              0.0f);
    std::fill(layer.outputNormalisationRing.begin(),
              layer.outputNormalisationRing.end(),
              0.0f);
}"""),
        ("reset crossfade estimators",
         """    secondaryLayerIndex_ = 1;
    wetMix_ = 0.0f;""",
         """    secondaryLayerIndex_ = 1;
    transitionMixState_ = {};
    dryWetMixState_ = {};
    wetMix_ = 0.0f;"""),
        ("reset transition estimator",
         """    secondary.phaseInitialised = primary.phaseInitialised;

    dualTransitionActive_ = true;""",
         """    secondary.phaseInitialised = primary.phaseInitialised;
    transitionMixState_ = {};

    dualTransitionActive_ = true;"""),
        ("write OLA normalisation",
         """        layer.outputAccumulationRing[static_cast<std::size_t>(outputIndex)] +=
            output;
    }
}""",
         """        const std::size_t ringIndex = static_cast<std::size_t>(outputIndex);
        layer.outputAccumulationRing[ringIndex] += output;
        layer.outputNormalisationRing[ringIndex] += synthesisWindow * synthesisWindow;
    }
}"""),
        ("low register phase anchor",
         """    const float phaseAnchor = resetAnalysis ? 0.0f
        : 0.32f * smoothStep(0.24f, 0.72f, spectralFlux);""",
         """    const float transientPhaseAnchor = resetAnalysis ? 0.0f
        : 0.32f * smoothStep(0.24f, 0.72f, spectralFlux);
    const float lowRegisterAnchor = resetAnalysis ? 0.0f
        : 0.18f
            * (1.0f - smoothStep(105.0f, 190.0f,
                                 harmonicNoiseContext.detectedPitchHz))
            * clamp01(harmonicNoiseContext.confidence)
            * clamp01(harmonicNoiseContext.voicing);
    const float phaseAnchor = std::max(transientPhaseAnchor,
                                       lowRegisterAnchor);"""),
        ("consume normalised OLA",
         """    const float accumulated = layer.outputAccumulationRing[index];
    layer.outputAccumulationRing[index] = 0.0f;
    return accumulated * synthesisGain_;
}""",
         """    const float accumulated = layer.outputAccumulationRing[index];
    const float normalisation = layer.outputNormalisationRing[index];
    layer.outputAccumulationRing[index] = 0.0f;
    layer.outputNormalisationRing[index] = 0.0f;
    if (!std::isfinite(accumulated) || !std::isfinite(normalisation)
        || normalisation <= 1.0e-8f)
        return 0.0f;
    const float output = accumulated / normalisation;
    return std::isfinite(output) ? output : 0.0f;
}"""),
        ("energy preserving layer crossfade",
         """float ModernPitchEngine::SpectralVoiceShifter::blendLayers(
    float primary,
    float secondary,
    float transitionBlend) noexcept
{
    // TransitionManager already publishes a smoothstep trajectory. A direct
    // complementary crossfade is both phase-stable and much cheaper than
    // evaluating sine/cosine for every audio sample.
    const float mix = clamp01(transitionBlend);
    return primary + mix * (secondary - primary);
}""",
         """float ModernPitchEngine::SpectralVoiceShifter::energyPreservingCrossfade(
    float primary,
    float secondary,
    float mix,
    CrossfadeEnergyState& state) noexcept
{
    if (!std::isfinite(primary) || !std::isfinite(secondary))
        return 0.0f;

    const float coefficient = crossfadeEnergyCoefficient_;
    state.primaryEnergy += coefficient
        * (primary * primary - state.primaryEnergy);
    state.secondaryEnergy += coefficient
        * (secondary * secondary - state.secondaryEnergy);
    state.crossEnergy += coefficient
        * (primary * secondary - state.crossEnergy);

    const float clampedMix = clamp01(mix);
    float sine = 0.0f;
    float cosine = 1.0f;
    fastSinCos(static_cast<double>(clampedMix) * 0.5 * pi, sine, cosine);

    const float predicted = cosine * cosine * state.primaryEnergy
        + sine * sine * state.secondaryEnergy
        + 2.0f * cosine * sine * state.crossEnergy;
    const float target = (1.0f - clampedMix) * state.primaryEnergy
        + clampedMix * state.secondaryEnergy;
    const float gain = std::clamp(
        std::sqrt(std::max(target, 1.0e-10f)
                  / std::max(predicted, 1.0e-10f)),
        0.67f,
        1.50f);
    const float output = gain * (cosine * primary + sine * secondary);
    return std::isfinite(output) ? output : 0.0f;
}

float ModernPitchEngine::SpectralVoiceShifter::blendLayers(
    float primary,
    float secondary,
    float transitionBlend) noexcept
{
    return energyPreservingCrossfade(primary,
                                     secondary,
                                     transitionBlend,
                                     transitionMixState_);
}"""),
        ("energy preserving dry wet mix",
         """    const float output = breathManagedDry
        + wetMix_ * (shifted - breathManagedDry);""",
         """    const float output = energyPreservingCrossfade(
        breathManagedDry,
        shifted,
        wetMix_,
        dryWetMixState_);"""),
        ("clear bypass normalisation",
         """    for (auto& layer : layers_)
        layer.outputAccumulationRing[static_cast<std::size_t>(outputIndex)] = 0.0f;""",
         """    for (auto& layer : layers_)
    {
        const std::size_t index = static_cast<std::size_t>(outputIndex);
        layer.outputAccumulationRing[index] = 0.0f;
        layer.outputNormalisationRing[index] = 0.0f;
    }"""),
        ("reset dry wet state on bypass",
         """    wetGateOpen_ = false;
    wetMix_ = 0.0f;""",
         """    wetGateOpen_ = false;
    transitionMixState_ = {};
    dryWetMixState_ = {};
    wetMix_ = 0.0f;"""),
    ])

    changed |= patch("Source/PluginEditor.cpp", [
        ("delete active custom snapshot",
         """            processorRef.activeCustomPresetIndex.store (-1);
            processorRef.currentScaleIndex.store (0);""",
         """            processorRef.selectBuiltInScale (0);"""),
        ("adjust custom index snapshot",
         """            processorRef.activeCustomPresetIndex.store (
                processorRef.activeCustomPresetIndex.load() - 1);""",
         """            processorRef.selectCustomPreset (
                processorRef.activeCustomPresetIndex.load() - 1);"""),
        ("select custom snapshot",
         """        processorRef.activeCustomPresetIndex.store (customIdx);
        return;""",
         """        processorRef.selectCustomPreset (customIdx);
        return;"""),
        ("select built in snapshot",
         """        processorRef.currentScaleIndex.store (scaleIdx);
        processorRef.activeCustomPresetIndex.store (-1);""",
         """        processorRef.selectBuiltInScale (scaleIdx);"""),
        ("define root and mode callbacks",
         """//==============================================================================
juce::String MicrotonalAutotuneAudioProcessorEditor::trackingStateToString (""",
         """void MicrotonalAutotuneAudioProcessorEditor::onRootNoteSelected()
{
    const int index = rootNoteSelector.getSelectedId() - 1;
    if (index >= 0)
        processorRef.selectRootNote(index);
}

void MicrotonalAutotuneAudioProcessorEditor::onModeSelected()
{
    const int mode = modeSelector.getSelectedId() - 1;
    if (mode >= 0)
        processorRef.updateProcessingMode(mode);
}

//==============================================================================
juce::String MicrotonalAutotuneAudioProcessorEditor::trackingStateToString ("""),
    ])

    changed |= patch("Source/CustomScaleEditor.cpp", [
        ("publish custom preset",
         """    if (success)
    {
        juce::AlertWindow::showMessageBoxAsync""",
         """    if (success)
    {
        processorRef.publishScaleSnapshot();
        juce::AlertWindow::showMessageBoxAsync"""),
    ])

    changed |= patch("CMakeLists.txt", [
        ("portable languages",
         "project(MicrotonalAutotune VERSION 1.0.0 LANGUAGES C CXX OBJCXX)",
         """project(MicrotonalAutotune VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()"""),
        ("portable formats",
         """# Plugin target — VST3 only
juce_add_plugin(MicrotonalAutotune""",
         """set(AUTOTUNE_PLUGIN_FORMATS VST3)
if(APPLE)
    list(APPEND AUTOTUNE_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(MicrotonalAutotune"""),
        ("use portable formats",
         "    FORMATS VST3 AU",
         "    FORMATS ${AUTOTUNE_PLUGIN_FORMATS}"),
        ("sanitizer option",
         """set(CMAKE_CXX_STANDARD_REQUIRED ON)
""",
         """set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AUTOTUNE_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers" OFF)
if(AUTOTUNE_ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
"""),
    ])

    print("hardening source changes applied" if changed else "source tree already hardened")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
