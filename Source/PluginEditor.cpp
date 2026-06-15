#include "PluginEditor.h"
#include "ScaleDefinitions.h"
#include "BinaryData.h"
#include <cmath>

//==============================================================================
MicrotonalAutotuneAudioProcessorEditor::MicrotonalAutotuneAudioProcessorEditor (
    MicrotonalAutotuneAudioProcessor& p)
    : AudioProcessorEditor (p),
      processorRef (p)
{
    // Load background images from BinaryData
    bgImage = juce::ImageCache::getFromMemory (BinaryData::sfondo1_jpeg,
                                                BinaryData::sfondo1_jpegSize);
    bgImageScaleEditor = juce::ImageCache::getFromMemory (BinaryData::sfondo2_jpg,
                                                           BinaryData::sfondo2_jpgSize);

    // ==================== Scale Selector ====================
    scaleSelectorLabel.setText ("Scala:", juce::dontSendNotification);
    scaleSelectorLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    scaleSelectorLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    scaleSelectorLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (scaleSelectorLabel);

    scaleSelector.setJustificationType (juce::Justification::centredLeft);
    scaleSelector.onChange = [this]() { onScaleSelected(); };
    addAndMakeVisible (scaleSelector);

    buildScaleMenu();

    // ==================== Root Note Selector ====================
    rootNoteSelectorLabel.setText ("Nota:", juce::dontSendNotification);
    rootNoteSelectorLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    rootNoteSelectorLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    rootNoteSelectorLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (rootNoteSelectorLabel);

    rootNoteSelector.setJustificationType (juce::Justification::centredLeft);
    rootNoteSelector.addSectionHeading ("Temperamento Equabile");
    rootNoteSelector.addItem ("C",  1);
    rootNoteSelector.addItem ("C#", 2);
    rootNoteSelector.addItem ("D",  3);
    rootNoteSelector.addItem ("D#", 4);
    rootNoteSelector.addItem ("E",  5);
    rootNoteSelector.addItem ("F",  6);
    rootNoteSelector.addItem ("F#", 7);
    rootNoteSelector.addItem ("G",  8);
    rootNoteSelector.addItem ("G#", 9);
    rootNoteSelector.addItem ("A",  10);
    rootNoteSelector.addItem ("A#", 11);
    rootNoteSelector.addItem ("B",  12);
    rootNoteSelector.addSectionHeading ("Bizantino");
    rootNoteSelector.addItem ("Ni", 13);
    rootNoteSelector.addItem ("Pa", 14);
    rootNoteSelector.addItem ("Vu", 15);
    rootNoteSelector.addItem ("Ga", 16);
    rootNoteSelector.addItem ("Di", 17);
    rootNoteSelector.addItem ("Ke", 18);
    rootNoteSelector.addItem ("Zo", 19);
    rootNoteSelector.setSelectedId (processorRef.rootNoteIndex.load() + 1, juce::dontSendNotification);
    rootNoteSelector.onChange = [this]() { onRootNoteSelected(); };
    addAndMakeVisible (rootNoteSelector);

    // ==================== Processing Mode Selector ====================
    modeSelectorLabel.setText ("Modo:", juce::dontSendNotification);
    modeSelectorLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    modeSelectorLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    modeSelectorLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (modeSelectorLabel);

    modeSelector.setJustificationType (juce::Justification::centredLeft);
    modeSelector.addItem ("Slow",         1);
    modeSelector.addItem ("Quality",      2);
    modeSelector.addItem ("Live",         3);
    modeSelector.addItem ("Experimental", 4);
    modeSelector.setSelectedId (processorRef.processingMode.load() + 1, juce::dontSendNotification);
    modeSelector.onChange = [this]() { onModeSelected(); };
    addAndMakeVisible (modeSelector);

    // ==================== Speed Knob ====================
    speedKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    speedKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    speedKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xFF6C63FF));
    speedKnob.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    speedKnob.setTextValueSuffix (" ms");
    addAndMakeVisible (speedKnob);

    speedLabel.setText ("Velocita (ms)", juce::dontSendNotification);
    speedLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    speedLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    speedLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (speedLabel);

    speedAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processorRef.getAPVTS(), "speed", speedKnob);

    // ==================== Amount Knob ====================
    amountKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    amountKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    amountKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xFFFF6B6B));
    amountKnob.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    amountKnob.setTextValueSuffix (" %");
    addAndMakeVisible (amountKnob);

    amountLabel.setText ("Amount", juce::dontSendNotification);
    amountLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    amountLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    amountLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (amountLabel);

    amountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processorRef.getAPVTS(), "amount", amountKnob);

    // ==================== Creative Tempo page ====================
    tempoPageButton.onClick = [this]() { showTempoPage(); };
    tempoPageButton.setColour (juce::TextButton::buttonColourId,
                               juce::Colour (0xFF38405F));
    tempoPageButton.setColour (juce::TextButton::buttonOnColourId,
                               juce::Colour (0xFF6C63FF));
    addAndMakeVisible (tempoPageButton);

    tempoBackButton.onClick = [this]() { closeTempoPage(); };
    addAndMakeVisible (tempoBackButton);

    const auto configureModeButton = [this](juce::TextButton& button, int mode)
    {
        button.setClickingTogglesState (true);
        button.setRadioGroupId (7710);
        button.setColour (juce::TextButton::buttonColourId,
                          juce::Colour (0xFF2A3048));
        button.setColour (juce::TextButton::buttonOnColourId,
                          juce::Colour (0xFF6C63FF));
        button.onClick = [this, mode]() { setTempoModeParameter (mode); };
        addAndMakeVisible (button);
    };
    configureModeButton (tempoOffButton, 0);
    configureModeButton (tempoGlideButton, 1);
    configureModeButton (glideLockButton, 2);

    tempoDivisionLabel.setText ("Divisione", juce::dontSendNotification);
    tempoDivisionLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    tempoDivisionLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    tempoDivisionLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (tempoDivisionLabel);

    tempoDivisionSelector.addItem ("1/128", 1);
    tempoDivisionSelector.addItem ("1/64", 2);
    tempoDivisionSelector.addItem ("1/32", 3);
    tempoDivisionSelector.addItem ("1/16", 4);
    tempoDivisionSelector.addItem ("1/8", 5);
    addAndMakeVisible (tempoDivisionSelector);
    tempoDivisionAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processorRef.getAPVTS(), "tempoDivision", tempoDivisionSelector);

    tempoGlideLength.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    tempoGlideLength.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    tempoGlideLength.setTextValueSuffix (" %");
    tempoGlideLength.setColour (juce::Slider::rotarySliderFillColourId,
                                juce::Colour (0xFF5BC0EB));
    tempoGlideLength.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    addAndMakeVisible (tempoGlideLength);
    tempoGlideLengthLabel.setText ("Durata glide", juce::dontSendNotification);
    tempoGlideLengthLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    tempoGlideLengthLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    tempoGlideLengthLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (tempoGlideLengthLabel);
    tempoGlideLengthAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::SliderAttachment> (
            processorRef.getAPVTS(), "tempoGlidePercent", tempoGlideLength);

    tempoLockStrength.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    tempoLockStrength.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    tempoLockStrength.setTextValueSuffix (" %");
    tempoLockStrength.setColour (juce::Slider::rotarySliderFillColourId,
                                 juce::Colour (0xFFFFA24A));
    tempoLockStrength.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    addAndMakeVisible (tempoLockStrength);
    tempoLockStrengthLabel.setText ("Lock", juce::dontSendNotification);
    tempoLockStrengthLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    tempoLockStrengthLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    tempoLockStrengthLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (tempoLockStrengthLabel);
    tempoLockStrengthAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::SliderAttachment> (
            processorRef.getAPVTS(), "tempoLockStrength", tempoLockStrength);

    tempoSmartOnset.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (tempoSmartOnset);
    tempoSmartOnsetAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment> (
            processorRef.getAPVTS(), "tempoSmartOnset", tempoSmartOnset);

    setTempoControlsVisible (false);
    updateTempoModeButtons();

    // ==================== Window setup ====================
    setSize (640, 510);
    setResizable (true, true);
    setResizeLimits (520, 460, 1200, 820);

    displayedMetering = processorRef.getPitchMetering();
    startTimerHz (30);
}

