#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>

struct CustomScale
{
    juce::String name;
    std::vector<double> ratios; // sorted, within [1.0, 2.0)
};

class CustomScalePresets
{
public:
    CustomScalePresets();

    // Max 7 presets
    static constexpr int maxPresets = 7;

    int getNumPresets() const;
    const CustomScale& getPreset (int index) const;

    // Returns true if added successfully (count < 7, name non-empty, 3 <= ratios <= 33)
    bool addPreset (const juce::String& name, const std::vector<double>& ratios);
    bool removePreset (int index);

    // Serialization for state save/restore
    juce::ValueTree toValueTree() const;
    void fromValueTree (const juce::ValueTree& tree);

private:
    std::vector<CustomScale> presets_;
};
