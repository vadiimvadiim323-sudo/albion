#pragma once
#include <string>
#include <cctype>

// Normalize only the fixed application prefix; the random suffix is case-sensitive.
inline bool NormalizeAlbionLicense(std::string in, std::string& out) {
    out.clear();
    const auto left = in.find_first_not_of(" \t\r\n");
    if (left == std::string::npos) return false;
    const auto right = in.find_last_not_of(" \t\r\n");
    in = in.substr(left, right - left + 1);
    if (in.size() > 136) return false; // allow the optional 8-byte KEYAUTH- prefix
    for (unsigned char ch : in)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) return false;
    auto starts = [](const std::string& value, const char* prefix) {
        size_t i = 0;
        for (; prefix[i]; ++i) {
            if (i >= value.size()) return false;
            unsigned char c = static_cast<unsigned char>(value[i]);
            if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
            if (c != static_cast<unsigned char>(prefix[i])) return false;
        }
        return true;
    };
    if (starts(in, "KEYAUTH-")) in.erase(0, 8);
    if (in.size() > 128) return false;
    if (!starts(in, "ALBION-") || in.size() <= 7 || in.back() == '-') return false;
    in.replace(0, 7, "ALBION-");
    out = "KEYAUTH-" + in;
    return true;
}
