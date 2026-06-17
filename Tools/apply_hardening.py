from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

def read(rel): return (ROOT/rel).read_text(encoding='utf-8')
def write(rel, text):
    p=ROOT/rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text, encoding='utf-8', newline='\n')

def repl(text, old, new, label):
    n=text.count(old)
    if n == 0 and new in text:
        return text
    if n != 1:
        raise RuntimeError(f'{label}: expected exactly one match, found {n}')
    return text.replace(old,new,1)

h=read('Source/ModernPitchEngine.h')
h=repl(h,
'''            std::vector<float> outputAccumulationRing;
            bool phaseInitialised = false;''',
'''            std::vector<float> outputAccumulationRing;
            // Exact per-sample OLA weight, including startup and transition pre-roll.
            std::vector<float> outputNormalisationRing;
            bool phaseInitialised = false;''','normalisation ring declaration')
h=repl(h,
'''        [[nodiscard]] float blendLayers(float primary,
                                         float secondary,
                                         float transitionBlend) noexcept;''',
'''        [[nodiscard]] float blendLayers(float primary,
                                         float secondary,
                                         float transitionBlend) noexcept;
        [[nodiscard]] float correlationAwareCrossfade(float a,
                                                      float b,
                                                      float mix,
                                                      float& energyA,
                                                      float& energyB,
                                                      float& crossEnergy) noexcept;''','crossfade declaration')
h=repl(h,
'''        float synthesisGain_ = 0.5f;
        float wetMix_ = 0.0f;''',
'''        float synthesisGain_ = 0.5f; // retained for state compatibility
        float wetMix_ = 0.0f;
        float layerEnergyPrimary_ = 1.0e-8f;
        float layerEnergySecondary_ = 1.0e-8f;
        float layerCrossEnergy_ = 0.0f;
        float dryEnergy_ = 1.0e-8f;
        float wetEnergy_ = 1.0e-8f;
        float dryWetCrossEnergy_ = 0.0f;
        float crossfadeEnergyCoefficient_ = 0.001f;''','crossfade state')
write('Source/ModernPitchEngine.h',h)

c=read('Source/ModernPitchEngine.cpp')
c=repl(c,
'''        layer.outputAccumulationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.phaseInitialised = false;''',
'''        layer.outputAccumulationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.outputNormalisationRing.assign(
            static_cast<std::size_t>(outputRingSize), 0.0f);
        layer.phaseInitialised = false;''','allocate OLA weights')
c=repl(c,
'''    dryBreathLowPassCoefficient_ = static_cast<float>(
        1.0 - std::exp(-twoPi * 2800.0 / sampleRate_));
    reset();''',
'''    dryBreathLowPassCoefficient_ = static_cast<float>(
        1.0 - std::exp(-twoPi * 2800.0 / sampleRate_));
    crossfadeEnergyCoefficient_ = static_cast<float>(
        1.0 - std::exp(-1.0 / std::max(1.0, 0.020 * sampleRate_)));
    reset();''','crossfade coefficient')
c=repl(c,
'''    std::fill(layer.outputAccumulationRing.begin(),
              layer.outputAccumulationRing.end(),
              0.0f);''',
'''    std::fill(layer.outputAccumulationRing.begin(),
              layer.outputAccumulationRing.end(),
              0.0f);
    std::fill(layer.outputNormalisationRing.begin(),
              layer.outputNormalisationRing.end(),
              0.0f);''','clear OLA weights')
c=repl(c,
'''    envelopeFrameCounter_ = 0;
}''',
'''    envelopeFrameCounter_ = 0;
    layerEnergyPrimary_ = 1.0e-8f;
    layerEnergySecondary_ = 1.0e-8f;
    layerCrossEnergy_ = 0.0f;
    dryEnergy_ = 1.0e-8f;
    wetEnergy_ = 1.0e-8f;
    dryWetCrossEnergy_ = 0.0f;
}''','reset crossfade state')
c=repl(c,
'''        layer.outputAccumulationRing[static_cast<std::size_t>(outputIndex)] +=
            output;''',
'''        const std::size_t ringIndex = static_cast<std::size_t>(outputIndex);
        layer.outputAccumulationRing[ringIndex] += output;
        layer.outputNormalisationRing[ringIndex] += synthesisWindow * synthesisWindow;''','accumulate OLA weights')
c=repl(c,
'''    const float accumulated = layer.outputAccumulationRing[index];
    layer.outputAccumulationRing[index] = 0.0f;
    return accumulated * synthesisGain_;''',
'''    const float accumulated = layer.outputAccumulationRing[index];
    const float normalisation = layer.outputNormalisationRing[index];
    layer.outputAccumulationRing[index] = 0.0f;
    layer.outputNormalisationRing[index] = 0.0f;
    if (!std::isfinite(accumulated) || !std::isfinite(normalisation)
        || normalisation <= 1.0e-8f)
        return 0.0f;
    return accumulated / normalisation;''','consume normalized OLA')