MicrotonalAutotuneAudioProcessorEditor::~MicrotonalAutotuneAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void MicrotonalAutotuneAudioProcessorEditor::setMainControlsVisible(
    bool shouldBeVisible)
{
    scaleSelector.setVisible (shouldBeVisible);
    scaleSelectorLabel.setVisible (shouldBeVisible);
    rootNoteSelector.setVisible (shouldBeVisible);
    rootNoteSelectorLabel.setVisible (shouldBeVisible);
    speedKnob.setVisible (shouldBeVisible);
    speedLabel.setVisible (shouldBeVisible);
    amountKnob.setVisible (shouldBeVisible);
    amountLabel.setVisible (shouldBeVisible);
    modeSelector.setVisible (shouldBeVisible);
    modeSelectorLabel.setVisible (shouldBeVisible);
    tempoPageButton.setVisible (shouldBeVisible);
}

void MicrotonalAutotuneAudioProcessorEditor::setTempoControlsVisible(
    bool shouldBeVisible)
{
    tempoBackButton.setVisible (shouldBeVisible);
    tempoOffButton.setVisible (shouldBeVisible);
    tempoGlideButton.setVisible (shouldBeVisible);
    glideLockButton.setVisible (shouldBeVisible);
    tempoDivisionSelector.setVisible (shouldBeVisible);
    tempoDivisionLabel.setVisible (shouldBeVisible);
    tempoGlideLength.setVisible (shouldBeVisible);
    tempoGlideLengthLabel.setVisible (shouldBeVisible);
    tempoLockStrength.setVisible (shouldBeVisible);
    tempoLockStrengthLabel.setVisible (shouldBeVisible);
    tempoSmartOnset.setVisible (shouldBeVisible);
}

