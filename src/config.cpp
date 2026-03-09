

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

    std::string panelStr = ToLower(GetValue("general", "paneltype", "wrgb"));
    if (panelStr == "qd_oled_triangle" || panelStr == "qdoled" || panelStr == "triangular") {
        m_data.panelType = PanelType::QD_OLED_TRIANGLE;
    } else {
        m_data.panelType = PanelType::WRGB;
    }

    try { m_data.filterStrength = std::stof(GetValue("general", "filterstrength", "1.0")); }
    catch (...) { m_data.filterStrength = 1.0f; }

    try { m_data.gamma = std::stof(GetValue("general", "gamma", "1.0")); }
    catch (...) { m_data.gamma = 1.0f; }

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
