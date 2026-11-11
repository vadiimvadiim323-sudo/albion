#pragma once
#include <windows.h>
// Returns true only after successful online license verification. Successful keys may be cached using per-user Windows DPAPI; online verification is never skipped.
bool RunAlbionAuthGate(HINSTANCE instance);
// After a successful gate the license screen stays on top as a click-through
// snapshot. The settings window takes its position and then calls
// AlbionAuthFinishHandoff to cross-fade the snapshot out (about 450 ms).
bool AlbionAuthHandoffRect(RECT* out);
void AlbionAuthFinishHandoff(HWND settingsWindow);