void MicrotonalAutotuneAudioProcessorEditor::showTempoPage()
{
    if (showingScaleEditor)
        return;

    showingTempoPage = true;
    setMainControlsVisible (false);
    setTempoControlsVisible (true);
    updateTempoModeButtons();
    resized();
    bgImageScaleEditor = juce::ImageCache::getFromMemory(BinaryData::sfondo3_jpeg,
        BinaryData::sfondo3_jpegSize);
    repaint();
}

void MicrotonalAutotuneAudioProcessorEditor::closeTempoPage()
{
    showingTempoPage = false;
    setTempoControlsVisible (false);
    setMainControlsVisible (true);
    resized();
    repaint();
}

void MicrotonalAutotuneAudioProcessorEditor::setTempoModeParameter(int modeIndex)
{
    modeIndex = juce::jlimit (0, 2, modeIndex);
    if (auto* parameter = processorRef.getAPVTS().getParameter ("tempoMode"))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (
            parameter->convertTo0to1 (static_cast<float> (modeIndex)));
        parameter->endChangeGesture();
    }
    updateTempoModeButtons();
}

void MicrotonalAutotuneAudioProcessorEditor::updateTempoModeButtons()
{
    const int mode = juce::jlimit (0, 2, static_cast<int> (std::lround (
        processorRef.getAPVTS().getRawParameterValue ("tempoMode")->load())));
    tempoOffButton.setToggleState (mode == 0, juce::dontSendNotification);
    tempoGlideButton.setToggleState (mode == 1, juce::dontSendNotification);
    glideLockButton.setToggleState (mode == 2, juce::dontSendNotification);

    const bool lockMode = mode == 2;
    tempoLockStrength.setEnabled (lockMode);
    tempoLockStrengthLabel.setEnabled (lockMode);
    tempoSmartOnset.setEnabled (lockMode);
}

