#include "ScaleDefinitions.h"
#include <cmath>
#include <algorithm>

bool ScaleDefinitions::initialized_ = false;
std::vector<ScaleInfo> ScaleDefinitions::scales_;

// Helper: build EDO scale (all notes)
static std::vector<double> buildEDO (int divisions)
{
    std::vector<double> ratios;
    ratios.reserve (static_cast<size_t> (divisions));
    for (int k = 0; k < divisions; ++k)
        ratios.push_back (std::pow (2.0, static_cast<double> (k) / static_cast<double> (divisions)));
    return ratios;
}

// Helper: pick specific degrees from 12-EDO
static std::vector<double> build12EDOSubset (const std::vector<int>& degrees)
{
    std::vector<double> ratios;
    ratios.reserve (degrees.size());
    for (int d : degrees)
        ratios.push_back (std::pow (2.0, static_cast<double> (d) / 12.0));
    return ratios;
}

std::vector<ScaleInfo> ScaleDefinitions::buildScales()
{
    std::vector<ScaleInfo> s;
    s.reserve (40);

    // ===================== 12-EDO Scales (Temperamento Equabile) =====================

    // Chromatic (all 12 notes)
    s.push_back ({ "Cromatica", "Temperamento Equabile",
        build12EDOSubset ({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }) });

    // Major / Ionian
    s.push_back ({ "Maggiore (Ionica)", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 4, 5, 7, 9, 11 }) });

    // Dorian
    s.push_back ({ "Dorica", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 3, 5, 7, 9, 10 }) });

    // Phrygian
    s.push_back ({ "Frigia", "Temperamento Equabile",
        build12EDOSubset ({ 0, 1, 3, 5, 7, 8, 10 }) });

    // Lydian
    s.push_back ({ "Lidia", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 4, 6, 7, 9, 11 }) });

    // Mixolydian
    s.push_back ({ "Misolidia", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 4, 5, 7, 9, 10 }) });

    // Aeolian / Natural Minor
    s.push_back ({ "Minore naturale (Eolia)", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 3, 5, 7, 8, 10 }) });

    // Locrian
    s.push_back ({ "Locria", "Temperamento Equabile",
        build12EDOSubset ({ 0, 1, 3, 5, 6, 8, 10 }) });

    // Melodic Minor (ascending)
    s.push_back ({ "Minore melodica", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 3, 5, 7, 9, 11 }) });

    // Harmonic Minor
    s.push_back ({ "Minore armonica", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 3, 5, 7, 8, 11 }) });

    // Major Pentatonic
    s.push_back ({ "Pentatonica maggiore", "Temperamento Equabile",
        build12EDOSubset ({ 0, 2, 4, 7, 9 }) });

    // Minor Pentatonic
    s.push_back ({ "Pentatonica minore", "Temperamento Equabile",
        build12EDOSubset ({ 0, 3, 5, 7, 10 }) });

    // ===================== Microtonal EDO =====================

    // 24 EDO (quarter tones)
    s.push_back ({ "24 EDO", "Microtonale", buildEDO (24) });

    // 19 EDO
    s.push_back ({ "19 EDO", "Microtonale", buildEDO (19) });

    // 31 EDO
    s.push_back ({ "31 EDO", "Microtonale", buildEDO (31) });

    // ===================== Pythagorean Scale =====================
    {
        // Based on pure fifths (3:2). 12 notes via cycle of fifths,
        // brought back into one octave and sorted.
        std::vector<double> pyth;
        pyth.reserve (12);
        for (int i = 0; i < 12; ++i)
        {
            double ratio = std::pow (3.0 / 2.0, static_cast<double> (i));
            // Bring into [1, 2)
            while (ratio >= 2.0) ratio /= 2.0;
            while (ratio < 1.0) ratio *= 2.0;
            pyth.push_back (ratio);
        }
        std::sort (pyth.begin(), pyth.end());
        s.push_back ({ "Pitagorica", "Storica", std::move (pyth) });
    }

    // ===================== Ptolemaic (Just Intonation, Syntonon Diatonic) =====================
    s.push_back ({ "Tolemaica (Just Intonation)", "Storica",
        { 1.0, 9.0/8.0, 5.0/4.0, 4.0/3.0, 3.0/2.0, 5.0/3.0, 15.0/8.0 } });

    // ===================== Byzantine Scales =====================
    // Byzantine music uses intervals measured in "moria" (72 per octave).
    // Mode I (Diatonic): intervals 12-10-8-12-12-10-8 moria
    {
        std::vector<int> intervals = { 12, 10, 8, 12, 12, 10, 8 };
        std::vector<double> ratios;
        ratios.push_back (1.0);
        int cum = 0;
        for (size_t i = 0; i < intervals.size() - 1; ++i)
        {
            cum += intervals[i];
            ratios.push_back (std::pow (2.0, static_cast<double> (cum) / 72.0));
        }
        s.push_back ({ "Bizantina - Modo I (Diatonico)", "Bizantina", std::move (ratios) });
    }
    // Mode II (Chromatic soft): 8-14-8-12-8-14-8
    {
        std::vector<int> intervals = { 8, 14, 8, 12, 8, 14, 8 };
        std::vector<double> ratios;
        ratios.push_back (1.0);
        int cum = 0;
        for (size_t i = 0; i < intervals.size() - 1; ++i)
        {
            cum += intervals[i];
            ratios.push_back (std::pow (2.0, static_cast<double> (cum) / 72.0));
        }
        s.push_back ({ "Bizantina - Modo II (Cromatico)", "Bizantina", std::move (ratios) });
    }
    // Mode III (Enharmonic): 6-20-4-12-6-20-4
    {
        std::vector<int> intervals = { 6, 20, 4, 12, 6, 20, 4 };
        std::vector<double> ratios;
        ratios.push_back (1.0);
        int cum = 0;
        for (size_t i = 0; i < intervals.size() - 1; ++i)
        {
            cum += intervals[i];
            ratios.push_back (std::pow (2.0, static_cast<double> (cum) / 72.0));
        }
        s.push_back ({ "Bizantina - Modo III (Enarmonico)", "Bizantina", std::move (ratios) });
    }

    // ===================== Arabic Maqamat =====================
    // Intervals in quarter-tones (24 EDO steps). Each maqam defined by its jins.

    // Rast: 4 3 3 4 4 3 3 (in quarter tones from 24-EDO)
    {
        std::vector<int> qt = { 0, 4, 7, 10, 14, 18, 21 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Rast", "Maqam Arabi", std::move (ratios) });
    }
    // Bayati: 3 3 4 4 2 4 4
    {
        std::vector<int> qt = { 0, 3, 6, 10, 14, 16, 20 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Bayati", "Maqam Arabi", std::move (ratios) });
    }
    // Saba: 3 3 2 6 2 4 4
    {
        std::vector<int> qt = { 0, 3, 6, 8, 14, 16, 20 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Saba", "Maqam Arabi", std::move (ratios) });
    }
    // Hijaz: 2 6 2 4 2 4 4
    {
        std::vector<int> qt = { 0, 2, 8, 10, 14, 16, 20 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Hijaz", "Maqam Arabi", std::move (ratios) });
    }
    // Nahawand: 4 2 4 4 2 4 4
    {
        std::vector<int> qt = { 0, 4, 6, 10, 14, 16, 20 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Nahawand", "Maqam Arabi", std::move (ratios) });
    }
    // Ajam: 4 4 2 4 4 4 2
    {
        std::vector<int> qt = { 0, 4, 8, 10, 14, 18, 22 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Ajam", "Maqam Arabi", std::move (ratios) });
    }
    // Kurd: 2 4 4 4 2 4 4
    {
        std::vector<int> qt = { 0, 2, 6, 10, 14, 16, 20 };
        std::vector<double> ratios;
        for (int q : qt)
            ratios.push_back (std::pow (2.0, static_cast<double> (q) / 24.0));
        s.push_back ({ "Maqam Kurd", "Maqam Arabi", std::move (ratios) });
    }

    // ===================== Slendro =====================
    // Approximately 5 quasi-equal steps per octave
    // Common approximation using 5-EDO
    s.push_back ({ "Slendro", "Gamelan",
        { std::pow (2.0, 0.0/5.0), std::pow (2.0, 1.0/5.0), std::pow (2.0, 2.0/5.0),
          std::pow (2.0, 3.0/5.0), std::pow (2.0, 4.0/5.0) } });

    // ===================== Pelog =====================
    // 7 notes, non-equal spacing. Ethnomusicological approximation in cents:
    // 0, 120, 270, 400, 535, 670, 800 (one common variant)
    {
        std::vector<double> centsValues = { 0.0, 120.0, 270.0, 400.0, 535.0, 670.0, 800.0 };
        std::vector<double> ratios;
        for (double c : centsValues)
            ratios.push_back (std::pow (2.0, c / 1200.0));
        s.push_back ({ "Pelog", "Gamelan", std::move (ratios) });
    }

    return s;
}

const std::vector<ScaleInfo>& ScaleDefinitions::getAllScales()
{
    if (! initialized_)
    {
        scales_ = buildScales();
        initialized_ = true;
    }
    return scales_;
}

int ScaleDefinitions::getScaleCount()
{
    return static_cast<int> (getAllScales().size());
}

const ScaleInfo& ScaleDefinitions::getScale (int index)
{
    return getAllScales()[static_cast<size_t> (index)];
}
