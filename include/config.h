#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace puretype
{
    enum class PanelType
    {
        RWBG, 
        RGWB, 
        QD_OLED_GEN1, 
        QD_OLED_GEN3, 
        QD_OLED_GEN4, 
    };

    enum class GammaMode
    {
        SRGB, 
        OLED, 
    };

    struct ConfigData
    {
        PanelType panelType = PanelType::RWBG;
        GammaMode gammaMode = GammaMode::SRGB;
        float filterStrength = 1.0f;
        float gamma = 1.0f;
        float oledGammaOutput = 1.0f;
        bool enableSubpixelHinting = true;
        bool enableFractionalPositioning = false;
        float lodThresholdSmall = 12.0f;
        float lodThresholdLarge = 24.0f;
        float woledCrossTalkReduction = 0.0f;
        float lumaContrastStrength = 1.20f;

        
        
        
        bool toneParityV2Enabled = true;
        bool dwriteFallbackV2Enabled = true;
        bool contrastSamplingCacheEnabled = true;
        bool colorGlyphBypassEnabled = true;

        
        
        float qdExpectedSepGen1 = -0.44f;
        float qdExpectedSepGen3 = -0.50f;
        float qdExpectedSepGen4 = -0.50f;
        
        float qdVerticalBlend = 0.15f;
        
        float chromaKeepScaleQD = 1.0f;
        float chromaKeepScaleWOLED = 1.0f;

        
        
        float textContrastHint = -1.0f;
        
        float dpiScaleHint = 1.0f;

        bool stemDarkeningEnabled = true;
        float stemDarkeningStrength = 0.45f;

        
        int interFontWeight = 400;
        float interOpticalSize = 18.0f;
        float interLetterSpacing = 0.3f;

        
        
        
        float highDpiThresholdLow = 144.0f;
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

        
        
        ConfigData GetData(const std::string& monitorName = "");

        static Config& Instance();

    private:
        std::string m_processName;
        std::mutex m_mutex;

        
        std::unordered_map<std::string, ConfigData> m_monitorDataCache;

        
        std::unordered_map<std::string, std::string> m_values;

        void ParseLine(const std::string& line, std::string& currentSection);

        
        std::string GetValue(const std::string& key, const std::string& defaultVal,
                             const std::string& monitorName = "") const;

        static std::string NormalizeMonitorName(const std::string& rawMonitorName);

        ConfigData ParseConfigData(const std::string& monitorName) const;
    };
}