c=repl(c,
'''float ModernPitchEngine::SpectralVoiceShifter::blendLayers(
    float primary,
    float secondary,
    float transitionBlend) noexcept
{
    // TransitionManager already publishes a smoothstep trajectory. A direct
    // complementary crossfade is both phase-stable and much cheaper than
    // evaluating sine/cosine for every audio sample.
    const float mix = clamp01(transitionBlend);
    return primary + mix * (secondary - primary);
}''',
'''float ModernPitchEngine::SpectralVoiceShifter::correlationAwareCrossfade(
    float a, float b, float mix,
    float& energyA, float& energyB, float& crossEnergy) noexcept
{
    if (!std::isfinite(a)) a = 0.0f;
    if (!std::isfinite(b)) b = 0.0f;
    mix = clamp01(mix);
    const float coefficient = crossfadeEnergyCoefficient_;
    energyA += coefficient * (a * a - energyA);
    energyB += coefficient * (b * b - energyB);
    crossEnergy += coefficient * (a * b - crossEnergy);
    const float angle = mix * static_cast<float>(0.5 * pi);
    const float gainA = std::cos(angle);
    const float gainB = std::sin(angle);
    const float raw = gainA * a + gainB * b;
    const float targetPower = (1.0f - mix) * energyA + mix * energyB;
    const float predictedPower = gainA * gainA * energyA
                               + gainB * gainB * energyB
                               + 2.0f * gainA * gainB * crossEnergy;
    float normaliser = 1.0f;
    if (targetPower > 1.0e-10f && predictedPower > 1.0e-10f)
        normaliser = std::sqrt(targetPower / predictedPower);
    normaliser = std::clamp(normaliser, 0.70710678f, 1.41421356f);
    const float result = raw * normaliser;
    return std::isfinite(result) ? result : 0.0f;
}

float ModernPitchEngine::SpectralVoiceShifter::blendLayers(
    float primary, float secondary, float transitionBlend) noexcept
{
    return correlationAwareCrossfade(primary, secondary, transitionBlend,
                                     layerEnergyPrimary_, layerEnergySecondary_,
                                     layerCrossEnergy_);
}''','correlation aware layer blend')
c=repl(c,
'''    const float output = breathManagedDry
        + wetMix_ * (shifted - breathManagedDry);''',
'''    const float output = correlationAwareCrossfade(
        breathManagedDry, shifted, wetMix_, dryEnergy_, wetEnergy_,
        dryWetCrossEnergy_);''','correlation aware wet dry')
c=repl(c,
'''    for (auto& layer : layers_)
        layer.outputAccumulationRing[static_cast<std::size_t>(outputIndex)] = 0.0f;''',
'''    for (auto& layer : layers_)
    {
        const std::size_t index = static_cast<std::size_t>(outputIndex);
        layer.outputAccumulationRing[index] = 0.0f;
        layer.outputNormalisationRing[index] = 0.0f;
    }''','bypass clears weights')
write('Source/ModernPitchEngine.cpp',c)

h=read('Source/PluginProcessor.h')
h=repl(h,'#include <vector>\n#include <atomic>','#include <vector>\n#include <array>\n#include <atomic>','processor includes')
h=repl(h,
'''    // Get current active scale ratios
    std::vector<double> getCurrentScaleRatios() const;''',
'''    // Message-thread API: fixed immutable snapshot for the callback.
    std::vector<double> getCurrentScaleRatios() const;
    void publishScaleSnapshot() noexcept;
    void selectBuiltInScale (int index) noexcept;
    void selectCustomScale (int index) noexcept;
    void selectRootNote (int index) noexcept;''','snapshot public API')
h=repl(h,
'''    // YIN pitch detection (Slow mode)
    float detectPitchYIN (const float* buffer, int numSamples, double sampleRate) const;''',
'''    struct ScaleSnapshot
    {
        std::array<double, ModernPitchEngine::maxScaleRatios> ratios {};
        int count = 0;
        double rootFrequency = 440.0;
    };
    void writeScaleSnapshot (const std::vector<double>& ratios,
                             double rootFrequency) noexcept;
    [[nodiscard]] ScaleSnapshot readScaleSnapshot() const noexcept;
    std::array<ScaleSnapshot, 2> scaleSnapshots_ {};
    std::atomic<int> publishedScaleSnapshot_ { 0 };

    // Uses yinAccumulator allocated by prepareToPlay.
    float detectPitchYIN (const float* buffer, int numSamples, double sampleRate) noexcept;''','snapshot private and YIN')
write('Source/PluginProcessor.h',h)

