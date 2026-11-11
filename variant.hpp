#pragma once
#include <string>
struct AlbionVariant {
 bool converted=true; float nmsIou=.45f; unsigned maxDetections=100; std::wstring compiler;
 static const AlbionVariant& Get() {static const AlbionVariant v{};return v;}
};
