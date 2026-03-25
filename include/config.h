#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

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

    enum class GammaMode
    {
        SRGB, // Standard IEC 61966-2-1 sRGB curve
        OLED, // OLED-calibrated: softer gamma (~2.0) below 20%, sRGB above
    };

    struct ConfigData
    {
        PanelType panelType = PanelType::RWBG;
        GammaMode gammaMode = GammaMode::SRGB;
        float filterStrength = 1.0f;
        float gamma = 1.0f;
        float oledGammaOutput = 1.0f;
        bool enableSubpixelHinting = true;
        bool enableFractionalPositioning = true;
        float lodThresholdSmall = 12.0f;
        float lodThresholdLarge = 24.0f;
        float woledCrossTalkReduction = 0.0f;
        float lumaContrastStrength = 2.0f;
        // Optional runtime hints used by ToneMapper for adaptive chroma behavior.
        // textContrastHint: [0..1], <0 disables the hint.
        float textContrastHint = -1.0f;
        // dpiScaleHint: [0..1], where 1.0 means no high-DPI attenuation.
        float dpiScaleHint = 1.0f;

        bool stemDarkeningEnabled = true;
        float stemDarkeningStrength = 0.35f;

        // DPI-aware fade-out thresholds.
        // Between dpiLow and dpiHigh, filterStrength and chromaKeep ramp down.
        // Above dpiHigh the filter is skipped entirely (GDI passthrough).
        float highDpiThresholdLow  = 144.0f;
        float highDpiThresholdHigh = 216.0f;

        bool debugEnabled = false;
        std::string logFile = "puretype.log";
        bool highlightRenderedGlyphs = false;

        std::vector<std::string> blacklist;
    };

    class Config
    {
    public:
        bool LoadFromFile(const std::string& iniPath, const std::string& processName);

        // Returns thread-safe references to monitor-specific config overrides
        const ConfigData& GetData(const std::string& monitorName = "");

        static Config& Instance();

    private:
        std::string m_processName;
        std::mutex m_mutex;

        // Cache for loaded monitor-specific configs
        std::unordered_map<std::string, ConfigData> m_monitorDataCache;

        // Raw key-value parsed from INI
        std::unordered_map<std::string, std::string> m_values;

        void ParseLine(const std::string& line, std::string& currentSection);
        
        // Context-aware value fetcher (Process > Monitor > General)
        std::string GetValue(const std::string& key, const std::string& defaultVal,
                             const std::string& monitorName = "") const;

        ConfigData ParseConfigData(const std::string& monitorName) const;
    };
}
