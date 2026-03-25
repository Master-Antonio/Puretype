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

    std::string Config::GetValue(const std::string& section, const std::string& key,
                                 const std::string& defaultVal) const
    {
        const std::string fullKey = ToLower(section) + "." + ToLower(key);
        if (const auto it = m_values.find(fullKey); it != m_values.end()) return it->second;
        return defaultVal;
    }

    bool Config::LoadFromFile(const std::string& iniPath)
    {
        std::ifstream file(iniPath);
        if (!file.is_open()) return false;

        std::string currentSection;
        std::string line;
        while (std::getline(file, line))
        {
            ParseLine(line, currentSection);
        }
        file.close();

        // Panel type parsing.
        //
        // QD-OLED subpixel geometry differs across generations — each gen has
        // different physical R/B center positions that affect interpolation weights:
        //
        //   Gen 1-2 (AW3423DW, AW3423DWF, Odyssey G8 OLED 34" gen1, Odyssey Neo G9 OLED):
        //     Oval subpixels, R clearly larger than B.
        //     R center ≈ 0.280, G center = 0.500, B center ≈ 0.720
        //     Use: qd_oled_gen1  (alias: qd_oled_triangle, qdoled, triangular)
        //
        //   Gen 3 (Samsung Odyssey G8 OLED 27" QHD, Dell AW2725DF, 32" 4K models):
        //     Rectangular subpixels, R slightly wider than B but nearly symmetric.
        //     R center ≈ 0.250, G center = 0.500, B center ≈ 0.750
        //     Use: qd_oled_gen3
        //
        //   Gen 4 (MSI MPG 272URX, 27" 4K UHD models, 2024-2025):
        //     Rectangular subpixels, R ≈ B near-equal width.
        //     R center ≈ 0.250, G center = 0.500, B center ≈ 0.750
        //     Use: qd_oled_gen4
        //     (Same weights as gen3; kept separate for future tuning)

        if (std::string panelStr = ToLower(GetValue("general", "paneltype", "rwbg"));
            panelStr == "qd_oled_gen1" || panelStr == "qd_oled_triangle" ||
            panelStr == "qdoled" || panelStr == "triangular")
        {
            m_data.panelType = PanelType::QD_OLED_GEN1;
        }
        else if (panelStr == "qd_oled_gen3")
        {
            m_data.panelType = PanelType::QD_OLED_GEN3;
        }
        else if (panelStr == "qd_oled_gen4")
        {
            m_data.panelType = PanelType::QD_OLED_GEN4;
        }
        else if (panelStr == "rgwb")
        {
            m_data.panelType = PanelType::RGWB;
        }
        else
        {
            m_data.panelType = PanelType::RWBG;
        }

        try { m_data.filterStrength = std::stof(GetValue("general", "filterstrength", "1.0")); }
        catch (...) { m_data.filterStrength = 1.0f; }
        // Validate filter strength range
        m_data.filterStrength = std::clamp(m_data.filterStrength, 0.0f, 5.0f);

        try { m_data.gamma = std::stof(GetValue("general", "gamma", "1.0")); }
        catch (...) { m_data.gamma = 1.0f; }
        // Validate gamma range — values outside [0.5, 3.0] produce
        // extreme over/under-exposure that corrupts visual output
        m_data.gamma = std::clamp(m_data.gamma, 0.5f, 3.0f);

        try { m_data.oledGammaOutput = std::stof(GetValue("general", "oledgammaoutput", "1.0")); }
        catch (...) { m_data.oledGammaOutput = 1.0f; }
        m_data.oledGammaOutput = std::clamp(m_data.oledGammaOutput, 1.0f, 2.0f);

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
        // Clamp to a sane range. Values above ~2.0 invert coverage through
        // applyStemDarkening and make text invisible; negative values brighten stems.
        m_data.stemDarkeningStrength = std::clamp(m_data.stemDarkeningStrength, 0.0f, 2.0f);

        // Gamma mode: "srgb" (default) or "oled" (softer shadows for OLED panels).
        std::string gammaStr = ToLower(GetValue("general", "gammamode", "srgb"));
        m_data.gammaMode = (gammaStr == "oled") ? GammaMode::OLED : GammaMode::SRGB;

        // DPI-aware fade-out thresholds.
        try { m_data.highDpiThresholdLow = std::stof(GetValue("general", "highdpithresholdlow", "144.0")); }
        catch (...) { m_data.highDpiThresholdLow = 144.0f; }
        m_data.highDpiThresholdLow = std::clamp(m_data.highDpiThresholdLow, 96.0f, 384.0f);

        try { m_data.highDpiThresholdHigh = std::stof(GetValue("general", "highdpithresholdhigh", "216.0")); }
        catch (...) { m_data.highDpiThresholdHigh = 216.0f; }
        m_data.highDpiThresholdHigh = std::clamp(m_data.highDpiThresholdHigh,
                                                 m_data.highDpiThresholdLow + 1.0f, 600.0f);

        std::string debugStr = ToLower(GetValue("debug", "enabled", "false"));
        m_data.debugEnabled = (debugStr == "true" || debugStr == "1" || debugStr == "yes");

        m_data.logFile = GetValue("debug", "logfile", "PURETYPE.log");

        std::string highlightStr = ToLower(GetValue("debug", "highlightrenderedglyphs", "false"));
        m_data.highlightRenderedGlyphs =
            (highlightStr == "true" || highlightStr == "1" || highlightStr == "yes");

        std::string blacklistStr = ToLower(GetValue("general", "blacklist", ""));
        m_data.blacklist.clear();
        if (blacklistStr.empty())
        {
            // Default essential anti-cheat and system processes
            m_data.blacklist = {
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
                if (!cleaned.empty())
                {
                    m_data.blacklist.push_back(cleaned);
                }
            }
        }

        return true;
    }
}