p=read('Source/PluginProcessor.cpp')
p=repl(p,
'''    slowMeterTargetHz.store (0.0f, std::memory_order_relaxed);

    // Prepare ModernPitchEngine-based live pitch processor''',
'''    slowMeterTargetHz.store (0.0f, std::memory_order_relaxed);
    publishScaleSnapshot();

    // Prepare ModernPitchEngine-based live pitch processor''','publish on prepare')
p=repl(p,
'''float MicrotonalAutotuneAudioProcessor::detectPitchYIN (const float* buffer, int numSamples, double sampleRate) const
{
    if (numSamples < 2)
        return 0.0f;

    const int halfWindow = numSamples / 2;
    std::vector<float> diff (static_cast<size_t> (halfWindow), 0.0f);''',
'''float MicrotonalAutotuneAudioProcessor::detectPitchYIN (const float* buffer, int numSamples, double sampleRate) noexcept
{
    if (numSamples < 2)
        return 0.0f;
    const int halfWindow = numSamples / 2;
    if (static_cast<int>(yinAccumulator.size()) < halfWindow)
        return 0.0f;
    float* diff = yinAccumulator.data();
    std::fill_n(diff, halfWindow, 0.0f);''','YIN no allocation')
p=p.replace('diff[static_cast<size_t> (tau)]','diff[tau]')
p=p.replace('diff[static_cast<size_t> (tau + 1)]','diff[tau + 1]')
p=p.replace('diff[static_cast<size_t> (tauEstimate - 1)]','diff[tauEstimate - 1]')
p=p.replace('diff[static_cast<size_t> (tauEstimate)]','diff[tauEstimate]')
p=p.replace('diff[static_cast<size_t> (tauEstimate + 1)]','diff[tauEstimate + 1]')
needle='''std::vector<double> MicrotonalAutotuneAudioProcessor::getCurrentScaleRatios() const
{
    int customIdx = activeCustomPresetIndex.load();
    if (customIdx >= 0 && customIdx < customPresets.getNumPresets())
        return customPresets.getPreset (customIdx).ratios;

    int scaleIdx = currentScaleIndex.load();
    if (scaleIdx >= 0 && scaleIdx < ScaleDefinitions::getScaleCount())
        return ScaleDefinitions::getScale (scaleIdx).ratios;

    // Fallback: chromatic 12-EDO
    return ScaleDefinitions::getScale (0).ratios;
}
'''
append=needle+'''
void MicrotonalAutotuneAudioProcessor::writeScaleSnapshot (
    const std::vector<double>& ratios, double rootFrequency) noexcept
{
    const int current = publishedScaleSnapshot_.load(std::memory_order_relaxed);
    const int next = 1 - current;
    auto& snapshot = scaleSnapshots_[static_cast<std::size_t>(next)];
    snapshot = {};
    snapshot.rootFrequency = std::isfinite(rootFrequency) && rootFrequency > 0.0
        ? rootFrequency : 440.0;
    for (double ratio : ratios)
    {
        if (snapshot.count >= ModernPitchEngine::maxScaleRatios) break;
        if (std::isfinite(ratio) && ratio >= 1.0 && ratio < 2.0)
            snapshot.ratios[static_cast<std::size_t>(snapshot.count++)] = ratio;
    }
    if (snapshot.count == 0)
        snapshot.ratios[static_cast<std::size_t>(snapshot.count++)] = 1.0;
    publishedScaleSnapshot_.store(next, std::memory_order_release);
}

MicrotonalAutotuneAudioProcessor::ScaleSnapshot
MicrotonalAutotuneAudioProcessor::readScaleSnapshot() const noexcept
{
    return scaleSnapshots_[static_cast<std::size_t>(
        publishedScaleSnapshot_.load(std::memory_order_acquire))];
}

void MicrotonalAutotuneAudioProcessor::publishScaleSnapshot() noexcept
{ writeScaleSnapshot(getCurrentScaleRatios(), getRootFrequency()); }

void MicrotonalAutotuneAudioProcessor::selectBuiltInScale (int index) noexcept
{
    currentScaleIndex.store(juce::jlimit(0, ScaleDefinitions::getScaleCount() - 1, index));
    activeCustomPresetIndex.store(-1);
    publishScaleSnapshot();
}

void MicrotonalAutotuneAudioProcessor::selectCustomScale (int index) noexcept
{
    if (index >= 0 && index < customPresets.getNumPresets())
    { activeCustomPresetIndex.store(index); publishScaleSnapshot(); }
}

void MicrotonalAutotuneAudioProcessor::selectRootNote (int index) noexcept
{ rootNoteIndex.store(juce::jlimit(0, 18, index)); publishScaleSnapshot(); }
'''
if append not in p: p=repl(p,needle,append,'snapshot definitions')
p=repl(p,
'''        auto scaleRatios = getCurrentScaleRatios();
        double rootFreq  = getRootFrequency();''',
'''        const ScaleSnapshot scale = readScaleSnapshot();''','fixed snapshot callback')
p=repl(p,
'''        livePitchProcessor.process (buffer, scaleRatios, rootFreq, speedMs, amount);''',
'''        if (amount <= 0.0f)
            livePitchProcessor.processBypassed(buffer);
        else
            livePitchProcessor.process (buffer, scale.ratios.data(), scale.count,
                                        scale.rootFrequency, speedMs, amount);''','modern snapshot process')
