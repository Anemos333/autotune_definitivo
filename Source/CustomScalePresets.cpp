#include "CustomScalePresets.h"
#include <algorithm>

CustomScalePresets::CustomScalePresets() {}

int CustomScalePresets::getNumPresets() const
{
    return static_cast<int> (presets_.size());
}

const CustomScale& CustomScalePresets::getPreset (int index) const
{
    return presets_[static_cast<size_t> (index)];
}

bool CustomScalePresets::addPreset (const juce::String& name, const std::vector<double>& ratios)
{
    if (static_cast<int> (presets_.size()) >= maxPresets)
        return false;
    if (name.isEmpty())
        return false;
    if (ratios.size() < 3 || ratios.size() > 33)
        return false;

    CustomScale cs;
    cs.name = name;
    cs.ratios = ratios;
    std::sort (cs.ratios.begin(), cs.ratios.end());

    presets_.push_back (std::move (cs));
    return true;
}

bool CustomScalePresets::removePreset (int index)
{
    if (index < 0 || index >= static_cast<int> (presets_.size()))
        return false;

    presets_.erase (presets_.begin() + index);
    return true;
}

juce::ValueTree CustomScalePresets::toValueTree() const
{
    juce::ValueTree tree ("CustomScales");

    for (const auto& preset : presets_)
    {
        juce::ValueTree scaleTree ("Scale");
        scaleTree.setProperty ("name", preset.name, nullptr);

        juce::String ratioStr;
        for (size_t i = 0; i < preset.ratios.size(); ++i)
        {
            if (i > 0) ratioStr += ",";
            ratioStr += juce::String (preset.ratios[i], 10);
        }
        scaleTree.setProperty ("ratios", ratioStr, nullptr);

        tree.addChild (scaleTree, -1, nullptr);
    }

    return tree;
}

void CustomScalePresets::fromValueTree (const juce::ValueTree& tree)
{
    presets_.clear();

    if (! tree.isValid() || tree.getType() != juce::Identifier ("CustomScales"))
        return;

    for (int i = 0; i < tree.getNumChildren() && i < maxPresets; ++i)
    {
        auto scaleTree = tree.getChild (i);
        if (scaleTree.getType() != juce::Identifier ("Scale"))
            continue;

        CustomScale cs;
        cs.name = scaleTree.getProperty ("name").toString();

        juce::String ratioStr = scaleTree.getProperty ("ratios").toString();
        juce::StringArray tokens;
        tokens.addTokens (ratioStr, ",", "");

        for (const auto& token : tokens)
        {
            double val = token.getDoubleValue();
            if (val >= 1.0 && val < 2.0)
                cs.ratios.push_back (val);
        }

        if (cs.ratios.size() >= 3 && cs.ratios.size() <= 33 && cs.name.isNotEmpty())
        {
            std::sort (cs.ratios.begin(), cs.ratios.end());
            presets_.push_back (std::move (cs));
        }
    }
}
