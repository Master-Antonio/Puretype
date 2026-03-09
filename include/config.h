

#pragma once

#include <string>
#include <unordered_map>

namespace puretype {

enum class PanelType {

    WRGB,

    QD_OLED_TRIANGLE
};

struct ConfigData {
    PanelType   panelType              = PanelType::WRGB;
    float       filterStrength         = 1.0f;
    float       gamma                  = 1.0f;

    bool        stemDarkeningEnabled   = true;
    float       stemDarkeningStrength  = 0.4f;

    bool        debugEnabled            = false;
    std::string logFile                 = "puretype.log";
    bool        highlightRenderedGlyphs = false;
};

class Config {
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
