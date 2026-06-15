#include "PluginEditor.h"
#include "ScaleDefinitions.h"
#include "BinaryData.h"

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

    // ==================== Window setup ====================
    setSize (600, 400);
    setResizable (true, true);
    setResizeLimits (400, 280, 1200, 800);
}

MicrotonalAutotuneAudioProcessorEditor::~MicrotonalAutotuneAudioProcessorEditor() {}

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

    // Hide main components
    scaleSelector.setVisible (false);
    scaleSelectorLabel.setVisible (false);
    rootNoteSelector.setVisible (false);
    rootNoteSelectorLabel.setVisible (false);
    speedKnob.setVisible (false);
    speedLabel.setVisible (false);
    amountKnob.setVisible (false);
    amountLabel.setVisible (false);

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
    scaleSelector.setVisible (true);
    scaleSelectorLabel.setVisible (true);
    rootNoteSelector.setVisible (true);
    rootNoteSelectorLabel.setVisible (true);
    speedKnob.setVisible (true);
    speedLabel.setVisible (true);
    amountKnob.setVisible (true);
    amountLabel.setVisible (true);

    // Rebuild menu to include any new preset
    buildScaleMenu();

    repaint();
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

    // Plugin title
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText ("Microtonal Autotune", getLocalBounds().removeFromBottom (40),
                juce::Justification::centred);
}

void MicrotonalAutotuneAudioProcessorEditor::resized()
{
    if (showingScaleEditor && customScaleEditorPage != nullptr)
    {
        customScaleEditorPage->setBounds (getLocalBounds());
        return;
    }

    auto bounds = getLocalBounds();
    int width = bounds.getWidth();
    int height = bounds.getHeight();

    // Scale selector: top row, 8px margin top (~2mm)
    int selectorHeight = 28;
    int topMargin = 8;
    int labelWidth = 50;
    int gap = 10;

    // Divide horizontal space: scale selector gets ~60%, root note gets ~30%
    int scaleSelectorWidth = juce::jmin (250, (width - 160) * 6 / 10);
    int rootSelectorWidth = juce::jmin (100, (width - 160) * 3 / 10);
    int rootLabelWidth = 42;

    int totalTopWidth = labelWidth + 6 + scaleSelectorWidth + gap + rootLabelWidth + 4 + rootSelectorWidth;
    int topStartX = (width - totalTopWidth) / 2;

    scaleSelectorLabel.setBounds (topStartX, topMargin, labelWidth, selectorHeight);
    scaleSelector.setBounds (topStartX + labelWidth + 6, topMargin, scaleSelectorWidth, selectorHeight);

    int rootStartX = topStartX + labelWidth + 6 + scaleSelectorWidth + gap;
    rootNoteSelectorLabel.setBounds (rootStartX, topMargin, rootLabelWidth, selectorHeight);
    rootNoteSelector.setBounds (rootStartX + rootLabelWidth + 4, topMargin, rootSelectorWidth, selectorHeight);

    // Knobs: centered vertically, at 1/4 and 3/4 width
    int knobSize = juce::jmin (120, width / 4, height / 3);
    int knobCenterY = height / 2;

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