p=repl(p,
'''    if (numSamples == 0 || totalNumInputChannels == 0)
        return;

    // Get parameters''',
'''    if (numSamples == 0 || totalNumInputChannels == 0)
        return;
    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        float* data = buffer.getWritePointer(channel);
        for (int sample = 0; sample < numSamples; ++sample)
            if (!std::isfinite(data[sample]) || std::fpclassify(data[sample]) == FP_SUBNORMAL)
                data[sample] = 0.0f;
    }

    // Get parameters''','processor input sanitization')
p=repl(p,
'''            if (customTree.isValid())
                customPresets.fromValueTree (customTree);
        }
    }
}''',
'''            if (customTree.isValid())
                customPresets.fromValueTree (customTree);
            publishScaleSnapshot();
        }
    }
}''','publish restored state')
write('Source/PluginProcessor.cpp',p)

e=read('Source/PluginEditor.cpp')
e=e.replace('processorRef.activeCustomPresetIndex.store (customIdx);','processorRef.selectCustomScale (customIdx);')
e=e.replace('''        processorRef.currentScaleIndex.store (scaleIdx);
        processorRef.activeCustomPresetIndex.store (-1);''','''        processorRef.selectBuiltInScale (scaleIdx);''')
if 'void MicrotonalAutotuneAudioProcessorEditor::onRootNoteSelected()' not in e:
    insertion='''void MicrotonalAutotuneAudioProcessorEditor::onRootNoteSelected()
{
    const int index = rootNoteSelector.getSelectedId() - 1;
    if (index >= 0) processorRef.selectRootNote(index);
}

void MicrotonalAutotuneAudioProcessorEditor::onModeSelected()
{
    const int mode = modeSelector.getSelectedId() - 1;
    if (mode >= 0) processorRef.updateProcessingMode(mode);
}

'''
    anchor='//==============================================================================\njuce::String MicrotonalAutotuneAudioProcessorEditor::trackingStateToString'
    if anchor not in e: raise RuntimeError('editor handler anchor missing')
    e=e.replace(anchor,insertion+anchor,1)
write('Source/PluginEditor.cpp',e)

ce=read('Source/CustomScaleEditor.cpp')
ce=repl(ce,'    if (success)\n    {\n        juce::AlertWindow::showMessageBoxAsync','    if (success)\n    {\n        processorRef.selectCustomScale(processorRef.getCustomPresets().getNumPresets() - 1);\n        juce::AlertWindow::showMessageBoxAsync','publish custom scale')
write('Source/CustomScaleEditor.cpp',ce)

cm=read('CMakeLists.txt')
cm=repl(cm,'project(MicrotonalAutotune VERSION 1.0.0 LANGUAGES C CXX OBJCXX)','project(MicrotonalAutotune VERSION 1.0.0 LANGUAGES C CXX)\nif(APPLE)\n    enable_language(OBJCXX)\nendif()','portable languages')
cm=repl(cm,'    FORMATS VST3 AU','    FORMATS VST3 $<$<PLATFORM_ID:Darwin>:AU>','portable formats')
if 'AUTOTUNE_ENABLE_SANITIZERS' not in cm:
    cm += '''\noption(AUTOTUNE_ENABLE_SANITIZERS "Enable ASan/UBSan" OFF)\nif(AUTOTUNE_ENABLE_SANITIZERS AND NOT MSVC)\n    foreach(target AutotuneModernPitchEngineTests AutotuneRealWorldRegression)\n        if(TARGET ${target})\n            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)\n            target_link_options(${target} PRIVATE -fsanitize=address,undefined)\n        endif()\n    endforeach()\nendif()\n'''
write('CMakeLists.txt',cm)

write('Docs/VALIDATION.md', '''# Release validation gate\n\nA build is not release-ready until pluginval strictness 10, sanitizers, randomized stress, deterministic state restore, editor lifecycle, all sample rates/block sizes, non-finite input, full corpus, low-register/subharmonic and stable-build comparison all pass on the same commit. Retain the VST3, complete log, candidate/diff CSV and failed-case WAV renders. Amount 0 must preserve latency-aligned samples, stereo width and correlation.\n''')
print('hardening patch applied')