//==============================================================================
void MicrotonalAutotuneAudioProcessorEditor::buildScaleMenu()
{
    scaleSelector.clear (juce::dontSendNotification);

    const auto& scales = ScaleDefinitions::getAllScales();
    int itemId = 1;

    // Group scales by category using section headers
    juce::String lastCategory;

    for (int i = 0; i < static_cast<int> (scales.size()); ++i)
    {
        const auto& scale = scales[static_cast<size_t> (i)];
        juce::String category (scale.category);

        if (category != lastCategory)
        {
            scaleSelector.addSectionHeading (category);
            lastCategory = category;
        }

        scaleSelector.addItem (juce::String (scale.name), itemId);
        itemId++;
    }

    // Separator before custom scales
    scaleSelector.addSeparator();
    scaleSelector.addSectionHeading ("Le tue scale");

    int numCustom = processorRef.getCustomPresets().getNumPresets();

    if (numCustom == 0)
    {
        // "Crea scala" item
        scaleSelector.addItem ("Crea scala...", 10000);
    }
    else
    {
        // Show custom presets
        for (int i = 0; i < numCustom; ++i)
        {
            const auto& preset = processorRef.getCustomPresets().getPreset (i);
            scaleSelector.addItem (preset.name, 10001 + i);
        }

        // "Crea scala" if under limit, with delete options
        if (numCustom < CustomScalePresets::maxPresets)
            scaleSelector.addItem ("Crea scala...", 10000);

        // Delete options
        scaleSelector.addSeparator();
        for (int i = 0; i < numCustom; ++i)
        {
            const auto& preset = processorRef.getCustomPresets().getPreset (i);
            scaleSelector.addItem ("Elimina: " + preset.name, 20001 + i);
        }
    }

    // Set current selection
    int customIdx = processorRef.activeCustomPresetIndex.load();
    if (customIdx >= 0 && customIdx < numCustom)
    {
        scaleSelector.setSelectedId (10001 + customIdx, juce::dontSendNotification);
    }
    else
    {
        int scaleIdx = processorRef.currentScaleIndex.load();
        scaleSelector.setSelectedId (scaleIdx + 1, juce::dontSendNotification);
    }
}

void MicrotonalAutotuneAudioProcessorEditor::onScaleSelected()
{
    int selectedId = scaleSelector.getSelectedId();

    if (selectedId == 0)
        return;

    // "Crea scala..."
    if (selectedId == 10000)
    {
        showCustomScaleEditor();
        return;
    }

    // Delete a custom preset
    if (selectedId >= 20001)
    {
        int deleteIdx = selectedId - 20001;
        processorRef.getCustomPresets().removePreset (deleteIdx);

        // If the deleted preset was active, revert to built-in
        if (processorRef.activeCustomPresetIndex.load() == deleteIdx)
        {
            processorRef.activeCustomPresetIndex.store (-1);
            processorRef.currentScaleIndex.store (0);
        }
        else if (processorRef.activeCustomPresetIndex.load() > deleteIdx)
        {
            // Adjust index after deletion
            processorRef.activeCustomPresetIndex.store (
                processorRef.activeCustomPresetIndex.load() - 1);
        }

        buildScaleMenu();
        return;
    }

    // Custom preset selected
    if (selectedId >= 10001 && selectedId < 20000)
    {
        int customIdx = selectedId - 10001;
        processorRef.activeCustomPresetIndex.store (customIdx);
        return;
    }

    // Built-in scale selected
    int scaleIdx = selectedId - 1;
    if (scaleIdx >= 0 && scaleIdx < ScaleDefinitions::getScaleCount())
    {
        processorRef.currentScaleIndex.store (scaleIdx);
        processorRef.activeCustomPresetIndex.store (-1);
    }
}

void MicrotonalAutotuneAudioProcessorEditor::showCustomScaleEditor()
{
    showingScaleEditor = true;

    // Hide main and creative-tempo controls.
    showingTempoPage = false;
    setMainControlsVisible (false);
    setTempoControlsVisible (false);

    // Create and show custom scale editor
    customScaleEditorPage = std::make_unique<CustomScaleEditor> (
        processorRef, *this, bgImageScaleEditor);
    addAndMakeVisible (*customScaleEditorPage);
    customScaleEditorPage->setBounds (getLocalBounds());
}

