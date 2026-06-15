#include "CustomScaleEditor.h"
#include "PluginProcessor.h"
#include <cmath>
#include <algorithm>

CustomScaleEditor::CustomScaleEditor (MicrotonalAutotuneAudioProcessor& processor,
                                      CustomScaleEditorListener& listener,
                                      juce::Image backgroundImage)
    : processorRef (processor),
      listenerRef (listener),
      bgImage (std::move (backgroundImage))
{
    // Title
    titleLabel.setText ("Crea Scala Personalizzata", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);

    // Octave label
    octaveLabel.setText ("Clicca sul rettangolo per aggiungere divisioni (min 3, max 33)", juce::dontSendNotification);
    octaveLabel.setFont (juce::FontOptions (14.0f));
    octaveLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    octaveLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (octaveLabel);

    // Name label
    nameLabel.setText ("Nome della scala:", juce::dontSendNotification);
    nameLabel.setFont (juce::FontOptions (14.0f));
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setJustificationType (juce::Justification::left);
    addAndMakeVisible (nameLabel);

    // Name editor
    nameEditor.setFont (juce::FontOptions (14.0f));
    nameEditor.setMultiLine (false);
    nameEditor.setTextToShowWhenEmpty ("Inserisci il nome...", juce::Colours::grey);
    nameEditor.onTextChange = [this]() { updateInfoLabel(); };
    addAndMakeVisible (nameEditor);

    // Allow this component to receive keyboard focus so clicking outside
    // the text editor unfocuses it
    setWantsKeyboardFocus (true);

    // Save button
    saveButton.setEnabled (false);
    saveButton.onClick = [this]() { onSave(); };
    addAndMakeVisible (saveButton);

    // Back button
    backButton.onClick = [this]() { listenerRef.customScaleEditorClosed(); };
    addAndMakeVisible (backButton);

    // Info label
    infoLabel.setFont (juce::FontOptions (13.0f));
    infoLabel.setColour (juce::Label::textColourId, juce::Colours::lightyellow);
    infoLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (infoLabel);

    updateInfoLabel();
}

CustomScaleEditor::~CustomScaleEditor() {}

