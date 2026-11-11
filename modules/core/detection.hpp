// modules/core/detection.hpp
#pragma once
struct Detection { float x, y, w, h, score; int cls;
 // Metadata only. Raw boxes remain unchanged for existing consumers.
 unsigned trackId=0;
};