void MicrotonalAutotuneAudioProcessorEditor::customScaleEditorClosed()
{
    showingScaleEditor = false;

    // Remove custom scale editor
    customScaleEditorPage.reset();

    // Show main components
    setMainControlsVisible (true);
    setTempoControlsVisible (false);

    // Rebuild menu to include any new preset
    buildScaleMenu();

    repaint();
}

//==============================================================================
juce::String MicrotonalAutotuneAudioProcessorEditor::trackingStateToString (
    ModernPitchEngine::TrackingState state)
{
    switch (state)
    {
        case ModernPitchEngine::TrackingState::unvoiced:   return "Unvoiced";
        case ModernPitchEngine::TrackingState::attack:     return "Attack";
        case ModernPitchEngine::TrackingState::acquire:    return "Acquire";
        case ModernPitchEngine::TrackingState::stable:     return "Stable";
        case ModernPitchEngine::TrackingState::transition: return "Transition";
        case ModernPitchEngine::TrackingState::release:    return "Release";
    }

    return "Unknown";
}

void MicrotonalAutotuneAudioProcessorEditor::timerCallback()
{
    displayedMetering = processorRef.getPitchMetering();
    if (showingTempoPage)
        updateTempoModeButtons();
    repaint();
}

void MicrotonalAutotuneAudioProcessorEditor::drawMeterPanel (
    juce::Graphics& g,
    juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty())
        return;

    const auto panel = bounds.toFloat();
    g.setColour (juce::Colour (0xB0101422));
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour (juce::Colour (0x507F8CFF));
    g.drawRoundedRectangle (panel.reduced (0.5f), 8.0f, 1.0f);

    auto content = bounds.reduced (12, 8);
    auto valueRow = content.removeFromTop (24);
    auto statusRow = content.removeFromTop (20);
    content.removeFromTop (4);

    const auto pitchText = displayedMetering.detectedPitchHz > 0.0f
        ? juce::String (displayedMetering.detectedPitchHz, 1) + " Hz"
        : juce::String ("--");
    const auto targetText = displayedMetering.targetPitchHz > 0.0f
        ? juce::String (displayedMetering.targetPitchHz, 1) + " Hz"
        : juce::String ("--");

    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.setColour (juce::Colours::white);
    g.drawText ("Pitch: " + pitchText,
                valueRow.removeFromLeft (content.getWidth() / 3),
                juce::Justification::centredLeft);
    g.drawText ("Target: " + targetText,
                valueRow.removeFromLeft (content.getWidth() / 3),
                juce::Justification::centredLeft);
    g.drawText ("Correction: "
                    + juce::String (displayedMetering.correctionCents, 1)
                    + " ct",
                valueRow,
                juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (12.0f));
    g.setColour (juce::Colour (0xFFD8DDF5));
    juce::String status = "State: " + trackingStateToString (displayedMetering.state)
        + "   Paths: " + juce::String (displayedMetering.detectorSupport)
        + "/4   Octave: " + juce::String (displayedMetering.octaveState);
    if (displayedMetering.pendingOctaveObservations > 0)
        status += "   Confirm: "
               + juce::String (displayedMetering.pendingOctaveObservations);
    if (displayedMetering.dualSynthesisActive)
        status += "   Dual: "
               + juce::String (displayedMetering.transitionBlend * 100.0f, 0)
               + "%";
    if (displayedMetering.noiseReductionDb > 0.05f)
        status += "   Breath GR: "
               + juce::String (displayedMetering.noiseReductionDb, 1)
               + " dB";
    g.drawText (status, statusRow, juce::Justification::centredLeft);

    const auto drawBar = [&g](juce::Rectangle<int> area,
                              float value,
                              const juce::String& name,
                              juce::Colour colour)
    {
        value = juce::jlimit (0.0f, 1.0f, value);
        auto labelArea = area.removeFromLeft (76);
        g.setColour (juce::Colour (0xFFD8DDF5));
        g.drawText (name, labelArea, juce::Justification::centredLeft);

        auto bar = area.reduced (2, 5).toFloat();
        g.setColour (juce::Colour (0xFF272C40));
        g.fillRoundedRectangle (bar, 3.0f);
        auto fill = bar;
        fill.setWidth (bar.getWidth() * value);
        g.setColour (colour);
        g.fillRoundedRectangle (fill, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.drawText (juce::String (value * 100.0f, 0) + "%",
                    bar.toNearestInt(),
                    juce::Justification::centred);
    };

    auto firstBarRow = content.removeFromTop (24);
    content.removeFromTop (2);
    auto secondBarRow = content.removeFromTop (24);

    const int firstBarWidth = firstBarRow.getWidth() / 3;
    drawBar (firstBarRow.removeFromLeft (firstBarWidth),
             displayedMetering.confidence,
             "Confidence",
             juce::Colour (0xFF6C63FF));
    drawBar (firstBarRow.removeFromLeft (firstBarWidth),
             displayedMetering.voicing,
             "Voicing",
             juce::Colour (0xFF00C878));
    drawBar (firstBarRow,
             displayedMetering.consensus,
             "Consensus",
             juce::Colour (0xFFFFA24A));

    const int secondBarWidth = secondBarRow.getWidth() / 3;
    drawBar (secondBarRow.removeFromLeft (secondBarWidth),
             displayedMetering.breathiness,
             "Breath",
             juce::Colour (0xFF5BC0EB));
    drawBar (secondBarRow.removeFromLeft (secondBarWidth),
             displayedMetering.harmonicity,
             "Harmonic",
             juce::Colour (0xFF9BE564));
    drawBar (secondBarRow,
             displayedMetering.noisePath,
             "Noise path",
             juce::Colour (0xFFFFD166));
}

