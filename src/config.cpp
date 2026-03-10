#include "config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace puretype {

Config& Config::Instance() {
    static Config instance;
    return instance;
}

static std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string ToLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

void Config::ParseLine(const std::string& line, std::string& currentSection) {
    std::string trimmed = Trim(line);

    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') return;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
        currentSection = ToLower(Trim(trimmed.substr(1, trimmed.size() - 2)));
        return;
    }

    auto eqPos = trimmed.find('=');
    if (eqPos == std::string::npos) return;

    std::string key = ToLower(Trim(trimmed.substr(0, eqPos)));
    std::string value = Trim(trimmed.substr(eqPos + 1));

    auto commentPos = value.find(';');
    if (commentPos != std::string::npos) {
        value = Trim(value.substr(0, commentPos));
    }

    std::string fullKey = currentSection.empty() ? key : (currentSection + "." + key);
    m_values[fullKey] = value;
}

std::string Config::GetValue(const std::string& section, const std::string& key,
                              const std::string& defaultVal) const {
    std::string fullKey = ToLower(section) + "." + ToLower(key);
    auto it = m_values.find(fullKey);
    if (it != m_values.end()) return it->second;
    return defaultVal;
}

bool Config::LoadFromFile(const std::string& iniPath) {
    std::ifstream file(iniPath);
    if (!file.is_open()) return false;

    std::string currentSection;
    std::string line;
    while (std::getline(file, line)) {
        ParseLine(line, currentSection);
    }
    file.close();

    std::string panelStr = ToLower(GetValue("general", "paneltype", "rwbg"));
    if (panelStr == "qd_oled_triangle" || panelStr == "qdoled" || panelStr == "triangular") {
        m_data.panelType = PanelType::QD_OLED_TRIANGLE;
    } else if (panelStr == "rgwb") {
        m_data.panelType = PanelType::RGWB;
    } else {
        m_data.panelType = PanelType::RWBG;
    }

    try { m_data.filterStrength = std::stof(GetValue("general", "filterstrength", "1.0")); }
    catch (...) { m_data.filterStrength = 1.0f; }

    try { m_data.gamma = std::stof(GetValue("general", "gamma", "1.0")); }
    catch (...) { m_data.gamma = 1.0f; }

    std::string hintingStr = ToLower(GetValue("general", "enablesubpixelhinting", "true"));
    m_data.enableSubpixelHinting =
        (hintingStr == "true" || hintingStr == "1" || hintingStr == "yes");

    std::string fracPosStr = ToLower(GetValue("general", "enablefractionalpositioning", "true"));
    m_data.enableFractionalPositioning =
        (fracPosStr == "true" || fracPosStr == "1" || fracPosStr == "yes");

    try { m_data.lodThresholdSmall = std::stof(GetValue("general", "lodthresholdsmall", "12.0")); }
    catch (...) { m_data.lodThresholdSmall = 12.0f; }

    try { m_data.lodThresholdLarge = std::stof(GetValue("general", "lodthresholdlarge", "24.0")); }
    catch (...) { m_data.lodThresholdLarge = 24.0f; }

    m_data.lodThresholdSmall = std::clamp(m_data.lodThresholdSmall, 6.0f, 96.0f);
    m_data.lodThresholdLarge = std::clamp(m_data.lodThresholdLarge, m_data.lodThresholdSmall + 1.0f, 160.0f);

    try { m_data.woledCrossTalkReduction = std::stof(GetValue("general", "woledcrosstalkreduction", "0.08")); }
    catch (...) { m_data.woledCrossTalkReduction = 0.08f; }
    m_data.woledCrossTalkReduction = std::clamp(m_data.woledCrossTalkReduction, 0.0f, 1.0f);

    try { m_data.lumaContrastStrength = std::stof(GetValue("general", "lumacontraststrength", "1.0")); }
    catch (...) { m_data.lumaContrastStrength = 1.0f; }
    m_data.lumaContrastStrength = std::clamp(m_data.lumaContrastStrength, 1.0f, 3.0f);

    std::string stemStr = ToLower(GetValue("general", "stemdarkeningenabled", "true"));
    m_data.stemDarkeningEnabled = (stemStr == "true" || stemStr == "1" || stemStr == "yes");

    try { m_data.stemDarkeningStrength = std::stof(GetValue("general", "stemdarkeningstrength", "0.4")); }
    catch (...) { m_data.stemDarkeningStrength = 0.4f; }

    std::string debugStr = ToLower(GetValue("debug", "enabled", "false"));
    m_data.debugEnabled = (debugStr == "true" || debugStr == "1" || debugStr == "yes");

    m_data.logFile = GetValue("debug", "logfile", "PURETYPE.log");

    std::string highlightStr = ToLower(GetValue("debug", "highlightrenderedglyphs", "false"));
    m_data.highlightRenderedGlyphs =
        (highlightStr == "true" || highlightStr == "1" || highlightStr == "yes");

    return true;
}

}
