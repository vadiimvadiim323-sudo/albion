// modules/core/runtime.hpp
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iterator>
inline constexpr int    kNetMax = 640;   // maksimalno podderzhivaemyy vkhod seti
inline constexpr size_t kTensorFloatsMax = size_t(kNetMax) * kNetMax * 3;

inline const wchar_t* kModelCandidates[] = {
    L"yolov8n.engine", L"yolov8n.plan", L"yolov8n.trt", L"yolov8n.onnx"
};
inline const wchar_t* kModelName = L"yolov8n.onnx";
inline constexpr float kConfThr = 0.35f;
// Dekodiruem tolko nuzhnyy klass (0 = person). -1 = vse klassy.
inline constexpr int    kDecodeClass = 0;
// Runtime porog uverennosti: menyaetsya polzunkom v okne nastroek.
inline std::atomic<float> gDecodeConf{ kConfThr };
// Realnyy vkhod seti beretsya iz modeli: 320x320, 416x416, 640x640 i t.d.
inline std::atomic<int> gNetSize{ kNetMax };
inline size_t TensorFloatsFor(int net) noexcept { return size_t(net) * size_t(net) * 3; }
// Forma vykhoda poslednego prognoza - dlya stroki statistiki.
inline std::atomic<int> gOutCh{ 0 };
inline std::atomic<int> gOutAnchors{ 0 };
inline constexpr float kIouThr = 0.45f;

inline const char* kClassNames[] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat",
    "dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack",
    "umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball",
    "kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket",
    "bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair",
    "couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
    "remote","keyboard","cell phone","microwave","oven","toaster","sink",
    "refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"
};
inline constexpr int kNumClasses = int(std::size(kClassNames));