void MicrotonalAutotuneAudioProcessorEditor::drawTempoPage(
    juce::Graphics& g,
    juce::Rectangle<int> bounds)
{
    auto panel = bounds.reduced (24, 18);
    g.setColour (juce::Colour (0xB0101422));
    g.fillRoundedRectangle (panel.toFloat(), 12.0f);
    g.setColour (juce::Colour (0x607F8CFF));
    g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 12.0f, 1.0f);

    auto status = panel.reduced (18, 12);
    auto title = status.removeFromTop (38);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText ("Creative Tempo", title, juce::Justification::centred);

    status.removeFromTop (250);
    auto textRow = status.removeFromTop (28);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.setColour (juce::Colour (0xFFD8DDF5));

    juce::String syncText;
    if (processorRef.processingMode.load() == 0)
        syncText = "Richiede Quality, Live o Experimental";
    else if (!displayedMetering.tempoActive)
        syncText = "Disattivato";
    else if (displayedMetering.tempoHostSyncValid)
        syncText = "Host sync";
    else
        syncText = "BPM fallback / glide immediato";

    juce::String modeText = "Off";
    if (displayedMetering.tempoMode == CreativeTempo::Mode::tempoGlide)
        modeText = "Tempo Glide";
    else if (displayedMetering.tempoMode == CreativeTempo::Mode::glideLock)
        modeText = "Glide Lock";

    g.drawText (modeText + "   |   " + syncText
                    + "   |   "
                    + juce::String (displayedMetering.tempoBpm, 1) + " BPM"
                    + "   |   Glide "
                    + juce::String (displayedMetering.tempoGlideTimeMs, 1)
                    + " ms",
                textRow, juce::Justification::centred);

    auto waitingRow = status.removeFromTop (24);
    g.setColour (displayedMetering.tempoWaitingForGrid
        ? juce::Colour (0xFFFFA24A)
        : juce::Colour (0xFF9BE564));
    g.drawText (displayedMetering.tempoWaitingForGrid
                    ? "Target in attesa del prossimo punto di lock"
                    : "Target libero",
                waitingRow, juce::Justification::centred);

    auto phaseArea = status.removeFromTop (28).reduced (40, 7).toFloat();
    g.setColour (juce::Colour (0xFF272C40));
    g.fillRoundedRectangle (phaseArea, 4.0f);
    auto fill = phaseArea;
    fill.setWidth (phaseArea.getWidth()
        * juce::jlimit (0.0f, 1.0f, displayedMetering.tempoGridPhase));
    g.setColour (juce::Colour (0xFF5BC0EB));
    g.fillRoundedRectangle (fill, 4.0f);
}

