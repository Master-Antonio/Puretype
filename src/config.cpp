#include "config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace puretype
{
    Config& Config::Instance()
    {
        static Config instance;
        return instance;
    }

    static std::string Trim(const std::string& s)
    {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static std::string ToLower(const std::string& s)
    {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    void Config::ParseLine(const std::string& line, std::string& currentSection)
    {
        std::string trimmed = Trim(line);

        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') return;

        if (trimmed.front() == '[' && trimmed.back() == ']')
        {
            currentSection = ToLower(Trim(trimmed.substr(1, trimmed.size() - 2)));
            return;
        }

        const auto eqPos = trimmed.find('=');
        if (eqPos == std::string::npos) return;

        std::string key = ToLower(Trim(trimmed.substr(0, eqPos)));
        std::string value = Trim(trimmed.substr(eqPos + 1));

        if (const auto commentPos = value.find(';'); commentPos != std::string::npos)
        {
            value = Trim(value.substr(0, commentPos));
        }

        const std::string fullKey = currentSection.empty() ? key : (currentSection + "." + key);
        m_values[fullKey] = value;
    }

    std::string Config::GetValue(const std::string& key, const std::string& defaultVal,
                                 const std::string& monitorName) const
    {
        const std::string k = ToLower(key);

        // 1. Try App-specific override
        if (!m_processName.empty())
        {
            const std::string appKey = "app_" + ToLower(m_processName) + "." + k;
            if (const auto it = m_values.find(appKey); it != m_values.end()) return it->second;
        }

        // 2. Try Monitor-specific override
        if (!monitorName.empty())
        {
            const std::string monKey = "monitor_" + ToLower(monitorName) + "." + k;
            if (const auto it = m_values.find(monKey); it != m_values.end()) return it->second;
        }

        // 3. Fallback to General
        const std::string genKey = "general." + k;
        if (const auto it = m_values.find(genKey); it != m_values.end()) return it->second;

        return defaultVal;
    }

    bool Config::LoadFromFile(const std::string& iniPath, const std::string& processName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_values.clear();
        m_monitorDataCache.clear();
        m_processName = processName;

        std::ifstream file(iniPath);
        if (!file.is_open()) return false;

        std::string currentSection;
        std::string line;
        while (std::getline(file, line))
        {
            ParseLine(line, currentSection);
        }
        file.close();

        return true;
    }

    const ConfigData& Config::GetData(const std::string& monitorName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_monitorDataCache.find(monitorName);
        if (it != m_monitorDataCache.end()) return it->second;

        ConfigData d = ParseConfigData(monitorName);
        m_monitorDataCache[monitorName] = d;
        return m_monitorDataCache[monitorName];
    }

    ConfigData Config::ParseConfigData(const std::string& monitorName) const
    {
        ConfigData data;

        std::string panelStr = ToLower(GetValue("paneltype", "rwbg", monitorName));
        if (panelStr == "qd_oled_gen1" || panelStr == "qd_oled_triangle" ||
            panelStr == "qdoled" || panelStr == "triangular")
        {
            data.panelType = PanelType::QD_OLED_GEN1;
        }
        else if (panelStr == "qd_oled_gen3")
        {
            data.panelType = PanelType::QD_OLED_GEN3;
        }
        else if (panelStr == "qd_oled_gen4")
        {
            data.panelType = PanelType::QD_OLED_GEN4;
        }
        else if (panelStr == "rgwb")
        {
            data.panelType = PanelType::RGWB;
        }
        else if (panelStr == "none" || panelStr == "lcd" || panelStr == "off")
        {
            data.panelType = PanelType::RWBG;
            data.filterStrength = 0.0f; // LCD bypass
        }
        else
        {
            data.panelType = PanelType::RWBG;
        }

        try { data.filterStrength = std::stof(GetValue("filterstrength", "1.0", monitorName)); }
        catch (...) { data.filterStrength = 1.0f; }
        if (panelStr == "none" || panelStr == "lcd" || panelStr == "off") data.filterStrength = 0.0f;
        data.filterStrength = std::clamp(data.filterStrength, 0.0f, 5.0f);

        try { data.gamma = std::stof(GetValue("gamma", "1.0", monitorName)); }
        catch (...) { data.gamma = 1.0f; }
        data.gamma = std::clamp(data.gamma, 0.5f, 3.0f);

        try { data.oledGammaOutput = std::stof(GetValue("oledgammaoutput", "1.0", monitorName)); }
        catch (...) { data.oledGammaOutput = 1.0f; }
        data.oledGammaOutput = std::clamp(data.oledGammaOutput, 1.0f, 2.0f);

        std::string hintingStr = ToLower(GetValue("enablesubpixelhinting", "true", monitorName));
        data.enableSubpixelHinting = (hintingStr == "true" || hintingStr == "1" || hintingStr == "yes");

        std::string fracPosStr = ToLower(GetValue("enablefractionalpositioning", "true", monitorName));
        data.enableFractionalPositioning = (fracPosStr == "true" || fracPosStr == "1" || fracPosStr == "yes");

        try { data.lodThresholdSmall = std::stof(GetValue("lodthresholdsmall", "12.0", monitorName)); }
        catch (...) { data.lodThresholdSmall = 12.0f; }

        try { data.lodThresholdLarge = std::stof(GetValue("lodthresholdlarge", "24.0", monitorName)); }
        catch (...) { data.lodThresholdLarge = 24.0f; }

        data.lodThresholdSmall = std::clamp(data.lodThresholdSmall, 6.0f, 96.0f);
        data.lodThresholdLarge = std::clamp(data.lodThresholdLarge, data.lodThresholdSmall + 1.0f, 160.0f);

        try { data.woledCrossTalkReduction = std::stof(GetValue("woledcrosstalkreduction", "0.08", monitorName)); }
        catch (...) { data.woledCrossTalkReduction = 0.08f; }
        data.woledCrossTalkReduction = std::clamp(data.woledCrossTalkReduction, 0.0f, 1.0f);

        try { data.lumaContrastStrength = std::stof(GetValue("lumacontraststrength", "1.0", monitorName)); }
        catch (...) { data.lumaContrastStrength = 1.0f; }
        data.lumaContrastStrength = std::clamp(data.lumaContrastStrength, 1.0f, 3.0f);

        std::string stemStr = ToLower(GetValue("stemdarkeningenabled", "true", monitorName));
        data.stemDarkeningEnabled = (stemStr == "true" || stemStr == "1" || stemStr == "yes");

        try { data.stemDarkeningStrength = std::stof(GetValue("stemdarkeningstrength", "0.4", monitorName)); }
        catch (...) { data.stemDarkeningStrength = 0.4f; }
        data.stemDarkeningStrength = std::clamp(data.stemDarkeningStrength, 0.0f, 2.0f);

        std::string gammaStr = ToLower(GetValue("gammamode", "srgb", monitorName));
        data.gammaMode = (gammaStr == "oled") ? GammaMode::OLED : GammaMode::SRGB;

        try { data.highDpiThresholdLow = std::stof(GetValue("highdpithresholdlow", "144.0", monitorName)); }
        catch (...) { data.highDpiThresholdLow = 144.0f; }
        data.highDpiThresholdLow = std::clamp(data.highDpiThresholdLow, 96.0f, 384.0f);

        try { data.highDpiThresholdHigh = std::stof(GetValue("highdpithresholdhigh", "216.0", monitorName)); }
        catch (...) { data.highDpiThresholdHigh = 216.0f; }
        data.highDpiThresholdHigh = std::clamp(data.highDpiThresholdHigh, data.highDpiThresholdLow + 1.0f, 600.0f);

        std::string debugStr = ToLower(GetValue("enabled", "false", monitorName));
        data.debugEnabled = (debugStr == "true" || debugStr == "1" || debugStr == "yes");

        data.logFile = GetValue("logfile", "PURETYPE.log", monitorName);
        // debug usually doesn't have app/monitor overrides but okay

        std::string highlightStr = ToLower(GetValue("highlightrenderedglyphs", "false", monitorName));
        data.highlightRenderedGlyphs = (highlightStr == "true" || highlightStr == "1" || highlightStr == "yes");

        std::string blacklistStr = ToLower(GetValue("blacklist", "", monitorName));
        data.blacklist.clear();
        if (blacklistStr.empty())
        {
            data.blacklist = {
                "vgc.exe", "vgtray.exe", "easyanticheat.exe", "easyanticheat_eos.exe",
                "beservice.exe", "bedaisy.exe", "gameguard.exe", "nprotect.exe",
                "pnkbstra.exe", "pnkbstrb.exe", "faceit.exe", "faceit_ac.exe",
                "csgo.exe", "cs2.exe", "valorant.exe", "valorant-win64-shipping.exe",
                "r5apex.exe", "fortniteclient-win64-shipping.exe", "eldenring.exe",
                "gta5.exe", "rdr2.exe", "overwatchlauncher.exe", "rainbowsix.exe",
                "destiny2.exe", "tarkov.exe"
            };
        }
        else
        {
            std::stringstream ss(blacklistStr);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                std::string cleaned = Trim(item);
                if (!cleaned.empty()) data.blacklist.push_back(cleaned);
            }
        }

        return data;
    }
}