#pragma once
// FIX-V11-01/07: Pure, versioned validation BEFORE changing live settings.
// Keep this file free of Win32/UI/model code so malformed inputs can be fuzzed.
#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>
#include <string>

namespace albion {
struct ConfigDocument {
    std::map<std::string, std::string> values;
    std::map<std::string, double> numbers;
    std::string error;
    double Number(const char* key, double fallback) const {
        auto i = numbers.find(key); return i == numbers.end() ? fallback : i->second;
    }
    static std::string Trim(const std::string& s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }
    struct Field { const char* name; double low, high; bool integer; };
    inline static constexpr Field schema[] = {
        {"version",1,1,true}, {"radius",30,600,true}, {"confidence",.05,.95,false},
        {"aimHeight",0,1,false}, {"targetBodyPart",0,3,true}, {"headYOffsetPixels",-500,500,true},
        {"smoothing",.02,2,false}, {"deadZone",0,1200,false}, {"targetClass",-1,79,true},
        {"sendThreshold",0,127,false}, {"maxDelta",1,32767,false},
        {"predictionEnabled",0,1,true}, {"predictionStrength",0,1,false}, {"predictionMaxGapMs",50,500,false},
        {"leadSeconds",0,1,false}, {"maxLeadPixels",0,32767,false}, {"latencySeconds",0,1,false},
        {"minLeadSpeed",0,100000,false}, {"velocityAlpha",0,1,false}, {"velocityJitterPx",0,1200,false},
        {"targetSmoothAlpha",0,1,false}, {"maxGain",0,4,false}, {"graceSeconds",0,10,false},
        {"matchRadius",0,32767,false}, {"bangBang",0,1,true}, {"hardMode",0,1,true},
        {"smoothTracking",0,1,true}, {"trackingOutputScale",.05,2,false}, {"bangGain",0,4,false},
        {"movementPath",0,4,true}, {"settleSeconds",0,10,false}, {"deadZoneRadius",0,1200,false},
        {"deadZoneVelCancel",0,100000,false}, {"deadZoneExit",1,4,false},
        {"autoSensitivity",0,1,true}, {"asyncMover",0,1,true}, {"moverHz",100,4000,false},
        {"calibMaxTargetSpeed",0,100000,false}, {"aimMethod",0,1,true}, {"captureMethod",0,1,true},
        {"fpsLimit",0,240,true}, {"showRegion",0,1,true}, {"showBoxes",0,1,true},
        {"showStats",0,1,true}, {"showConfidence",0,1,true}, {"activationKey",1,254,true}
    };
    bool Fail(const std::string& why) { error = why; return false; }
    bool Parse(const std::string& input) {
        values.clear(); numbers.clear(); error.clear();
        if (input.empty() || input.size() > 4u * 1024u * 1024u) return Fail("Empty or oversized config");
        if (input.find('\0') != std::string::npos) return Fail("Embedded NUL in config");
        std::string text = input;
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        std::istringstream lines(text); std::string line; unsigned lineNo = 0;
        while (std::getline(lines, line)) {
            ++lineNo; line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.size() > 8192) return Fail("Config line too long");
            const auto eq = line.find('=');
            if (eq == std::string::npos) return Fail("Expected key=value at line " + std::to_string(lineNo));
            const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
            if (key.empty() || value.empty()) return Fail("Empty key/value at line " + std::to_string(lineNo));
            if (!values.emplace(key, value).second) return Fail("Duplicate field: " + key);
            if (key == "model") {
                // Only a filename from the scanned models directory is accepted by the caller.
                if (value.size() > 1024 || value == "." || value == ".." ||
                    value.find_first_of("\\/:*?\"<>|\r\n") != std::string::npos)
                    return Fail("Invalid model filename");
                continue;
            }
            const Field* field = nullptr;
            for (const auto& f : schema) if (key == f.name) { field = &f; break; }
            if (!field) return Fail("Unknown field: " + key);
            double number = 0;
            std::istringstream numeric(value); numeric.imbue(std::locale::classic());
            numeric >> std::noskipws >> number;
            if (!numeric || !numeric.eof() || !std::isfinite(number) ||
                number < field->low || number > field->high ||
                (field->integer && std::trunc(number) != number)) {
                // FIX-V11.1-VERSION: say WHY a newer/older file is rejected
                // instead of a generic range error; versions are never guessed.
                if (key == "version") return Fail("Unsupported config version: " + value.substr(0, 32) + " (expected 1)");
                return Fail("Invalid/out-of-range field: " + key);
            }
            if (key == "fpsLimit" && number != 0 && number != 30 && number != 60 &&
                number != 120 && number != 144 && number != 240) return Fail("Unsupported fpsLimit");
            numbers.emplace(key, number);
        }
        // Old v1 files may omit later optional fields; restore DEFAULTS, not the
        // previously loaded profile. Unknown future versions must not be guessed.
        for (const char* required : {"version", "radius", "confidence", "smoothing", "model"})
            if (!values.count(required)) return Fail(std::string("Missing field: ") + required);
        return true;
    }
};
} // namespace albion