//==============================================================================
void MicrotonalAutotuneAudioProcessorEditor::paint (juce::Graphics& g)
{
    if (showingScaleEditor)
        return; // CustomScaleEditor paints itself

    // Draw background image
    if (bgImage.isValid())
    {
        g.drawImage (bgImage, getLocalBounds().toFloat(),
                     juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        g.fillAll (juce::Colour (0xFF0F0F23));
    }

    // Semi-transparent overlay for readability
    g.setColour (juce::Colour (0x88000000));
    g.fillRect (getLocalBounds());

    if (showingTempoPage)
    {
        drawTempoPage (g, getLocalBounds());
        return;
    }

    auto lowerArea = getLocalBounds();
    auto titleArea = lowerArea.removeFromBottom (38);
    auto meterArea = lowerArea.removeFromBottom (136).reduced (18, 4);
    drawMeterPanel (g, meterArea);

    // Plugin title
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText ("Microtonal Autotune", titleArea,
                juce::Justification::centred);

    // Mode indicator dot — color depends on the selected mode
    int mode = processorRef.processingMode.load();
    if (mode > 0)
    {
        juce::Colour dotColour;
        switch (mode)
        {
            case 1:  dotColour = juce::Colour (0xFF4488FF); break; // Quality: blue
            case 2:  dotColour = juce::Colour (0xFF00CC66); break; // Live: green
            case 3:  dotColour = juce::Colour (0xFFFF8800); break; // Experimental: orange
            default: dotColour = juce::Colour (0xFF888888); break;
        }

        auto modeBounds = modeSelector.getBounds();
        int dotX = modeBounds.getRight() + 6;
        int dotY = modeBounds.getCentreY();
        g.setColour (dotColour);
        g.fillEllipse (static_cast<float> (dotX - 4), static_cast<float> (dotY - 4), 8.0f, 8.0f);
        // Outer glow
        g.setColour (dotColour.withAlpha (0.3f));
        g.fillEllipse (static_cast<float> (dotX - 7), static_cast<float> (dotY - 7), 14.0f, 14.0f);
    }
}

void MicrotonalAutotuneAudioProcessorEditor::resized()
{
    if (showingScaleEditor && customScaleEditorPage != nullptr)
    {
        customScaleEditorPage->setBounds (getLocalBounds());
        return;
    }

    if (showingTempoPage)
    {
        auto area = getLocalBounds().reduced (34, 26);
        tempoBackButton.setBounds (area.removeFromTop (30).removeFromLeft (90));
        area.removeFromTop (44);

        auto modeRow = area.removeFromTop (38);
        const int modeWidth = modeRow.getWidth() / 3;
        tempoOffButton.setBounds (modeRow.removeFromLeft (modeWidth).reduced (5, 2));
        tempoGlideButton.setBounds (modeRow.removeFromLeft (modeWidth).reduced (5, 2));
        glideLockButton.setBounds (modeRow.reduced (5, 2));

        area.removeFromTop (18);
        auto divisionRow = area.removeFromTop (54);
        tempoDivisionLabel.setBounds (divisionRow.removeFromLeft (120));
        tempoDivisionSelector.setBounds (divisionRow.removeFromLeft (150).reduced (6, 10));
        tempoSmartOnset.setBounds (divisionRow.reduced (18, 10));

        area.removeFromTop (12);
        auto knobs = area.removeFromTop (150);
        const int half = knobs.getWidth() / 2;
        auto glideArea = knobs.removeFromLeft (half);
        auto lockArea = knobs;
        const int knobSize = juce::jmin (112, glideArea.getHeight() - 28);
        tempoGlideLength.setBounds (glideArea.getCentreX() - knobSize / 2,
                                    glideArea.getY(), knobSize, knobSize);
        tempoGlideLengthLabel.setBounds (glideArea.getX(),
                                         glideArea.getY() + knobSize + 2,
                                         glideArea.getWidth(), 22);
        tempoLockStrength.setBounds (lockArea.getCentreX() - knobSize / 2,
                                     lockArea.getY(), knobSize, knobSize);
        tempoLockStrengthLabel.setBounds (lockArea.getX(),
                                          lockArea.getY() + knobSize + 2,
                                          lockArea.getWidth(), 22);
        return;
    }

    auto bounds = getLocalBounds();
    int width = bounds.getWidth();
    int height = bounds.getHeight();

    // Top row layout: Scale selector, Root note selector, Mode selector
    int selectorHeight = 28;
    int topMargin = 8;
    int gap = 10;

    // Calculate widths
    int scaleLabelWidth = 50;
    int scaleSelectorWidth = juce::jmin (220, (width - 220) * 5 / 10);
    int rootLabelWidth = 42;
    int rootSelectorWidth = juce::jmin (80, (width - 220) * 2 / 10);
    int modeLabelWidth = 42;
    int modeSelectorWidth = juce::jmin (120, (width - 220) * 3 / 10);

    int totalTopWidth = scaleLabelWidth + 6 + scaleSelectorWidth
                      + gap + rootLabelWidth + 4 + rootSelectorWidth
                      + gap + modeLabelWidth + 4 + modeSelectorWidth;
    int topStartX = (width - totalTopWidth) / 2;

    // Scale selector
    scaleSelectorLabel.setBounds (topStartX, topMargin, scaleLabelWidth, selectorHeight);
    scaleSelector.setBounds (topStartX + scaleLabelWidth + 6, topMargin, scaleSelectorWidth, selectorHeight);

    // Root note selector
    int rootStartX = topStartX + scaleLabelWidth + 6 + scaleSelectorWidth + gap;
    rootNoteSelectorLabel.setBounds (rootStartX, topMargin, rootLabelWidth, selectorHeight);
    rootNoteSelector.setBounds (rootStartX + rootLabelWidth + 4, topMargin, rootSelectorWidth, selectorHeight);

    // Mode selector
    int modeStartX = rootStartX + rootLabelWidth + 4 + rootSelectorWidth + gap;
    modeSelectorLabel.setBounds (modeStartX, topMargin, modeLabelWidth, selectorHeight);
    modeSelector.setBounds (modeStartX + modeLabelWidth + 4, topMargin, modeSelectorWidth, selectorHeight);

    tempoPageButton.setBounds (width / 2 - 45, topMargin + selectorHeight + 10, 90, 28);

    // Knobs: centered vertically, at 1/4 and 3/4 width
    int knobSize = juce::jmin (120, width / 4, height / 3);
    int knobCenterY = juce::jmax (90,
        juce::jmin (height / 2 - 20, height - 215));

    // Speed knob at 1/4
    int speedCenterX = width / 4;
    speedKnob.setBounds (speedCenterX - knobSize / 2, knobCenterY - knobSize / 2,
                         knobSize, knobSize);
    speedLabel.setBounds (speedCenterX - knobSize / 2, knobCenterY + knobSize / 2 + 4,
                          knobSize, 20);

    // Amount knob at 3/4
    int amountCenterX = width * 3 / 4;
    amountKnob.setBounds (amountCenterX - knobSize / 2, knobCenterY - knobSize / 2,
                          knobSize, knobSize);
    amountLabel.setBounds (amountCenterX - knobSize / 2, knobCenterY + knobSize / 2 + 4,
                           knobSize, 20);
}

void MicrotonalAutotuneAudioProcessorEditor::onRootNoteSelected()
{
    int selectedId = rootNoteSelector.getSelectedId();
    if (selectedId > 0)
        processorRef.rootNoteIndex.store (selectedId - 1);
}

void MicrotonalAutotuneAudioProcessorEditor::onModeSelected()
{
    int selectedId = modeSelector.getSelectedId();
    if (selectedId > 0)
    {
        int newMode = selectedId - 1; // ComboBox ID 1-4 → mode 0-3
        processorRef.updateProcessingMode (newMode);
        repaint(); // refresh the mode indicator dot
    }
}
