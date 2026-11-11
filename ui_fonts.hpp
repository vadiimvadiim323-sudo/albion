#pragma once
// Shared UI typography. Montserrat (the font of the reference screenshots) is
// loaded privately from extras\fonts next to the EXE. Nothing is installed in
// the system. If the files are missing, Segoe UI is used instead.
#include <windows.h>
#include <string>
#include "app_paths.hpp"

inline std::wstring AlbionExeDir()
{
    return AlbionProgramDirectory();
}

inline std::wstring AlbionFontPath(const wchar_t* file)
{
    return AlbionExeDir() + L"extras\\fonts\\" + file;
}

// Returns true when Montserrat is available. Safe to call many times.
inline bool LoadAlbionFonts()
{
    static int state = -1;
    if (state >= 0) return state == 1;
    const wchar_t* files[] = { L"Montserrat-Regular.ttf", L"Montserrat-Medium.ttf",
                               L"Montserrat-SemiBold.ttf", L"Montserrat-Bold.ttf" };
    int loaded = 0;
    for (const wchar_t* f : files)
        if (AddFontResourceExW(AlbionFontPath(f).c_str(), FR_PRIVATE, nullptr) > 0) ++loaded;
    state = loaded == 4 ? 1 : 0;
    return state == 1;
}

// weight: 400 regular, 500 medium, 600 semibold, 700 bold. px is the cell
// height in pixels (negative lfHeight), like the rest of the UI code.
inline HFONT AlbionFont(int px, int weight)
{
    const bool m = LoadAlbionFonts();
    const wchar_t* face = L"Segoe UI";
    int w = weight;
    if (m) {
        if (weight >= 700)      { face = L"Montserrat"; w = FW_BOLD; }
        else if (weight >= 600) { face = L"Montserrat SemiBold"; w = FW_NORMAL; }
        else if (weight >= 500) { face = L"Montserrat Medium"; w = FW_NORMAL; }
        else                    { face = L"Montserrat"; w = FW_NORMAL; }
    } else if (weight == 600) face = L"Segoe UI Semibold";
    return CreateFontW(-px, 0, 0, 0, w, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH | FF_SWISS, face);
}
