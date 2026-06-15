#pragma once

#include <vector>
#include <string>
#include <utility>

// Each scale is a list of frequency ratios within one octave [1.0, 2.0).
// The unison (1.0) is always implicit and included.
// The octave (2.0) is NOT included — it is handled by octave transposition.

struct ScaleInfo
{
    std::string name;
    std::string category;
    std::vector<double> ratios; // sorted ascending, starting with 1.0
};

class ScaleDefinitions
{
public:
    static const std::vector<ScaleInfo>& getAllScales();
    static int getScaleCount();
    static const ScaleInfo& getScale (int index);

private:
    static std::vector<ScaleInfo> buildScales();
    static std::vector<ScaleInfo> scales_;
    static bool initialized_;
};