void CustomScaleEditor::paint (juce::Graphics& g)
{
    // Draw background image
    if (bgImage.isValid())
    {
        g.drawImage (bgImage, getLocalBounds().toFloat(),
                     juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        g.fillAll (juce::Colour (0xFF1A1A2E));
    }

    // Semi-transparent overlay for readability
    g.setColour (juce::Colour (0x99000000));
    g.fillRect (getLocalBounds());

    // Draw the octave rectangle
    g.setColour (juce::Colour (0xFF2A2A4A));
    g.fillRect (octaveRect);

    g.setColour (juce::Colour (0xFFCCCCFF));
    g.drawRect (octaveRect, 2);

    // Draw frequency labels at start and end
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (juce::Colours::white);
    g.drawText ("1x", octaveRect.getX(), octaveRect.getBottom() + 2, 30, 16,
                juce::Justification::centredLeft);
    g.drawText ("2x", octaveRect.getRight() - 30, octaveRect.getBottom() + 2, 30, 16,
                juce::Justification::centredRight);

    // Draw divisions as vertical lines
    g.setColour (juce::Colour (0xFFFF6B6B));
    auto rectF = octaveRect.toFloat();

    for (size_t i = 0; i < divisions.size(); ++i)
    {
        double logPos = divisions[i]; // [0, 1] in log2 space
        float xPos = rectF.getX() + static_cast<float> (logPos) * rectF.getWidth();

        g.drawLine (xPos, rectF.getY(), xPos, rectF.getBottom(), 2.0f);

        // Draw cents value above the line
        double cents = logPos * 1200.0;
        juce::String centsStr = juce::String (static_cast<int> (std::round (cents))) + "c";
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (centsStr, static_cast<int> (xPos) - 20, octaveRect.getY() - 16, 40, 14,
                    juce::Justification::centred);
    }
}

void CustomScaleEditor::resized()
{
    auto bounds = getLocalBounds().reduced (20);

    // Title at top
    titleLabel.setBounds (bounds.removeFromTop (30));
    bounds.removeFromTop (10);

    // Octave label
    octaveLabel.setBounds (bounds.removeFromTop (20));
    bounds.removeFromTop (10);

    // Octave rectangle — central, good height
    int rectHeight = juce::jmax (60, bounds.getHeight() / 4);
    int rectMargin = bounds.getWidth() / 10;
    octaveRect = bounds.removeFromTop (rectHeight).reduced (rectMargin, 0);
    bounds.removeFromTop (25); // space for cents labels + gap

    // Info label
    infoLabel.setBounds (bounds.removeFromTop (20));
    bounds.removeFromTop (10);

    // Name section
    auto nameRow = bounds.removeFromTop (30);
    nameLabel.setBounds (nameRow.removeFromLeft (130));
    nameEditor.setBounds (nameRow);
    bounds.removeFromTop (10);

    // Buttons
    auto buttonRow = bounds.removeFromTop (35);
    int btnWidth = 120;
    int gap = 20;
    int totalBtnWidth = btnWidth * 2 + gap;
    int startX = buttonRow.getCentreX() - totalBtnWidth / 2;

    backButton.setBounds (startX, buttonRow.getY(), btnWidth, 35);
    saveButton.setBounds (startX + btnWidth + gap, buttonRow.getY(), btnWidth, 35);
}

void CustomScaleEditor::mouseDown (const juce::MouseEvent& event)
{
    auto clickPos = event.getPosition();

    // Clicking anywhere outside the text editor unfocuses it
    if (! nameEditor.getBounds().contains (clickPos))
    {
        nameEditor.unfocusAllComponents();
        grabKeyboardFocus();
    }

    if (! octaveRect.contains (clickPos))
        return;

    if (static_cast<int> (divisions.size()) >= maxDivisions)
        return;

    // Convert click position to log2 ratio position [0, 1]
    float relX = static_cast<float> (clickPos.getX() - octaveRect.getX())
                 / static_cast<float> (octaveRect.getWidth());

    relX = juce::jlimit (0.01f, 0.99f, relX); // Avoid exact boundaries

    // relX is already in log2 space since the rectangle IS the log representation
    double logPos = static_cast<double> (relX);

    // Check that we don't place too close to an existing division
    constexpr double minDistance = 0.01; // ~12 cents minimum separation
    bool tooClose = false;
    for (double d : divisions)
    {
        if (std::abs (d - logPos) < minDistance)
        {
            tooClose = true;
            break;
        }
    }

    if (tooClose)
        return;

    divisions.push_back (logPos);
    std::sort (divisions.begin(), divisions.end());

    updateInfoLabel();
    repaint();
}

void CustomScaleEditor::updateInfoLabel()
{
    int count = static_cast<int> (divisions.size());
    juce::String text = "Divisioni: " + juce::String (count);

    if (count < minDivisions)
        text += " (servono almeno " + juce::String (minDivisions) + " per salvare)";
    else if (count >= maxDivisions)
        text += " (massimo raggiunto)";

    infoLabel.setText (text, juce::dontSendNotification);

    // Enable save only if >= 3 divisions and name is not empty
    bool canSave = count >= minDivisions && nameEditor.getText().isNotEmpty();
    saveButton.setEnabled (canSave);
}

void CustomScaleEditor::onSave()
{
    juce::String name = nameEditor.getText().trim();
    if (name.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Errore", "Inserisci un nome per la scala.");
        return;
    }

    if (static_cast<int> (divisions.size()) < minDivisions)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Errore", "Servono almeno 3 divisioni.");
        return;
    }

    // Convert log positions to frequency ratios
    // Each division position d (in [0,1] log2 space) → ratio = 2^d
    std::vector<double> ratios;
    ratios.push_back (1.0); // Always include unison

    for (double d : divisions)
        ratios.push_back (std::pow (2.0, d));

    std::sort (ratios.begin(), ratios.end());

    // Remove duplicates
    auto last = std::unique (ratios.begin(), ratios.end(),
        [](double a, double b) { return std::abs (a - b) < 1e-8; });
    ratios.erase (last, ratios.end());

    bool success = processorRef.getCustomPresets().addPreset (name, ratios);

    if (success)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            "Scala salvata", "La scala \"" + name + "\" e' stata salvata con successo.");
        listenerRef.customScaleEditorClosed();
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Errore", "Impossibile salvare la scala. Verifica che ci siano meno di 7 preset.");
    }
}
