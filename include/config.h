#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace puretype
{
    enum class PanelType
    {
        RWBG, // LG WOLED — subpixel order R W B G
        RGWB, // LG WOLED — subpixel order R G W B (newer models)
        QD_OLED_GEN1, // Samsung QD-OLED gen 1-2 — oval, asymmetric R>B (AW3423DW etc.)
        QD_OLED_GEN3, // Samsung QD-OLED gen 3   — rectangular, R≈B nearly symmetric
        QD_OLED_GEN4, // Samsung QD-OLED gen 4   — rectangular, R=B equal width
    };

    struct ConfigData
    {
        PanelType panelType = PanelType::RWBG;
        float filterStrength = 1.0f;
        float gamma = 1.0f;
        bool enableSubpixelHinting = true;
        bool enableFractionalPositioning = true;
        float lodThresholdSmall = 12.0f;
        float lodThresholdLarge = 24.0f;
        float woledCrossTalkReduction = 0.0f;
        float lumaContrastStrength = 2.0f;

        bool stemDarkeningEnabled = true;
        float stemDarkeningStrength = 0.35f;

        bool debugEnabled = false;
        std::string logFile = "puretype.log";
        bool highlightRenderedGlyphs = false;

        std::vector<std::string> blacklist;
    };

    class Config
    {
    public:
        bool LoadFromFile(const std::string& iniPath);

        const ConfigData& Data() const { return m_data; }
        ConfigData& Data() { return m_data; }

        static Config& Instance();

    private:
        ConfigData m_data;

        std::unordered_map<std::string, std::string> m_values;

        void ParseLine(const std::string& line, std::string& currentSection);
        std::string GetValue(const std::string& section, const std::string& key,
                             const std::string& defaultVal) const;
    };
}